// unx-kernel: cs_6_6 main
// Ripples: a calm surface (state and source accumulation zero), once when the module starts.
#include "Ripple.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= RIPPLE_N * RIPPLE_N) return;
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    state.Store2(8 * i, uint2(0, 0));
    accum.Store2(8 * i, uint2(0, 0));
}
