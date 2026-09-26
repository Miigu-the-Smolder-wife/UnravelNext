// unx-kernel: cs_6_6 main
// Water stage 2 caustics (FEATURES_GAME 1.3 (c) on the sun-space map; WaterLight.hlsli reads them): every water texel of
// the sun map sends its sunlight along the exact Snell refraction at its surface point S (normal and IOR from the map)
// to the slice depths z_k = 0.25, 0.5, 1, 2, 4 m below its entry point along the entry's normal (a receiver's depth is
// measured the same way: below its own surface point along that point's normal, WaterLight.hlsli; planes across the sun
// would not do - under an oblique sun a flat pool's own surface spans metres of depth along the sun). A slice texel then
// holds the light that arrives there per unit of the light that entered one texel of the surface: flat water shifts the
// whole texel grid uniformly, so every texel gets exactly 1 (the caustic factor of no focusing); a curved surface
// concentrates or spreads it, and the total is conserved.
// Beams, not points: texel (i, j) carries the light of the cell between the texel centres (i, j), (i+1, j), (i+1, j+1),
// (i, j+1) - one texel of area - and lays it on the cell spanned by those four centres' landing points, split into two
// triangles that share it by their areas (|area|: a fold, where the landing map's Jacobian changes sign, keeps each
// triangle convex and the share positive). Sharing by area gives both triangles one density, unit / (|a1| + |a2|), so the
// cell is deposited in one pass over its bounding box: on every caustic texel, density x (|S1| + |S2|), S1 and S2 the two
// triangles' exact overlaps with the texel as signed boundary integrals -sum_edges int clamp(y(x)) dx over the texel's
// x-range (clamp to the texel's y-range; between its crossings of the two bounds clamp(y) is affine, so the midpoint rule
// per piece is exact) - five edges per texel (four sides and the shared diagonal, whose two uses cancel when the cell does
// not fold), all in registers; fixed point (2^-16). (It replaced a Sutherland-Hodgman clip on local arrays, which spilled
// to scratch memory: 78-106 ms on the D0 fluid's 2048^2 map [measured].) So light that spreads leaves no gaps between the texels'
// landing points (a point splat of width one texel did: holes at the map's spacing where the surface diverges, a grid
// seen through a rippled bath - engine 2's finding). Where a corner is not water or refracts totally (the pool's edge),
// the texel's light lands as its own square, one map texel wide, centred on its landing point. A triangle spanning more than
// WATER_CAUSTIC_SPAN caustic texels (a beam spread wider than that at the slice) is splatted as a point too and counted
// (P[1].y): the dispatch's worst case stays bounded.
// Binned by the landing point's own sun-map coordinates on the caustic grid (min(map texels, 1024) per side over the same
// extent; a map texel carries (grid / map)^2 of a grid texel), so a receiver X reads the grid texel of X's projection.
// P[0] depth SRV, normal SRV, medium SRV, constants SRV; P[1] caustics UAV (RWTexture2DArray<uint>, WATER_CAUSTIC_SLICES),
// overflow counter UAV (raw, one word; UNX_NONE: not counted)
#include "WaterLight.hlsli"

#define WATER_CAUSTIC_SPAN 16u

struct CausticMap
{
    uint n;
    float4 r, u, s, k;
};

// Texel q's surface point S and its refracted ray per unit depth along the entry normal (t / cos theta_t): the landing
// point at slice depth zk is S + zk x step. False where q is not water or the light reflects totally.
bool causticRay(CausticMap m, Texture2D<float> depth, Texture2D<float2> normals, Texture2D<float4> media, int2 q, out float3 S, out float3 step)
{
    S = 0;
    step = 0;
    if (any(q < 0) || any(q >= int(m.n))) return false;
    const float d = depth[q];
    if (d <= 0) return false;
    float3 nrm = waterOctDecode(normals[q]);
    if (dot(nrm, m.s.xyz) < 0) nrm = -nrm;
    float3 t;
    if (!waterRefract(m.s.xyz, nrm, 1.0 / max(media[q].w, 1.0001), t)) return false;
    const float cosT = dot(-t, nrm);  // the refracted ray's descent along the normal per unit length
    if (cosT <= 0) return false;
    const float extent = 1.0 / m.k.x;
    const float2 uv = (float2(q) + 0.5) / float(m.n);
    S = m.r.xyz * (m.r.w + uv.x * extent) + m.u.xyz * (m.u.w + (1 - uv.y) * extent) + m.s.xyz * (m.s.w + d * m.k.z);
    step = t / cosT;
    return true;
}
// A world point's position on the caustic grid (texel c covers [c, c + 1)).
float2 causticGrid(CausticMap m, float3 Q, float nc) { return float2((dot(Q, m.r.xyz) - m.r.w) * m.k.x, 1 - (dot(Q, m.u.xyz) - m.u.w) * m.k.y) * nc; }

void causticAdd(RWTexture2DArray<uint> caustics, int2 at, uint slice, float amount, uint nc)
{
    if (any(at < 0) || any(at >= int(nc)) || !(amount > 0)) return;
    InterlockedAdd(caustics[uint3(at, slice)], uint(round(amount * 65536.0)));
}

// Bilinear point splat (texel centres at c + 0.5).
void causticPoint(RWTexture2DArray<uint> caustics, float2 p, uint slice, float amount, uint nc)
{
    const float2 c = p - 0.5;
    const int2 i0 = int2(floor(c));
    const float2 f = c - float2(i0);
    causticAdd(caustics, i0, slice, amount * (1 - f.x) * (1 - f.y), nc);
    causticAdd(caustics, i0 + int2(1, 0), slice, amount * f.x * (1 - f.y), nc);
    causticAdd(caustics, i0 + int2(0, 1), slice, amount * (1 - f.x) * f.y, nc);
    causticAdd(caustics, i0 + int2(1, 1), slice, amount * f.x * f.y, nc);
}

// -int clamp(y(x), 0, 1) dx along edge p -> q over x in [0, 1] (coordinates relative to the texel's corner): the edge's
// part of the signed boundary integral of a polygon's overlap with the texel [0, 1]^2.
float causticEdge(float2 p, float2 q)
{
    const float u = max(min(p.x, q.x), 0.0), v = min(max(p.x, q.x), 1.0);
    if (!(v > u)) return 0;
    const float slope = (q.y - p.y) / (q.x - p.x);  // q.x != p.x: the x-range is not empty
    const float yu = p.y + slope * (u - p.x), dy = slope * (v - u);
    float integral;
    if (dy == 0) integral = saturate(yu);
    else
    {
        // Pieces between the crossings of y = 0 and y = 1 (t along [u, v]); clamp(y) is affine on each: midpoint rule.
        const float t0 = saturate(-yu / dy), t1 = saturate((1 - yu) / dy), ta = min(t0, t1), tb = max(t0, t1);
        integral = ta * saturate(yu + dy * 0.5 * ta) + (tb - ta) * saturate(yu + dy * 0.5 * (ta + tb)) + (1 - tb) * saturate(yu + dy * 0.5 * (1 + tb));
    }
    integral *= v - u;
    return q.x > p.x ? -integral : integral;
}

// Deposits light of density `density` (per caustic texel of area) over the cell (c0, c1, c2, c3) = triangles (c0, c1, c2)
// and (c0, c2, c3), each counted by its unsigned overlap with every texel of the bounding box; false (nothing deposited)
// when it spans too many texels.
bool causticCell(RWTexture2DArray<uint> caustics, float2 c0, float2 c1, float2 c2, float2 c3, uint slice, float density, uint nc)
{
    const float2 lo = floor(min(min(c0, c1), min(c2, c3))), hi = floor(max(max(c0, c1), max(c2, c3)));
    if (hi.x - lo.x >= WATER_CAUSTIC_SPAN || hi.y - lo.y >= WATER_CAUSTIC_SPAN) return false;
    for (float y = lo.y; y <= hi.y; y += 1)
        for (float x = lo.x; x <= hi.x; x += 1)
        {
            // Relative to the texel's corner: the shoelace-type sums on absolute grid positions (hundreds) cancel a small
            // polygon's area away in fp32 (measured: flat water off by 5 %).
            const float2 k = float2(x, y), p0 = c0 - k, p1 = c1 - k, p2 = c2 - k, p3 = c3 - k;
            const float diagonal = causticEdge(p2, p0);
            const float s1 = causticEdge(p0, p1) + causticEdge(p1, p2) + diagonal, s2 = causticEdge(p2, p3) + causticEdge(p3, p0) - diagonal;
            const float part = abs(s1) + abs(s2);
            if (part > 0) causticAdd(caustics, int2(k), slice, density * part, nc);
        }
    return true;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ByteAddressBuffer cb = ResourceDescriptorHeap[P[0].w];
    const uint4 head = cb.Load4(0);
    CausticMap m;
    m.n = head.y;
    if (head.x == 0 || any(id.xy >= m.n)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    if (depth[id.xy] <= 0) return;
    Texture2D<float2> normals = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> media = ResourceDescriptorHeap[P[0].z];
    m.r = asfloat(cb.Load4(16));
    m.u = asfloat(cb.Load4(32));
    m.s = asfloat(cb.Load4(48));
    m.k = asfloat(cb.Load4(64));
    RWTexture2DArray<uint> caustics = ResourceDescriptorHeap[P[1].x];
    const uint nc = min(m.n, WATER_CAUSTIC_MAX);                        // the caustic grid over the same extent
    const float unit = float(nc) * float(nc) / (float(m.n) * float(m.n));  // one map texel's share of a caustic texel
    const int2 q = int2(id.xy);
    float3 S0, S1, S2, S3, d0, d1, d2, d3;
    if (!causticRay(m, depth, normals, media, q, S0, d0)) return;  // (total reflection: no light enters)
    const bool beam = causticRay(m, depth, normals, media, q + int2(1, 0), S1, d1) && causticRay(m, depth, normals, media, q + int2(1, 1), S2, d2) &&
                      causticRay(m, depth, normals, media, q + int2(0, 1), S3, d3);
    uint overflow = 0;
    [loop] for (uint slice = 0; slice < WATER_CAUSTIC_SLICES; ++slice)
    {
        const float zk = waterCausticDepth(slice);
        const float2 c0 = causticGrid(m, S0 + d0 * zk, float(nc));
        if (!beam)
        {
            // The water's edge (a corner is not water or reflects totally): the texel's own square of light, one map
            // texel wide, centred on its landing point - the shape its interior neighbours' cells take on flat water, so
            // the two tile (a point splat one caustic texel wide beside them did not: edges off by up to 25 %).
            const float half = 0.5 * float(nc) / float(m.n);
            const float2 p0 = c0 + float2(-half, -half), p1 = c0 + float2(half, -half), p2 = c0 + float2(half, half), p3 = c0 + float2(-half, half);
            if (!causticCell(caustics, p0, p1, p2, p3, slice, unit / (4 * half * half), nc)) ++overflow;  // (a square of one map texel never spans the bound)
            continue;
        }
        const float2 c1 = causticGrid(m, S1 + d1 * zk, float(nc)), c2 = causticGrid(m, S2 + d2 * zk, float(nc)), c3 = causticGrid(m, S3 + d3 * zk, float(nc));
        const float a1 = 0.5 * abs((c1.x - c0.x) * (c2.y - c0.y) - (c2.x - c0.x) * (c1.y - c0.y));
        const float a2 = 0.5 * abs((c2.x - c0.x) * (c3.y - c0.y) - (c3.x - c0.x) * (c2.y - c0.y));
        const float total = a1 + a2;
        // The cell between the four texel centres lands half a texel on from texel q's own light: shifted back by half its
        // edges so its centre lands where q's centre does (flat water: the texel grid moved whole, every caustic texel 1).
        const float2 shift = -0.5 * ((c1 - c0) + (c3 - c0));
        if (!(total >= 1e-12)) { causticPoint(caustics, (c0 + c2) * 0.5 + shift, slice, unit, nc); continue; }  // a focus: all of it at one point
        if (!causticCell(caustics, c0 + shift, c1 + shift, c2 + shift, c3 + shift, slice, unit / total, nc))
        {
            causticPoint(caustics, (c0 + c1 + c2) / 3 + shift, slice, unit * a1 / total, nc);
            causticPoint(caustics, (c0 + c2 + c3) / 3 + shift, slice, unit * a2 / total, nc);
            overflow += 2;
        }
    }
    if (overflow != 0 && P[1].y != UNX_NONE)
    {
        RWByteAddressBuffer counter = ResourceDescriptorHeap[P[1].y];
        counter.InterlockedAdd(0, overflow);
    }
}
