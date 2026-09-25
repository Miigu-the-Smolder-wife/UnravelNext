// Radiance arriving along a reflection ray (R-internal; ReflectionTrace and the planar test's stand-in view): sky on a
// miss, else the hit's radiance toward the ray origin (RayTracing/HitShading.hlsli). Needs GiSky.hlsli's root constants
// (P[1] to P[3]), the GI cache (RW) and P[5].y = the specular albedo LUT SRV.
#ifndef UNX_REFLECTION_HIT_HLSLI
#define UNX_REFLECTION_HIT_HLSLI
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/GiSky.hlsli"

// A reflection hit consumes the cache like a GI hit: its cell (the lobe's footprint there, 2 t tan(lobe), at least) is
// found or created and requested for update next frame (hit tier), so cells only reflections see are kept converged.
// The cache read (irradiance and the mirror direction's radiance) starts at that level and climbs until an updated cell
// exists.
float3 reflHitRadiance(RtSceneSrvs scene, RWByteAddressBuffer cache, GiHeader h, RayDesc r, float coneTan, uint seed, out float hitDistance)
{
    const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_REFLECTION);
    if (hit.t < 0)
    {
        hitDistance = 65000;
        return giSkyRadiance(r.Direction);
    }
    hitDistance = hit.t;
    const RtSurface s = rtSurface(scene, hit, r.Origin, r.Direction);
    const GpuMaterial m = rtHitMaterial(loadMaterial(s.material), s, 2 * hit.t * coneTan, dot(s.normal, r.Direction));
    if (!s.frontFace && (m.classFlags & MATERIAL_TWO_SIDED) == 0) return 0;
    const uint footprintLevel = giLevelForSize(h, 2 * hit.t * coneTan);
    bool created;
    const uint e = giFindOrCreate(cache, h, giSurfaceKey(h, s.position, s.normal, footprintLevel), s.position, s.normal, created);
    if (e != GI_ENTRY_PENDING)
    {
        giTouch(cache, h, e);
        giRequestHit(cache, h, e);
    }
    RtHitLighting L;
    giCacheLightingAt(cache, h, s.position, s.normal, reflect(r.Direction, s.normal), footprintLevel, L.irradiance, L.specularRadiance);
    const float3 v = -r.Direction;
    L.sunIlluminance = 0;
    L.sunVisibility = 0;
    const float3 l = normalize(g_sunDirection);
    if (dot(s.normal, l) > 0 || materialClass(m) == MATERIAL_FOLIAGE)
    {
        L.sunIlluminance = giSunIlluminance(s.position);
        if (any(L.sunIlluminance > 0))
        {
            RayDesc sr;
            const float side = dot(s.normal, l) > 0 ? 1.0 : -1.0;  // leaves transmit: the shadow ray leaves the lit side
            sr.Origin = s.position + side * s.normal * (1e-3 + 2e-4 * distance(s.position, g_cameraPosition));
            sr.Direction = giSunDirection(seed);
            sr.TMin = 0;
            sr.TMax = giRayLength();
            L.sunVisibility = rtVisible(scene, sr, RT_MASK_REFLECTION) ? 1.0 : 0.0;
        }
    }
    return rtHitRadiance(m, P[5].y, s.normal, v, L, 2 * coneTan);
}

#endif
