// unx-kernel: cs_6_6 main
// Resets the per-class tile dispatch arguments of the material resolve to (0, 1, 1).
// P[0].x tile args UAV (raw), P[0].y class count
#include "Bindless.hlsli"

[numthreads(32, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].x];
    args.Store3(12 * i, uint3(0, 1, 1));
}
