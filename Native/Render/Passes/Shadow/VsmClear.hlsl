// unx-kernel: cs_6_6 main
// Clears each dirty physical page to "no caster" before the raster. One group per dirty page (indirect).
// P[0].x dirty list SRV (raw), P[0].y pool UAV (RWTexture2D<uint>), P[0].z VSM constants SRV, P[0].w constants offset
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer dirty = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint> pool = ResourceDescriptorHeap[P[0].y];
    const VsmConstants c = vsmLoadConstants(P[0].z, P[0].w);
    const uint phys = dirty.Load(8 + group.x * 8 + 4);
    const uint2 base = vsmPhysBase(c, phys);
    [unroll] for (uint i = 0; i < VSM_PAGE * VSM_PAGE / 256; ++i)
    {
        const uint t = i * 256 + lane;
        pool[base + uint2(t % VSM_PAGE, t / VSM_PAGE)] = VSM_EMPTY;
    }
}
