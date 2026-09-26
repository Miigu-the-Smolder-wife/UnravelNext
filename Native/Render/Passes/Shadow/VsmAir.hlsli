// Sun visibility of the air (froxel integration, ARCHITECTURE 2.3 "볼륨 산란"). Owner: S.
// The VSM stores, per texel, the top of a solid caster column seen from the sun; an air point is in the casters' shadow
// when a column above it reaches higher towards the sun. Along a straight segment the light-space height is linear, so
// the shadowed length is a 1D measure that the block hierarchy settles for whole sub-segments at once.
#ifndef UNX_VSM_AIR_HLSLI
#define UNX_VSM_AIR_HLSLI
#include "Passes/Shadow/VsmSample.hlsli"

// Level for the air of a froxel 'width' metres wide: the finest level whose texel is not larger than width / texelsPerTile
// (atmosphere.froxels.shadow_texels_per_tile: a shadow boundary crossing the tile-centre segment is placed to that
// fraction of the tile). False when that texel is coarser than the coarsest clipmap level: the clipmap holds no level
// for that air (at 4K about 1 km away), which then gets no caster shadows (S_STATUS_KO.md: coarser levels for far air).
// The page requests (VsmMarkAir) and the lookup use this rule.
bool vsmAirLevel(ConstantBuffer<VsmConstants> c, float width, float texelsPerTile, out uint k)
{
    const float level = floor(log2(max(width / texelsPerTile, 1e-30) * 1024.0) + c.lodBias);  // tau_0 = 2^-10 m
    k = (uint)clamp(level, 0.0, float(VSM_LEVELS - 1));
    return level <= float(VSM_LEVELS - 1);
}

// ---- Segment walk over the block hierarchy (VsmPageMax): cells of 128 (the page), 32 and 8 texels, then texels.
// Cells that the segment A + D t (texel coordinates of level k, t in [t0, t1]) crosses, in order (2D DDA).
struct VsmAirWalk
{
    float2 tNext, tDelta;
    int2 cell, step;
    float t, tEnd;
};
VsmAirWalk vsmAirWalkBegin(float2 A, float2 D, float size, float t0, float t1)
{
    VsmAirWalk w;
    const float2 p = A + D * (t0 + (t1 - t0) * 1e-4);  // inside the first cell (not on its boundary)
    w.cell = int2(floor(p / size));
    w.step = int2(sign(D));
    [unroll] for (uint i = 0; i < 2; ++i)
    {
        const bool moves = abs(D[i]) > 1e-12;
        const float boundary = (w.cell[i] + (w.step[i] > 0 ? 1 : 0)) * size;
        w.tNext[i] = moves ? (boundary - A[i]) / D[i] : 3.0e38;
        w.tDelta[i] = moves ? size / abs(D[i]) : 3.0e38;
    }
    w.t = t0;
    w.tEnd = t1;
    return w;
}
// Next cell and its parameter interval; false when the walk has passed t1.
bool vsmAirWalkNext(inout VsmAirWalk w, out float ta, out float tb, out int2 cell)
{
    ta = w.t;
    tb = w.t;
    cell = w.cell;
    if (ta >= w.tEnd) return false;
    const bool x = w.tNext.x < w.tNext.y;
    tb = min(x ? w.tNext.x : w.tNext.y, w.tEnd);
    if (x)
    {
        w.cell.x += w.step.x;
        w.tNext.x += w.tDelta.x;
    }
    else
    {
        w.cell.y += w.step.y;
        w.tNext.y += w.tDelta.y;
    }
    w.t = tb;
    return true;
}

// The sub-segment [ta, tb] against one block (plane + residual bounds of its non-empty texels): VSM_REGION_LIT when it
// lies at or above every texel, VSM_REGION_UMBRA when every texel is present and above it, else VSM_REGION_MIXED.
// q = page-local texel coordinates (the plane's own: texel i spans [i, i + 1)); the texels under the segment have
// centres within half a texel diagonal of it, so the plane is compared with that margin.
uint vsmAirBlock(VsmBlock blk, float2 qa, float2 qb, float ha, float hb)
{
    if (blk.range.y == VSM_EMPTY) return VSM_REGION_LIT;
    const float da = ha - (blk.ref + dot(blk.plane.xy, qa) + blk.plane.z), db = hb - (blk.ref + dot(blk.plane.xy, qb) + blk.plane.z);
    const float margin = 0.70711 * length(blk.plane.xy);
    if (min(da, db) - margin >= blk.residual.y) return VSM_REGION_LIT;
    if (blk.range.x != VSM_EMPTY && max(da, db) + margin < blk.residual.x) return VSM_REGION_UMBRA;
    return VSM_REGION_MIXED;
}

// Fraction (by length) of the world segment a -> b whose points lie below the height field of level k. The segment's
// light-space height is linear in its parameter; it is walked through the pages it crosses, and each page, 32-texel and
// 8-texel block it crosses is settled against the segment's own heights at the block's entry and exit (vsmAirBlock);
// only mixed blocks are walked at the next size, and at texel size the fraction below the texel's height is exact.
// Pages not resident at level k hold no caster information for the air (VsmMarkAir requests them): lit.
// Loads of one walk (atmosphere.froxels.walk_stats): pages classified mixed, 32- and 8-texel blocks, texels. capped: a
// walk stopped at its hard cap with cells left (INTERFACES 3.6; the caller raises VSM_ERR_AIR_WALK). The caps are above
// the walks' lengths: a segment crosses at most 7 cells of a 4 x 4 grid (blocks) and 15 of an 8 x 8 one (texels), and a
// froxel slice about one page at its level (512 pages is a slice 64 k texels long).
struct VsmAirWalkCount
{
    uint slices, mixedPages, blocks32, blocks8, texels;  // slices: filled by the caller
    uint capped;
};
// pageOnly (cost attribution only, atmosphere.froxels.experiment_disable 32): mixed pages are not descended.
float vsmAirShadowFraction(VsmResources r, float3 a, float3 b, uint k, inout VsmAirWalkCount count, bool pageOnly = false)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    const float3 pa = vsmLightSpaceAt(vc, a, k), pb = vsmLightSpaceAt(vc, b, k);
    const float texel = vsmTexel(k);
    const float2 A = pa.xy / texel, D = (pb.xy - pa.xy) / texel;
    const float h0 = pa.z, dh = pb.z - pa.z;
    float shadowed = 0;
    VsmAirWalk wp = vsmAirWalkBegin(A, D, VSM_PAGE, 0, 1);
    float ta, tb;
    int2 page;
    [loop] for (uint gp = 0; gp < 512 && vsmAirWalkNext(wp, ta, tb, page); ++gp)
    {
        const uint e = vsmEntry(r, page, k);
        if (e == 0) continue;
        const uint base = (e & VSM_PHYS_MASK) * VSM_BLOCK_ENTRIES;
        const float2 origin = float2(page * (int)VSM_PAGE);
        uint cls = vsmAirBlock(r.blocks.Load<VsmBlock>((base + VSM_BLOCK_OFFSET_128) * VSM_BLOCK_BYTES), A + D * ta - origin, A + D * tb - origin, h0 + dh * ta, h0 + dh * tb);
        if (cls == VSM_REGION_UMBRA) shadowed += tb - ta;
        if (cls != VSM_REGION_MIXED) continue;
        ++count.mixedPages;
        if (pageOnly) continue;
        VsmAirWalk w32 = vsmAirWalkBegin(A, D, 32, ta, tb);
        float ua, ub;
        int2 c32;
        [loop] for (uint g32 = 0; g32 < 16 && vsmAirWalkNext(w32, ua, ub, c32); ++g32)
        {
            ++count.blocks32;
            const int2 l32 = clamp(c32 - page * 4, 0, 3);  // rounding at a page boundary
            cls = vsmAirBlock(r.blocks.Load<VsmBlock>((base + VSM_BLOCK_OFFSET_32 + l32.y * 4 + l32.x) * VSM_BLOCK_BYTES), A + D * ua - origin, A + D * ub - origin,
                              h0 + dh * ua, h0 + dh * ub);
            if (cls == VSM_REGION_UMBRA) shadowed += ub - ua;
            if (cls != VSM_REGION_MIXED) continue;
            VsmAirWalk w8 = vsmAirWalkBegin(A, D, 8, ua, ub);
            float va, vb;
            int2 c8;
            [loop] for (uint g8 = 0; g8 < 16 && vsmAirWalkNext(w8, va, vb, c8); ++g8)
            {
                ++count.blocks8;
                const int2 l8 = clamp(c8 - page * 16, 0, 15);
                cls = vsmAirBlock(r.blocks.Load<VsmBlock>((base + VSM_BLOCK_OFFSET_8 + l8.y * 16 + l8.x) * VSM_BLOCK_BYTES), A + D * va - origin, A + D * vb - origin,
                                  h0 + dh * va, h0 + dh * vb);
                if (cls == VSM_REGION_UMBRA) shadowed += vb - va;
                if (cls != VSM_REGION_MIXED) continue;
                VsmAirWalk w1 = vsmAirWalkBegin(A, D, 1, va, vb);
                float xa, xb;
                int2 c1;
                [loop] for (uint g1 = 0; g1 < 24 && vsmAirWalkNext(w1, xa, xb, c1); ++g1)
                {
                    ++count.texels;
                    const uint hv = vsmSunKey(r.pool.Load(vsmAtlasTexel(e & VSM_PHYS_MASK, uint2(clamp(c1 - page * (int)VSM_PAGE, 0, (int)VSM_PAGE - 1)))),
                                              vc.hMin, vc.hMax);
                    if (hv == VSM_EMPTY) continue;
                    const float H = vsmDecode(hv), ha = h0 + dh * xa, hb = h0 + dh * xb, lo = min(ha, hb), hi = max(ha, hb);
                    shadowed += (xb - xa) * (hi > lo ? saturate((H - lo) / (hi - lo)) : (lo < H ? 1.0 : 0.0));
                }
                if (w1.t < w1.tEnd) count.capped = 1;
            }
            if (w8.t < w8.tEnd) count.capped = 1;
        }
        if (w32.t < w32.tEnd) count.capped = 1;
    }
    if (wp.t < wp.tEnd) count.capped = 1;
    return shadowed;
}
float vsmAirShadowFraction(VsmResources r, float3 a, float3 b, uint k)
{
    VsmAirWalkCount count = (VsmAirWalkCount)0;
    return vsmAirShadowFraction(r, a, b, k, count);
}

#endif
