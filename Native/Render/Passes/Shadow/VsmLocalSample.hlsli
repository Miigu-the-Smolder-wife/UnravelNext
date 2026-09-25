// Local-light visibility through the local virtual shadow maps (VsmLocal.hlsli). Owner: S.
// The light is a disk of radius r_L seen from the receiver (sphere, disk; rect and tube by their half extent). Same
// estimator as the sun's (VsmSample.hlsli) in the light's face tangent space:
//  1. blocker search: `searchTaps` bilinear taps over the disk of tangent radius r_L (1 / z_near - 1 / z_r) around the
//     receiver (the largest reach a caster between the light's near plane and the receiver can have); a stored depth
//     z_s occludes some light direction when it is nearer than the receiver's plane there and its tap lies within its
//     own reach r_L (1 / z_s - 1 / z_r); the blockers' mean 1 / z_s gives the penumbra radius; none: lit;
//  2. filter: 1 - mean occupancy of `filterTaps` sunflower taps over the disk of radius r_L (mean(1 / z_b) - 1 / z_r),
//     on the mip whose texel matches the tap spacing.
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
    ByteAddressBuffer pool;
};

// Stored key (vsmEncode(-z)) of the texel of mip m (or the nearest coarser resident mip) holding direction c.
uint vsmLocalKeyAt(VsmLocalResources r, VsmLocalLight l, uint slot, float3 c, uint m, out uint mipUsed)
{
    const VsmLocalPoint q = vsmLocalProject(l, l.position + c);
    mipUsed = m;
    [loop] for (int j = (int)m; j >= 0; --j)
    {
        const uint2 t = min(uint2(vsmLocalTexel(q.xy, (uint)j)), vsmLocalRes((uint)j) - 1);
        const uint2 e = r.table.Load2(vsmLocalSlot(slot, q.face, (uint)j, t >> VSM_PAGE_SHIFT) * 8);
        if ((e.x & VSM_FLAG_RESIDENT) != 0 && e.y == l.generation)
        {
            mipUsed = (uint)j;
            return r.pool.Load(vsmPoolAddress(e.x & VSM_PHYS_MASK, t & (VSM_PAGE - 1)));
        }
    }
    return VSM_EMPTY;
}

// Face depth of the receiver's plane along direction c (unit-depth direction from the light), and the tolerance.
float vsmLocalPlaneDepth(VsmLocalLight l, float3 receiver, float3 normal, float3 c)
{
    const VsmLocalPoint q = vsmLocalProject(l, l.position + c);
    float3 right, up, axis;
    vsmCubeBasis(q.face, right, up, axis);
    const float nc = dot(normal, c);
    // Plane n . (x - receiver) = 0 along x = light + t c: t = n . (receiver - light) / n . c (grazing: far away).
    const float t = abs(nc) > 1e-6 ? dot(normal, receiver - l.position) / nc : 3.0e38;
    return t > 0 ? t * dot(c, axis) : 3.0e38;
}

float vsmLocalVisibility(VsmLocalResources r, VsmLocalLight l, uint slot, float3 receiver, float3 normal, float footprint, float biasTexels,
                         float maxSlope, uint searchTaps, uint filterTaps)
{
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
    const float searchR = min(l.radius * (1 / l.nearM - invZr), 1.0);
    if (searchR <= texelTan)
    {
        float occ = 0;
        [unroll] for (uint j = 0; j < 4; ++j)
        {
            const float2 o = (float2(j & 1, j >> 1) - 0.5) * texelTan;
            const float3 c = c0 + o.x * right - o.y * up;
            uint mu;
            const uint key = vsmLocalKeyAt(r, l, slot, c, m, mu);
            const float zp = vsmLocalPlaneDepth(l, receiver, normal, c);
            const float tol = biasTexels * (2 * pr.z / vsmLocalRes(mu)) * (1 + slope);
            occ += key != VSM_EMPTY && -vsmDecode(key) < zp - tol ? 0.25 : 0.0;
        }
        return 1 - occ;
    }
    // 1. Blocker search.
    const uint ms = min(m, vsmLocalMip(searchR * sqrt(ATMO_PI_FOR_LOCAL / searchTaps) * pr.z, pr.z));
    float sumInvZ = 0, count = 0;
    [loop] for (uint i = 0; i < searchTaps; ++i)
    {
        const float rr = sqrt((i + 0.5) / searchTaps), a = i * 2.399963229728653;
        const float2 o = rr * float2(cos(a), sin(a)) * searchR;
        const float3 c = c0 + o.x * right + o.y * up;
        uint mu;
        const uint key = vsmLocalKeyAt(r, l, slot, c, ms, mu);
        if (key == VSM_EMPTY) continue;
        const float zs = -vsmDecode(key);
        const float zp = vsmLocalPlaneDepth(l, receiver, normal, c);
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
    float occ = 0;
    [loop] for (uint i2 = 0; i2 < filterTaps; ++i2)
    {
        const float rr = sqrt((i2 + 0.5) / filterTaps), a = i2 * 2.399963229728653;
        const float2 o = rr * float2(cos(a), sin(a)) * radius;
        const float3 c = c0 + o.x * right + o.y * up;
        uint mu;
        const uint key = vsmLocalKeyAt(r, l, slot, c, mf, mu);
        if (key == VSM_EMPTY) continue;
        const float zp = vsmLocalPlaneDepth(l, receiver, normal, c);
        const float tol = biasTexels * (2 * pr.z / vsmLocalRes(mu)) * (1 + slope);
        occ += -vsmDecode(key) < zp - tol ? 1.0 : 0.0;
    }
    return 1 - occ / filterTaps;
}

#endif
