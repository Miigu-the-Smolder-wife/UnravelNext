// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// P0 = { queue UAV, args UAV, 0, 0 }.
#include "Bindless.hlsli"
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer queue = ResourceDescriptorHeap[P[0].x];
#if MODE == 0
    queue.Store4(0, uint4(0, 0, 0, 0));
#else
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    const uint groups = (queue.Load(0) + 63) / 64;
    args.Store4(0, uint4(min(groups, 65535u), max((groups + 65534u) / 65535u, 1u), 1, 0));
#endif
}
