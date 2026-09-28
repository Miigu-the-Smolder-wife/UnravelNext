// Radiance leaving a ray hit toward the ray's origin (R's reflection and GI hits), with material model v1 and the direct
// view's own sun terms: M's pure functions in Passes/Shading/ShadingCommon.hlsli (sun-disk specular in three regimes,
// specular albedo split by f0), so a reflected surface is shaded like the same surface seen directly. Request
// Docs/Design/Requests/20260925_R_hit_shading.md asks to make those functions and the (A, B) table an interface.
// Terms and the conditions under which each equals the direct view's:
//   emission                                      exact;
//   sun, diffuse + specular (disk integral)       exact given the visibility (one shadow ray to a uniform disk point,
//                                                 unbiased for the disk-averaged visibility);
//   diffuse indirect = albedo / pi x E_cache      the cache's L2 SH irradiance at the hit (cell footprint);
//   specular indirect = E_spec x L_cache(r)       the cache's incident radiance from the mirror direction at texel
//                                                 resolution (8 x 8 hemisphere, ~22 deg texels): exact when the hit's
//                                                 lobe is about a texel wide; narrower lobes see the texel average,
//                                                 wider ones the bilinear texel instead of the lobe average.
// Material textures at hits: rtHitMaterial (M's published textures, INTERFACES v1.11, at the ray cone's level of detail).
//   local lights                                  one next-event sample per hit (HitLocalLights.hlsli, the reference's
//                                                 estimator), visibility by one shadow ray: unbiased, averaged by the
//                                                 hit's history.
#ifndef UNX_RT_HIT_SHADING_HLSLI
#define UNX_RT_HIT_SHADING_HLSLI
#include "Passes/Shading/ShadingCommon.hlsli"

// The material at a ray hit with its textures applied (baseColor x texture, roughness / metallic x the RG8 factors,
// emissive x texture), each at the ray cone's level of detail (Akenine-Moller et al. 2019):
//     lod = 0.5 log2(W H uvPerWorldArea) + log2(cone width at the hit / |cos|).
float rtTextureLod(Texture2D t, float uvPerWorldArea, float footprintLog2)
{
    uint w, h;
    t.GetDimensions(w, h);
    return 0.5 * log2(max((float)w * h * uvPerWorldArea, 1e-20)) + footprintLog2;
}

GpuMaterial rtHitMaterial(GpuMaterial m, RtSurface s, float coneWidth, float cosTheta)
{
    const float footprint = log2(max(coneWidth, 1e-8) / max(abs(cosTheta), 1e-3));
    if (m.baseColorTexture != UNX_NONE)
    {
        Texture2D t = ResourceDescriptorHeap[m.baseColorTexture];
        m.baseColor *= materialBaseColorLevel(m, s.uv, rtTextureLod(t, s.uvPerWorldArea, footprint)).rgb;
    }
    if (m.roughMetalTexture != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[m.roughMetalTexture];
        const float lod = rtTextureLod(t, s.uvPerWorldArea, footprint);
        const float2 rm = (m.textureClamp & MATERIAL_TEXTURE_ROUGH_METAL) ? t.SampleLevel(g_anisoClamp, s.uv, lod).rg : t.SampleLevel(g_anisoWrap, s.uv, lod).rg;
        m.roughness *= rm.r;
        m.metallic *= rm.g;
    }
    if (m.emissiveTexture != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[m.emissiveTexture];
        const float lod = rtTextureLod(t, s.uvPerWorldArea, footprint);
        m.emissive *= (m.textureClamp & MATERIAL_TEXTURE_EMISSIVE) ? t.SampleLevel(g_anisoClamp, s.uv, lod).rgb : t.SampleLevel(g_anisoWrap, s.uv, lod).rgb;
    }
    return m;
}

struct RtHitLighting
{
    float3 sunIlluminance;    // E on a surface facing the sun (transmittance included), 0 below the horizon
    float sunVisibility;      // in [0, 1]: S's VSM disk integral, or one shadow ray (0 or 1)
    float3 irradiance;        // indirect irradiance at the hit
    float3 specularRadiance;  // indirect incident radiance from the hit's mirror direction
    float3 local;             // local lights: the hit's outgoing radiance from them (HitLocalLights.hlsli, one NEE sample)
};

// n faces the ray origin side (RtSurface); v = unit vector toward the ray origin; pixelAngle = the ray cone's angular
// width at the hit (filters a mirror hit's sun-disk edge like the direct view's pixel).
// sunFull: the sun's term at full visibility (the radiance is linear in L.sunVisibility: radiance = the rest + sunFull x
// visibility), evaluated also at visibility 0 when wantSun - callers that get the visibility later (a shadow ray, the
// deferred penumbra filter) take both from one evaluation instead of shading the hit twice.
float3 rtHitRadianceParts(GpuMaterial m, float3 n, float3 v, RtHitLighting L, float pixelAngle, bool wantSun, out float3 sunFull)
{
    ModelSurface s;
    s.cls = m.classFlags & 0xFFu;
    s.baseColor = m.baseColor;
    s.roughness = m.roughness;
    s.metallic = m.metallic;
    s.specular = m.specular;
    s.transmission = m.transmission;
    const float3 l0 = normalize(g_sunDirection);
    const float NoV = max(dot(n, v), 1e-4);
    const float NoL = dot(n, l0);
    const float3 albedo = s.baseColor * ((1 - s.metallic) / MODEL_PI);
    const bool foliage = s.cls == MATERIAL_FOLIAGE;
    const float3 diffuseAlbedo = foliage ? albedo * (1 - s.transmission) : albedo;
    const float alpha = modelAlpha(s.roughness);
    const float3 f0 = modelF0(s);
    const float3 compensation = 1 + f0 * (1 / modelDirectionalAlbedo(NoV, s.roughness) - 1);
    sunFull = 0;
    if ((wantSun || L.sunVisibility > 0) && any(L.sunIlluminance > 0))
    {
        if (NoL > 0)
            sunFull = diffuseAlbedo * L.sunIlluminance * NoL + shSunSpecular(f0, s.roughness, alpha, compensation, n, v, NoV, l0, L.sunIlluminance, pixelAngle);
        else if (foliage)
            sunFull = albedo * s.transmission * L.sunIlluminance * -NoL;  // transmitted through the leaf (model v1)
    }
    const float3 sun = sunFull * L.sunVisibility;  // fractional in penumbrae (the VSM estimate); was only tested > 0, giving full sun there
    return m.emissive + sun + diffuseAlbedo * L.irradiance + shSpecularAlbedo(f0, NoV, s.roughness) * L.specularRadiance + L.local;
}
float3 rtHitRadiance(GpuMaterial m, float3 n, float3 v, RtHitLighting L, float pixelAngle)
{
    float3 sunFull;
    return rtHitRadianceParts(m, n, v, L, pixelAngle, false, sunFull);
}

#endif
