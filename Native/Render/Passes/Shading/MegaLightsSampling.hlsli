// Light sampling of shading.mega_lights for one point (MegaLights.hlsli; owner A): the point's record, a light's unshadowed
// radiance at it, the target weight and the stratified reservoir. Nothing here depends on the screen: m.ml.sample feeds it
// the pixel's surface and the froxel list's lights; a world-space consumer (S2's surface cache direct light) feeds it a
// point and the lights of R's world light grid (MegaLightsWorld.hlsli wraps that walk and the shadow ray).
// ML_AREA (default 1): area lights by their exact diffuse and LTC integrals; 0 compiles them out (scenes without them).
// ML_SUBSURFACE (default 0; m.ml.sample sets 1): a point can carry the Subsurface class's model (mlPointSubsurface: its two
// specular lobes and the light through thin parts under point and spot lights, as ShadeOpaque's Subsurface variant shades
// them; under area lights one lobe at the two's average roughness - a sampling weight, the shading takes both - and the
// light through thin parts as the far side's cosine integral); world points have none. An eye's iris (MATERIAL_EYE,
// mlPointEye) is weighed as the shading lights it: its diffuse term on the iris plane with the caustic, so a light below
// the cornea's horizon and in front of the iris plane is offered. Its ray starts at the cornea's surface on the light's
// side, inside the eyeball: an eyeball that casts shadows hides that light from its own iris (an eyeball instance
// without InstanceCastShadow lets it through; the lids and the head still shadow it).
#ifndef UNX_MEGA_LIGHTS_SAMPLING_HLSLI
#define UNX_MEGA_LIGHTS_SAMPLING_HLSLI
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Shading/MegaLights.hlsli"
#ifndef ML_AREA
#define ML_AREA 1
#endif
#ifndef ML_SUBSURFACE
#define ML_SUBSURFACE 0
#endif

struct MlPoint
{
    float3 offset;  // position - g_cameraPosition
    float3 n, v;    // unit normal (turned to v) and direction to the viewer (a point without a viewer: v = n)
    float NoV;
    float3 front, back;  // diffuse f_d on the viewer's side, and across the surface (Foliage)
    bool foliage, specular;
    bool store;     // a world-space store of light that GI carries on (mlPointLambert): the light's indirect scale applies
    uint channels;  // the point's lighting channels (Scene.hlsli g_lightChannels; 7: no test)
    float3 f0, compensation;
    float alpha;
#if ML_AREA
    float3x3 frame, frameBack, specularLtc;
    float3 specularAlbedo;
#endif
#if ML_SUBSURFACE
    bool subsurface;       // the Subsurface class: the specular lobe is 'skin' (alpha, compensation, LTC: its average roughness)
    ModelSubsurface skin;
    float3 thin;           // f_d x transmission: the light through thin parts (0: none)
    ModelEye eye;          // an eye's pixel (mask 0: none): the iris plane and the caustic normal (MaterialModel.hlsli)
#if ML_AREA
    float3x3 frameIris;    // the iris plane's shading frame
#endif
#endif
};

// A shaded surface: the base model's diffuse and specular lobe (INTERFACES 8.1). ltcSrv: M's LTC table (area lights).
MlPoint mlPointOf(ModelSurface s, float3 offset, float3 n, float3 v, uint ltcSrv)
{
    MlPoint p;
    p.offset = offset;
    p.n = n;
    p.v = v;
    p.NoV = dot(n, v);
    const float3 diffuse = s.baseColor * ((1 - s.metallic) / SH_PI);
    p.foliage = s.cls == MATERIAL_FOLIAGE;
    p.front = p.foliage ? diffuse * (1 - s.transmission) : diffuse;
    p.back = p.foliage ? diffuse * s.transmission : 0;
    p.specular = true;
    p.store = false;
    p.channels = 7u;
    p.f0 = modelF0(s);
    p.alpha = modelAlpha(s.roughness);
    p.compensation = 1 + p.f0 * (1 / modelDirectionalAlbedo(max(p.NoV, 1e-4), s.roughness) - 1);
#if ML_AREA
    p.frame = shShadingFrame(n, v, p.NoV);
    p.frameBack = float3x3(p.frame[0], -p.frame[1], -p.frame[2]);
    p.specularLtc = mul(shLtcInverse(ltcSrv, max(p.NoV, 1e-4), s.roughness), p.frame);
    p.specularAlbedo = shSpecularAlbedo(p.f0, max(p.NoV, 1e-4), s.roughness);
#endif
#if ML_SUBSURFACE
    p.subsurface = false;
    p.skin = (ModelSubsurface)0;
    p.thin = 0;
    p.eye = (ModelEye)0;
#if ML_AREA
    p.frameIris = p.frame;
#endif
#endif
    return p;
}

#if ML_SUBSURFACE
// Turns the point of a Subsurface-class surface (mlPointOf's) into that class's model: 'skin' = its lobes
// (modelSubsurfaceOf at the surface's roughness).
void mlPointSubsurface(inout MlPoint p, ModelSurface s, ModelSubsurface skin, uint ltcSrv)
{
    p.subsurface = true;
    p.skin = skin;
    p.thin = s.transmission > 0 ? s.baseColor * ((1 - s.metallic) / SH_PI) * s.transmission : 0;
    p.alpha = modelAlpha(skin.roughness);
    p.compensation = 1 + p.f0 * (1 / modelDirectionalAlbedo(max(p.NoV, 1e-4), skin.roughness) - 1);
#if ML_AREA
    p.specularLtc = mul(shLtcInverse(ltcSrv, max(p.NoV, 1e-4), skin.roughness), p.frame);
    p.specularAlbedo = shSpecularAlbedo(p.f0, max(p.NoV, 1e-4), skin.roughness);
#endif
}
// The point of an eye's pixel (a Subsurface point): 'eye' = modelEyeOf of the pixel's eye word.
void mlPointEye(inout MlPoint p, ModelEye eye)
{
    p.eye = eye;
#if ML_AREA
    p.frameIris = shShadingFrame(eye.iris, p.v, dot(eye.iris, p.v));
#endif
}
#endif

// A Lambert point of a world-space store (no viewer, no specular): its radiance is albedo / pi x irradiance.
MlPoint mlPointLambert(float3 worldPos, float3 n, float3 albedo)
{
    MlPoint p;
    p.offset = worldPos - g_cameraPosition;
    p.n = n;
    p.v = n;
    p.NoV = 1;
    p.front = albedo / SH_PI;
    p.back = 0;
    p.foliage = false;
    p.specular = false;
    p.store = true;
    p.channels = 7u;
    p.f0 = 0;
    p.compensation = 1;
    p.alpha = 1;
#if ML_AREA
    p.frame = shShadingFrame(n, n, 1);
    p.frameBack = float3x3(p.frame[0], -p.frame[1], -p.frame[2]);
    p.specularLtc = p.frame;
    p.specularAlbedo = 0;
#endif
    return p;
}

// The light's unshadowed radiance leaving the point toward v (before exposure): punctual lights exactly, area lights by
// their diffuse and LTC integrals. stableMask: B2's mask of area lights whose specular R's reflections carry (UNX_NONE: none).
// The light's components are in it (Scene.hlsli): its diffuse and specular scales, its window with the view's fade, 0
// for a light in none of the point's channels; a store's point (mlPointLambert) takes the light's indirect scale too.
float3 mlLightUnshadowedDirect(MlPoint p, GpuLight light, uint lightIndex, uint stableMask);
float3 mlLightUnshadowed(MlPoint p, GpuLight light, uint lightIndex, uint stableMask)
{
    g_lightChannels = p.channels;
    const float3 radiance = mlLightUnshadowedDirect(p, light, lightIndex, stableMask);
    g_lightChannels = 7u;
    g_shLightSpecular = 1;  // (the light's specular scale is in 'radiance': not left for the caller's other lobes)
    return p.store ? radiance * lightIndirectScale(light) : radiance;
}
float3 mlLightUnshadowedDirect(MlPoint p, GpuLight light, uint lightIndex, uint stableMask)
{
    if (lightType(light) > LIGHT_SPOT)
    {
#if ML_AREA
        const float3 toCentre = (light.position - g_cameraPosition) - p.offset;
        const float window = shAreaWindow(light, toCentre);
        if (window <= 0) return 0;
        float3 c = 0;
        if (p.NoV > 0)
        {
            float Id = shAreaIntegral(light, toCentre, p.frame, true);
#if ML_SUBSURFACE
            // an eye's iris: the plane's cosine integral with the caustic at the light's centre, on the mask's share
            if (p.eye.mask > 0) Id = lerp(Id, shAreaIntegral(light, toCentre, p.frameIris, true) * modelEyeCaustic(p.eye, normalize(toCentre)), p.eye.mask);
#endif
            c = p.front * (SH_PI * Id);
            if (p.specular && !shSpecularInReflections(stableMask, lightIndex)) c += p.specularAlbedo * shAreaIntegral(light, toCentre, p.specularLtc, false);
        }
        // what crosses the surface: Foliage's transmission, a Subsurface point's light through thin parts
        float3 across = p.foliage ? p.back : 0;
#if ML_SUBSURFACE
        if (p.subsurface) across = p.thin;
#endif
        if (any(across > 0)) c += across * (SH_PI * shAreaIntegral(light, toCentre, p.NoV > 0 ? p.frameBack : p.frame, true));
        return shAreaColor(light, toCentre) * c * (light.intensity * window);
#else
        return 0;
#endif
    }
    const float3 toLight = (light.position - g_cameraPosition) - p.offset;
    float3 l;
    const float3 E = shPunctualIlluminance(light, toLight, l);
    const float cosL = dot(p.n, l);
    if (all(E == 0)) return 0;
    float3 f = 0;
#if ML_SUBSURFACE
    if (p.subsurface)
    {
        if (p.NoV > 0 && cosL > 0) f = p.front + shSpecularSubsurface(p.f0, p.skin, p.compensation, p.n, p.v, l, p.NoV, cosL);
        else if (p.NoV * cosL < 0) f = p.thin * (modelSubsurfaceThin(abs(cosL), p.v, l) / abs(cosL));
        float3 radiance = f * abs(cosL);
        // an eye's iris: the light on the iris plane in place of the cornea's surface, whatever the surface's cosine
        if (p.eye.mask > 0 && p.NoV > 0) radiance += p.front * (modelEyeCosine(p.eye, cosL, l) - max(cosL, 0.0));
        return radiance * E;
    }
#endif
    if (p.NoV > 0 && cosL > 0) f = p.front + (p.specular ? shSpecular(p.f0, p.alpha, p.compensation, p.n, p.v, l, p.NoV, cosL) : 0.0);
    else if (p.foliage && p.NoV * cosL < 0) f = p.back;
    return f * E * abs(cosL);
}

// The sampling weight of a light whose unshadowed exposed luminance at the point is 'lum': log2(1 + lum m(lum)), m the
// smooth cut under the minimum sample weight (0: the light is not offered).
float mlTargetWeight(float lum, float minWeight) { return log2(1 + lum * mlFalloffMask(lum, minWeight)); }

// N strata over one random number; each offer keeps a stratum's light with probability sum / (sum + w), and the
// stratum's number stays uniform either way. After the last offer a kept light's sample weight is sum / its weight.
struct MlReservoir
{
    uint light[ML_MAX_SAMPLES];
    float weight[ML_MAX_SAMPLES];
    bool wasVisible[ML_MAX_SAMPLES];
    float u[ML_MAX_SAMPLES];
    float sum;
};
MlReservoir mlReservoirBegin(float u0, uint count)
{
    MlReservoir r;
    r.sum = 0;
    for (uint i = 0; i < ML_MAX_SAMPLES; ++i)
    {
        r.light[i] = ML_LIGHT_NONE;
        r.weight[i] = 0;
        r.wasVisible[i] = true;
        r.u[i] = (u0 + i) / max(count, 1u);
    }
    return r;
}
void mlOffer(inout MlReservoir r, uint count, float w, uint light, bool wasVisible)
{
    const float keep = r.sum / (r.sum + w);
    r.sum += w;
    for (uint i = 0; i < count; ++i)
    {
        if (r.u[i] < keep) r.u[i] /= keep;
        else
        {
            r.u[i] = (r.u[i] - keep) / (1 - keep);
            r.light[i] = light;
            r.weight[i] = w;
            r.wasVisible[i] = wasVisible;
        }
        r.u[i] = clamp(r.u[i], 0.0, 0.99999994);
    }
}
#endif
