// unx-kernel: cs_6_6 main
// The cull pass appends 16-thread work blocks, preserving original ray IDs.
// P0={work-list SRV, arguments UAV, record capacity, first ray-description offset}
// P1={description stride, Width offset, threads per chunk, chunk count}
#include "Bindless.hlsli"
[numthreads(1, 1, 1)]
void main()
{
    ByteAddressBuffer work = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    const uint threads = min(work.Load(0), P[0].z) * 16;
    for (uint chunk = 0; chunk < P[1].w; ++chunk)
    {
        const uint first = chunk * P[1].z;
        const uint width = threads > first ? min(threads - first, P[1].z) : 0u;
        args.Store3(P[0].w + chunk * P[1].x + P[1].y, width ? uint3(width, 1, 1) : uint3(0, 0, 0));
    }
}
