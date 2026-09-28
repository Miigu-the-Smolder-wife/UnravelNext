// Local-light visibility through the local virtual shadow maps (VsmLocal.hlsli). Owner: S.
// The light is a disk of radius r_L seen from the receiver (sphere, disk; rect and tube by their half extent). Same
// estimator as the sun's (VsmSample.hlsli) in the light's face tangent space:
//  1. blocker search: `searchTaps` taps over the disk of tangent radius r_L (1 / z_min - 1 / z_r) around the receiver,
//     z_min the nearest caster of the receiver's page and its four neighbours two mips coarser (their block maxima,
//     VsmPageMax; the largest reach a caster there can have); a stored depth
//     z_s occludes some light direction when it is nearer than the receiver's plane there and its tap lies within its
//     own reach r_L (1 / z_s - 1 / z_r); the blockers' mean 1 / z_s gives the penumbra radius; none: lit;
//  2. filter: 1 - mean occupancy of `filterTaps` sunflower taps over the disk of radius r_L (mean(1 / z_b) - 1 / z_r),
//     on the mip whose texel matches the tap spacing; each tap a bilinear test of its 2 x 2 texels (vsmLocalTapOcclusion:
//     the visibility is continuous in the receiver's position), taps without a resident page left out of the mean.
// Exact for an occluder at one depth when the taps resolve it; the receiver's plane is evaluated per tap along the tap's
// own direction (no normal offset), with receiver_bias_texels of the compared mip as tolerance. Taps re-project through
// the cube, so a penumbra across a face edge reads the neighbouring face. Pages not resident at a mip fall back to the
// coarser mips of the same face (VsmLocalMark requests the two coarser ones).
#ifndef UNX_VSM_LOCAL_SAMPLE_HLSLI
#define UNX_VSM_LOCAL_SAMPLE_HLSLI
#include "Passes/Shadow/VsmLocal.hlsli"

#define ATMO_PI_FOR_LOCAL 3.14159265358979323846

struct VsmLocalResources
{
    ByteAddressBuffer table;
    Texture2D<float> pool;     // the page atlas (VsmCommon.hlsli vsmAtlasTexel)
    ByteAddressBuffer blocks;  // VsmPageMax: per physical page, the page block's range.y = nearest caster key
};

// Nearest caster key of the page holding direction c at mip m (or the nearest coarser resident mip); VSM_EMPTY if none.
uint vsmLocalPageNearest(VsmLocalResources r, VsmLocalLight l, uint slot, float3 c, uint m)
{
    const VsmLocalPoint q = vsmLocalProject(l, l.position + c);
    [loop] for (int j = (int)m; j >= 0; --j)
    {
        const uint2 t = min(uint2(vsmLocalTexel(q.xy, (uint)j)), vsmLocalRes((uint)j) - 1);
        const uint2 e = r.table.Load2(vsmLocalSlot(slot, q.face, (uint)j, t >> VSM_PAGE_SHIFT) * 8);
        if ((e.x & VSM_FLAG_RESIDENT) != 0 && e.y == l.generation)
            return r.blocks.Load(((e.x & VSM_PHYS_MASK) * VSM_BLOCK_ENTRIES + VSM_BLOCK_OFFSET_128) * VSM_BLOCK_BYTES + 4);  // range.y
    }
    return VSM_EMPTY;
}

// Stored key (vsmEncode(-z)) of the texel of mip m (or the nearest coarser resident mip) holding direction c.
uint vsmLocalKeyAt(VsmLocalResources r, VsmLocalLight l, uint slot, float3 c, uint m, out uint mipUsed, out uint face)
{
    const VsmLocalPoint q = vsmLocalProject(l, l.position + c);
    face = q.face;
    mipUsed = m;
    [loop] for (int j = (int)m; j >= 0; --j)
    {
        const uint2 t = min(uint2(vsmLocalTexel(q.xy, (uint)j)), vsmLocalRes((uint)j) - 1);
        const uint2 e = r.table.Load2(vsmLocalSlot(slot, q.face, (uint)j, t >> VSM_PAGE_SHIFT) * 8);
        if ((e.x & VSM_FLAG_RESIDENT) != 0 && e.y == l.generation)
        {
            mipUsed = (uint)j;
            return vsmLocalKeyOfDepth(r.pool.Load(vsmAtlasTexel(e.x & VSM_PHYS_MASK, t & (VSM_PAGE - 1))), l.nearM, l.farM);
        }
    }
    return VSM_EMPTY;
}

// Bilinear shadow test (PCF) of one tap: the 2 x 2 texels of mip m (or the nearest coarser resident mip) around direction
// c, each compared with the receiver's plane depth zp less its tolerance, weighted by the tap's bilinear position.
// tolerancePerTexel = biasTexels x 2 z_r x (1 + slope): the tolerance is that over the used mip's resolution. Returns the
// occluded share in [0, 1], or -1 where no mip holds c's page (no data). One point-sampled texel per tap made the
// visibility a sum of 16 step functions whose steps lie on the texel grid of the chosen mip: stable blotches of about
// one texel projected onto the receivers (walls near a lamp shade's bulb: wide penumbrae, coarse mips, large blotches),
// and the hard-light path's four point samples stepped in quarters along the same grid. The 2 x 2 texels of a page's
// interior come in one gather; at a page edge each texel resolves its own page (a missing one takes the tap's texel).
// The receiver's plane depth zp along c comes from the tap's own projection (vsmLocalPlaneDepthAt: its face), and the
// page-table entry of the last page the caller's taps read is kept (cacheSlot, cacheEntry: most taps of a filter disk fall
// in the page of the previous one, whose table word no longer needs a dependent load). The same values as projecting c
// again for zp and loading every entry.
float vsmLocalPlaneDepthAt(VsmLocalLight l, float3 receiver, float3 normal, float3 c, uint face);
float vsmLocalTapOcclusion(VsmLocalResources r, VsmLocalLight l, uint slot, float3 c, uint m, float3 receiver, float3 normal, float tolerancePerTexel,
                           out uint mipUsed, inout uint cacheSlot, inout uint2 cacheEntry)
{
    const VsmLocalPoint q = vsmLocalProject(l, l.position + c);
    const float zp = vsmLocalPlaneDepthAt(l, receiver, normal, c, q.face);
    mipUsed = m;
    [loop] for (int j = (int)m; j >= 0; --j)
    {
        const uint res = vsmLocalRes((uint)j);
        const float2 tf = vsmLocalTexel(q.xy, (uint)j);
        const uint2 t = min(uint2(tf), res - 1);
        const uint pageSlot = vsmLocalSlot(slot, q.face, (uint)j, t >> VSM_PAGE_SHIFT);
        if (pageSlot != cacheSlot)
        {
            cacheEntry = r.table.Load2(pageSlot * 8);
            cacheSlot = pageSlot;
        }
        const uint2 e = cacheEntry;
        if ((e.x & VSM_FLAG_RESIDENT) == 0 || e.y != l.generation) continue;
        mipUsed = (uint)j;
        const float zLimit = zp - tolerancePerTexel / res;
        const float2 g = tf - 0.5;
        const float2 f = g - floor(g);
        const int2 i0 = int2(floor(g));
        const uint2 lo = uint2(clamp(i0, 0, (int)res - 1)), hi = uint2(clamp(i0 + 1, 0, (int)res - 1));
        const uint2 page = t >> VSM_PAGE_SHIFT;
        float4 d;  // atlas depths of (lo.x, lo.y), (hi.x, lo.y), (lo.x, hi.y), (hi.x, hi.y)
        if (all((lo >> VSM_PAGE_SHIFT) == page) && all((hi >> VSM_PAGE_SHIFT) == page) && all(hi == lo + 1))
        {
            uint aw, ah;
            r.pool.GetDimensions(aw, ah);
            const int3 a = vsmAtlasTexel(e.x & VSM_PHYS_MASK, lo & (VSM_PAGE - 1));
            const float4 gt = r.pool.GatherRed(g_pointClamp, (float2(a.xy) + 1) / float2(aw, ah));  // (x0 y1, x1 y1, x1 y0, x0 y0)
            d = float4(gt.w, gt.z, gt.x, gt.y);
        }
        else
        {
            const float dt = r.pool.Load(vsmAtlasTexel(e.x & VSM_PHYS_MASK, t & (VSM_PAGE - 1)));
            [unroll] for (uint k = 0; k < 4; ++k)
            {
                const uint2 tk = uint2(k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y);
                const uint2 ek = r.table.Load2(vsmLocalSlot(slot, q.face, (uint)j, tk >> VSM_PAGE_SHIFT) * 8);
                d[k] = (ek.x & VSM_FLAG_RESIDENT) != 0 && ek.y == l.generation ? r.pool.Load(vsmAtlasTexel(ek.x & VSM_PHYS_MASK, tk & (VSM_PAGE - 1))) : dt;
            }
        }
        float4 occluded;
        [unroll] for (uint k2 = 0; k2 < 4; ++k2)
        {
            const uint key = vsmLocalKeyOfDepth(d[k2], l.nearM, l.farM);
            occluded[k2] = key != VSM_EMPTY && -vsmDecode(key) < zLimit ? 1.0 : 0.0;
        }
        return lerp(lerp(occluded.x, occluded.y, f.x), lerp(occluded.z, occluded.w, f.x), f.y);
    }
    return -1;
}

// Face depth of the receiver's plane along direction c (unit-depth direction from the light), and the tolerance.
float vsmLocalPlaneDepthAt(VsmLocalLight l, float3 receiver, float3 normal, float3 c, uint face)
{
    float3 right, up, axis;
    vsmCubeBasis(face, right, up, axis);
    const float nc = dot(normal, c);
    // Plane n . (x - receiver) = 0 along x = light + t c: t = n . (receiver - light) / n . c (grazing: far away).
    const float t = abs(nc) > 1e-6 ? dot(normal, receiver - l.position) / nc : 3.0e38;
    return t > 0 ? t * dot(c, axis) : 3.0e38;
}

float vsmLocalVisibility(VsmLocalResources r, VsmLocalLight l, uint slot, float3 receiver, float3 normal, float footprint, float biasTexels,
                         float maxSlope, uint searchTaps, uint filterTaps)
{
    // Past the light's reach (farM = range + emitter radius) its shading window is 0 (shPunctualIlluminance,
    // shAreaWindow: w(d) = 0 for d >= range), so the visibility there multiplies nothing: 1 without the estimator. The
    // froxel lists hold the lights whose sphere meets the froxel, not each pixel (VsmLocalMark skips the same pixels).
    const float3 dl = receiver - l.position;
    if (dot(dl, dl) >= l.farM * l.farM) return 1;
    const VsmLocalPoint pr = vsmLocalProject(l, receiver);
    if (pr.z <= l.nearM) return 1;
    float3 right, up, axis;
    vsmCubeBasis(pr.face, right, up, axis);
    const float3 c0 = axis + pr.xy.x * right + pr.xy.y * up;  // receiver direction at unit depth
    const float3 toLight = normalize(l.position - receiver);
    const float slope = min(sqrt(max(1 - dot(normal, toLight) * dot(normal, toLight), 0.0)) / max(abs(dot(normal, toLight)), 1e-4), maxSlope);
    const uint m = vsmLocalMip(footprint, pr.z);
    const float invZr = 1 / pr.z;
    // Hard light (no extent beyond a texel): bilinear occupancy of the 2 x 2 texels around the receiver.
    const float texelTan = 2.0 / vsmLocalRes(m);
    // Nearest caster around the receiver (its page and the four neighbours, two mips coarser).
    const uint mp = m >= 2 ? m - 2 : 0;
    const float hp = float(VSM_PAGE) / vsmLocalRes(mp) * 2;  // a page of mip mp in tangent units
    uint nearest = VSM_EMPTY;
    nearest = max(nearest, vsmLocalPageNearest(r, l, slot, c0, mp));
    nearest = max(nearest, vsmLocalPageNearest(r, l, slot, c0 + hp * right, mp));
    nearest = max(nearest, vsmLocalPageNearest(r, l, slot, c0 - hp * right, mp));
    nearest = max(nearest, vsmLocalPageNearest(r, l, slot, c0 + hp * up, mp));
    nearest = max(nearest, vsmLocalPageNearest(r, l, slot, c0 - hp * up, mp));
    if (nearest == VSM_EMPTY) return 1;
    const float zMin = max(-vsmDecode(nearest), l.nearM);
    if (zMin >= pr.z) return 1;  // nothing nearer to the light than the receiver
    const float searchR = min(l.radius * (1 / zMin - invZr), 1.0);
    const float tolerancePerTexel = biasTexels * 2 * pr.z * (1 + slope);
    if (searchR <= texelTan)
    {
        // hard light: one bilinear shadow test at the receiver
        uint mu, cacheSlot = 0xFFFFFFFFu;
        uint2 cacheEntry = 0;
        const float occ = vsmLocalTapOcclusion(r, l, slot, c0, m, receiver, normal, tolerancePerTexel, mu, cacheSlot, cacheEntry);
        return occ > 0 ? 1 - occ : 1;
    }
    // 1. Blocker search.
    const uint ms = min(m, vsmLocalMip(searchR * sqrt(ATMO_PI_FOR_LOCAL / searchTaps) * pr.z, pr.z));
    float sumInvZ = 0, count = 0;
    [loop] for (uint i = 0; i < searchTaps; ++i)
    {
        const float2 o = vsmDiskPoint(i, searchTaps) * searchR;
        const float3 c = c0 + o.x * right + o.y * up;
        uint mu, face;
        const uint key = vsmLocalKeyAt(r, l, slot, c, ms, mu, face);
        if (key == VSM_EMPTY) continue;
        const float zs = -vsmDecode(key);
        const float zp = vsmLocalPlaneDepthAt(l, receiver, normal, c, face);
        const float tol = biasTexels * (2 * pr.z / vsmLocalRes(mu)) * (1 + slope);
        if (zs >= zp - tol) continue;
        if (length(o) - 0.7071 * 2.0 / vsmLocalRes(mu) > l.radius * (1 / max(zs, l.nearM) - invZr)) continue;
        sumInvZ += 1 / max(zs, l.nearM);
        count += 1;
    }
    if (count == 0) return 1;
    // 2. Penumbra filter.
    const float radius = l.radius * (sumInvZ / count - invZr);
    const uint mf = min(m, vsmLocalMip(radius * sqrt(ATMO_PI_FOR_LOCAL / filterTaps) * pr.z, pr.z));
    // Each tap a bilinear shadow test (vsmLocalTapOcclusion); taps whose direction no resident page holds carry no
    // information and leave the mean (counting them lit made page-shaped light patches wherever the filter reached past
    // the pages the marking requested).
    float occ = 0, taps = 0;
    uint cacheSlot = 0xFFFFFFFFu;
    uint2 cacheEntry = 0;
    [loop] for (uint i2 = 0; i2 < filterTaps; ++i2)
    {
        const float2 o = vsmDiskPoint(i2, filterTaps) * radius;
        const float3 c = c0 + o.x * right + o.y * up;
        uint mu;
        const float t = vsmLocalTapOcclusion(r, l, slot, c, mf, receiver, normal, tolerancePerTexel, mu, cacheSlot, cacheEntry);
        if (t < 0) continue;
        occ += t;
        taps += 1;
    }
    return taps > 0 ? 1 - occ / taps : 1;
}

#endif
