// Local lights' air shadows (FroxelIntegrate's shadowed air in-scattering, VsmLocalMarkAir's page requests): the
// texel-boundary walk of a straight segment through a light's cube faces. Owner: S.
//
// The segment is d0 + D t, t in [0, len], relative to the light. On a face with basis (right, up, axis) its depth
// z(t) = dot(d0 + D t, axis) is linear, and its tangent coordinates are monotone in t (a line projects to a line; the
// sign of dx/dt is that of (D.right)(d0.axis) - (d0.right)(D.axis), constant). A texel boundary x = x_b is the plane
// through the light with normal right - x_b axis, so the parameter where the segment crosses it is exact:
// t = -dot(d0, n) / dot(D, n). The lookup rule is vsmLocalKeyAt's: the target mip m = vsmLocalMip(width, z) (changes
// where z crosses 64 width 2^k: also exact on the linear z), the coarser resident mips as fallback, receiver tolerance
// bias x 2 z / res of the mip used, lit where z <= nearM or where no page holds the point. So every texel the segment
// crosses is visited exactly once, in order, and within a texel the lit part is exact: lit where z (1 - 2 bias / res)
// <= the caster's depth, one end of the texel's interval since z is linear.
//
// Hierarchy (VsmPageMax key ranges; local keys are vsmEncode(-z), so range.y is the nearest caster, range.x the farthest
// or VSM_EMPTY when a texel has none): cells of the mip-m page (128 target texels), then of the resident page's 32- and
// 8-texel blocks, then its texels. A cell is lit when its z range lies in front of the nearest caster, umbra when it lies
// behind the farthest (every texel present), else it is descended. The ranges are conservative (the block contains the
// texels crossed), so the result is the texel walk's.
//
// Result: the lit set as Legendre moments over the Gauss-Legendre variable of the caller's integral (x in [-1, 1],
// theta = mid + half x, t = tc + h tan theta): m_k = integral over the lit set of P_k(x) dx, k < 8. The caller turns
// them into node weights (vsmLocalAirWeights): the 8-node Gauss rule of the unshadowed integrand is the integral of its
// degree-7 interpolant, and the same interpolant integrated over the lit set gives weights w'_i = sum_k (2k+1)/2 w_i
// P_k(x_i) m_k (fully lit: the Gauss weights; fully dark: 0). The visibility steps are exact; the smooth integrand has
// the unshadowed lights' accuracy.
//
// Bound (INTERFACES 3.6): one loop iteration per cell (one table, block or texel load). A slice at target texel
// footprint width / 2 .. width (mips 1..6; coarser than width at mip 6) crosses at most 4 len / (width / 2) texels per
// axis; near the light (mip 0, z < 64 width) at most 128 per axis and face. With the block levels that is below
// VSM_LOCAL_AIR_STEPS for the froxel grid's slices (S_STATUS 9b); a walk stopped at the cap counts the rest lit and
// raises VSM_ERR_LOCAL_AIR_WALK.
#ifndef UNX_VSM_LOCAL_AIR_WALK_HLSLI
#define UNX_VSM_LOCAL_AIR_WALK_HLSLI
#include "Passes/Shadow/VsmLocalSample.hlsli"

#define VSM_LOCAL_AIR_STEPS 2048u       // cells per entry (cap: infinite-loop guard above the bound)
#define VSM_LOCAL_AIR_PAGE_STEPS 256u   // page cells per entry (VsmLocalMarkAir)

struct VsmLocalAirCursor
{
    float3 d0, D;        // segment relative to the light
    float width;         // footprint of the mip rule (vsmLocalMip)
    uint face;
    float3 right, up, axis;
    float z0, dz;        // z(t) = z0 + dz t on this face
    float su, sv;        // direction of the texel coordinates u, v along t (+1, -1, 0)
    uint m;              // target mip at the current point
    int2 T;              // its texel (target mip)
};

void vsmLocalAirSetFace(inout VsmLocalAirCursor c, uint face)
{
    c.face = face;
    vsmCubeBasis(face, c.right, c.up, c.axis);
    c.z0 = dot(c.d0, c.axis);
    c.dz = dot(c.D, c.axis);
    c.su = sign(dot(c.D, c.right) * c.z0 - dot(c.d0, c.right) * c.dz);
    c.sv = -sign(dot(c.D, c.up) * c.z0 - dot(c.d0, c.up) * c.dz);  // v = (1 - y) res / 2 runs against y
}
int2 vsmLocalAirTexel(VsmLocalAirCursor c, float t, uint m)
{
    const float3 p = c.d0 + c.D * t;
    const float z = max(dot(p, c.axis), 1e-6);
    return clamp(int2(floor(vsmLocalTexel(float2(dot(p, c.right), dot(p, c.up)) / z, m))), 0, int(vsmLocalRes(m)) - 1);
}
void vsmLocalAirLocate(inout VsmLocalAirCursor c, float t)
{
    c.m = vsmLocalMip(c.width, max(c.z0 + c.dz * t, 1e-6));
    c.T = vsmLocalAirTexel(c, t, c.m);
}
VsmLocalAirCursor vsmLocalAirBegin(float3 d0, float3 D, float len, float width)
{
    VsmLocalAirCursor c = (VsmLocalAirCursor)0;
    c.d0 = d0;
    c.D = D;
    c.width = width;
    vsmLocalAirSetFace(c, vsmCubeFace(d0 + D * (1e-5 * len)));  // the face the segment enters (d0 may lie on an edge)
    vsmLocalAirLocate(c, 0);
    return c;
}

// Exit of the aligned cell of 'span' target texels holding c.T, from t (<= tEnd). event: 1 u boundary, 2 v boundary,
// 4 target mip change, 0 tEnd.
float vsmLocalAirExit(VsmLocalAirCursor c, uint span, float t, float tEnd, out uint event)
{
    const float res = vsmLocalRes(c.m);
    const int2 C0 = (c.T / (int)span) * (int)span;
    float tu = 3.0e38, tv = 3.0e38, tz = 3.0e38;
    if (c.su != 0)
    {
        const float3 n = c.right - (2 * float(c.su > 0 ? C0.x + (int)span : C0.x) / res - 1) * c.axis;
        const float den = dot(c.D, n);
        if (den != 0) tu = -dot(c.d0, n) / den;
    }
    if (c.sv != 0)
    {
        const float3 n = c.up - (1 - 2 * float(c.sv > 0 ? C0.y + (int)span : C0.y) / res) * c.axis;
        const float den = dot(c.D, n);
        if (den != 0) tv = -dot(c.d0, n) / den;
    }
    // Target mip m holds z in (64 width 2^(m-1), 64 width 2^m] (vsmLocalMip).
    if (c.dz > 0 && c.m + 1 < VSM_LOCAL_MIPS) tz = (64 * c.width * exp2(float(c.m)) - c.z0) / c.dz;
    if (c.dz < 0 && c.m > 0) tz = (64 * c.width * exp2(float(c.m) - 1) - c.z0) / c.dz;
    tu = tu < t ? 3.0e38 : tu;  // behind: the plane's other half (z < 0) or rounding at the entry
    tv = tv < t ? 3.0e38 : tv;
    tz = tz < t ? t : tz;       // rounding at a mip boundary: change now
    float tE = tEnd;
    event = 0;
    if (tz <= tE) { tE = tz; event = 4; }
    if (tu < tE) { tE = tu; event = 1; }
    if (tv < tE) { tE = tv; event = 2; }
    else if (tv == tE && event == 1) event = 3;  // a corner
    return tE;
}

// Moves the cursor past the exit at tE of the cell of 'span' target texels (event from vsmLocalAirExit). True when the
// face or the target mip changed (the caller's page and blocks no longer apply).
bool vsmLocalAirAdvance(inout VsmLocalAirCursor c, uint span, float tE, uint event)
{
    if (event & 4)
    {
        c.m = c.dz > 0 ? c.m + 1 : c.m - 1;
        c.T = vsmLocalAirTexel(c, tE, c.m);
        return true;
    }
    const int res = (int)vsmLocalRes(c.m);
    const int2 C0 = (c.T / (int)span) * (int)span;
    int2 T = c.T;
    if (event & 1) T.x = c.su > 0 ? C0.x + (int)span : C0.x - 1;
    if (event & 2) T.y = c.sv > 0 ? C0.y + (int)span : C0.y - 1;
    if (any(T < 0) || any(T >= res))
    {
        // Past the face's edge x = +-1 or y = +-1: the next face's axis is the crossed edge's direction.
        const float3 next = (T.x >= res) ? c.right : (T.x < 0) ? -c.right : (T.y < 0) ? c.up : -c.up;
        vsmLocalAirSetFace(c, vsmCubeFace(next));
        vsmLocalAirLocate(c, tE);
        return true;
    }
    if ((event & 1) == 0) T.x = vsmLocalAirTexel(c, tE, c.m).x;
    if ((event & 2) == 0) T.y = vsmLocalAirTexel(c, tE, c.m).y;
    c.T = T;
    return false;
}

// Legendre moments of the lit set: m_k += integral over [xa, xb] of P_k (Q_k(x) = integral from -1: Q_0 = x + 1,
// Q_k = (P_{k+1} - P_{k-1}) / (2k + 1)).
void vsmLocalAirMomentsAdd(inout float m[8], float xa, float xb)
{
    float pa0 = 1, pa1 = xa, pb0 = 1, pb1 = xb;  // P_{k-1}, P_k
    m[0] += xb - xa;
    [unroll] for (uint k = 1; k < 8; ++k)
    {
        const float pa2 = ((2 * k + 1) * xa * pa1 - k * pa0) / (k + 1), pb2 = ((2 * k + 1) * xb * pb1 - k * pb0) / (k + 1);
        m[k] += ((pb2 - pb0) - (pa2 - pa0)) / (2 * k + 1);
        pa0 = pa1; pa1 = pa2;
        pb0 = pb1; pb1 = pb2;
    }
}

struct VsmLocalAirResult
{
    float m[8];        // Legendre moments of the lit set (x = (theta - mid) / half)
    uint litRuns;      // maximal lit runs
    bool fullyLit;     // one run over the whole segment (the moments are the Gauss rule's)
    uint steps;        // cells visited (loads)
    bool capped;       // stopped at VSM_LOCAL_AIR_STEPS (the rest counted lit)
};

// The lit set of the segment against light slot 'slot' (VsmLocalLight l). tc, h, mid, half: the caller's angle map.
VsmLocalAirResult vsmLocalAirLit(VsmLocalResources r, VsmLocalLight l, uint slot, float3 d0, float3 D, float len, float width, float biasTexels,
                                 float tc, float h, float mid, float half)
{
    VsmLocalAirResult res = (VsmLocalAirResult)0;
    VsmLocalAirCursor c = vsmLocalAirBegin(d0, D, len, width);
    const float invHalf = half > 0 ? 1 / half : 0;
    float t = 0, runA = 0, runB = -1;  // open lit run [runA, runB] (runB < 0: none)
    float end1 = len, end2 = len, end3 = len;  // ends of the cells being descended (levels 1..3 walk inside them)
    uint lvl = 0, mu = 0, phys = 0;  // level: 0 the target page, 1 the resident page's 32-texel blocks, 2 its 8-texel blocks, 3 its texels
    bool resident = false;
    uint stuck = 0;
    [loop] while (t < len)
    {
        if (res.steps >= VSM_LOCAL_AIR_STEPS)
        {
            res.capped = true;
            break;
        }
        ++res.steps;
        if (lvl == 0)
        {
            // The target page's residency (vsmLocalKeyAt's fallback): mu is the same over the whole mip-m page.
            resident = false;
            mu = c.m;
            [loop] for (int j = (int)c.m; j >= 0; --j)
            {
                const uint2 e = r.table.Load2(vsmLocalSlot(slot, c.face, (uint)j, (uint2(c.T) >> (c.m - (uint)j)) >> VSM_PAGE_SHIFT) * 8);
                if ((e.x & VSM_FLAG_RESIDENT) != 0 && e.y == l.generation)
                {
                    resident = true;
                    mu = (uint)j;
                    phys = e.x & VSM_PHYS_MASK;
                    break;
                }
            }
        }
        const uint scale = c.m - mu;  // target texels per resident texel (log2)
        const uint levelTexels = lvl == 0 ? VSM_PAGE : lvl == 1 ? 32u : lvl == 2 ? 8u : 1u;  // resident texels of the level's cells
        const uint span = min(levelTexels << scale, VSM_PAGE);
        uint event;
        const float tE = vsmLocalAirExit(c, span, t, lvl == 0 ? len : lvl == 1 ? end1 : lvl == 2 ? end2 : end3, event);
        if (tE <= t)
        {
            // An empty cell (its exit rounds onto the entry): cross it without a lookup. Repeated (a segment grazing a
            // face edge or corner can hand the point back and forth): move on by 1e-6 of the segment and locate again.
            if (++stuck > 2)
            {
                t = min(t + 1e-6 * len, len);
                vsmLocalAirSetFace(c, vsmCubeFace(c.d0 + c.D * t));
                vsmLocalAirLocate(c, t);
                stuck = 0;
            }
            else if (!vsmLocalAirAdvance(c, span, tE, event))
            {
                while (lvl > 0 && t >= (lvl == 1 ? end1 : lvl == 2 ? end2 : end3)) --lvl;
                continue;
            }
            lvl = 0;
            continue;
        }
        stuck = 0;
        const float za = c.z0 + c.dz * t, zb = c.z0 + c.dz * tE;
        const float k = 1 - biasTexels * 2 / vsmLocalRes(mu);  // receiver tolerance of the mip used: lit where z k <= caster z
        float la = t, lb = tE;  // lit part of [t, tE]
        bool descend = false;
        if (!resident) {}
        else if (lvl < 3)
        {
            const uint2 local = (uint2(c.T) >> scale) & (VSM_PAGE - 1);
            const uint perRow = VSM_PAGE / levelTexels;
            const uint2 cell = local / levelTexels;
            const uint offset = lvl == 0 ? VSM_BLOCK_OFFSET_128 : lvl == 1 ? VSM_BLOCK_OFFSET_32 : VSM_BLOCK_OFFSET_8;
            const uint2 range = r.blocks.Load2((phys * VSM_BLOCK_ENTRIES + offset + cell.y * perRow + cell.x) * VSM_BLOCK_BYTES);
            const float zmin = min(za, zb), zmax = max(za, zb);
            const bool lit = range.y == VSM_EMPTY || zmax <= l.nearM || zmax * k <= -vsmDecode(range.y);
            const bool umbra = range.x != VSM_EMPTY && zmin > l.nearM && zmin * k > -vsmDecode(range.x);
            if (umbra) lb = la - 1;
            else if (!lit) descend = true;
        }
        else
        {
            const uint key = vsmLocalKeyOfDepth(r.pool.Load(vsmAtlasTexel(phys, (uint2(c.T) >> scale) & (VSM_PAGE - 1))), l.nearM, l.farM);
            if (key != VSM_EMPTY)
            {
                const float zLit = max(l.nearM, -vsmDecode(key) / k);  // lit where z <= zLit
                if (c.dz > 0) lb = min(lb, (zLit - c.z0) / c.dz);
                else if (c.dz < 0) la = max(la, (zLit - c.z0) / c.dz);
                else if (za > zLit) lb = la - 1;
            }
        }
        if (descend)
        {
            if (lvl == 0) end1 = tE;
            else if (lvl == 1) end2 = tE;
            else end3 = tE;
            ++lvl;
            continue;
        }
        if (lb > la)
        {
            if (runB >= 0 && la <= runB) runB = max(runB, lb);
            else
            {
                if (runB >= 0)
                {
                    vsmLocalAirMomentsAdd(res.m, (atan((runA - tc) / h) - mid) * invHalf, (atan((runB - tc) / h) - mid) * invHalf);
                    ++res.litRuns;
                }
                runA = la;
                runB = lb;
            }
        }
        t = tE;
        if (t >= len) break;
        if (vsmLocalAirAdvance(c, span, tE, event)) lvl = 0;
        else
            while (lvl > 0 && t >= (lvl == 1 ? end1 : lvl == 2 ? end2 : end3)) --lvl;
    }
    if (res.capped)  // the rest counted lit (the error bit reports it)
    {
        if (runB >= 0 && t <= runB) runB = len;
        else
        {
            if (runB >= 0)
            {
                vsmLocalAirMomentsAdd(res.m, (atan((runA - tc) / h) - mid) * invHalf, (atan((runB - tc) / h) - mid) * invHalf);
                ++res.litRuns;
            }
            runA = t;
            runB = len;
        }
    }
    if (runB >= 0)
    {
        res.fullyLit = res.litRuns == 0 && runA <= 0 && runB >= len;
        vsmLocalAirMomentsAdd(res.m, (atan((runA - tc) / h) - mid) * invHalf, (atan((runB - tc) / h) - mid) * invHalf);
        ++res.litRuns;
    }
    return res;
}

// Node weights of the 8-point Gauss-Legendre rule (nodes x_i, weights w_i) that integrate the rule's interpolant over the
// lit set of moments m: w'_i = w_i sum_k (2k+1)/2 P_k(x_i) m_k.
float vsmLocalAirWeight(float x, float w, float m[8])
{
    float p0 = 1, p1 = x, s = 0.5 * m[0] + 1.5 * x * m[1];
    [unroll] for (uint k = 2; k < 8; ++k)
    {
        const float p2 = ((2 * k - 1) * x * p1 - (k - 1) * p0) / k;
        s += (k + 0.5) * p2 * m[k];
        p0 = p1;
        p1 = p2;
    }
    return w * s;
}

#endif
