// unx-kernel: cs_6_6 main
// Fluid surface: clear the block table and the counters (FluidSurface.hlsli).
#include "FluidSurface.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    if (i < fsTableSize()) table.Store(4 * i, 0);
    if (i < 16) counters.Store(4 * i, 0);
}
