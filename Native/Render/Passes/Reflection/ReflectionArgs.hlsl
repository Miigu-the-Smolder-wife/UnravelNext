// unx-kernel: cs_6_6 main
// Writes this frame's job count into the indirect DispatchRays descriptions (Width; Height = Depth = 1) and resets the
// rays buffer's header (P[1] = { rays UAV, capacity, ray layers UAV, job layers UAV }, P[2].x = the hit shading's flags, P[2].y = the GI
// hit accumulator pool's SRV: ReflectionRay.hlsli).
// P[0] = { arguments UAV (raw: uint job counter at 0, descriptions from byte 16), description count, stride bytes,
//          Width offset in a description }
// Bands (ReflectionRay.hlsli REFL_BAND): the arguments are held once per band (P[3].x bytes each); band b's
// descriptions get the jobs of [b x P[3].y, (b + 1) x P[3].y) - the last of the P[2].w bands the rest.
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].x];
    const uint jobs = args.Load(0);
    const uint bands = max(P[2].w, 1u);
    for (uint b = 0; b < bands; ++b)
    {
        const uint first = min(b * P[3].y, jobs);
        const uint width = b + 1 == bands ? jobs - first : min(jobs - first, P[3].y);
        for (uint i = 0; i < P[0].y; ++i) args.Store3(b * P[3].x + 16 + i * P[0].z + P[0].w, uint3(width, 1, 1));
    }
    // The rays buffer's header for this frame (ReflectionRay.hlsli): no slots yet, its capacity, no shadow rays, the jobs;
    // no penumbra hits.
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[1].x];
    rays.Store4(0, uint4(0, P[1].y, 0, jobs));
    rays.Store4(16, uint4(0, P[1].z, P[1].w, P[2].x));  // the layer buffers' UAVs (reflection.layers; UNX_NONE: off), hit flags
    rays.Store4(32, uint4(P[2].y, P[2].z, 0, 0));       // the GI hit accumulator pool's SRV, the surface cache's UAV (UNX_NONE: none)
}
