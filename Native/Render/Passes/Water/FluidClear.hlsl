// unx-kernel: cs_6_6 main
// Fluid surface: clear the block table and the counters (FluidSurface.hlsli).
#include "FluidSurface.hlsli"
#include "WaterLinear.hlsli"

[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint i = waterLinear(group, thread, 256);
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    if (i < fsTableSize()) table.Store(4 * i, 0);
    if (i < 16) counters.Store(4 * i, 0);
}
