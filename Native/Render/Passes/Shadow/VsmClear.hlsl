// unx-kernel: cs_6_6 main
// Clears each dirty physical page to "no caster" before the raster. One group per dirty page (indirect).
// P[0].x dirty list SRV (raw), P[0].y pool UAV (raw), P[0].z VSM constants CBV, P[0].w unused
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer dirty = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer pool = ResourceDescriptorHeap[P[0].y];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].z];
    const uint phys = dirty.Load(8 + group.x * 8 + 4);
    [unroll] for (uint i = 0; i < VSM_PAGE * VSM_PAGE / 1024; ++i)
        pool.Store4(((phys << 14) + (i * 256 + lane) * 4) * 4, uint4(VSM_EMPTY, VSM_EMPTY, VSM_EMPTY, VSM_EMPTY));
}
