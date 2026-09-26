// Sun visibility through the virtual shadow map (ARCHITECTURE 2.3 "SMRT", 2.11). Owner: S.
// The sun is a disk of angular radius theta_s; every stored texel is the top of a solid column (the height field seen
// from the sun). See vsmSunVisibility for the estimator and its exactness condition.
#ifndef UNX_VSM_SAMPLE_HLSLI
#define UNX_VSM_SAMPLE_HLSLI
#include "Passes/Shadow/VsmCommon.hlsli"

#define ATMO_PI_FOR_VSM 3.14159265358979323846

struct VsmResources
{
    ByteAddressBuffer table;
    Texture2D<float> pool;          // the page atlas (VsmCommon.hlsli vsmAtlasTexel)
    ByteAddressBuffer searchBound;  // VsmSearchGrid MODE 1: per slot, highest caster over its 3 x 3 pages
    ByteAddressBuffer blocks;       // VsmPageMax: min/max hierarchy per physical page
    uint cbv;  // VsmConstants constant buffer view (a ConstantBuffer member makes DXC fail; bound per function)
};

// Page entry of an absolute page at level k, or 0 when not resident or out of the window.
uint vsmEntry(VsmResources r, int2 page, uint k)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    if (!vsmInWindow(vc, page, k)) return 0;
    const uint2 e = r.table.Load2(vsmSlot(page, k) * 8);
    const bool hit = (e.x & VSM_FLAG_RESIDENT) != 0 && e.y == vsmTag(page);
    if (hit && vc.useStats != 0)
    {
        RWByteAddressBuffer use = ResourceDescriptorHeap[vc.useStats - 1];  // measurement only (shadow.vsm.use_stats)
        use.InterlockedOr(vsmSlot(page, k) * 4, 1u);
    }
    return hit ? e.x : 0;
}

// The receiver's plane in light space: its height above a lateral offset q from the receiver is hr + dot(slope, q).
// Comparing every texel with the plane at the texel's own centre removes self-shadowing on any planar receiver without
// moving the lookup point (a normal offset shifts the whole penumbra laterally by about a texel).
struct VsmReceiver
{
    float3 world;   // world position and geometric normal (re-projection onto another level's basis)
    float3 normal;
    uint level;     // the level whose basis uv, h and slope are in
    float2 uv;      // light-space lateral position
    float h;        // light-space height (towards the sun)
    float2 slope;   // height gradient of the receiver's plane, clamped
    float bias;     // texel-scale tolerance (the stored height is the surface at the texel centre)
};

VsmReceiver vsmMakeReceiver(ConstantBuffer<VsmConstants> c, float3 p, float3 n, uint k)
{
    const float3 ls = vsmLightSpaceAt(c, p, k);
    const float3 nl = vsmLightSpaceAt(c, n, k);
    VsmReceiver rc;
    rc.world = p;
    rc.normal = n;
    rc.level = k;
    rc.uv = ls.xy;
    rc.h = ls.z;
    const float2 slope = -nl.xy / max(abs(nl.z), 1e-4);
    rc.slope = slope * min(1.0, c.maxReceiverSlope / max(length(slope), 1e-6));
    rc.bias = c.receiverBiasTexels;
    return rc;
}
// The receiver in level j's basis: the same numbers when j shares the basis of the receiver's level (nested grids).
VsmReceiver vsmReceiverAt(ConstantBuffer<VsmConstants> c, VsmReceiver rc, uint j)
{
    if (vsmSameBasis(c, rc.level, j))
    {
        rc.level = j;
        return rc;
    }
    return vsmMakeReceiver(c, rc.world, rc.normal, j);
}

// Encoded height at an absolute texel of level k, from the finest resident level at or above k (a coarser level stores
// the same height field at a coarser texel). Returns VSM_EMPTY when no level holds it. A coarser level with another
// basis takes the texel centre at the receiver's height into its own grid (rc: the receiver at level k).
uint vsmHeightAt(VsmResources r, VsmReceiver rc, int2 texel, uint k)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    [loop] for (uint j = k; j < VSM_LEVELS; ++j)
    {
        int2 t = texel >> (int)(j - k);
        if (!vsmSameBasis(vc, k, j))
        {
            const float2 centre = (float2(texel) + 0.5) * vsmTexel(k);
            const float3 w = vc.level[k].lightX * centre.x + vc.level[k].lightY * centre.y + vc.level[k].lightZ * rc.h;
            t = int2(floor(vsmLightSpaceAt(vc, w, j).xy / vsmTexel(j)));
        }
        const uint e = vsmEntry(r, t >> (int)VSM_PAGE_SHIFT, j);
        if (e != 0) return vsmSunKey(r.pool.Load(vsmAtlasTexel(e & VSM_PHYS_MASK, uint2(t & (int)(VSM_PAGE - 1)))), vc.hMin, vc.hMax);
    }
    return VSM_EMPTY;
}

// Height the receiver's plane has under texel tj of level k, plus the level's tolerance.
float vsmPlaneHeight(ConstantBuffer<VsmConstants> c, VsmReceiver rc, int2 tj, uint k)
{
    const float2 centre = (float2(tj) + 0.5) * vsmTexel(k);
    return rc.h + dot(rc.slope, centre - rc.uv) + rc.bias * vsmTexel(k) * (1 + length(rc.slope));
}

// The 2 x 2 texels around uv on the finest resident level at or above k: one page-table walk per tap and one gather
// when the four texels share a page (127 of 128 positions per axis: two 8-byte loads); across a page border each
// texel is looked up.
struct VsmQuad
{
    uint4 h;      // encoded heights of t0, t0 + (1,0), t0 + (0,1), t0 + (1,1)
    int2 t0;
    float2 f;     // bilinear weights
    uint level;   // level the texels come from
};

// At the receiver's position plus a lateral offset (m, in the light plane).
VsmQuad vsmFetchQuad(VsmResources r, VsmReceiver rc, float2 offset, uint k)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    VsmQuad q;
    q.h = VSM_EMPTY;
    q.level = k;
    q.t0 = 0;
    q.f = 0;
    [loop] for (uint j = k; j < VSM_LEVELS; ++j)
    {
        const VsmReceiver rj = vsmReceiverAt(vc, rc, j);
        const float2 t = (rj.uv + offset) / vsmTexel(j) - 0.5;
        const int2 t0 = int2(floor(t));
        const uint e = vsmEntry(r, t0 >> (int)VSM_PAGE_SHIFT, j);
        if (e == 0) continue;
        q.t0 = t0;
        q.f = t - float2(t0);
        q.level = j;
        const uint2 local = uint2(t0 & (int)(VSM_PAGE - 1));
        if (all(local < VSM_PAGE - 1))
        {
            // The 2 x 2 texels in one gather (point sampler at their shared corner: the footprint centre is half a texel
            // from every texel edge, so float rounding cannot pick another quad). Gather order: w (0,0), z (1,0),
            // x (0,1), y (1,1).
            const int3 a = vsmAtlasTexel(e & VSM_PHYS_MASK, local);
            const float2 atlasSize = float2(vc.poolPagesX, vc.poolPagesY) * VSM_PAGE;
            const float4 g = r.pool.GatherRed(g_pointClamp, (float2(a.xy) + 1.0) / atlasSize);
            q.h = uint4(vsmSunKey(g.w, vc.hMin, vc.hMax), vsmSunKey(g.z, vc.hMin, vc.hMax), vsmSunKey(g.x, vc.hMin, vc.hMax), vsmSunKey(g.y, vc.hMin, vc.hMax));
        }
        else
        {
            q.h.x = vsmSunKey(r.pool.Load(vsmAtlasTexel(e & VSM_PHYS_MASK, local)), vc.hMin, vc.hMax);
            q.h.y = vsmHeightAt(r, rj, t0 + int2(1, 0), j);
            q.h.z = vsmHeightAt(r, rj, t0 + int2(0, 1), j);
            q.h.w = vsmHeightAt(r, rj, t0 + int2(1, 1), j);
        }
        return q;
    }
    return q;
}

// Fraction (bilinear over the 2 x 2 texels around uv, finest resident level at or above k) of columns that rise above
// the receiver's plane.
float vsmOccupancy(VsmResources r, VsmReceiver rc, float2 offset, uint k)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    const VsmQuad q = vsmFetchQuad(r, rc, offset, k);
    const VsmReceiver rq = vsmReceiverAt(vc, rc, q.level);
    float o[4];
    [unroll] for (uint j = 0; j < 4; ++j)
        o[j] = q.h[j] > vsmEncode(vsmPlaneHeight(vc, rq, q.t0 + int2(j & 1, j >> 1), q.level)) ? 1.0 : 0.0;
    return lerp(lerp(o[0], o[1], q.f.x), lerp(o[2], o[3], q.f.x), q.f.y);
}

// Highest caster the blocker search of a receiver at uv (level k) can meet: one read of the search bound grid.
// rc: the receiver at level k (heights in level k's basis; a coarser level with another basis differs from it by less
// than the refresh bound, like its pages' content).
float vsmSearchHeight(VsmResources r, VsmReceiver rc, uint k)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    const int2 page = vsmAbsPage(vsmAbsTexel(vc, rc.uv, k));
    [loop] for (uint j = k; j < VSM_LEVELS; ++j)
    {
        float slack;
        const int2 a = vsmPageAcross(vc, page, k, j, slack);  // across a basis change: raised by the height slack
        if (vsmInWindow(vc, a, j))
        {
            const uint m = r.searchBound.Load(vsmSlot(a, j) * 4);
            return m == VSM_EMPTY ? -3.0e38 : vsmDecode(m) + slack;
        }
    }
    return -3.0e38;
}

// Exact classification of the texels in the square [uv - radius, uv + radius] against the receiver's plane, from the
// finest level at or above k on which the square fits 2 x 2 blocks and every page it touches is resident:
//   VSM_REGION_LIT: no caster texel rises above the plane (+ tolerance); VSM_REGION_UMBRA: every texel is a caster above
//   it; VSM_REGION_MIXED otherwise. Per block, texel height <= block plane + residual max; the block plane minus the
//   receiver plane is linear, so its extremes over the block are at the block's corners.
#define VSM_REGION_MIXED 0u
#define VSM_REGION_LIT 1u
#define VSM_REGION_UMBRA 2u

// levelOnly: level k alone (MIXED when the square does not fit or a page is not resident there): the classification then
// speaks for exactly the height field a level-k lookup reads (fragment visibility, vsmFragmentSegmentClassify).
uint vsmRegionClassify(VsmResources r, VsmReceiver rcIn, float radius, uint k, bool levelOnly = false)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    [loop] for (uint j = k; j < (levelOnly ? k + 1 : VSM_LEVELS); ++j)
    {
        const VsmReceiver rc = vsmReceiverAt(vc, rcIn, j);
        const float2 uv = rc.uv;
        const float texel = vsmTexel(j);
        const int2 t0 = int2(floor((uv - radius) / texel)), t1 = int2(floor((uv + radius) / texel));
        const uint size = (uint)max(t1.x - t0.x, t1.y - t0.y) + 1;
        const uint m = size <= 8 ? 0u : (uint)ceil(log2(size / 8.0));
        if (m > 4) continue;                    // wider than two pages here: the next level (texel x 2)
        const int shift = 3 + (int)m;           // texel -> block
        const int perPage = (int)(16u >> m);    // blocks per page axis
        const int2 b0 = t0 >> shift, b1 = t1 >> shift;
        uint e[4];
        bool resident = true;
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            const int2 b = int2((i & 1) ? b1.x : b0.x, (i & 2) ? b1.y : b0.y);
            e[i] = vsmEntry(r, b >> (int)(4 - m), j);
            resident = resident && e[i] != 0;
        }
        if (!resident) continue;
        const float tolerance = rc.bias * texel * (1 + length(rc.slope));
        const float blockTexels = float(8u << m);
        bool lit = true, umbra = true;
        [unroll] for (uint i2 = 0; i2 < 4; ++i2)
        {
            const int2 b = int2((i2 & 1) ? b1.x : b0.x, (i2 & 2) ? b1.y : b0.y);
            const int2 page = b >> (int)(4 - m);
            const uint2 local = uint2(b & (perPage - 1));
            const VsmBlock blk = r.blocks.Load<VsmBlock>(((e[i2] & VSM_PHYS_MASK) * VSM_BLOCK_ENTRIES + vsmBlockOffset(m) + local.y * perPage + local.x) * VSM_BLOCK_BYTES);
            if (blk.range.y == VSM_EMPTY)
            {
                umbra = false;  // no caster at all: lit block
                continue;
            }
            // D(x, y) = block plane - receiver plane at page texel coordinates (x, y), over the block's corners.
            float dlo = 3.0e38, dhi = -3.0e38;
            [unroll] for (uint q = 0; q < 4; ++q)
            {
                const float2 xy = (float2(local) + float2(q & 1, q >> 1)) * blockTexels;               // page texels
                const float2 world = (float2(page * (int)VSM_PAGE) + xy) * texel;                        // light-space lateral
                const float d = blk.ref + dot(blk.plane.xy, xy) + blk.plane.z - (rc.h + dot(rc.slope, world - rc.uv) + tolerance);
                dlo = min(dlo, d);
                dhi = max(dhi, d);
            }
            lit = lit && dhi + blk.residual.y <= 0;
            umbra = umbra && blk.range.x != VSM_EMPTY && dlo + blk.residual.x > 0;
        }
        return lit ? VSM_REGION_LIT : umbra ? VSM_REGION_UMBRA : VSM_REGION_MIXED;
    }
    return VSM_REGION_MIXED;
}

// One piece of a segment (vsmSegmentClassify): the reach square of any of its points lies inside the square around its
// endpoints' reach squares; its light-space height is linear along its projection: the plane h(uv) = h_a + g . (uv - uv_a),
// g = (h_b - h_a) d / |d|^2 (d = uv_b - uv_a), bounds every point whose reach square holds a texel to within
// sqrt 2 |g| reach of the texel's plane height. Texels farther than reach from the piece's line concern no point, so the
// comparison plane may also tilt across the line: by the surface's light-space slope (side, from the band A surface the
// fragments stand on; 0 in free air), which keeps a surface rising beside the line below it; texels within reach of the
// line stay within |side_perp| reach more. No texel of the square above plane - margin -> lit, every texel above
// plane + margin -> umbra, both exact (conservative). A piece nearly along the sun (projection shorter than 2 sqrt 2
// reach) uses the flat bounds [h_lo, h_hi] tilted the same way.
// Near a surface (surface: its plane h = surface.z + side . (uv - surface.xy); valid = the fragments stand on it) first
// against that plane raised by the piece's least height above it: every point of the piece is at least delta_min above
// the plane beside it, so no texel above plane + delta_min - sqrt 2 |side| reach -> lit (the surface itself and any
// texel behind the piece pass), every texel above plane + delta_max + sqrt 2 |side| reach -> umbra.
// levelOnly: every region test on level k alone (vsmRegionClassify). marginLit / marginUmbra: the lit test's plane lowered and
// the umbra test's raised by these heights (fragment visibility: any receiver plane of slope <= maxReceiverSlope through
// a point of the piece stays within them over the reach square, so the class holds for such a receiver too).
uint vsmPieceClassify(VsmResources r, ConstantBuffer<VsmConstants> vc, VsmReceiver a, VsmReceiver b, float reach, uint k, float2 side, float3 surface,
                      bool onSurface, bool levelOnly = false, float marginLit = 0, float marginUmbra = 0)
{
    const float3 axis = vc.level[k].lightZ;
    const float2 lo = min(a.uv, b.uv) - reach, hi = max(a.uv, b.uv) + reach;
    VsmReceiver c = a;
    c.uv = 0.5 * (lo + hi);
    const float radius = 0.5 * max(hi.x - lo.x, hi.y - lo.y);
    if (onSurface)
    {
        const float da = a.h - surface.z - dot(side, a.uv - surface.xy), db = b.h - surface.z - dot(side, b.uv - surface.xy);
        const float base = surface.z + dot(side, c.uv - surface.xy), margin = 1.4142136 * length(side) * reach;
        c.slope = side;
        c.h = base + min(da, db) - margin - marginLit;
        c.world = vc.level[k].lightX * c.uv.x + vc.level[k].lightY * c.uv.y + axis * c.h;
        if (vsmRegionClassify(r, c, radius, k, levelOnly) == VSM_REGION_LIT) return VSM_REGION_LIT;
        c.h = base + max(da, db) + margin + marginUmbra;
        c.world = vc.level[k].lightX * c.uv.x + vc.level[k].lightY * c.uv.y + axis * c.h;
        if (vsmRegionClassify(r, c, radius, k, levelOnly) == VSM_REGION_UMBRA) return VSM_REGION_UMBRA;
    }
    const float2 d = b.uv - a.uv;
    const float len = length(d);
    const float2 dir = len > 0 ? d / len : float2(1, 0);
    const float2 sidePerp = side - dot(side, dir) * dir;  // the surface slope across the line
    const float sideMargin = length(sidePerp) * reach;
    float hLit = min(a.h, b.h) - sideMargin, hUmbra = max(a.h, b.h) + sideMargin;
    c.slope = sidePerp;
    if (len > 2.8284271 * reach)
    {
        const float2 along = (b.h - a.h) * d / (len * len);
        c.slope = along + sidePerp;
        const float centre = a.h + dot(c.slope, c.uv - a.uv), margin = 1.4142136 * length(along) * reach + sideMargin;
        hLit = centre - margin;
        hUmbra = centre + margin;
    }
    else
    {
        // Flat along the line: the bounds are taken at the square's centre of the tilted plane.
        const float shift = dot(sidePerp, c.uv - a.uv);
        hLit += shift;
        hUmbra += shift;
    }
    c.h = hLit - marginLit;
    c.world = vc.level[k].lightX * c.uv.x + vc.level[k].lightY * c.uv.y + axis * c.h;
    const uint lit = vsmRegionClassify(r, c, radius, k, levelOnly);
    if (lit == VSM_REGION_LIT) return VSM_REGION_LIT;
    c.h = hUmbra + marginUmbra;
    c.world = vc.level[k].lightX * c.uv.x + vc.level[k].lightY * c.uv.y + axis * c.h;
    return vsmRegionClassify(r, c, radius, k, levelOnly) == VSM_REGION_UMBRA ? VSM_REGION_UMBRA : VSM_REGION_MIXED;
}

// Segment p0 -> p1 (world; a pixel's fragment depth range, COVERAGE_REDESIGN 4.3): VSM_REGION_LIT when every point of it
// sees the whole sun disk, VSM_REGION_UMBRA when none sees any of it, VSM_REGION_MIXED otherwise. Points are receivers
// without a surface plane (flat in light space); surfacePoint / surfaceNormal: the band A surface the fragments stand on
// (normal 0 = free air): each piece is tried against that surface's plane first (vsmPieceClassify). The segment is cut into pieces whose projection is at most 8 texels or
// 2 sqrt 2 reach (at most VSM_SEGMENT_PIECES; each piece's square then stays near its line), each classified exactly
// (vsmPieceClassify): lit / umbra when every piece is. Level: the pixel's, coarser while the segment is longer than a
// page (so the search bound of its endpoints covers it).
#define VSM_SEGMENT_PIECES 8u
// The segment on level k (its projection at most a page long; vsmSegmentClassify picks k, vsmFragmentSegmentClassify fixes
// it). anyNormal: the class must also hold for a receiver plane of any slope <= maxReceiverSlope through each point
// (fragment visibility against the per-record SMRT), and every region test is on level k alone.
uint vsmSegmentClassifyAt(VsmResources r, float3 p0, float3 p1, uint k, float tanSun, float3 surfacePoint, float3 surfaceNormal, bool anyNormal)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    const float3 axis = vc.level[k].lightZ;
    const VsmReceiver a = vsmMakeReceiver(vc, p0, axis, k), b = vsmMakeReceiver(vc, p1, axis, k);
    // The surface's light-space slope (vsmMakeReceiver's plane of the normal; 0 for no surface).
    const bool onSurface = dot(surfaceNormal, surfaceNormal) > 0;
    const VsmReceiver sr = vsmMakeReceiver(vc, onSurface ? surfacePoint : p0, onSurface ? surfaceNormal : axis, k);
    const float2 side = onSurface ? sr.slope : float2(0, 0);
    const float3 surface = float3(sr.uv, sr.h);
    const float dmax = max(vsmSearchHeight(r, a, k), vsmSearchHeight(r, b, k)) - min(a.h, b.h);
    if (dmax <= 0) return VSM_REGION_LIT;
    const float reach = dmax * tanSun;
    // Any receiver plane: over the reach square (half-diagonal sqrt 2 reach) its height departs from the point's by at most
    // sqrt 2 slope reach, and its tolerance grows by bias texel slope (vsmPlaneHeight).
    const float marginLit = anyNormal ? 1.4142136 * vc.maxReceiverSlope * reach : 0;
    const float marginUmbra = anyNormal ? marginLit + vc.receiverBiasTexels * vsmTexel(k) * vc.maxReceiverSlope : 0;
    const float pieceLen = max(8 * vsmTexel(k), 2.8284271 * reach);
    const uint pieces = clamp((uint)ceil(length(b.uv - a.uv) / pieceLen), 1u, VSM_SEGMENT_PIECES);
    uint lit = 0, umbra = 0;
    [loop] for (uint i = 0; i < pieces; ++i)
    {
        VsmReceiver pa = a, pb = a;
        const float t0 = float(i) / pieces, t1 = float(i + 1) / pieces;
        pa.uv = lerp(a.uv, b.uv, t0);
        pa.h = lerp(a.h, b.h, t0);
        pa.world = lerp(p0, p1, t0);
        pb.uv = lerp(a.uv, b.uv, t1);
        pb.h = lerp(a.h, b.h, t1);
        pb.world = lerp(p0, p1, t1);
        const uint cls = vsmPieceClassify(r, vc, pa, pb, reach, k, side, surface, onSurface, anyNormal, marginLit, marginUmbra);
        if (cls == VSM_REGION_MIXED) return VSM_REGION_MIXED;
        lit += cls == VSM_REGION_LIT ? 1u : 0u;
        umbra += cls == VSM_REGION_UMBRA ? 1u : 0u;
    }
    return lit == pieces ? VSM_REGION_LIT : umbra == pieces ? VSM_REGION_UMBRA : VSM_REGION_MIXED;
}
uint vsmSegmentClassify(VsmResources r, float3 p0, float3 p1, float footprint, float tanSun, float3 surfacePoint, float3 surfaceNormal)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    uint k = vsmLevelForFootprint(vc, footprint);
    [loop] while (k + 1 < VSM_LEVELS && length(vsmLightSpaceAt(vc, p1, k).xy - vsmLightSpaceAt(vc, p0, k).xy) > vsmPageSize(k)) ++k;
    return vsmSegmentClassifyAt(r, p0, p1, k, tanSun, surfacePoint, surfaceNormal, false);
}

// Fragment segments (coverage layer, ShadowFragments): the pixel's view ray from linear depth z0 to z1 (points p0, p1)
// cut where the footprint level changes (a record at depth z is looked up on vsmLevelForFootprint(z x pixelScale), the
// per-record SMRT's level), each level's piece cut again into sub-pieces at most a page long, each classified on its own
// level only (vsmSegmentClassifyAt, anyNormal): lit / umbra when every sub-piece is. Pieces overlap by 1e-4 of the depth
// at their shared end, so a record at a level boundary is covered on either level. More than VSM_FRAGMENT_PIECES
// sub-pieces: MIXED (the records are evaluated one by one). VsmMarkFragments requests the same pages.
#define VSM_FRAGMENT_PIECES 16u
uint vsmFragmentSegmentClassify(VsmResources r, float3 p0, float3 p1, float z0, float z1, float pixelScale, float tanSun, float3 surfacePoint,
                                float3 surfaceNormal)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    uint k = vsmLevelForFootprint(vc, z0 * pixelScale);
    float z = z0;
    uint lit = 0, umbra = 0, pieces = 0;
    [loop] for (uint guard = 0; guard < VSM_LEVELS && pieces <= VSM_FRAGMENT_PIECES; ++guard)
    {
        const float zEnd = min(z1, vsmFragmentLevelEnd(vc, k, pixelScale) * 1.0001);
        const float3 a = z1 > z0 ? lerp(p0, p1, (z - z0) / (z1 - z0)) : p0, b = z1 > z0 ? lerp(p0, p1, (zEnd - z0) / (z1 - z0)) : p1;
        const float len = length(vsmLightSpaceAt(vc, b, k).xy - vsmLightSpaceAt(vc, a, k).xy);
        const uint n = max(1u, (uint)ceil(len / vsmPageSize(k)));
        if (pieces + n > VSM_FRAGMENT_PIECES) return VSM_REGION_MIXED;
        [loop] for (uint i = 0; i < n; ++i)
        {
            const uint cls = vsmSegmentClassifyAt(r, lerp(a, b, float(i) / n), lerp(a, b, float(i + 1) / n), k, tanSun, surfacePoint, surfaceNormal, true);
            if (cls == VSM_REGION_MIXED) return VSM_REGION_MIXED;
            lit += cls == VSM_REGION_LIT ? 1u : 0u;
            umbra += cls == VSM_REGION_UMBRA ? 1u : 0u;
        }
        pieces += n;
        if (zEnd >= z1) break;
        z = zEnd / 1.0002;  // the next level's piece starts just before this one ended (overlap at the boundary)
        ++k;
    }
    return lit == pieces ? VSM_REGION_LIT : umbra == pieces ? VSM_REGION_UMBRA : VSM_REGION_MIXED;
}

// Sunflower point i of n in the unit disk (equal area).
float2 vsmDiskPoint(uint i, uint n)
{
    const float rr = sqrt((i + 0.5) / n);
    const float a = i * 2.399963229728653;
    return rr * float2(cos(a), sin(a));
}

// Level at or above k whose texel is closest to (not above) 'size' metres.
uint vsmLevelForSize(ConstantBuffer<VsmConstants> c, uint k, float size)
{
    return min(k + (uint)max(0.0, floor(log2(max(size / vsmTexel(k), 1.0)))), VSM_LEVELS - 1);
}

// Sun visibility in [0, 1] of a receiver at world position p with geometric normal n and pixel footprint (m).
// A column of the height field at lateral offset q from the receiver, at height d above it, blocks exactly the disk
// directions within one texel of q / (d tan theta_s): an occluder at a single height d_b blocks the part of the disk
// that its footprint covers inside the lateral disk of radius d_b tan theta_s. So:
//  1. reach: the highest caster over the neighbouring pages (search bound grid) bounds d; nothing above -> lit; the block
//     hierarchy over the square around the reach disk settles lit (no caster texel above the receiver's plane) or
//     umbra (every texel above it) exactly (vsmSunClassify);
//  2. blocker height: `searchTaps` bilinear taps over the reach disk, on the level whose texel matches their spacing;
//     d_b = mean height above the receiver of the texels that can block some sun direction; none -> lit;
//  3. visibility = 1 - mean occupancy of `filterTaps` bilinear taps (sunflower) over the disk of radius d_b tan theta_s,
//     taken from the level whose texel matches their spacing (continuous in the receiver position), unless the block
//     hierarchy settles the disk's square (vsmSunPenumbra).
// Exact for an occluder at one height above the receiver when the taps resolve its footprint; with blockers at several
// heights d_b is their mean (contact hardening follows the nearest dominant blocker). No noise, no per-pixel pattern.
#define VSM_PATH_NO_CASTER 0u      // nothing above the receiver within reach
#define VSM_PATH_REGION_LIT 1u     // block hierarchy over the reach square: no texel above the plane
#define VSM_PATH_REGION_UMBRA 2u   // block hierarchy over the reach square: every texel above the plane
#define VSM_PATH_SEARCH_LIT 3u     // blocker search found no blocker that can shade
#define VSM_PATH_FILTERED 4u       // penumbra filter
#define VSM_PATH_DISK_LIT 5u       // block hierarchy over the penumbra disk: no texel above the plane
#define VSM_PATH_DISK_UMBRA 6u     // block hierarchy over the penumbra disk: every texel above the plane

// Steps 1: VSM_REGION_LIT / VSM_REGION_UMBRA when settled, else VSM_REGION_MIXED with the level and reach for step 2.
uint vsmSunClassify(VsmResources r, VsmReceiver rc, float footprint, float tanSun, out uint k, out float reach, out uint path)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    k = vsmLevelForFootprint(vc, footprint);
    reach = 0;
    path = VSM_PATH_NO_CASTER;
    const VsmReceiver rk = vsmReceiverAt(vc, rc, k);
    const float dmax = vsmSearchHeight(r, rk, k) - rk.h;
    if (dmax <= 0) return VSM_REGION_LIT;
    reach = dmax * tanSun;
    const uint cls = vsmRegionClassify(r, rk, reach, k);
    path = cls == VSM_REGION_LIT ? VSM_PATH_REGION_LIT : cls == VSM_REGION_UMBRA ? VSM_PATH_REGION_UMBRA : VSM_PATH_FILTERED;
    return cls;
}

// Steps 2 and 3 for a receiver vsmSunClassify left mixed.
float vsmSunPenumbra(VsmResources r, VsmReceiver rc, uint k, float reach, float tanSun, uint searchTaps, uint filterTaps, out uint path)
{
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[r.cbv];
    rc = vsmReceiverAt(c, rc, k);
    const uint ks = vsmLevelForSize(c, k, reach * sqrt(ATMO_PI_FOR_VSM / searchTaps));
    float sum = 0, count = 0;
    [loop] for (uint i = 0; i < searchTaps; ++i)
    {
        const VsmQuad q = vsmFetchQuad(r, rc, vsmDiskPoint(i, searchTaps) * reach, ks);
        const VsmReceiver rq = vsmReceiverAt(c, rc, q.level);
        const float texel = vsmTexel(q.level);
        [unroll] for (uint j = 0; j < 4; ++j)
        {
            const int2 tj = q.t0 + int2(j & 1, j >> 1);
            const float plane = vsmPlaneHeight(c, rq, tj, q.level);
            if (q.h[j] <= vsmEncode(plane)) continue;
            const float d = vsmDecode(q.h[j]) - plane;
            // Lateral distance of the texel from the receiver (less half a texel diagonal) within the cone it can shade.
            const float2 centre = (float2(tj) + 0.5) * texel - rq.uv;
            if (length(centre) - 0.7071 * texel <= d * tanSun)
            {
                sum += d;
                count += 1;
            }
        }
    }
    if (count == 0)
    {
        path = VSM_PATH_SEARCH_LIT;
        return 1;
    }
    const float radius = (sum / count) * tanSun;
    const uint diskClass = vsmRegionClassify(r, rc, radius, k);
    if (diskClass == VSM_REGION_LIT)
    {
        path = VSM_PATH_DISK_LIT;
        return 1;
    }
    if (diskClass == VSM_REGION_UMBRA)
    {
        path = VSM_PATH_DISK_UMBRA;
        return 0;
    }
    path = VSM_PATH_FILTERED;
    const uint kf = vsmLevelForSize(c, k, radius * sqrt(ATMO_PI_FOR_VSM / filterTaps));
    float occ = 0;
    [loop] for (uint i2 = 0; i2 < filterTaps; ++i2)
        occ += vsmOccupancy(r, rc, vsmDiskPoint(i2, filterTaps) * radius, kf);
    return 1 - occ / filterTaps;
}

float vsmSunVisibility(VsmResources r, float3 p, float3 n, float footprint, float tanSun, uint searchTaps, uint filterTaps, out uint path)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    const VsmReceiver rc = vsmMakeReceiver(vc, p, n, vsmLevelForFootprint(vc, footprint));
    uint k;
    float reach;
    const uint cls = vsmSunClassify(r, rc, footprint, tanSun, k, reach, path);
    if (cls == VSM_REGION_LIT) return 1;
    if (cls == VSM_REGION_UMBRA) return 0;
    return vsmSunPenumbra(r, rc, k, reach, tanSun, searchTaps, filterTaps, path);
}

#endif
