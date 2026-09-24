// unx-kernel: cs_6_6 main
// Screen probe radiance maps (K reflection path, ScreenProbes.hlsli), one 64-thread group per probe, thread = texel:
// the 8 x 8 incident radiance of the cache entry GiProbeGather chose (block texel (5, 3).x), RGB9E5, with the 4 x 4 and
// 2 x 2 solid-angle-weighted mips and the map's frame normal. Texel-parallel so every load of the entry is in flight at
// once (the per-probe serial copy cost 0.18 ms at 4K [measured]).
// P[0] = { cache SRV (raw), probes UAV, probesX, probesY }
#include "Passes/GI/GiInternal.hlsli"

groupshared float3 g_radiance[64];
groupshared float g_weight[64];
groupshared uint g_packed[64];

[numthreads(64, 1, 1)]
void main(uint2 probe : SV_GroupID, uint texel : SV_GroupIndex)
{
    if (probe.x >= P[0].z || probe.y >= P[0].w) return;
    ByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint4> t = ResourceDescriptorHeap[P[0].y];
    const GiHeader h = giHeader(b);
    const uint x = probe.x * 8, y = probe.y * 4;
    const uint entry = t[uint2(x + 5, y + 3)].x;
    float3 radiance = 0;
    if (entry != GI_ENTRY_PENDING)
    {
        const uint2 v = b.Load2(h.offTexels + (entry * GI_TEXEL_COUNT + texel) * 8);
        radiance = float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y));  // x GI_STORE_SCALE, as stored
    }
    const uint2 tx = uint2(texel % 8, texel / 8);
    const float w = giMapTexelWeight(tx, 8);
    g_radiance[texel] = radiance * w;
    g_weight[texel] = w;
    g_packed[texel] = giPackRgb9e5(radiance);
    GroupMemoryBarrierWithGroupSync();
    if (texel < 16)  // 8 x 8 map: 16 RGBA32 texels of 4
        t[uint2(x + texel % 8, y + 1 + texel / 8)] = uint4(g_packed[4 * texel], g_packed[4 * texel + 1], g_packed[4 * texel + 2], g_packed[4 * texel + 3]);
    // 4 x 4 mip: thread c < 16 sums its 2 x 2 children; 2 x 2 mip: thread c < 4 sums its 4 x 4 children.
    float3 m1 = 0, m2 = 0;
    float w1 = 0, w2 = 0;
    if (texel < 16)
    {
        const uint2 c = uint2(texel % 4, texel / 4) * 2;
        [unroll] for (uint k = 0; k < 4; ++k)
        {
            const uint i = (c.y + k / 2) * 8 + c.x + k % 2;
            m1 += g_radiance[i];
            w1 += g_weight[i];
        }
    }
    if (texel < 4)
    {
        const uint2 c = uint2(texel % 2, texel / 2) * 4;
        [unroll] for (uint k = 0; k < 16; ++k)
        {
            const uint i = (c.y + k / 4) * 8 + c.x + k % 4;
            m2 += g_radiance[i];
            w2 += g_weight[i];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (texel < 16) g_packed[texel] = giPackRgb9e5(m1 / w1);
    if (texel < 4) g_packed[16 + texel] = giPackRgb9e5(m2 / w2);
    GroupMemoryBarrierWithGroupSync();
    if (texel < 4) t[uint2(x + texel, y + 3)] = uint4(g_packed[4 * texel], g_packed[4 * texel + 1], g_packed[4 * texel + 2], g_packed[4 * texel + 3]);
    if (texel == 4) t[uint2(x + 4, y)] = uint4(g_packed[16], g_packed[17], g_packed[18], g_packed[19]);
    if (texel == 5)
    {
        const float3 n = entry != GI_ENTRY_PENDING ? giAnchorNormal(b, h, entry) : float3(0, 0, 1);
        t[uint2(x + 4, y + 3)] = uint4(giPackNormal(n), 0, 0, 0);
    }
}
