// unx-kernel: cs_6_6 main
// The refraction list's indirect DispatchRays size (FrameServices::traceRefractions): Width = min(the list's job count,
// the caller's capacity), Height = Depth = 1, into the description copied from R's template (shader tables known on the
// CPU). An empty list dispatches no ray generation thread.
// P[0] = { arguments UAV (raw), jobs SRV (raw: count at 0), capacity, Width offset in the description }
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer jobs = ResourceDescriptorHeap[P[0].y];
    args.Store3(P[0].w, uint3(min(jobs.Load(0), P[0].z), 1, 1));
}
