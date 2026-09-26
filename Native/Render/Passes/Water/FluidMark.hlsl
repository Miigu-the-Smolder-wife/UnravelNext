// unx-kernel: cs_6_6 main
// Fluid surface: mark the blocks a particle reaches: its 3^3 splat nodes, the marching cubes cells that use them and
// the gradient neighbours of their corners (nodes [base - 2, base + 4]).
#include "FluidSurface.hlsli"

[numthreads(256, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= fsCount()) return;
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[4].x];
    int3 base = (int3)floor(fsParticle(i) - 0.5);
    int3 top = (int3)fsBlocks() - 1;
    int3 lo = clamp((base - 2) >> 3, 0, top), hi = clamp((base + 4) >> 3, 0, top);
    for (int z = lo.z; z <= hi.z; ++z) for (int y = lo.y; y <= hi.y; ++y) for (int x = lo.x; x <= hi.x; ++x) table.Store(4 * fsBlockIndex(int3(x, y, z)), 1);
}
