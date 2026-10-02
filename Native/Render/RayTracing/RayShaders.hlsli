// Shared DispatchRays entry points and trace helpers of every R ray library (lib_6_6). A library includes this file,
// then defines its ray generation shader(s). Hit group "RtHitGroup" = RtClosestHit + RtAnyHit (alpha test); miss 0 =
// RtMiss (keeps the payload), miss 1 = RtMissVisible (visibility rays).
//
// Root constants: P[6], P[7] hold RtSceneSrvs (RayScene.hlsli) for every R ray library; P[0..5] are the library's own.
// Alpha-tested geometry runs the any-hit shader (ARCHITECTURE 1.3-5: DispatchRays any-hit, never a RayQuery candidate
// loop); geometries whose material is not alpha-tested are built OPAQUE and skip it in hardware.
// Glass / Water geometry is non-opaque too (raytracing.see_through_translucent; RayScene.hlsli): the any-hit shader
// ignores it for a see-through ray - the trace helpers below mark such a ray in the payload's pad word from its mask -
// and accepts it for the others (reflection and refraction rays).
//   RT_NO_SEE_THROUGH (defined before this file): the library's any-hit shader is the alpha test alone and its rays
//   carry no mark - for a library none of whose rays is see-through (every mask has RT_MASK_REFLECTION).
//   RT_NO_FAR_FIELD (defined before this file): the procedural hit group holds the area lights alone - for a library
//   none of whose rays asks for RT_MASK_FAR.
//   (Neither is defined by a library now: ReflectionTraceInline set both while it stood at the DXIL size limit, and
//   holds the full shaders since its lookups stand once in it - 179 KB of 200.)
//   RT_SHADOW_TRANSMITTANCE (defined before this file): rtShadowTransmittance gathers what the Glass on a shadow ray
//   takes (rtGlassOpticalDepth) in the any-hit shader - no closest-hit shading, no further ray. Without it the function
//   is rtVisible's answer as 0 or 1.
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
#ifdef RT_NO_SEE_THROUGH
    if (!rtAlphaOpaque(rtScene(), InstanceID(), GeometryIndex(), PrimitiveIndex(), a.barycentrics)) IgnoreHit();
#else
    const RtSceneSrvs s = rtScene();
    RtHit h = rtMiss();
    h.instance = InstanceID();
    h.geometry = GeometryIndex();
    h.primitive = PrimitiveIndex();
    h.barycentrics = a.barycentrics;
    GpuInstance inst;
    GpuMesh mesh;
    RtGeometry g;
    const GpuMaterial m = loadMaterial(rtMaterial(s, h, inst, mesh, g));
    if ((p.pad & RT_RAY_SEE_THROUGH) != 0 && rtSeeThroughMaterial(m))
    {
#ifdef RT_SHADOW_TRANSMITTANCE
        // (the payload kept across IgnoreHit: what the pane takes from the light on this ray. Glass alone - a water
        // surface passes the ray whole: the view's own sun under water is not this function's to decide)
        if ((p.pad & RT_RAY_TRANSMITTANCE) != 0 && (m.classFlags & 0xFFu) == MATERIAL_GLASS)
        {
            const float cosI = rtCandidateCos(s, rtLoadInstance(s, h.instance), g, mesh, h.primitive, ObjectRayDirection());
            float3 tint = m.baseColor;
            if (m.baseColorTexture != UNX_NONE)
            {
                const RtTriangle tri = rtTriangle(s, g, h.primitive);
                tint *= materialBaseColorLevel(m, rtUv(mesh, tri.meshVertex, h.barycentrics), 0).rgb;
            }
            const float3 depth = rtGlassOpticalDepth(m, saturate(tint), cosI, HitKind() == HIT_KIND_TRIANGLE_BACK_FACE, RayTCurrent());
            p.instance = asuint(asfloat(p.instance) + depth.r);
            p.geometry = asuint(asfloat(p.geometry) + depth.g);
            p.primitive = asuint(asfloat(p.primitive) + depth.b);
        }
#endif
        IgnoreHit();
    }
    if (m.alphaCutoff > 0 && m.baseColorTexture != UNX_NONE)
    {
        // The raster's alpha test at the texture's level 0 (rtAlphaOpaque)
        const RtTriangle tri = rtTriangle(s, g, h.primitive);
        if (materialBaseColorLevel(m, rtUv(mesh, tri.meshVertex, h.barycentrics), 0).a < m.alphaCutoff) IgnoreHit();
    }
#endif
}

// ---- analytic area lights (INTERFACES 8.2: rect and disk one-sided along +forward, up = forward x right; sphere radius
// size.x; tube = capsule along right, length size.x, radius size.y). The emitter BLAS holds one AABB per scene light
// (inactive for point and spot lights), so PrimitiveIndex() is the light's index.
struct RtEmitterAttributes
{
    float2 unused;
};

float rtCapsuleT(float3 o, float3 d, float3 a, float3 b, float r)
{
    // Ray-capsule, nearest entry (Quilez); -1 when missed.
    const float3 ba = b - a, oa = o - a;
    const float baba = dot(ba, ba), bard = dot(ba, d), baoa = dot(ba, oa), rdoa = dot(d, oa), oaoa = dot(oa, oa);
    const float A = baba - bard * bard, B = baba * rdoa - baoa * bard, C = baba * oaoa - baoa * baoa - r * r * baba;
    float h = B * B - A * C;
    if (h >= 0 && A > 1e-12)
    {
        const float t = (-B - sqrt(h)) / A;
        const float y = baoa + t * bard;
        if (y > 0 && y < baba) return t;
        const float3 oc = y <= 0 ? oa : o - b;
        const float bb = dot(d, oc), cc = dot(oc, oc) - r * r;
        h = bb * bb - cc;
        if (h > 0) return -bb - sqrt(h);
    }
    else
    {
        // Parallel to the axis: the end caps.
        [unroll] for (uint k = 0; k < 2; ++k)
        {
            const float3 oc = o - (k == 0 ? a : b);
            const float bb = dot(d, oc), cc = dot(oc, oc) - r * r;
            const float hh = bb * bb - cc;
            if (hh > 0 && -bb - sqrt(hh) > 0) return -bb - sqrt(hh);
        }
    }
    return -1;
}

#ifndef RT_NO_FAR_FIELD
// The far field's proxies (RayScene.hlsli) share the area lights' hit group: one procedural instance each, told apart by
// the instance's id. A proxy of a group that is in the near structure does not exist for the ray. The others: the ray's
// entry into the box, when it lies in the ray's interval - a ray that starts inside a box leaves it freely (a ray shot
// from a hit on a proxy, or from a surface among culled small things, is not stopped by the box it stands in) - and
// only for the share of the rays the proxy's opacity gives: which rays is a hash of the proxy and the ray (its origin
// and direction, quantised), so the mean over rays is the opacity (the members' projected area over the box's,
// RayScene.cpp) and a ray shot again from the same place keeps its answer.
void rtFarIntersect()
{
    const RtSceneSrvs s = rtScene();
    if (s.pad == 0xFFFFFFFFu) return;
    ByteAddressBuffer lightHeader = ResourceDescriptorHeap[s.pad];
    const uint headerSrv = lightHeader.Load(40);
    if (headerSrv == 0xFFFFFFFFu) return;
    ByteAddressBuffer header = ResourceDescriptorHeap[headerSrv];
    const uint4 head = header.Load4(0);
    ByteAddressBuffer records = ResourceDescriptorHeap[head.w];
    const uint at = PrimitiveIndex() * RT_FAR_RECORD_BYTES;
    const float4 centreRadius = asfloat(records.Load4(at + 24));
    const float3 toAnchor = centreRadius.xyz - asfloat(head.xyz);
    if (dot(toAnchor, toAnchor) <= centreRadius.w * centreRadius.w) return;  // (near: its instances are in the TLAS)
    const float3 lo = asfloat(records.Load3(at)), hi = asfloat(records.Load3(at + 12));
    const float3 o = ObjectRayOrigin(), d = ObjectRayDirection();
    const float3 inv = float3(abs(d.x) > 1e-12 ? 1 / d.x : 1e12, abs(d.y) > 1e-12 ? 1 / d.y : 1e12, abs(d.z) > 1e-12 ? 1 / d.z : 1e12);
    const float3 t0 = (lo - o) * inv, t1 = (hi - o) * inv;
    const float3 tNear = min(t0, t1), tFar = max(t0, t1);
    const float tEnter = max(tNear.x, max(tNear.y, tNear.z)), tExit = min(tFar.x, min(tFar.y, tFar.z));
    if (!(tEnter <= tExit) || !(tEnter > RayTMin()) || !(tEnter <= RayTCurrent())) return;
    // (the ray: its origin to 12.5 cm and its direction to 1 / 1024 - a card texel's sun ray keeps its answer while the
    // camera's distance moves its bias, and while the sun creeps)
    const int3 qo = int3(floor(o * 8.0)), qd = int3(floor(d * 1024.0));
    uint hash = PrimitiveIndex() * 0x9E3779B1u + (uint)qo.x * 0x85EBCA77u + (uint)qo.y * 0xC2B2AE3Du + (uint)qo.z * 0x27D4EB2Fu;
    hash = (hash ^ (hash >> 15)) * 0x2C1B3C6Du + (uint)qd.x * 0x165667B1u + (uint)qd.y * 0xD3A2646Cu + (uint)qd.z * 0xFD7046C5u;
    hash = (hash ^ (hash >> 13)) * 0x297A2D39u;
    hash ^= hash >> 16;
    if ((hash >> 8) * (1.0 / 16777216.0) >= asfloat(records.Load(at + 40))) return;
    const uint axis = tNear.x >= tNear.y && tNear.x >= tNear.z ? 0u : tNear.y >= tNear.z ? 1u : 2u;
    RtEmitterAttributes a;
    a.unused = float2((float)(axis + (d[axis] > 0 ? 3u : 0u)), 0);
    ReportHit(tEnter, 0, a);
}
#endif

[shader("intersection")]
void RtEmitterIntersect()
{
#ifndef RT_NO_FAR_FIELD
    if (InstanceID() == RT_INSTANCE_FAR)
    {
        rtFarIntersect();
        return;
    }
#endif
    StructuredBuffer<GpuLight> lights = ResourceDescriptorHeap[g_lights];
    const GpuLight l = lights[PrimitiveIndex()];
    const uint type = l.typeFlags & 0xFFu;
    const float3 o = WorldRayOrigin(), d = WorldRayDirection();
    float t = -1;
    if (type == 2 || type == 3)
    {
        const float dn = dot(d, l.forward);
        if (dn < 0)  // emitting side only
        {
            const float tp = dot(l.position - o, l.forward) / dn;
            const float3 q = o + d * tp - l.position;
            const float3 up = cross(l.forward, l.right);
            const bool inside = type == 2 ? abs(dot(q, l.right)) <= 0.5 * l.size.x && abs(dot(q, up)) <= 0.5 * l.size.y : dot(q, q) <= l.size.x * l.size.x;
            if (inside) t = tp;
        }
    }
    else if (type == 4)
    {
        const float3 oc = o - l.position;
        const float b = dot(d, oc), c = dot(oc, oc) - l.size.x * l.size.x, h = b * b - c;
        if (h >= 0) t = -b - sqrt(h);
    }
    else if (type == 5)
    {
        const float3 half = l.right * (0.5 * l.size.x);
        t = rtCapsuleT(o, d, l.position - half, l.position + half, l.size.y);
    }
    if (t >= RayTMin() && t <= RayTCurrent())
    {
        RtEmitterAttributes a;
        a.unused = 0;
        ReportHit(t, 0, a);
    }
}

[shader("closesthit")]
void RtEmitterClosestHit(inout RtHit p, in RtEmitterAttributes a)
{
    p.t = RayTCurrent();
#ifdef RT_NO_FAR_FIELD
    p.instance = RT_INSTANCE_EMITTER;
    p.barycentrics = 0;
#else
    p.instance = InstanceID();   // RT_INSTANCE_EMITTER, or RT_INSTANCE_FAR: the far field's proxy (RayScene.hlsli)
    p.barycentrics = a.unused;   // (a proxy: x = the face the ray entered by; a light: 0)
#endif
    p.geometry = 0;
    p.primitive = PrimitiveIndex();
    p.frontFace = 1;
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
#ifndef RT_NO_SEE_THROUGH
    h.pad = (mask & RT_MASK_REFLECTION) == 0 ? RT_RAY_SEE_THROUGH : 0u;  // (the any-hit shader's: RayScene.hlsli)
#endif
    RaytracingAccelerationStructure dynamicTlas = ResourceDescriptorHeap[s.tlasDynamic];
    TraceRay(dynamicTlas, rayFlags, mask, 0, 0, 0, ray, h);
    if (h.t >= 0) ray.TMax = h.t;
    RaytracingAccelerationStructure staticTlas = ResourceDescriptorHeap[s.tlasStatic];
    TraceRay(staticTlas, rayFlags, mask, 0, 0, 0, ray, h);
    h.pad = 0;
    return h;
}

// True when nothing blocks the segment (any hit ends the search; the closest-hit shader is skipped).
bool rtVisible(RtSceneSrvs s, RayDesc ray, uint mask, uint extraFlags = RAY_FLAG_NONE)
{
    const uint flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | extraFlags;
    RtHit h = rtMiss();
    h.t = 1;
#ifndef RT_NO_SEE_THROUGH
    h.pad = (mask & RT_MASK_REFLECTION) == 0 ? RT_RAY_SEE_THROUGH : 0u;
#endif
    RaytracingAccelerationStructure staticTlas = ResourceDescriptorHeap[s.tlasStatic];
    TraceRay(staticTlas, flags, mask, 0, 0, 1, ray, h);
    if (h.t >= 0) return false;
    h.t = 1;
    RaytracingAccelerationStructure dynamicTlas = ResourceDescriptorHeap[s.tlasDynamic];
    TraceRay(dynamicTlas, flags, mask, 0, 0, 1, ray, h);
    return h.t < 0;
}

// What a shadow segment lets through to its origin, per channel: 0 when a caster blocks it, else what the Glass it
// crosses leaves of the light - the panes' transmittance (their base colour x its texture) and the solid bodies'
// absorption (RayScene.hlsli rtGlassOpticalDepth), gathered by the any-hit shader in the payload as the candidates are met (no closest-hit shading,
// no second ray; the geometry is built NO_DUPLICATE_ANYHIT, so a pane counts once). mask: a shadow mask (RT_MASK_SHADOW,
// RT_MASK_HIT_SHADOW); the ray also takes RT_MASK_SHADOW_TINT - the shadow casters made of Glass alone, which plain shadow
// rays never enter.
// Without RT_SHADOW_TRANSMITTANCE (a library that did not ask for it): rtVisible's answer - Glass passes the ray whole.
// The same two TraceRay calls as rtVisible either way.
float3 rtShadowTransmittance(RtSceneSrvs s, RayDesc ray, uint mask, uint extraFlags = RAY_FLAG_NONE)
{
#if defined(RT_SHADOW_TRANSMITTANCE) && !defined(RT_NO_SEE_THROUGH)
    if ((mask & RT_MASK_REFLECTION) == 0)
    {
        const uint flags = RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | extraFlags;
        RtHit h = rtMiss();  // (instance, geometry, primitive = 0: the optical depth starts at asfloat(0) = 0)
        h.t = 1;
        h.pad = RT_RAY_SEE_THROUGH | RT_RAY_TRANSMITTANCE;
        RaytracingAccelerationStructure staticTlas = ResourceDescriptorHeap[s.tlasStatic];
        TraceRay(staticTlas, flags, mask | RT_MASK_SHADOW_TINT, 0, 0, 1, ray, h);
        if (h.t >= 0) return 0;
        h.t = 1;
        RaytracingAccelerationStructure dynamicTlas = ResourceDescriptorHeap[s.tlasDynamic];
        TraceRay(dynamicTlas, flags, mask | RT_MASK_SHADOW_TINT, 0, 0, 1, ray, h);
        if (h.t >= 0) return 0;
        return exp(-max(float3(asfloat(h.instance), asfloat(h.geometry), asfloat(h.primitive)), 0.0));
    }
#endif
    return rtVisible(s, ray, mask, extraFlags) ? float3(1, 1, 1) : float3(0, 0, 0);
}

#endif
