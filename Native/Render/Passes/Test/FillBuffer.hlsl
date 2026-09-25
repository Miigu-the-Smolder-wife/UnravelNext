// unx-kernel: cs_6_6 main
// Render-graph test kernel: writes word i = i ^ seed into a raw UAV (aliasing tests read it back).
// P[0].x RWByteAddressBuffer UAV, P[0].y words, P[0].z seed
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    b.Store(4 * i, i ^ P[0].z);
}
