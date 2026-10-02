// A ray hit on a proxy of the far field (raytracing.far_field; RayScene.hlsli: RT_INSTANCE_FAR) - what distant instances
// that are not in the near structure return to a ray: the reference's far-field hits read the cards of its merged
// proxies; here a proxy has no card, and its light is the far field's rule for every hit past the mesh cards
// (GiSky.hlsli giFarSkyIrradiance): the sun through one shadow ray and the sky's light on the face, unoccluded.
//   surface   a matte face of the box the ray entered by, of the colour of the material that covers most of the
//             group's members - its base colour x its texture's last level (the texture's mean) x (1 - metallic);
//   sun       E x cos on that face x the shadow ray's answer (the casters' mask and the far field's own proxies: a
//             forest's proxies shadow each other with their opacity's share of the rays);
//   sky       pi x the sky's radiance along the face's normal.
// The sun's shadow rays shot from other ray hits and from card texels take RT_MASK_FAR too (the Lumen hit kernels,
// HitHair.hlsli, CardDirectTrace.hlsl): the proxies shadow the bounce light of what is near. Local lights' shadow rays do
// not (their reach is short of the proxies).
// The includer is a ray library (RayShaders.hlsli) with GiSky.hlsli's sky and sun. The hit's thread traces one shadow
// ray, the one a hit without cards would trace for the sun: the dispatches' ray counts stand.
#ifndef UNX_RT_HIT_FAR_FIELD_HLSLI
#define UNX_RT_HIT_FAR_FIELD_HLSLI
#include "Passes/Material/MaterialTextures.hlsli"

// sun: whether the sun's light is taken (the caller's experiment switches).
float3 rtFarRadiance(RtSceneSrvs scene, RtHit hit, float3 origin, float3 direction, bool sun)
{
    if (scene.pad == 0xFFFFFFFFu) return 0;
    ByteAddressBuffer lightHeader = ResourceDescriptorHeap[scene.pad];
    const uint headerSrv = lightHeader.Load(40);
    if (headerSrv == 0xFFFFFFFFu) return 0;
    ByteAddressBuffer header = ResourceDescriptorHeap[headerSrv];
    ByteAddressBuffer records = ResourceDescriptorHeap[header.Load(12)];
    const GpuMaterial m = loadMaterial(records.Load(hit.primitive * RT_FAR_RECORD_BYTES + 44));
    float3 albedo = saturate(m.baseColor) * (1 - saturate(m.metallic));
    if (m.baseColorTexture != UNX_NONE) albedo *= materialBaseColorLevelAt(m, float2(0.5, 0.5), 16.0).rgb;  // (the last level: the mean)
    const uint face = (uint)hit.barycentrics.x;
    float3 n = 0;
    n[face % 3u] = face >= 3u ? -1.0 : 1.0;
    const float3 x = origin + direction * hit.t;
    float3 e = 3.14159265 * giSkyRadiance(n);
    const float3 l = normalize(g_sunDirection);
    const float cosSun = dot(n, l);
    if (sun && cosSun > 0)
    {
        const float3 e0 = giSunIlluminance(x);
        if (any(e0 > 0))
        {
            RayDesc sr;
            sr.Origin = x + n * (0.05 + 2e-4 * distance(x, g_cameraPosition));
            sr.Direction = l;
            sr.TMin = 0;
            sr.TMax = giRayLength();
            if (rtVisible(scene, sr, RT_MASK_HIT_SHADOW | RT_MASK_FAR)) e += e0 * cosSun;
        }
    }
    const float3 radiance = albedo * e * (1.0 / 3.14159265);
    return any(isnan(radiance)) || any(isinf(radiance)) ? float3(0, 0, 0) : radiance;
}

#endif
