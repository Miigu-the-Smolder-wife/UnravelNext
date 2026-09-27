// unx-kernel: cs_6_6 main
// Writes this frame's job count into the indirect DispatchRays descriptions (Width; Height = Depth = 1) and resets the
// rays buffer's header (P[1] = { rays UAV, capacity }).
// P[0] = { arguments UAV (raw: uint job counter at 0, descriptions from byte 16), description count, stride bytes,
//          Width offset in a description }
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].x];
    const uint jobs = args.Load(0);
    for (uint i = 0; i < P[0].y; ++i) args.Store3(16 + i * P[0].z + P[0].w, uint3(jobs, 1, 1));
    // The rays buffer's header for this frame (ReflectionRay.hlsli): no slots yet, its capacity, no shadow rays, the jobs;
    // no penumbra hits.
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[1].x];
    rays.Store4(0, uint4(0, P[1].y, 0, jobs));
    rays.Store4(16, uint4(0, 0, 0, 0));
}
