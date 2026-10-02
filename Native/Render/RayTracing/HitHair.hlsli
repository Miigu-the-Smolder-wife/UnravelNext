// Strand hair at ray hits (R; raytracing.hair). The grooms are not in the TLAS: their proxy for a ray is E's density
// volume (Passes/Hair/HairDensity.hlsli) - per body with a block, a box of cells holding how many fibres a ray crosses
// per metre. Unreal's Lumen does the same with its hair voxels (LumenHairTracing: the screen probes' and the reflections'
// rays walk them after their trace, and a hit lets nothing through); here the hit is also lit.
//   where    the ray's first fibre (hairFirstFibreAmong: per body the count at which a ray's first fibre lies is
//            exponential with mean 1 - a ray passes a thin part of a groom as often as the part's fibres let it). The draw
//            is a function of the ray's seed (its pixel or texel and the frame, rtHairSeed) and of the ray from its
//            surface point: a pass that follows the same ray earlier (the screen traces: the screen's depth holds no hair)
//            finds the same fibre and leaves the ray to the world trace when it lies before its own hit.
//   light    a matte surface of the hair material's base colour (the colour a reader without the fibre model takes),
//            facing out of the groom (hairDensityOutward: against the gradient of the cells' means; turned to the ray):
//            the sun through its shadow ray and one local-light sample through its own (HitLocalLights.hlsli), each
//            times what the hair between the point and the light lets through (hairTransmittance); the indirect
//            light from the Lumen translucency volume at the hit, as the strands take theirs (CoverageHair.hlsl) - the
//            volume the previous frame left (word 23 of the local-light header; this frame's is built after the first
//            of these rays), its irradiance on the proxy's side.
// Limits: the volume's cells (about 1 cm on a head: no strands in a mirror); the fibre model is not evaluated (no
// highlights, no light through the hair towards the ray); the bodies with a block (shading.hair_density_bodies).
// The shading part needs a ray library (RayShaders.hlsli: rtVisible) with HitLocalLights.hlsli and GiSky.hlsli's sun
// (giSunIlluminance, giRayLength) included before this file.
#ifndef UNX_RT_HIT_HAIR_HLSLI
#define UNX_RT_HIT_HAIR_HLSLI
#include "Passes/Hair/HairDensity.hlsli"
#include "Passes/GI/LumenTranslucencyVolume.hlsli"

#define RT_HAIR_STEPS 32u  // coarse cells a march through a body walks at most (HairDensity.hlsli hairDensityAcross)

struct RtHairHit
{
    float t;  // along the ray from its surface point; < 0: none
    uint body, material;
};
uint rtHairSeed(uint2 at, uint frame) { return at.x * 0x9E3779B1u + at.y * 0x85EBCA77u + frame * 0xC2B2AE3Du; }
// params: the density parameters' SRV (UNX_NONE: no volume). origin: world; direction: unit. outsideOnly: the grooms the
// ray starts inside are left out (hairFirstFibreAmong).
RtHairHit rtHairFirst(uint params, float3 origin, float3 direction, float reach, uint seed, bool outsideOnly = false)
{
    RtHairHit h;
    h.t = -1;
    h.body = h.material = 0;
    if (params == 0xFFFFFFFFu) return h;
    ByteAddressBuffer record = ResourceDescriptorHeap[params];
    h.t = hairFirstFibreAmong(params, origin - hairDensityOrigin(record), direction, reach, RT_HAIR_STEPS, seed, h.body, h.material, outsideOnly);
    return h;
}

#ifdef UNX_RT_RAYSHADERS_HLSLI
// The frame's density parameters: word 22 of the local-light data's header (RayScene::recordHair).
uint rtHairParams(RtSceneSrvs scene)
{
    if (scene.pad == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    ByteAddressBuffer header = ResourceDescriptorHeap[scene.pad];
    return header.Load(88);
}
// The translucency volume the hits take their indirect light from: word 23 (UNX_NONE: none).
uint rtHairIndirect(RtSceneSrvs scene)
{
    if (scene.pad == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    ByteAddressBuffer header = ResourceDescriptorHeap[scene.pad];
    return header.Load(92);
}
#endif

#if defined(UNX_RT_RAYSHADERS_HLSLI) && defined(UNX_RT_HIT_LOCAL_LIGHTS_HLSLI)
// The hit's radiance toward the ray's origin (nits). footprint: the ray cone's width at the hit (the light functions');
// bias: the shadow rays' offset; sun / local: whether those lights are taken (the caller's experiments).
float3 rtHairRadiance(RtSceneSrvs scene, uint params, RtHairHit hit, float3 origin, float3 direction, float footprint, float bias, uint seed, bool sun, bool local)
{
    ByteAddressBuffer record = ResourceDescriptorHeap[params];
    const float3 x = origin + direction * hit.t, rel = x - hairDensityOrigin(record);
    float3 n = hairDensityOutward(params, hit.body, rel);
    if (dot(n, n) < 0.5f) n = -direction;        // (no gradient there)
    else if (dot(n, direction) > 0) n = -n;      // (seen from inside the groom)
    const float3 albedo = saturate(loadMaterial(hit.material).baseColor) * (1 / 3.14159265f);
    float3 radiance = 0;
    const float3 l = normalize(g_sunDirection);
    const float cosSun = dot(n, l);
    if (sun && cosSun > 0)
    {
        const float3 e0 = giSunIlluminance(x);
        if (any(e0 > 0))
        {
            const float through = hairTransmittance(params, rel, l, 3.0e38f, RT_HAIR_STEPS);
            RayDesc sr;
            sr.Origin = x + n * bias;
            sr.Direction = l;
            sr.TMin = 0;
            sr.TMax = giRayLength();
            if (through > 1e-3f && rtVisible(scene, sr, RT_MASK_HIT_SHADOW)) radiance += albedo * e0 * (cosSun * through);
        }
    }
    if (local)
    {
        const RtLocalSample ls = rtLocalLightFinish(scene, rtLocalLightChooseOriented(scene, x, n, false, hairDensityUnit(seed, 101)), x, hairDensityUnit(seed, 102),
                                                    hairDensityUnit(seed, 103), footprint);
        const float cosL = ls.valid ? dot(n, ls.wi) : 0;
        if (cosL > 0)
        {
            const float through = hairTransmittance(params, rel, ls.wi, ls.distance, RT_HAIR_STEPS);
            if (through > 1e-3f && (!ls.castShadow || rtVisible(scene, rtLocalShadowRay(x, n, ls, bias), RT_MASK_HIT_SHADOW))) radiance += albedo * ls.weight * (cosL * through);
        }
    }
    radiance += albedo * ltvIrradiance(rtHairIndirect(scene), x, n);
    return radiance;
}
#endif

#endif
