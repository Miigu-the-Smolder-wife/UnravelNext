// unx-kernel: cs_6_6 main
// Round basins (RoundPool.hlsli): a calm basin - modes, accumulation, previous eta, the output field and the centre zero.
#include "Passes/Water/RoundPool.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer modes = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    RWByteAddressBuffer previous = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer centre = ResourceDescriptorHeap[P[4].y];
    if (i < ROUND_SAMPLES)
    {
        accum.Store2(8 * i, uint2(0, 0));
        previous.Store(4 * i, 0);
        output[uint2(i % ROUND_THETA, i / ROUND_THETA)] = float4(0, 0, 0, 0);
    }
    const uint modeTotal = roundOrder(ROUND_ORDERS - 1).x + roundOrder(ROUND_ORDERS - 1).y;
    if (i < modeTotal) modes.Store4(16 * i, uint4(0, 0, 0, 0));
    if (i == 0)
    {
        previous.Store(4 * ROUND_SAMPLES, 0);  // the centre's previous eta
        centre.Store4(0, uint4(0, 0, 0, 0));
    }
}
