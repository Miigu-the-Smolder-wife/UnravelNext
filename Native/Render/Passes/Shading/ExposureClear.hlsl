// unx-kernel: cs_6_6 main
// Clears M's exposure histogram (Exposure.cpp). P[0] = { histogram UAV (raw), bins, 0, 0 }.
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    RWByteAddressBuffer h = ResourceDescriptorHeap[P[0].x];
    h.Store(4 * i, 0u);
}
