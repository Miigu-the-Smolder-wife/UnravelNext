// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const uint side = P[0].z, perStream = side * side;
    if (i >= perStream * max(1u, P[0].w)) return;
    const uint s = i / perStream, at = i % perStream;
    RayDesc ray;
    ray.Origin = float3(float(s) * 3 + 2.0 * (float(at % side) + 0.231) / side - 1, 2, 2.0 * (float(at / side) + 0.419) / side - 1);
    ray.Direction = float3(0, -1, 0); ray.TMin = 0; ray.TMax = 10;
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[P[0].x];
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 8, ray);
    while (q.Proceed()) {}
    const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].y];
    uint4 result = uint4(asuint(-1.0), 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    float2 bary = 0;
    if (hit)
    {
        result = uint4(asuint(q.CommittedRayT()), q.CommittedPrimitiveIndex(), q.CommittedInstanceID(), q.CommittedTriangleFrontFace() ? 1u : 0u);
        bary = q.CommittedTriangleBarycentrics();
    }
    output.Store4(i * 24, result);
    output.Store2(i * 24 + 16, asuint(bary));
}
