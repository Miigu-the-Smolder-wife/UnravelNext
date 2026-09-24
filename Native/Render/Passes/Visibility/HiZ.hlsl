// unx-kernel: cs_6_6 main
// unx-variants: FIRST=0,1
// HiZ (Frame.h ViewResources::hiz): texel (i, j) of mip m holds the farthest depth (minimum reversed-Z value) of the
// pixels [i 2^(m+1), (i+1) 2^(m+1)) x [j 2^(m+1), (j+1) 2^(m+1)); mip sizes round up, reads beyond an edge clamp to it
// (a clamped read repeats a texel of the same block, so every value stays a true minimum).
// One dispatch writes up to five levels: each 16 x 16 group reduces a 32 x 32 source tile to 16 x 16, 8 x 8, 4 x 4,
// 2 x 2 and 1 x 1 in group shared memory.
//   FIRST=1: source = the depth buffer (SRV), output levels start at mip 0. FIRST=0: source = a HiZ mip (UAV).
//   P[0] source SRV/UAV, source width, source height, output level count (1..5)
//   P[1] UAVs of output levels 0..3, P[2].x UAV of output level 4, P[2].yz size of output level 0
#include "Bindless.hlsli"

groupshared float g_tile[16][16];

float loadSource(uint2 p)
{
    const uint2 clamped = min(p, P[0].yz - 1);
#if FIRST
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    return depth.Load(int3(clamped, 0));
#else
    RWTexture2D<float> source = ResourceDescriptorHeap[P[0].x];
    return source[clamped];
#endif
}

void store(uint level, uint2 texel, float value)
{
    const uint index = level < 4 ? P[1][level] : P[2].x;
    const uint2 size = (P[2].yz + (1u << level) - 1) >> level;
    if (all(texel < size))
    {
        RWTexture2D<float> target = ResourceDescriptorHeap[index];
        target[texel] = value;
    }
}

[numthreads(16, 16, 1)]
void main(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID)
{
    const uint2 texel = group * 16 + local;
    const uint2 s = texel * 2;
    float value = min(min(loadSource(s), loadSource(s + uint2(1, 0))), min(loadSource(s + uint2(0, 1)), loadSource(s + uint2(1, 1))));
    store(0, texel, value);
    g_tile[local.y][local.x] = value;
    const uint levels = P[0].w;
    for (uint level = 1; level < levels; ++level)
    {
        const uint n = 16u >> level;
        GroupMemoryBarrierWithGroupSync();
        const bool active = all(local < n);
        if (active)
        {
            const uint2 a = local * 2;
            value = min(min(g_tile[a.y][a.x], g_tile[a.y][a.x + 1]), min(g_tile[a.y + 1][a.x], g_tile[a.y + 1][a.x + 1]));
        }
        GroupMemoryBarrierWithGroupSync();
        if (active)
        {
            g_tile[local.y][local.x] = value;
            store(level, group * n + local, value);
        }
    }
}
