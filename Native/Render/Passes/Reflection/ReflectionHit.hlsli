// Radiance arriving along a reflection ray, traced and shaded in one ray generation thread (R-internal): the planar
// test's stand-in view, and reflection jobs that do not fit this frame's ray records (ReflectionTrace). Sky on a miss,
// else ReflectionShade.hlsli's hit shading with its shadow ray traced here. Root constants as ReflectionShade.hlsli.
#ifndef UNX_REFLECTION_HIT_HLSLI
#define UNX_REFLECTION_HIT_HLSLI
#define REFL_LOCAL_TRACE 1  // ray generation: local-light shadow rays are traced in reflShadeHit
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionShade.hlsli"

// Visibility of the sun from 'origin' toward a point of the solar disk (seed), for the reflection passes.
float reflSunVisibility(RtSceneSrvs scene, float3 origin, uint seed)
{
    RayDesc sr;
    sr.Origin = origin;
    sr.Direction = giSunDirection(seed);
    sr.TMin = 0;
    sr.TMax = giRayLength();
    return rtVisible(scene, sr, RT_MASK_REFLECTION, ((P[5].x >> 24) & 1) ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE) ? 1.0 : 0.0;
}

float3 reflHitRadiance(RtSceneSrvs scene, RWByteAddressBuffer cache, GiHeader h, RayDesc r, float coneWidth, float coneSpread, uint seed, out float hitDistance,
                       out float motion)
{
    const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_REFLECTION | RT_MASK_EMITTER);
    motion = 0;
    if (hit.t < 0)
    {
        hitDistance = 65000;
        return giSkyRadiance(r.Direction);
    }
    hitDistance = hit.t;
    const ReflHitShade o = reflShadeHit(scene, cache, h, hit, r.Origin, r.Direction, coneWidth, coneSpread, giRandom(seed * 3u + 101u), false);
    motion = o.motion;
    return o.needsShadowRay ? o.radiance + o.sunTerm * reflSunVisibility(scene, o.shadowOrigin, seed) : o.radiance;
}

#endif
