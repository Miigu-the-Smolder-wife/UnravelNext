// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// P[0]: queue UAV, indirect argument UAV, descriptor stride, Width byte offset.
// P[1]: number of chunks, maximum rays per chunk, record capacity, reserved.
#include "Bindless.hlsli"
[numthreads(64, 1, 1)]
void main(uint chunk : SV_DispatchThreadID)
{
#if MODE == 0
    RWByteAddressBuffer queue = ResourceDescriptorHeap[P[0].x];
    if (chunk == 0) queue.Store4(0, uint4(0, 0, 0, 0));
#else
    ByteAddressBuffer queue = ResourceDescriptorHeap[P[0].x];
    if (chunk >= P[1].x) return;
    const uint count = min(queue.Load(0), P[1].z), first = chunk * P[1].y;
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    args.Store3(chunk * P[0].z + P[0].w, uint3(first < count ? min(count - first, P[1].y) : 0u, 1, 1));
#endif
}
