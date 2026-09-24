// unx-kernel: cs_6_6 main
// Indirect arguments of ShadowPenumbra.hlsl: one 64-thread group per 64 listed pixels.
// P[0].x list SRV (raw), P[0].y args UAV (raw, 3 uints)
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    args.Store3(0, uint3((list.Load(0) + 63) / 64, 1, 1));
}
