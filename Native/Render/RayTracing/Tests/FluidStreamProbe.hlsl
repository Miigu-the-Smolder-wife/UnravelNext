// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const uint side = P[0].z, facePixels = side * side;
    if (i >= 6 * facePixels) return;
    const uint face = i / facePixels, at = i % facePixels, axis = face / 2;
    const float3 lo = asfloat(P[1].xyz), hi = asfloat(P[2].xyz);
    float3 uv = 0.5;
    uv[(axis + 1) % 3] = (float(at % side) + 0.231) / side;
    uv[(axis + 2) % 3] = (float(at / side) + 0.419) / side;
    uv[axis] = (face & 1u) ? 1.1 : -0.1;
    RayDesc ray;
    ray.Origin = lerp(lo, hi, uv); ray.Direction = 0;
    ray.Direction[axis] = (face & 1u) ? -1 : 1;
    ray.TMin = 0; ray.TMax = 10;
    RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[P[0].x];
    RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(tlas, RAY_FLAG_NONE, 8, ray);
    while (q.Proceed()) {}
    uint4 result = uint4(asuint(-1.0), 0xFFFFFFFFu, 0xFFFFFFFFu, 0);
    float2 bary = 0;
    if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        result = uint4(asuint(q.CommittedRayT()), q.CommittedPrimitiveIndex(), q.CommittedInstanceID(), q.CommittedTriangleFrontFace() ? 1u : 0u);
        bary = q.CommittedTriangleBarycentrics();
    }
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].y];
    output.Store4(i * 24, result); output.Store2(i * 24 + 16, asuint(bary));
}
