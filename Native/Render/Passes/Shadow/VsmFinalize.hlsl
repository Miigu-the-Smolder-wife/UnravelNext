// unx-kernel: cs_6_6 main
// Indirect dispatch arguments for the per-dirty-page passes (one group per page) and the free-page count.
// P[0].x dirty list SRV (raw), P[0].y indirect args UAV (raw: 3 uints), P[0].z free list SRV (raw), P[0].w stats UAV
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    ByteAddressBuffer dirty = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer freeList = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].w];
    args.Store3(0, uint3(dirty.Load(0), 1, 1));
    stats.Store(16, freeList.Load(0));
}
