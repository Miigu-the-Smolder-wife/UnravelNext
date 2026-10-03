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
// Material inputs at hits (scene::Material; Scene.hlsli GpuMaterialInputs): the uv transform (with the level of detail
// by its determinant), the emissive mask, the vertex tint (the emission's scale is in the material's emissive) and the
// detail colour on its uv set - the mesh's first or second (RtSurface::uv1) -, weighed by the vertex alpha where the
// material says so: the base colour a mirror or the bounce light carries is the resolve's (MaterialInputs.hlsli mDetail's
// colour factor at the cone's level of detail). Not at hits: the detail normal (a hit shades its interpolated normal; the
// base normal map is not read either), the occlusion map, and the parallax - a hit's textures are read at the surface's
// uv, not where the ray would meet the height field (in a mirror a parallax wall's bricks sit where the flat wall's do).
//   local lights                                  one next-event sample per hit (HitLocalLights.hlsli, the reference's
//                                                 estimator), visibility by one shadow ray: unbiased, averaged by the
//                                                 hit's history.
#ifndef UNX_RT_HIT_SHADING_HLSLI
#define UNX_RT_HIT_SHADING_HLSLI
#include "Passes/Shading/ShadingCommon.hlsli"
#include "RayTracing/HitLayers.hlsli"

// The material at a ray hit with its textures applied (baseColor x texture, roughness / metallic x the RG8 factors,
// emissive x texture), each at the ray cone's level of detail (Akenine-Moller et al. 2019):
//     lod = 0.5 log2(W H uvPerWorldArea) + log2(cone width at the hit / |cos|).
float rtTextureLod(Texture2D t, float uvPerWorldArea, float footprintLog2)
{
    uint w, h;
    t.GetDimensions(w, h);
    return 0.5 * log2(max((float)w * h * uvPerWorldArea, 1e-20)) + footprintLog2;
}

// uvColor: where the base colour is read (the surface's uv; an eye's iris point), on the mesh's uv.
GpuMaterial rtHitMaterialAt(GpuMaterial m, RtSurface s, float2 uvColor, float coneWidth, float cosTheta)
{
    const float footprint = log2(max(coneWidth, 1e-8) / max(abs(cosTheta), 1e-3));
    float2 uv = s.uv;
    float uvPerWorldArea = s.uvPerWorldArea;
    if (m.inputs != UNX_NONE)
    {
        // the material's uv transform: its uv, and its texture area per world area by the determinant
        const GpuMaterialInputs r = loadMaterialInputs(m.inputs);
        uv = materialInputsUv(r, uv);
        uvColor = materialInputsUv(r, uvColor);
        uvPerWorldArea *= abs(r.uvU.x * r.uvV.y - r.uvU.y * r.uvV.x);
        if ((r.flags & MATERIAL_INPUT_VERTEX_TINT) != 0) m.baseColor *= s.color.rgb;
        if (r.detailColorTexture != UNX_NONE)
        {
            // the detail colour over the base: lerp(1, detail x 2^2.2, w x strength), on the detail's uv set (the mesh's
            // uv, not the material's) x its scale, at the cone's level in that set
            const bool set1 = (r.flags & MATERIAL_INPUT_DETAIL_UV1) != 0;
            const float2 scale = float2(r.detailScaleU, r.detailScaleV);
            const float2 uvDetail = (set1 ? s.uv1 : s.uv) * scale + r.detailOffset;
            Texture2D t = ResourceDescriptorHeap[r.detailColorTexture];
            const float lod = rtTextureLod(t, (set1 ? s.uv1PerWorldArea : s.uvPerWorldArea) * abs(scale.x * scale.y), footprint);
            const float3 detail = (r.textureClamp & 1u) ? t.SampleLevel(g_anisoClamp, uvDetail, lod).rgb : t.SampleLevel(g_anisoWrap, uvDetail, lod).rgb;
            const float weight = (r.flags & MATERIAL_INPUT_VERTEX_BLEND) != 0 ? saturate(s.color.a) : 1.0;
            m.baseColor *= lerp(1.0, detail * 4.59479380, weight * r.detailColor);
        }
        if (r.emissiveMaskTexture != UNX_NONE)
        {
            Texture2D t = ResourceDescriptorHeap[r.emissiveMaskTexture];
            const float lod = rtTextureLod(t, uvPerWorldArea, footprint);
            m.emissive *= (r.textureClamp & 8u) ? t.SampleLevel(g_anisoClamp, uv, lod).x : t.SampleLevel(g_anisoWrap, uv, lod).x;
        }
    }
    if (m.baseColorTexture != UNX_NONE)
    {
        Texture2D t = ResourceDescriptorHeap[m.baseColorTexture];
        m.baseColor *= materialBaseColorLevelAt(m, uvColor, rtTextureLod(t, uvPerWorldArea, footprint)).rgb;
    }
    if (m.roughMetalTexture != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[m.roughMetalTexture];
        const float lod = rtTextureLod(t, uvPerWorldArea, footprint);
        const float2 rm = (m.textureClamp & MATERIAL_TEXTURE_ROUGH_METAL) ? t.SampleLevel(g_anisoClamp, uv, lod).rg : t.SampleLevel(g_anisoWrap, uv, lod).rg;
        m.roughness *= rm.r;
        m.metallic *= rm.g;
    }
    if (m.emissiveTexture != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[m.emissiveTexture];
        const float lod = rtTextureLod(t, uvPerWorldArea, footprint);
        m.emissive *= (m.textureClamp & MATERIAL_TEXTURE_EMISSIVE) ? t.SampleLevel(g_anisoClamp, uv, lod).rgb : t.SampleLevel(g_anisoWrap, uv, lod).rgb;
    }
    return m;
}
// (a macro, not a function: a wrapper's copies of the material and the surface cost a kernel at the DXIL limit 176 B)
#define rtHitMaterial(m, s, coneWidth, cosTheta) rtHitMaterialAt(m, s, (s).uv, coneWidth, cosTheta)

// An eye (MATERIAL_EYE) at a ray hit: the base colour's uv - the iris point seen through the cornea along the ray
// (modelEyeFrame through the hit triangle, modelEyePoint, as the resolve's MaterialEye.hlsli) - and the limbal ring's
// factor (returned).
#ifndef RT_HIT_EYE
#define RT_HIT_EYE 0
#endif
#if RT_HIT_EYE
float rtHitEye(GpuMaterial m, RtSurface s, float3 direction, inout float2 uv)
{
    const GpuMaterialEye e = loadMaterialEye(m.classFlags >> 16);
    float3 t = float3(0, 0, -1);  // (the sclera: no frame)
    if (length(s.uv - 0.5) < e.irisRadius)
        t = modelEyeRay(modelEyeFrame(e.axis, s.restE1, s.restE2, s.worldE1, s.worldE2, s.uvE1, s.uvE2), direction, s.normal, e.eta);
    const ModelEyePoint p = modelEyePoint(e, s.uv, t);
    uv = p.uv;
    return p.darkening;
}
// The material of a hit as a viewer along 'direction' (the ray's, unit) sees it: rtHitMaterial, and with RT_HIT_EYE = 1
// (the reflection kernels set it; default 0: GI hits take the surface's uv) an eye's base colour at its iris point.
GpuMaterial rtHitMaterialSeen(GpuMaterial m, RtSurface s, float3 direction, float coneWidth, float cosTheta)
{
    float2 uvColor = s.uv;
    if ((m.classFlags & MATERIAL_EYE) != 0) m.baseColor *= rtHitEye(m, s, direction, uvColor);
    return rtHitMaterialAt(m, s, uvColor, coneWidth, cosTheta);
}
#else
#define rtHitMaterialSeen(m, s, direction, coneWidth, cosTheta) rtHitMaterialAt(m, s, (s).uv, coneWidth, cosTheta)
#endif

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
// A9 layers at ray hits (redesign V2.2 P1'-b; before, hits shaded the bare base: a glazed wall bounced its base's light
// with no coat transmission loss, and the bath's multi-bounce GI came out +20-25 % over the coat-aware reference
// [measured, 1080p]): the clearcoat as the direct view shades it (ShadeOpaque LAYERED 1: modelEvaluateCoated for the sun;
// for the cache's light, the base through the coat with the coat's mean transmission and the coat lobe's albedo), the
// sheen as ShadeOpaque LAYERED 2. The coat lobe is widened by the hit's cone (HitLayers.hlsli g_rtHitCone, GI texel rays: its half-width)
// as the base lobe is for local lights; the coat lobe's incident radiance is the base lobe's mirror-direction radiance
// (the hit has no second cone lookup).
// The split for the reconstruction layers (RENDERER_REDESIGN_V2 1.2, P2): the returned radiance = deterministic part +
// 'stochastic', where
//   deterministic  emission and the sun term (the direct view's estimators: no noise but the off-screen shadow ray's
//                  penumbra) - the hit's identity, which no spatial filter may touch;
//   stochastic     the cache's light at the hit (diffuse and specular indirect: young cells, cell-sized errors) and the
//                  one-sample local-light estimate (relative variance ~ the lights reaching the hit - 1): the terms the
//                  layer L_rs reconstructs over neighbouring hits of the same surface;
//   albedo         the hit's directional reflectance (diffuse albedo + specular albedo toward v, base layer): stochastic /
//                  albedo is free of the hit's texture detail (exactly for the diffuse part under any light; for the
//                  specular part up to the lobe's shape), so the layer filter does not blur the reflected image's
//                  textures. A demodulation key only: the composition multiplies it back, so its model error (the coat
//                  and sheen are left out) changes what the filter averages, never the unfiltered value.
struct RtHitSplit
{
    float3 stochastic;
    float3 albedo;
};
// A Subsurface-class hit: one specular lobe at the material's roughness (not the class's two lobes, MaterialModel.hlsli
// ModelSubsurface), Lambert diffuse light with no scattering pass behind it, and the light through thin parts
// (modelSubsurfaceThin) from the sun and the light sample on the far side of the shading normal - the hit's one shadow
// ray decides, as the direct view's visibility does. An eye (MATERIAL_EYE) is such a hit with its base colour at the
// surface's uv: no refraction onto the iris - except in the reflection kernels (RT_HIT_EYE, rtHitMaterialSeen), where
// its base colour is read at the iris point seen through the cornea along the ray, under the limbal ring, so a mirror
// shows the eye the direct view shows; its shading stays the hit's (no iris plane, no caustic).
// The cloth blend (a sheen's cloth factor): the base's specular share of the sun term and of the cache's light x (1 - cloth).
float3 rtHitRadianceSplit(GpuMaterial m, float3 n, float3 v, RtHitLighting L, float pixelAngle, bool wantSun, out float3 sunFull, out RtHitSplit split)
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
    float3 sunSpecular = 0;  // the base lobe's share of sunFull (the cloth blend scales it, and its share of the cache's light)
    if ((wantSun || L.sunVisibility > 0) && any(L.sunIlluminance > 0))
    {
        if (NoL > 0)
        {
            sunSpecular = shSunSpecular(f0, s.roughness, alpha, compensation, n, v, NoV, l0, L.sunIlluminance, pixelAngle);
            sunFull = diffuseAlbedo * L.sunIlluminance * NoL + sunSpecular;
        }
        else if (foliage || s.cls == MATERIAL_SUBSURFACE)
            // across the surface: through the leaf (model v1), or through a Subsurface hit's thin part (W)
            sunFull = albedo * s.transmission * L.sunIlluminance * (foliage ? -NoL : modelSubsurfaceThin(-NoL, v, l0));
    }
    const float3 cachedSpecular = shSpecularAlbedo(f0, NoV, s.roughness) * L.specularRadiance;
    float3 cached = diffuseAlbedo * L.irradiance + cachedSpecular;
    if ((m.classFlags & MATERIAL_LAYERED) != 0 && !foliage)
    {
        const ModelCoat coat = rtHitCoat(m);
        const ModelSheen sheen = modelSheenOf(m);
        if (coat.cover > 0)
        {
            const float keep = 1 - coat.cover;
            if (NoL > 0 && any(sunFull > 0))
                sunFull = keep * sunFull + coat.cover * (modelCoatLobe(coat, n, v, l0) + modelCoatUnder(s, coat, n, v, l0)) * L.sunIlluminance * NoL;
            const float tv = 1 - modelCoatEms(coat, NoV), tBar = 1 - modelCoatLookup1(coat.coat * MODEL_COAT_STRIDE + 4096, coat.roughness);
            const float3 under = tv * tBar * ((diffuseAlbedo + modelCoatReturned(s, coat, modelCoatRefractedCos(2.0 / 3.0, coat.eta)) / MODEL_PI) * L.irradiance +
                                              L.specularRadiance * shSpecularAlbedo(f0, modelCoatRefractedCos(NoV, coat.eta), modelCoatBaseRoughness(s, coat, NoV)));
            cached = keep * cached + coat.cover * (under + L.specularRadiance * modelCoatEms(coat, NoV));
        }
        else if (any(sheen.color > 0))
        {
            const float keepS = modelSheenKeep(sheen, NoV);
            if (NoL > 0 && any(sunFull > 0))
                sunFull = keepS * (sunFull - sunSpecular * sheen.cloth) + sheen.color * modelSheenLobe(sheen.roughness, n, v, l0) * L.sunIlluminance * NoL;
            cached = keepS * (cached - cachedSpecular * sheen.cloth) + sheen.color * (modelSheenAlbedo(NoV, sheen.roughness) / MODEL_PI) * L.irradiance;
        }
    }
    const float3 sun = sunFull * L.sunVisibility;  // fractional in penumbrae (the VSM estimate); was only tested > 0, giving full sun there
    split.stochastic = cached + L.local;
    split.albedo = diffuseAlbedo * MODEL_PI + shSpecularAlbedo(f0, NoV, s.roughness);
    return m.emissive + sun + cached + L.local;
}
float3 rtHitRadianceParts(GpuMaterial m, float3 n, float3 v, RtHitLighting L, float pixelAngle, bool wantSun, out float3 sunFull)
{
    RtHitSplit split;
    return rtHitRadianceSplit(m, n, v, L, pixelAngle, wantSun, sunFull, split);
}
float3 rtHitRadiance(GpuMaterial m, float3 n, float3 v, RtHitLighting L, float pixelAngle)
{
    float3 sunFull;
    return rtHitRadianceParts(m, n, v, L, pixelAngle, false, sunFull);
}

// A hit's diffuse direct light in the hit accumulator's light-side form (RENDERER_REDESIGN_V2 12.1; GiInternal.hlsli
// GI_ACC_*), for GI and reflection hits alike: the arithmetic GiTrace.hlsl carries inline for its hits, as a function.
//   A, B, C      the hit's direct irradiance terms: A = E_sun + E_local (plain surface), B the same through the coat's
//                entry transmission, C the coat's returned light (/ pi); B = A and C = 0 without a coat;
//   kA, kB, kC   the reader's factors: its diffuse-direct radiance toward v is kA A + kB B + kC C ('own' for the hit's
//                own terms). A cell's means (mA, mB, mC) give the reader rtHitDirectFromMeans - its own albedo and
//                layers on the cell's mean light.
// eSun = the sun's illuminance x max(n.l, 0) x visibility, muS = max(n.l_sun, 0); localE = the local-light sample's
// irradiance (weight x max(n.wi, 0), 0 when not visible), muL = max(n.wi, 0). Both terms are linear in eSun and localE:
// a caller that learns the sun's visibility later passes eSun at full visibility and takes own - ownSun (visibility 0)
// and ownSun (the sun's share of 'own') apart.
// lambert: GiAnalytic's closed forms (no layers). Foliage is not handled (its transmission has no accumulator term).
struct RtHitDirect
{
    float3 A, B, C;
    float3 kA, kB;
    float kC;
    float3 own, ownSun;
};
RtHitDirect rtHitDirectTerms(GpuMaterial m, float3 n, float3 v, float3 eSun, float muS, float3 localE, float muL, bool lambert)
{
    ModelSurface ms;
    ms.cls = m.classFlags & 0xFFu;
    ms.baseColor = m.baseColor;
    ms.roughness = m.roughness;
    ms.metallic = m.metallic;
    ms.specular = m.specular;
    ms.transmission = m.transmission;
    ModelCoat coat = (ModelCoat)0;
    ModelSheen sheen = (ModelSheen)0;
    if (!lambert)
    {
        coat = rtHitCoat(m);
        sheen = modelSheenOf(m);
    }
    RtHitDirect d;
    d.A = eSun + localE;
    d.B = d.A;
    d.C = 0;
    float3 sunB = eSun, sunC = 0;  // the sun's parts of B and C
    if (coat.cover > 0)
    {
        const float tS = 1 - modelCoatEms(coat, muS), tL = 1 - modelCoatEms(coat, muL);
        d.B = tS * eSun + tL * localE;
        // (the coat's returned light toward the sun and toward the sample by one inlined lookup: it stood three times
        // here, 770 instructions of the hit kernels at the size limit)
        float3 returned[2];
        [loop] for (uint k = 0; k < 2; ++k) returned[k] = modelCoatReturned(ms, coat, modelCoatRefractedCos(k == 0 ? muS : muL, coat.eta));
        d.C = (tS * eSun * returned[0] + tL * localE * returned[1]) / MODEL_PI;
        sunB = tS * eSun;
        sunC = tS * eSun * returned[0] / MODEL_PI;
    }
    const float NoV = max(dot(n, v), 1e-4);
    const float3 albedo = m.baseColor * ((1 - m.metallic) / MODEL_PI);
    const float plain = (1 - coat.cover) * (any(sheen.color > 0) ? modelSheenKeep(sheen, NoV) : 1.0);
    const float coated = coat.cover > 0 ? coat.cover * (1 - modelCoatEms(coat, NoV)) / (coat.eta * coat.eta) : 0.0;
    d.kA = plain * albedo;
    d.kB = coated * albedo;
    d.kC = coated;
    d.own = plain * albedo * d.A + coated * (albedo * d.B + d.C);
    d.ownSun = plain * albedo * eSun + coated * (albedo * sunB + sunC);
    return d;
}
float3 rtHitDirectFromMeans(RtHitDirect d, float3 mA, float3 mB, float3 mC) { return d.kA * mA + d.kB * mB + d.kC * mC; }

#endif
