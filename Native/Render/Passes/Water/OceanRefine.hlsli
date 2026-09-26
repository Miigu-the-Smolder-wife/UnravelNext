// First intersection of a ray with the water height clipmap (FEATURES_GAME 1.8): the first point where the ray o + t v
// (world, v unit) meets the bilinear clipmap surface H (OceanHeight.hlsli), searched from tShell with the max mips. The
// clipmap is the world-space water surface for secondary rays, the reference path tracer and queries; its texels are
// 14-29 px at 4K where the camera sees them, so the camera view has its own pixel-density surface (1.8, view grid).
//   - level: the finest level whose window holds the point, or the coarsest with s_l <= f / 2 (f = t pixelAngle, the ray
//     footprint) when that is coarser; the level only grows along the ray (distance from the camera grows)
//   - integer traversal: the current cell (mip 0) is the state; the node at mip m is cell >> m. A node whose max lies
//     below the ray's lowest height across it has no hit (the bilinear surface is below its max corner): the ray moves
//     to the neighbour across the node's exit face (exact integer step on that axis; on the other axis the cell at the
//     exit point, clamped into the node) and climbs a mip; otherwise it descends. A cell is intersected exactly: along
//     the ray the bilinear surface is quadratic in t and the first root in the cell is taken. Positions are relative to
//     the level origin (the subtraction near the camera is exact), so the cell arithmetic keeps full precision far from
//     the world origin
//   - the search starts at the coarsest mip and keeps its mip across level switches
//   - worst case (a proof, so the visit bound never changes a result): every skip and every missed leaf moves the cell
//     one step along the ray's direction on one axis (a node skip moves it past the node), so per level there are at
//     most 511 + 511 such steps; a descent undoes an ascent, ascents happen only at those steps (plus the 8 of the
//     starting mip), and level switches are at most L - 1. Visits <= 2 x 1024 x L + 32 = OCEAN_REFINE_VISITS(L)
// Parameter buffer (raw, `params` SRV): row 0 height SRV, bounds SRV, levels, 0; row 1 camera x, camera z, s_0, water
// level; row 2 pixel angle (rad), t max (m), 0, 0.
#ifndef UNX_WATER_OCEAN_REFINE_HLSLI
#define UNX_WATER_OCEAN_REFINE_HLSLI
#include "OceanHeight.hlsli"

#define OCEAN_REFINE_VISITS(levels) (2u * 1024u * (levels) + 32u)
#define OCEAN_REFINE_MISS 0u  // the ray left the outermost level or passed t max
#define OCEAN_REFINE_HIT 1u
#define OCEAN_REFINE_CAP 2u   // the proven visit bound was reached (never, unless the proof above is broken)

struct OceanRefineParams
{
    uint heightSrv, boundsSrv, levels;
    float2 camera;
    float s0, waterLevel, pixelAngle, tMax;
};
OceanRefineParams oceanRefineParams(uint params)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[params];
    const uint4 r0 = b.Load4(0), r1 = b.Load4(16), r2 = b.Load4(32);
    OceanRefineParams p;
    p.heightSrv = r0.x; p.boundsSrv = r0.y; p.levels = r0.z;
    p.camera = asfloat(r1.xy); p.s0 = asfloat(r1.z); p.waterLevel = asfloat(r1.w);
    p.pixelAngle = asfloat(r2.x); p.tMax = asfloat(r2.y);
    return p;
}
uint oceanLevelFor(OceanRefineParams p, float t)
{
    const float half = 0.5 * max(t, 1e-3) * p.pixelAngle;
    const int l = int(floor(log2(max(half / p.s0, 1.0))));
    return uint(clamp(l, 0, int(p.levels) - 1));
}
// First root in [0, len] of f(x) = c x^2 + b x + a (the caller's combined coefficients), or -1.
float oceanQuadraticFirst(float c, float b, float a, float len)
{
    if (abs(c) < 1e-12)
    {
        if (abs(b) < 1e-20) return -1;
        const float x = -a / b;
        return (x >= 0 && x <= len) ? x : -1;
    }
    const float disc = b * b - 4 * c * a;
    if (disc < 0) return -1;
    const float sq = sqrt(disc);
    const float q = -0.5 * (b + (b >= 0 ? sq : -sq));  // stable roots
    float x0 = q / c, x1 = abs(q) > 0 ? a / q : x0;
    if (x0 > x1) { const float t = x0; x0 = x1; x1 = t; }
    if (x0 >= 0 && x0 <= len) return x0;
    if (x1 >= 0 && x1 <= len) return x1;
    return -1;
}

uint oceanRefineCounted(uint params, float3 rayOrigin, float3 rayDir, float tShell, out float tHit, out uint steps)
{
    tHit = 0;
    steps = 0;
    const OceanRefineParams p = oceanRefineParams(params);
    Texture2DArray<float4> height = ResourceDescriptorHeap[p.heightSrv];
    Texture2DArray<float2> bounds = ResourceDescriptorHeap[p.boundsSrv];
    const int2 stepDir = int2(rayDir.x > 0 ? 1 : -1, rayDir.z > 0 ? 1 : -1);
    const int last = int(OH_CELLS) - 1;  // cells 0..510 of a level's window
    float t = max(tShell, 0.0);
    uint level = oceanLevelFor(p, t);
    int mip = int(OH_MIPS) - 1;
    bool enter = true;
    float2 rel = 0, dir = 0;
    int2 cell = 0;
    float ruleSwitch = 0;
    const uint visits = OCEAN_REFINE_VISITS(p.levels);
    for (uint step = 0; step < visits; ++step)
    {
        steps = step + 1;
        if (t > p.tMax) return OCEAN_REFINE_MISS;
        if (enter)
        {
            level = max(level, oceanLevelFor(p, t));
            const float s = p.s0 * float(1u << level);
            const int2 origin = int2(floor(p.camera / s)) - int2(256, 256);
            rel = (rayOrigin.xz - float2(origin) * s) / s;
            dir = rayDir.xz / s;
            const float2 u = rel + t * dir;
            if (any(u < 0) || any(u >= float(OH_CELLS)))
            {
                if (level + 1 >= p.levels) return OCEAN_REFINE_MISS;
                ++level;
                continue;
            }
            cell = clamp(int2(floor(u)), int2(0, 0), int2(last, last));
            ruleSwitch = level + 1 < p.levels ? exp2(float(level + 2)) * p.s0 / p.pixelAngle : 3.0e38;
            enter = false;
        }
        if (t >= ruleSwitch)
        {
            ++level;
            enter = true;
            continue;
        }
        const int n = int(1u << uint(mip));
        const int2 node = cell >> mip;
        const int2 lo = node * n, hi = min(lo + n, int2(OH_CELLS, OH_CELLS));
        float exitT = 3.0e38;
        int axis = -1;
        [unroll] for (int a = 0; a < 2; ++a)
        {
            if (dir[a] == 0) continue;
            const float ta = (float(dir[a] > 0 ? hi[a] : lo[a]) - rel[a]) / dir[a];
            if (ta < exitT) { exitT = ta; axis = a; }
        }
        exitT = max(exitT, t);
        const float yEntry = rayOrigin.y + t * rayDir.y, yExit = rayOrigin.y + exitT * rayDir.y;
        const float nodeMax = bounds.Load(int4(node, level, mip)).x;
        const bool skip = min(yEntry, yExit) > nodeMax;
        if (!skip && mip > 0) { --mip; continue; }
        if (!skip)
        {
            // Cell: bilinear heights at its corners, the local (a, b) linear in the ray parameter.
            const float h00 = height.Load(int4(cell, level, 0)).x, h10 = height.Load(int4(cell + int2(1, 0), level, 0)).x;
            const float h01 = height.Load(int4(cell + int2(0, 1), level, 0)).x, h11 = height.Load(int4(cell + int2(1, 1), level, 0)).x;
            const float2 f0 = rel + t * dir - float2(cell);
            // H(x) = h00 + e a + g b + k a b, a = f0.x + dir.x x, b = f0.y + dir.y x (x = ray parameter from t).
            const float e = h10 - h00, g = h01 - h00, k = h11 - h10 - h01 + h00;
            const float A = k * dir.x * dir.y;
            const float B = e * dir.x + g * dir.y + k * (f0.x * dir.y + f0.y * dir.x);
            const float C = h00 + e * f0.x + g * f0.y + k * f0.x * f0.y;
            // f(x) = (yEntry + rayDir.y x) - H(x): a surface already above the ray at the cell entry (only after a level
            // switch: the coarser level's surface differs within its interpolation error) is hit there.
            if (yEntry - C < 0) { tHit = t; return OCEAN_REFINE_HIT; }
            const float x = oceanQuadraticFirst(-A, rayDir.y - B, yEntry - C, exitT - t);
            if (x >= 0) { tHit = t + x; return OCEAN_REFINE_HIT; }
        }
        if (axis < 0) return OCEAN_REFINE_MISS;  // a vertical ray that neither hits nor leaves its cell column
        // Move past the node (a skip) or the cell (a missed leaf) across the exit face.
        t = exitT;
        const int other = 1 - axis;
        int2 next;
        next[axis] = stepDir[axis] > 0 ? hi[axis] : lo[axis] - 1;
        next[other] = clamp(int(floor(rel[other] + t * dir[other])), lo[other], hi[other] - 1);
        mip = skip ? min(mip + 1, int(OH_MIPS) - 1) : 1;
        if (next[axis] < 0 || next[axis] > last)
        {
            if (level + 1 >= p.levels) return OCEAN_REFINE_MISS;
            ++level;
            enter = true;
            continue;
        }
        cell = next;
    }
    return OCEAN_REFINE_CAP;
}
bool oceanRefine(uint params, float3 rayOrigin, float3 rayDir, float tShell, out float tHit)
{
    uint steps;
    return oceanRefineCounted(params, rayOrigin, rayDir, tShell, tHit, steps) == OCEAN_REFINE_HIT;
}
#endif
