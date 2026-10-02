// Radiance arriving along a reflection ray, traced and shaded in one ray generation thread (R-internal): the planar
// test's stand-in view, and reflection jobs that do not fit this frame's ray records (ReflectionTrace). Sky on a miss,
// else ReflectionShade.hlsli's hit shading with its shadow ray traced here. Root constants as ReflectionShade.hlsli.
#ifndef UNX_REFLECTION_HIT_HLSLI
#define UNX_REFLECTION_HIT_HLSLI
#define REFL_LOCAL_TRACE 1  // ray generation: local-light shadow rays are traced in reflShadeHit
#define REFL_DEFER_PENUMBRA 1  // identical classification and sun-term arithmetic to the split path
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionValue.hlsli"
#include "Passes/Reflection/ReflectionShade.hlsli"

// Visibility of the sun from 'origin' toward a point of the solar disk (seed), for the reflection passes.
float reflSunVisibility(RtSceneSrvs scene, float3 origin, uint seed)
{
    RayDesc sr;
    sr.Origin = origin;
    sr.Direction = giSunDirection(seed);
    sr.TMin = 0;
    sr.TMax = giRayLength();
    return rtVisible(scene, sr, RT_MASK_HIT_SHADOW, ((P[5].x >> 24) & 1) ? RAY_FLAG_FORCE_OPAQUE : RAY_FLAG_NONE) ? 1.0 : 0.0;
}

// localSeed: the local-light sample's seed, sunSeed: the sun shadow ray's (reflLocalSeed, reflSunSeed of the job and ray:
// the same draws as the split passes, so a job gets the same value on either path).
// layer: the ray's reconstruction layer record (ReflectionInternal.hlsli reflLayerRay, as ReflectionShadeRays stores it).
float3 reflHitRadiance(RtSceneSrvs scene, RWByteAddressBuffer cache, GiHeader h, RayDesc r, float coneWidth, float coneSpread, uint localSeed, uint sunSeed,
                       out float hitDistance, out float motion, out uint4 layer)
{
    RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_REFLECTION | RT_MASK_EMITTER);
    motion = 0;
    layer = reflLayerRay(0, 1, -r.Direction, 0, false);
    if (hit.t < 0)
    {
        hitDistance = 65000;
#if REFL_OVERFLOW
        return reflValueRadiance(reflStoreValue(giSkyRadiance(r.Direction), 0, hitDistance, 0));
#else
        return giSkyRadiance(r.Direction);
#endif
    }
    hitDistance = hit.t;
#if REFL_OVERFLOW
    hit.barycentrics = reflUnpackBarycentrics(reflPackBarycentrics(hit.barycentrics));
#endif
    const ReflHitShade o = reflShadeHit(scene, cache, h, hit, r.Origin, r.Direction, coneWidth, coneSpread, localSeed, false);
    motion = o.motion;
    layer = reflLayerRay(o.stochastic, o.albedo, o.hitNormal, hit.instance, o.surface, o.noData);
    float visibility = 0;
    if (o.needsPenumbra)
        visibility = shadowSunPenumbraDeferred(reflShadowSrvs(), o.shadowOrigin, o.penumbraNormal, o.penumbraLevel, o.penumbraReach);
    else if (o.needsShadowRay)
        visibility = reflSunVisibility(scene, o.shadowOrigin, sunSeed);
#if REFL_OVERFLOW
    uint4 value = reflStoreValue(o.radiance, o.sunTerm, hitDistance, motion);
    if (o.needsPenumbra || o.needsShadowRay) value = reflResolveSunValue(value, visibility);
    hitDistance = f16tof32(value.y >> 16);
    motion = f16tof32(value.w >> 17);
    return reflValueRadiance(value);
#else
    return (o.needsPenumbra || o.needsShadowRay) ? o.radiance + o.sunTerm * visibility : o.radiance;
#endif
}

#endif
