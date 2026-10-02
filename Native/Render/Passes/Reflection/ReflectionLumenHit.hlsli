// Hit lighting of the reflection and refraction rays on the Lumen path (reflection.lumen_only): the surface cache, as
// Unreal's default (r.Lumen.HardwareRayTracing.LightingMode 0), with the hit's own material.
//   emitter proxy   the area light's radiance (RayScene.hlsli rtEmitterRadiance) where the ray paths own its specular;
//   surface         its material at the ray cone's footprint (textures, decals), its emission, and the light of the mesh
//                   cards of its instance (CardLighting.hlsli clReadCards: direct light with the sun, radiosity) through
//                   the material - diffuse albedo x E / pi and the specular albedo x E / pi (the lobe at the hit sees the
//                   cards' light as uniform); no light sample, no shadow ray;
//   without cards   (a deforming instance: skin, wind; a texel the cards do not cover) the sun through one shadow ray
//                   to the disk's centre, and no indirect light - the reference lights such a hit with nothing;
//   back of a one-sided surface: 0 (inside closed geometry).
// exactCounts: RayScene's exact set hit counts (UAV; UNX_NONE: none) - a hit on a skinned instance counts there, as the
// other reflection paths' hits do (RayScene picks the most-hit characters for exact refits).
// The includer is a ray library (RayShaders.hlsli) with GiSky.hlsli's sky and sun in P[1..3].
#ifndef UNX_REFLECTION_LUMEN_HIT_HLSLI
#define UNX_REFLECTION_LUMEN_HIT_HLSLI
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitDecals.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"

struct RlHit
{
    float3 radiance;  // toward the ray's origin (nits)
    float motion;     // the hit point's displacement since the previous frame over the ray's footprint there (0: still)
    bool surface;     // a lit surface (not an emitter proxy, not the back of a one-sided surface)
};

RlHit rlShadeHit(RtSceneSrvs scene, RtHit hit, float3 origin, float3 direction, float coneWidth, float coneSpread, uint cardFrame, uint exactCounts)
{
    RlHit o;
    o.radiance = 0;
    o.motion = 0;
    o.surface = false;
    if (hit.instance == RT_INSTANCE_EMITTER)
    {
        o.radiance = rtEmitterCounts(scene.pad, hit.primitive) ? rtEmitterRadiance(hit.primitive, origin) : float3(0, 0, 0);
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
    m = rtHitMaterial(m, s, footprint, dot(s.normal, direction));
    rtHitDecals(scene, s, footprint, m);
    if (!s.frontFace && (m.classFlags & MATERIAL_TWO_SIDED) == 0) return o;
    o.surface = true;
    g_rtHitCone = 0.5 * coneSpread;
    RtHitLighting L = (RtHitLighting)0;
    bool fromCards = false;
    if (cardFrame != UNX_NONE)
    {
        const float3 face = dot(s.geometricNormal, direction) > 0 ? -s.geometricNormal : s.geometricNormal;
        const ClSample cards = clReadCards(mcFrame(cardFrame), s.sceneInstance, s.position, face, CL_READ_IRRADIANCE);
        if (cards.valid)
        {
            L.irradiance = cards.direct + cards.indirect;
            L.specularRadiance = L.irradiance / MODEL_PI;
            fromCards = true;
        }
    }
    if (!fromCards)
    {
        const float3 l = normalize(g_sunDirection);
        if (dot(s.normal, l) > 0 || materialClass(m) == MATERIAL_FOLIAGE)
        {
            const float3 e0 = giSunIlluminance(s.position);
            if (any(e0 > 0))
            {
                RayDesc sr;
                sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * (1e-3 + 2e-4 * distance(s.position, g_cameraPosition));
                sr.Direction = l;
                sr.TMin = 0;
                sr.TMax = giRayLength();
                L.sunIlluminance = e0;
                L.sunVisibility = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
            }
        }
    }
    o.radiance = rtHitRadiance(m, s.normal, -direction, L, coneSpread);
    if (any(isnan(o.radiance)) || any(isinf(o.radiance))) o.radiance = 0;
    return o;
}

#endif
