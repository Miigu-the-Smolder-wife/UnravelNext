// unx-kernel: cs_6_6 main
// Indirect arguments of the split reflection passes (ReflectionRay.hlsli). Stage 0, after the trace: Dispatch arguments
// of the shade pass (one thread per allocated slot) and the combine pass (one thread per job). Stage 1, after the shade:
// the shadow pass's DispatchRays width (queued shadow rays).
// P[0] = { arguments UAV (raw), rays UAV (raw), stage, 0 }, P[1] = { shade args offset, combine args offset, shadow
// description Width offset, 0 }
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[0].y];
    const uint4 header = rays.Load4(0);  // allocated, capacity, shadow rays, jobs
    if (P[0].z == 0)
    {
        args.Store3(P[1].x, uint3((min(header.x, header.y) + 63) / 64, 1, 1));
        args.Store3(P[1].y, uint3((header.w + 63) / 64, 1, 1));
    }
    else
        args.Store3(P[1].z, uint3(header.z, 1, 1));
}
