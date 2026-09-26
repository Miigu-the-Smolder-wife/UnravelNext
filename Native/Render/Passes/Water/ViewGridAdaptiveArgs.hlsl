// unx-kernel: cs_6_6 main
// Water view grid, adaptive near field: the dispatch arguments of one level's pass from its list (one group per block;
// x at most 65535, y the rest), and the other list's count cleared for the blocks this pass will append.
// P[0] list UAV (raw: count at 0), argument UAV (raw: x, y, z), capacity, other list UAV
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer arguments = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer other = ResourceDescriptorHeap[P[0].w];
    const uint n = min(list.Load(0), P[0].z), x = min(n, 65535u), y = x ? (n + x - 1) / x : 0;
    arguments.Store3(0, uint3(x, y, n ? 1u : 0u));
    other.Store(0, 0u);
}
