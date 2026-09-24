// unx-kernel: cs_6_6 main
// Highest caster of each rendered page (the visibility pass bounds its blocker search with it). One group per dirty page.
// P[0].x dirty list SRV (raw), P[0].y pool SRV (Texture2D<uint>), P[0].z page metadata UAV, P[0].w VSM constants SRV,
// P[1].x constants offset
#include "Passes/Shadow/VsmCommon.hlsli"

groupshared uint g_max;

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer dirty = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint> pool = ResourceDescriptorHeap[P[0].y];
    const VsmConstants c = vsmLoadConstants(P[0].w, P[1].x);
    const uint phys = dirty.Load(8 + group.x * 8 + 4);
    const uint2 base = vsmPhysBase(c, phys);
    if (lane == 0) g_max = VSM_EMPTY;
    GroupMemoryBarrierWithGroupSync();
    uint m = VSM_EMPTY;
    [unroll] for (uint i = 0; i < VSM_PAGE * VSM_PAGE / 256; ++i)
    {
        const uint t = i * 256 + lane;
        m = max(m, pool.Load(int3(base + uint2(t % VSM_PAGE, t / VSM_PAGE), 0)));
    }
    m = WaveActiveMax(m);
    if (WaveIsFirstLane()) InterlockedMax(g_max, m);
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
    {
        RWStructuredBuffer<uint4> meta = ResourceDescriptorHeap[P[0].z];
        meta[phys].w = g_max;
    }
}
