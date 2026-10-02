// Hit lighting of the reflection and refraction rays on the Lumen path (reflection.lumen_only): the surface cache, as
// Unreal's default (r.Lumen.HardwareRayTracing.LightingMode 0), with the hit's own material.
//   emitter proxy   the area light's radiance (RayScene.hlsli rtEmitterRadiance) where the ray paths own its specular;
//   far proxy       (raytracing.far_field) the proxy's colour under the sun and the sky (RayTracing/HitFarField.hlsli);
//   surface         its material at the ray cone's footprint (textures, decals), its emission, and the light of the mesh
//                   cards of its instance (CardLighting.hlsli clReadCards: direct light with the sun, radiosity) through
//                   the material - diffuse albedo x E / pi and the specular albedo x E / pi (the lobe at the hit sees the
//                   cards' light as uniform); no light sample, no shadow ray; a leaf also takes the other side's
//                   card light through it (LumenHitIndirect.hlsli lhiFoliageThrough);
//   without cards   (a deforming instance: skin, wind; a texel the cards do not cover) the sun through one shadow ray
//                   to the disk's centre and, with localSample, one local-light sample with its shadow ray
//                   (HitLocalSample.hlsli: a mirror showed such surfaces unlit); the indirect light the card frame
//                   names (LumenHitIndirect.hlsli: this frame's translucency volume, else the radiance cache's
//                   irradiance probes - a skinned character in a mirror is lit by the room it stands in); past the
//                   mesh cards' end, where neither answers, the sky's light on the hit (the far field: GiSky.hlsli
//                   giFarSkyIrradiance, as the probes' rays - distant terrain in a mirror was lit by the sun alone);
//   leaking         the skylight leaking the card frame names (lumen.skylight_leaking, LumenHitIndirect.hlsli
//                   lhiSkyLeakingReflection; 0 by default);
//   back of a one-sided surface: 0 (inside closed geometry).
// hiRes (reflection.lumen_hi_res_surface; Unreal's r.Lumen.Reflections.HiResSurface): the hit reads the cards' highest
// mapped level and reports the page and level its footprint wants (CardLighting.hlsli clFeedback; feedbackCoord: the
// reader's pixel, of which one in a feedback tile reports a frame) - a mirror then shows the cards at the resolution it
// sees them at, a frame or two after it first looks. false: the resident level, no report.
// exactCounts: RayScene's exact set hit counts (UAV; UNX_NONE: none) - a hit on a skinned instance counts there, as the
// other reflection paths' hits do (RayScene picks the most-hit characters for exact refits).
// The includer is a ray library (RayShaders.hlsli) with GiSky.hlsli's sky and sun in P[1..3].
#ifndef UNX_REFLECTION_LUMEN_HIT_HLSLI
#define UNX_REFLECTION_LUMEN_HIT_HLSLI
#ifndef RT_HIT_EYE
#define RT_HIT_EYE 1  // a reflected eye shows its iris through the cornea (HitShading.hlsli rtHitMaterialSeen)
#endif
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitDecals.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "RayTracing/HitLocalSample.hlsli"
#include "Passes/GI/LumenHitIndirect.hlsli"
#include "RayTracing/HitFarField.hlsli"

struct RlHit
{
    float3 radiance;  // toward the ray's origin (nits)
    float motion;     // the hit point's displacement since the previous frame over the ray's footprint there (0: still)
    bool surface;     // a lit surface (not an emitter proxy, not the back of a one-sided surface)
};

RlHit rlShadeHit(RtSceneSrvs scene, RtHit hit, float3 origin, float3 direction, float coneWidth, float coneSpread, uint cardFrame, uint exactCounts,
                 bool localSample = false, bool hiRes = false, uint2 feedbackCoord = uint2(0, 0))
{
    RlHit o;
    o.radiance = 0;
    o.motion = 0;
    o.surface = false;
    if (hit.instance == RT_INSTANCE_FAR)
    {
        // a proxy of the far field (raytracing.far_field; RayTracing/HitFarField.hlsli): no mesh records behind it
        o.radiance = rtFarRadiance(scene, hit, origin, direction, true);
        o.surface = true;
        return o;
    }
    if (hit.instance == RT_INSTANCE_EMITTER)
    {
        o.radiance = rtEmitterCounts(scene.pad, hit.primitive) ? rtEmitterRadiance(hit.primitive, origin, origin + direction * hit.t) : float3(0, 0, 0);
        return o;
    }
    GpuInstance inst;
    GpuMesh mesh;
    RtInstance ri;
    RtTriangle tri;
    const RtSurface s = rtSurfaceParts(scene, hit, origin, direction, inst, mesh, ri, tri);
    const float footprint = coneWidth + hit.t * coneSpread;
    if (exactCounts != UNX_NONE)
    {
        const uint deformedIndex = ri.flags >> 8;
        if ((ri.flags & RT_INSTANCE_DEFORMED) != 0 && deformedIndex != 0xFFFFFFu)
        {
            RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[exactCounts];
            InterlockedAdd(counts[deformedIndex], 1u);
        }
    }
    {
        // the hit point's motion: a deformed instance's stored vertex motion, else the instance's transform
        const bool deformed = (ri.flags & RT_INSTANCE_DEFORMED) != 0;
        const float3 w = rtBary(hit.barycentrics);
        float3 delta = 0;
        if (deformed)
        {
            StructuredBuffer<RtDeformedVertex> d = ResourceDescriptorHeap[scene.deformed];
            const uint2 a = d[ri.vertexBase + tri.poolIndex.x].motion, b = d[ri.vertexBase + tri.poolIndex.y].motion, c = d[ri.vertexBase + tri.poolIndex.z].motion;
            delta = float3(f16tof32(a.x), f16tof32(a.x >> 16), f16tof32(a.y)) * w.x + float3(f16tof32(b.x), f16tof32(b.x >> 16), f16tof32(b.y)) * w.y +
                    float3(f16tof32(c.x), f16tof32(c.x >> 16), f16tof32(c.y)) * w.z;
        }
        else if (any(inst.objectToWorld[0] != inst.prevObjectToWorld[0]) || any(inst.objectToWorld[1] != inst.prevObjectToWorld[1]) ||
                 any(inst.objectToWorld[2] != inst.prevObjectToWorld[2]))
        {
            const float3 p = loadVertex(mesh, tri.meshVertex.x).position * w.x + loadVertex(mesh, tri.meshVertex.y).position * w.y +
                             loadVertex(mesh, tri.meshVertex.z).position * w.z;
            delta = transformPoint(inst.objectToWorld, p) - transformPoint(inst.prevObjectToWorld, p);
        }
        o.motion = length(delta) / max(footprint, 1e-6);
    }
    GpuMaterial m = loadMaterial(s.material);
    m = rtHitMaterialSeen(m, s, direction, footprint, dot(s.normal, direction));
    rtHitDecals(scene, s, footprint, m);
    if (!s.frontFace && (m.classFlags & MATERIAL_TWO_SIDED) == 0) return o;
    o.surface = true;
    g_rtHitCone = 0.5 * coneSpread;
    RtHitLighting L = (RtHitLighting)0;
    bool fromCards = false;
    const LhiRules rules = lhiRules(cardFrame);
    if (cardFrame != UNX_NONE)
    {
        const float3 face = dot(s.geometricNormal, direction) > 0 ? -s.geometricNormal : s.geometricNormal;
        const ClSample cards = clReadCardsAt(mcFrame(cardFrame), s.sceneInstance, s.position, face, CL_READ_IRRADIANCE, hiRes, 0.5 * footprint, feedbackCoord);
        if (cards.valid)
        {
            L.irradiance = cards.direct + cards.indirect;
            L.specularRadiance = L.irradiance / MODEL_PI;
            fromCards = true;
        }
    }
    if (!fromCards)
    {
        const uint hitSeed = DispatchRaysIndex().x * 9781u + DispatchRaysIndex().y * 6271u + g_frameIndex * 26699u;
        const float4 e = lhiIrradiance(lhiSources(cardFrame), s.position, s.normal, hitSeed);
        L.irradiance = e.a > 0 ? e.rgb : giFarSkyIrradiance(s.position, s.normal, rules.farStart);
        L.specularRadiance = e.rgb / MODEL_PI;
        const float3 l = normalize(g_sunDirection);
        if (dot(s.normal, l) > 0 || rtHitTransmits(m))
        {
            const float3 e0 = giSunIlluminance(s.position);
            if (any(e0 > 0))
            {
                RayDesc sr;
                sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * (1e-3 + 2e-4 * distance(s.position, g_cameraPosition));
                sr.Direction = l;
                sr.TMin = 0;
                sr.TMax = giRayLength();
                // (the sun through the Glass on the way: what the panes leave of it - RayShaders.hlsli rtShadowTransmittance)
                const float3 through = rtShadowTransmittance(scene, sr, RT_MASK_HIT_SHADOW | RT_MASK_FAR);
                L.sunIlluminance = e0 * through;
                L.sunVisibility = any(through > 0) ? 1.0 : 0.0;
            }
        }
        if (localSample) L.local = rtHitLocalSample(scene, s, m, -direction, footprint, 1e-3 + 2e-4 * distance(s.position, g_cameraPosition), hitSeed);
    }
    o.radiance = rtHitRadiance(m, s.normal, -direction, L, coneSpread) + lhiSkyLeakingReflection(rules, s.normal);
    // a leaf lit from its cards: the other side's light through it (LumenHitIndirect.hlsli)
    if (fromCards)
        o.radiance += lhiFoliageThrough(rules, mcFrame(cardFrame), m, s.sceneInstance, s.position, dot(s.geometricNormal, direction) > 0 ? -s.geometricNormal : s.geometricNormal);
    if (any(isnan(o.radiance)) || any(isinf(o.radiance))) o.radiance = 0;
    return o;
}

#endif
