// Shared DispatchRays entry points and trace helpers of every R ray library (lib_6_6). A library includes this file,
// then defines its ray generation shader(s). Hit group "RtHitGroup" = RtClosestHit + RtAnyHit (alpha test); miss 0 =
// RtMiss (keeps the payload), miss 1 = RtMissVisible (visibility rays).
//
// Root constants: P[6], P[7] hold RtSceneSrvs (RayScene.hlsli) for every R ray library; P[0..5] are the library's own.
// Alpha-tested geometry runs the any-hit shader (ARCHITECTURE 1.3-5: DispatchRays any-hit, never a RayQuery candidate
// loop); geometries whose material is not alpha-tested are built OPAQUE and skip it in hardware.
#ifndef UNX_RT_RAYSHADERS_HLSLI
#define UNX_RT_RAYSHADERS_HLSLI
#include "RayTracing/RayScene.hlsli"

RtSceneSrvs rtScene() { return rtSceneSrvs(P[6], P[7]); }

[shader("closesthit")]
void RtClosestHit(inout RtHit p, in BuiltInTriangleIntersectionAttributes a)
{
    p.t = RayTCurrent();
    p.instance = InstanceID();
    p.geometry = GeometryIndex();
    p.primitive = PrimitiveIndex();
    p.barycentrics = a.barycentrics;
    p.frontFace = HitKind() == HIT_KIND_TRIANGLE_FRONT_FACE ? 1u : 0u;
}

[shader("anyhit")]
void RtAnyHit(inout RtHit p, in BuiltInTriangleIntersectionAttributes a)
{
    if (!rtAlphaOpaque(rtScene(), InstanceID(), GeometryIndex(), PrimitiveIndex(), a.barycentrics)) IgnoreHit();
}

[shader("miss")]
void RtMiss(inout RtHit p)
{
}

[shader("miss")]
void RtMissVisible(inout RtHit p)
{
    p.t = -1;
}

// Closest hit over both TLASes (2.12: the dynamic TLAS is small; the static one is traced with TMax clipped to the
// dynamic hit). Returns t < 0 on a miss.
RtHit rtTraceClosest(RtSceneSrvs s, RayDesc ray, uint rayFlags, uint mask)
{
    RtHit h = rtMiss();
    RaytracingAccelerationStructure dynamicTlas = ResourceDescriptorHeap[s.tlasDynamic];
    TraceRay(dynamicTlas, rayFlags, mask, 0, 0, 0, ray, h);
    if (h.t >= 0) ray.TMax = h.t;
    RaytracingAccelerationStructure staticTlas = ResourceDescriptorHeap[s.tlasStatic];
    TraceRay(staticTlas, rayFlags, mask, 0, 0, 0, ray, h);
    return h;
}

// True when nothing blocks the segment (any hit ends the search; the closest-hit shader is skipped).
bool rtVisible(RtSceneSrvs s, RayDesc ray, uint mask, uint extraFlags = RAY_FLAG_NONE)
{
    const uint flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | extraFlags;
    RtHit h = rtMiss();
    h.t = 1;
    RaytracingAccelerationStructure staticTlas = ResourceDescriptorHeap[s.tlasStatic];
    TraceRay(staticTlas, flags, mask, 0, 0, 1, ray, h);
    if (h.t >= 0) return false;
    h.t = 1;
    RaytracingAccelerationStructure dynamicTlas = ResourceDescriptorHeap[s.tlasDynamic];
    TraceRay(dynamicTlas, flags, mask, 0, 0, 1, ray, h);
    return h.t < 0;
}

#endif
