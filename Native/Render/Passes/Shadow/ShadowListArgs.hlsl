// unx-kernel: cs_6_6 main
// Indirect arguments of ShadowPenumbra.hlsl: one 64-thread group per 64 listed pixels (either list).
// P[0].x list SRV (raw), P[0].y args UAV (raw, 3 uints), P[0].z the filter list UAV to reset before STAGE=0 appends to
// it (raw; 0xFFFFFFFF: none)
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    args.Store3(0, uint3((list.Load(0) + 63) / 64, 1, 1));
    if (P[0].z != 0xFFFFFFFFu)
    {
        RWByteAddressBuffer next = ResourceDescriptorHeap[P[0].z];
        next.Store(0, 0u);
    }
}
