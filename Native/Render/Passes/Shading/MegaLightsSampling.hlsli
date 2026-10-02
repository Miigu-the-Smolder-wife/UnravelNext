// Light sampling of shading.mega_lights for one point (MegaLights.hlsli; owner A): the point's record, a light's unshadowed
// radiance at it, the target weight and the stratified reservoir. Nothing here depends on the screen: m.ml.sample feeds it
// the pixel's surface and the froxel list's lights; a world-space consumer (S2's surface cache direct light) feeds it a
// point and the lights of R's world light grid (MegaLightsWorld.hlsli wraps that walk and the shadow ray).
// ML_AREA (default 1): area lights by their exact diffuse and LTC integrals; 0 compiles them out (scenes without them).
#ifndef UNX_MEGA_LIGHTS_SAMPLING_HLSLI
#define UNX_MEGA_LIGHTS_SAMPLING_HLSLI
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Shading/MegaLights.hlsli"
#ifndef ML_AREA
#define ML_AREA 1
#endif

struct MlPoint
{
    float3 offset;  // position - g_cameraPosition
    float3 n, v;    // unit normal (turned to v) and direction to the viewer (a point without a viewer: v = n)
    float NoV;
    float3 front, back;  // diffuse f_d on the viewer's side, and across the surface (Foliage)
    bool foliage, specular;
    float3 f0, compensation;
    float alpha;
#if ML_AREA
    float3x3 frame, frameBack, specularLtc;
    float3 specularAlbedo;
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
    p.f0 = modelF0(s);
    p.alpha = modelAlpha(s.roughness);
    p.compensation = 1 + p.f0 * (1 / modelDirectionalAlbedo(max(p.NoV, 1e-4), s.roughness) - 1);
#if ML_AREA
    p.frame = shShadingFrame(n, v, p.NoV);
    p.frameBack = float3x3(p.frame[0], -p.frame[1], -p.frame[2]);
    p.specularLtc = mul(shLtcInverse(ltcSrv, max(p.NoV, 1e-4), s.roughness), p.frame);
    p.specularAlbedo = shSpecularAlbedo(p.f0, max(p.NoV, 1e-4), s.roughness);
#endif
    return p;
}

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
float3 mlLightUnshadowed(MlPoint p, GpuLight light, uint lightIndex, uint stableMask)
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
            c = p.front * (SH_PI * shAreaIntegral(light, toCentre, p.frame, true));
            if (p.specular && !shSpecularInReflections(stableMask, lightIndex)) c += p.specularAlbedo * shAreaIntegral(light, toCentre, p.specularLtc, false);
        }
        if (p.foliage) c += p.back * (SH_PI * shAreaIntegral(light, toCentre, p.NoV > 0 ? p.frameBack : p.frame, true));
        return light.color * c * (light.intensity * window);
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
    float sumSquares;  // of the offered weights: sum^2 / sumSquares = the effective number of lights (mlWeightCap)
};
MlReservoir mlReservoirBegin(float u0, uint count)
{
    MlReservoir r;
    r.sum = 0;
    r.sumSquares = 0;
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
    r.sumSquares += w * w;
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

// The cap of a sample's weight (sum / its light's weight = 1 / its probability). The reference's cap is a constant (20:
// MaxShadingWeight; 5 for a light guided as hidden): a light is held back when it is chosen less than once in 20 - which
// a place lit by more than 20 comparable lights is for EVERY light, and the cap then removes light in proportion (the
// lobby's walls and floor, lit by tens of lights each, stood at a third of the path-traced reference, 2026-10-02; the
// ceiling beside few strong lights at 0.7). The cap is against a sample far less probable than its neighbours, so it is
// taken relative to the place's effective number of lights M = sum^2 / sum of squares (M equal lights: every weight is
// M): max(cap, scale x M x cap / 20). scale 2 leaves alone every light within half of the typical probability; 0 = the
// reference's constant.
float mlWeightCap(MlReservoir r, float cap, float scale)
{
    const float lights = r.sumSquares > 0 ? r.sum * r.sum / r.sumSquares : 1.0;
    return max(cap, scale * lights * cap * (1.0 / 20.0));
}
#endif
