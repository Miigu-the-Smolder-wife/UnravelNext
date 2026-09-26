// unx-kernel: cs_6_6 main
// Closed basins (Pool.hlsli): a calm basin (mode amplitudes, output, previous eta and source accumulation zero), once when
// the basin starts.
#include "Pool.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= POOL_Q * POOL_Q) return;
    RWByteAddressBuffer modes = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer previous = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].z];
    modes.Store2(8 * i, uint2(0, 0));
    accum.Store2(8 * i, uint2(0, 0));
    previous.Store(4 * i, 0);
    output[uint2(i % POOL_Q, i / POOL_Q)] = float4(0, 0, 0, 0);
}
