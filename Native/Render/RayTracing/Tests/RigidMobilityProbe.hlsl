// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
#include "Passes/Common/Deformation.hlsli"
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= 2 * P[0].w) return;
    const uint object = i % P[0].w, bank = i / P[0].w;
    RayDesc ray;
    ray.Origin = float3(object * 2.0 + bank * 0.75, 2, 0) - asfloat(P[1].xyz);
    ray.Direction = float3(0, -1, 0); ray.TMin = 0; ray.TMax = 4;
    uint id = 0xFFFFFFFFu; float distance = -1;
    for (uint set = 0; set < 2; ++set)
    {
        RaytracingAccelerationStructure tlas = ResourceDescriptorHeap[set ? P[0].y : P[0].x];
        RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(tlas, RAY_FLAG_NONE, 255, ray);
        while (q.Proceed()) {}
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && (distance < 0 || q.CommittedRayT() < distance))
        { id = q.CommittedInstanceID(); distance = q.CommittedRayT(); }
    }
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].z];
    const GpuInstance inst = loadInstance(object);
    const bool referenceStill = inst.morph == UNX_NONE && (inst.flags & (INSTANCE_SKINNED | INSTANCE_WIND)) == 0 &&
        all(inst.objectToWorld[0] == inst.prevObjectToWorld[0]) && all(inst.objectToWorld[1] == inst.prevObjectToWorld[1]) &&
        all(inst.objectToWorld[2] == inst.prevObjectToWorld[2]);
    if (deformInstanceStillAt(object) != referenceStill || deformInstanceStill(inst) != referenceStill) id = 0xFFFFFFF0u;
    GpuInstance gpuWritten = inst;
    gpuWritten.morphPad = 0x5354494Cu; // a template's old proof is not authoritative in the GPU-owned range
    gpuWritten.prevObjectToWorld[0].w = gpuWritten.objectToWorld[0].w + 1;
    if (instanceTransformStill(g_instanceCount, gpuWritten)) id = 0xFFFFFFF0u;
    output.Store2(i * 8, uint2(id, asuint(distance)));
}
