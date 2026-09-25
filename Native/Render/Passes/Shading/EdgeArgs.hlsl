// unx-kernel: cs_6_6 main
// Edge composite dispatch arguments from the edge pixel count: x = ceil(count / 64) (Edge.hlsli edgeAppendPixel layout).
// The count is bounded by the list's capacity (the view's pixels), which it cannot exceed by construction: a clobbered
// count then costs at most one full-view composite instead of an unbounded dispatch.
// P[0] = { edge args UAV (raw), edge pixel list UAV (raw): the count goes to entry 0 for the composite, capacity }
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].y];
    const uint count = min(args.Load(12), P[0].z);
    args.Store(0, (count + 63) / 64);
    list.Store(0, count);
}
