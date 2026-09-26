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
// per piece is exact) - each edge weighted by its triangle's orientation, so the shared diagonal cancels and a texel costs
// four edges unless the cell folds; an edge's slope and crossings are set up once per cell, all in registers; fixed point
// (2^-16). (It replaced a Sutherland-Hodgman clip on local arrays, which spilled
// to scratch memory: 78-106 ms on the D0 fluid's 2048^2 map [measured].) So light that spreads leaves no gaps between the texels'
// landing points (a point splat of width one texel did: holes at the map's spacing where the surface diverges, a grid
// seen through a rippled bath - engine 2's finding). Where a corner is not water or refracts totally (the pool's edge),
// the texel's light lands as its own square, one map texel wide, centred on its landing point.
// Levels (the dispatch's worst case bounded by structure): a cell is rasterized at the finest level of the caustic grid's
// pyramid (level L: texels 2^L wide) where its bounding box spans at most WATER_CAUSTIC_BOX texels per side, so a beam
// costs at most WATER_CAUSTIC_BOX^2 texels whatever the surface; level 0 writes the slice, coarser levels a raw buffer
// (P[1].z) that WaterCausticsPull.hlsl spreads evenly over their fine texels. Exact at level 0 (flat and smoothly curved
// water: a map texel's cell spans about a caustic texel); a beam spread wider than WATER_CAUSTIC_BOX - 1 texels keeps its
// light and energy exactly but places it to within one texel of its level (at most a third of its own spread) - the
// chaotic focus under a splashing surface at depth, where the cell's piecewise-linear landing is itself that uncertain.
// (A 16-texel span bound with a point splat past it cost 78-106 ms, then 3.5-10.7 ms, on the D0 fluid's bumpy surface
// [measured]: the work followed the beams' spread.)
// Binned by the landing point's own sun-map coordinates on the caustic grid (min(map texels, 1024) per side over the same
// extent; a map texel carries (grid / map)^2 of a grid texel), so a receiver X reads the grid texel of X's projection.
// P[0] depth SRV, normal SRV, medium SRV, constants SRV; P[1] caustics UAV (RWTexture2DArray<uint>, WATER_CAUSTIC_SLICES),
// overflow counter UAV (raw, one word, stays 0: no beam is bounded out any more), level buffer UAV (raw, waterCausticLevelWords per slice)
#include "WaterLight.hlsli"

#define WATER_CAUSTIC_BOX 6u

// Study hooks (Gates/WaterCausticsStudy.hlsl defines them to count and to split the time by term; nothing here).
#ifndef WATER_CAUSTIC_STUDY
#define CAUSTIC_TEX_ADD(tex, at, slice, v) InterlockedAdd(tex[uint3(at, slice)], v)
#define CAUSTIC_LEVEL_ADD(buf, address, v) buf.InterlockedAdd(address, v)
#define CAUSTIC_STUDY_CELL(L, slice)
#define CAUSTIC_STUDY_TEXEL(slice)
#define CAUSTIC_STUDY_MERGED(slice)
#define CAUSTIC_STUDY_BEGIN()
#define CAUSTIC_STUDY_RAYS(sum)
#define CAUSTIC_STUDY_SKIP(slice) false
#define CAUSTIC_STUDY_NO_EDGES() false
#define CAUSTIC_STUDY_NO_RASTER() false
#define CAUSTIC_STUDY_END()
#endif

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
    CAUSTIC_TEX_ADD(caustics, at, slice, uint(round(amount * 65536.0)));
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

// Word offset of level L (>= 1) inside a slice's part of the level buffer, and the words of one slice.
uint waterCausticLevelBase(uint nc, uint L)
{
    uint base = 0;
    for (uint l = 1; l < L; ++l) base += (nc >> l) * (nc >> l);
    return base;
}
uint waterCausticLevelWords(uint nc) { return waterCausticLevelBase(nc, firstbithigh(nc) + 1); }

// The finest level where the box [mn, mx] (caustic texels) spans at most WATER_CAUSTIC_BOX texels per side.
uint causticLevel(float2 mn, float2 mx, uint nc)
{
    uint L = 0;
    float s = 1;
    [loop] while ((nc >> L) > 1 && any(floor(mx * s) - floor(mn * s) >= float(WATER_CAUSTIC_BOX))) { ++L; s *= 0.5; }
    return L;
}

// An edge p -> q of a cell's triangle, set up once per cell (no division per texel): its x-range, slope and inverse
// slope, and its weight = the orientation of the triangle it bounds x the sign of -int clamp(y) dx along its direction
// (a vertical edge has an empty x-range: weight 0).
struct CausticEdge
{
    float2 p;
    float lo, hi, m, im, w;
};
CausticEdge causticEdgeSetup(float2 p, float2 q, float orientation)
{
    CausticEdge e;
    e.p = p;
    e.lo = min(p.x, q.x);
    e.hi = max(p.x, q.x);
    const float dx = q.x - p.x, dy = q.y - p.y;
    e.m = dx != 0 ? dy / dx : 0;
    e.im = dy != 0 ? dx / dy : 0;  // a horizontal edge: any split point is exact (clamp(y) is constant along it)
    e.w = dx > 0 ? -orientation : dx < 0 ? orientation : 0;
    return e;
}
// The edge's part of the signed boundary integral of its triangle's overlap with texel [X, X + 1] x [Y, Y + 1]:
// -int clamp(y(x) - Y, 0, 1) dx over the texel's x-range, weighted. Between the crossings of y = Y and y = Y + 1 clamp(y)
// is affine, so the midpoint rule per piece is exact. Positions relative to p, so a steep edge keeps its precision.
float causticEdgeAt(CausticEdge e, float X, float Y)
{
    const float U = max(e.lo, X), V = max(min(e.hi, X + 1), U);
    const float xa = e.p.x + (Y - e.p.y) * e.im, xb = xa + e.im;
    const float x1 = clamp(min(xa, xb), U, V), x2 = clamp(max(xa, xb), U, V), py = e.p.y - Y, px = e.p.x;
    return e.w * ((x1 - U) * saturate(py + (0.5 * (U + x1) - px) * e.m) + (x2 - x1) * saturate(py + (0.5 * (x1 + x2) - px) * e.m) +
                  (V - x2) * saturate(py + (0.5 * (x2 + V) - px) * e.m));
}

// Deposits light of density `density` (per caustic texel of area) over the cell (c0, c1, c2, c3) = triangles (c0, c1, c2)
// and (c0, c2, c3), each counted by its unsigned overlap with every texel of its bounding box at the cell's level: its
// edges weighted by its orientation, so the shared diagonal cancels unless the cell folds (the landing map's Jacobian
// changes sign inside it) - four edges per texel, five in a fold.
void causticCell(RWTexture2DArray<uint> caustics, RWByteAddressBuffer levels, float2 c0, float2 c1, float2 c2, float2 c3, uint slice, float density, uint nc)
{
    const float2 mn = min(min(c0, c1), min(c2, c3)), mx = max(max(c0, c1), max(c2, c3));
    const uint L = causticLevel(mn, mx, nc);
    CAUSTIC_STUDY_CELL(L, slice);
    if (CAUSTIC_STUDY_NO_RASTER()) return;
    const float s = 1.0 / float(1u << L);
    const float2 lo = floor(mn * s), span = floor(mx * s) - lo;
    const float side = float(nc >> L);
    const float scale = float(1u << (2 * L));  // fine texels per texel of the level: the level holds light, not density
    const uint base = L == 0 ? 0 : slice * waterCausticLevelWords(nc) + waterCausticLevelBase(nc, L);
    // Relative to the box's corner at the level (at most WATER_CAUSTIC_BOX texels): the shoelace-type sums on absolute grid
    // positions (hundreds) cancel a small polygon's area away in fp32 (measured: flat water off by 5 %).
    const float2 p0 = c0 * s - lo, p1 = c1 * s - lo, p2 = c2 * s - lo, p3 = c3 * s - lo;
    const float2 e1 = p1 - p0, e2 = p2 - p0, e3 = p3 - p0;
    const float o1 = e1.x * e2.y - e2.x * e1.y >= 0 ? 1.0 : -1.0, o2 = e2.x * e3.y - e3.x * e2.y >= 0 ? 1.0 : -1.0;
    const CausticEdge E0 = causticEdgeSetup(p0, p1, o1), E1 = causticEdgeSetup(p1, p2, o1), E2 = causticEdgeSetup(p2, p3, o2), E3 = causticEdgeSetup(p3, p0, o2),
                      E4 = causticEdgeSetup(p2, p0, o1 - o2);
    const bool fold = o1 != o2;
    for (float y = 0; y <= span.y; y += 1)
        for (float x = 0; x <= span.x; x += 1)
        {
            CAUSTIC_STUDY_TEXEL(slice);
            const float2 at = lo + float2(x, y);
            if (any(at < 0) || any(at >= side)) continue;
            float part;
            if (CAUSTIC_STUDY_NO_EDGES()) part = 0.25;
            else
            {
                part = causticEdgeAt(E0, x, y) + causticEdgeAt(E1, x, y) + causticEdgeAt(E2, x, y) + causticEdgeAt(E3, x, y);
                if (fold) part += causticEdgeAt(E4, x, y);
            }
            if (!(part > 0)) continue;
            if (L == 0) causticAdd(caustics, int2(at), slice, density * part, nc);
            else CAUSTIC_LEVEL_ADD(levels, 4 * (base + uint(at.y) * uint(side) + uint(at.x)), uint(round(density * part * scale * 65536.0)));
        }
}

// A step's move on the caustic grid per metre of slice depth (causticGrid's linear part).
float2 causticGridStep(CausticMap m, float3 v, float nc) { return float2(dot(v, m.r.xyz) * m.k.x, -dot(v, m.u.xyz) * m.k.y) * nc; }

// One thread per 2 x 2 block of map texels (their cells span the texel centres base .. base + 2, so the block needs the
// 3 x 3 rays around it). The group's 16 x 16 texels' rays and the ones past them (17 x 17) are computed once into group
// memory in the landing map's affine form on the caustic grid - G(S) and G'(step): the landing point at slice depth z is
// G(S) + z G'(step) (causticGrid is affine) - so each ray is traced once, not 2.25 times. Per slice, the four cells are
// one quad with four times the light where the landing map is affine over the block to WATER_CAUSTIC_AFFINE caustic
// texels (flat water, and smooth water mostly), or where that quad goes to a coarser level L anyway and its deviation from
// affine is within a quarter of a level-L texel (the level places light to within one of its texels: the chaotic spread
// under a bumpy surface at depth). The surface points' grid positions are exactly affine in the texel index (the map's
// grid, the sun axis dropping out of the projection), so the deviation from affine is z times the steps' second
// differences. Otherwise each texel deposits its own cell: its cell to the neighbours' landing points when all four are
// water (beam), else its own square (the water's edge). Every cell goes through the one raster call site (causticCell),
// so the kernel holds one copy of the raster loop.
#define WATER_CAUSTIC_AFFINE 2.5e-4
#define WATER_CAUSTIC_TILE 17u
#define WATER_CAUSTIC_NONE -1e30
groupshared float4 gCausticRay[WATER_CAUSTIC_TILE * WATER_CAUSTIC_TILE];

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer cb = ResourceDescriptorHeap[P[0].w];
    const uint4 head = cb.Load4(0);
    if (head.x == 0) return;  // (the whole dispatch)
    CausticMap m;
    m.n = head.y;
    m.r = asfloat(cb.Load4(16));
    m.u = asfloat(cb.Load4(32));
    m.s = asfloat(cb.Load4(48));
    m.k = asfloat(cb.Load4(64));
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    Texture2D<float2> normals = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> media = ResourceDescriptorHeap[P[0].z];
    RWTexture2DArray<uint> caustics = ResourceDescriptorHeap[P[1].x];
    RWByteAddressBuffer levels = ResourceDescriptorHeap[P[1].z];
    const uint nc = min(m.n, WATER_CAUSTIC_MAX);  // the caustic grid over the same extent
    const float fnc = float(nc);
    const float unit = fnc * fnc / (float(m.n) * float(m.n));  // one map texel's share of a caustic texel
    const int2 origin = int2(group.xy) * 16;
    for (uint r = lane; r < WATER_CAUSTIC_TILE * WATER_CAUSTIC_TILE; r += 64)
    {
        float3 S, D;
        const bool ok = causticRay(m, depth, normals, media, origin + int2(r % WATER_CAUSTIC_TILE, r / WATER_CAUSTIC_TILE), S, D);
        gCausticRay[r] = ok ? float4(causticGrid(m, S, fnc), causticGridStep(m, D, fnc)) : float4(WATER_CAUSTIC_NONE, 0, 0, 0);
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(int2(id.xy) * 2 >= int(m.n))) return;
    const uint at = local.y * 2 * WATER_CAUSTIC_TILE + local.x * 2;  // the block's first ray in the tile
    // (not water, or total reflection: no light enters)
    if (!(gCausticRay[at].x > WATER_CAUSTIC_NONE) && !(gCausticRay[at + 1].x > WATER_CAUSTIC_NONE) && !(gCausticRay[at + WATER_CAUSTIC_TILE].x > WATER_CAUSTIC_NONE) &&
        !(gCausticRay[at + WATER_CAUSTIC_TILE + 1].x > WATER_CAUSTIC_NONE))
        return;
    CAUSTIC_STUDY_BEGIN();
    // The block's affine deviation per metre of slice depth (caustic texels): zero on flat water.
    bool all = true;
    [unroll] for (uint j = 0; j < 3; ++j)
        [unroll] for (uint i = 0; i < 3; ++i) all = all && gCausticRay[at + j * WATER_CAUSTIC_TILE + i].x > WATER_CAUSTIC_NONE;
    float deviation = 3.0e38;
    if (all)
    {
        const float2 g00 = gCausticRay[at].zw, A = 0.5 * (gCausticRay[at + 2].zw - g00), B = 0.5 * (gCausticRay[at + 2 * WATER_CAUSTIC_TILE].zw - g00);
        deviation = 0;
        [unroll] for (uint j2 = 0; j2 < 3; ++j2)
            [unroll] for (uint i2 = 0; i2 < 3; ++i2)
            {
                const float2 e = gCausticRay[at + j2 * WATER_CAUSTIC_TILE + i2].zw - (g00 + float(i2) * A + float(j2) * B);
                deviation = max(deviation, max(abs(e.x), abs(e.y)));
            }
    }
    CAUSTIC_STUDY_RAYS(gCausticRay[at + WATER_CAUSTIC_TILE + 1].xyz + deviation);
    const float halfTexel = 0.5 * fnc / float(m.n);  // half a map texel on the caustic grid
    [loop] for (uint slice = 0; slice < WATER_CAUSTIC_SLICES; ++slice)
    {
        if (CAUSTIC_STUDY_SKIP(slice)) continue;
        const float zk = waterCausticDepth(slice);
        bool merged = false;
        if (all)
        {
            const float4 r0 = gCausticRay[at], r1 = gCausticRay[at + 2], r2 = gCausticRay[at + 2 * WATER_CAUSTIC_TILE + 2], r3 = gCausticRay[at + 2 * WATER_CAUSTIC_TILE];
            const float2 c0 = r0.xy + zk * r0.zw, c1 = r1.xy + zk * r1.zw, c2 = r2.xy + zk * r2.zw, c3 = r3.xy + zk * r3.zw;
            const float2 shift = -0.25 * ((c1 - c0) + (c3 - c0));
            const uint L = causticLevel(min(min(c0, c1), min(c2, c3)) + shift, max(max(c0, c1), max(c2, c3)) + shift, nc);
            merged = deviation * zk <= WATER_CAUSTIC_AFFINE || (L > 0 && deviation * zk <= 0.25 * float(1u << L));
        }
        if (merged) CAUSTIC_STUDY_MERGED(slice);
        // The merged quad (the block's corner rays, four texels' light), or the four texels' own cells.
        const uint cells = merged ? 1 : 4, stride = merged ? 2 : 1;
        const float light = merged ? 4 * unit : unit;
        [loop] for (uint q = 0; q < cells; ++q)
        {
            const uint i0 = at + (q >> 1) * WATER_CAUSTIC_TILE + (q & 1);
            const float4 r0 = gCausticRay[i0];
            if (!(r0.x > WATER_CAUSTIC_NONE)) continue;
            const float4 r1 = gCausticRay[i0 + stride], r2 = gCausticRay[i0 + stride * (WATER_CAUSTIC_TILE + 1)], r3 = gCausticRay[i0 + stride * WATER_CAUSTIC_TILE];
            const float2 c0 = r0.xy + zk * r0.zw;
            float2 p0, p1, p2, p3;
            float density;
            if (!(r1.x > WATER_CAUSTIC_NONE && r2.x > WATER_CAUSTIC_NONE && r3.x > WATER_CAUSTIC_NONE))
            {
                // The water's edge (a corner is not water or reflects totally): the texel's own square of light, one map
                // texel wide, centred on its landing point - the shape its interior neighbours' cells take on flat water,
                // so the two tile (a point splat one caustic texel wide beside them did not: edges off by up to 25 %).
                p0 = c0 + float2(-halfTexel, -halfTexel), p1 = c0 + float2(halfTexel, -halfTexel), p2 = c0 + float2(halfTexel, halfTexel), p3 = c0 + float2(-halfTexel, halfTexel);
                density = light / (4 * halfTexel * halfTexel);
            }
            else
            {
                const float2 c1 = r1.xy + zk * r1.zw, c2 = r2.xy + zk * r2.zw, c3 = r3.xy + zk * r3.zw;
                const float a1 = 0.5 * abs((c1.x - c0.x) * (c2.y - c0.y) - (c2.x - c0.x) * (c1.y - c0.y));
                const float a2 = 0.5 * abs((c2.x - c0.x) * (c3.y - c0.y) - (c3.x - c0.x) * (c2.y - c0.y));
                const float total = a1 + a2;
                // The cell between the ray points lands half a texel on from the texel's own light (half the merged quad's
                // edges over 2): shifted back so its centre lands where the texel's centre does (flat water: the texel
                // grid moved whole, every caustic texel 1).
                const float2 shift = -(0.5 / float(stride)) * ((c1 - c0) + (c3 - c0));
                if (!(total >= 1e-12))
                {
                    causticPoint(caustics, (c0 + c2) * 0.5 + shift, slice, light, nc);  // a focus: all of it at one point
                    continue;
                }
                p0 = c0 + shift, p1 = c1 + shift, p2 = c2 + shift, p3 = c3 + shift;
                density = light / total;
            }
            causticCell(caustics, levels, p0, p1, p2, p3, slice, density, nc);
        }
    }
    CAUSTIC_STUDY_END();
}
