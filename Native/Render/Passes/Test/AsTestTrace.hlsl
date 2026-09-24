// unx-kernel: cs_6_6 main
// Render-graph acceleration-structure test: one inline ray from the origin along +z against the TLAS built in the same
// frame; stores the hit distance (or -1) so the test sees whether the builds and barriers were ordered.
// P[0].x TLAS SRV, P[0].y result UAV (raw)
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[0].y];
    RayDesc ray;
    ray.Origin = float3(0, 0, 0);
    ray.Direction = float3(0, 0, 1);
    ray.TMin = 0;
    ray.TMax = 100;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed();
    const float t = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? q.CommittedRayT() : -1.0;
    result.Store(0, asuint(t));
}
