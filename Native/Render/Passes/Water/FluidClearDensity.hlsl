// unx-kernel: cs_6_6 main
// Fluid surface: clear the density of the active blocks (indirect, argument 0).
#include "FluidSurface.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[4].w];
    if (i >= min(counters.Load(4 * FS_COUNTER_ACTIVE), fsMaxBlocks()) * FS_BLOCK_NODES) return;
    RWByteAddressBuffer density = ResourceDescriptorHeap[P[4].z];
    density.Store4(16 * i, uint4(0, 0, 0, 0));
}
