// Surface state layers of the material (FEATURES_GAME 5.2 / 13, WORLD_VFX 10.4; A7 join, M): the VFX surface state field
// (E's SurfaceState.hlsli: wet, scorch, frost, dust, blood in [0, 1], a signed snow height change in m) and the World's
// weather (S's WeatherField.hlsli: snow depth, rain exposure) become upper layers of the pixel material - the layer rule
// the decals use (every parameter blends towards the layer's by its coverage), so no new shading model:
//   base -> (decals) -> blood -> scorch -> dust -> frost -> snow
// Blood, scorch, dust and frost are thin stains and deposits that follow the surface's relief (the shading normal and
// slope variance stay); snow is a thick layer (its normal is the geometric normal, its variance the curvature part).
// Coverage: the channel value for the thin layers; snow d = max(0, exposure x weather snow depth + field snow change),
// coverage (1 - exp(-d / SL_SNOW_EXTINCTION)) x slope factor (full up to SL_SNOW_FULL_DEG from up, none from
// SL_SNOW_NONE_DEG: snow slides off steeper faces).
// Wetness: a water film over the surface - the field's wet channel, and the weather's wetness where the rain reaches
// (WeatherFrame::wetness x S's rain exposure: the one weather record). The film keeps light in: what the substrate
// reflects (albedo a) meets the film's surface from inside and a share p of it returns to the substrate, so the wet
// albedo is (1 - R) a (1 - p) / (1 - p a) (Lekner & Dorf 1988) with R = 0.02 the film's reflectance at normal incidence
// and p = 1 - (1 - 0.066) / n^2 = 0.472 for water (n = 1.33; 0.066: the film's reflectance for diffuse light from
// outside) - a = 0.3 becomes 0.18. The film's own surface is smooth: the lobe's roughness goes toward SL_WET_ROUGHNESS
// with the square of the wetness (a damp surface keeps its relief, a soaked one shines). A metal's colour is its
// surface reflection and stays. The film as its own layer (the clearcoat of ior 1.33, A9) is M's to add in the resolve;
// this is the single-lobe form until then.
// Shores (S's weather record: WeatherField.hlsli shoreWetness; shading.water_shore_wet): the frame's water bodies wet
// what they touch - the surfaces under a basin's or the sea's still level and a band above it. A surface in the water
// takes the film's albedo (its pores hold water) and keeps its own lobe: the film's surface is the water's, shaded by W.
// A kernel at the size limit compiles without them (UNX_LAYERS_WITHOUT_SHORE).
// Layer materials [authoring values of measured order; constants until a game needs its own]: soot (charcoal albedo
// 0.02-0.05), mineral dust (0.3-0.45), fresh blood (red ~0.3, green/blue ~0.01, liquid gloss), hoarfrost (0.7-0.8),
// fresh snow (0.85-0.95 visible).
// Cost [expected, FEATURES_GAME 5.2]: per resolved pixel one field lookup (8 trilinear corners, a hash probe per brick
// met) + one rain-exposure tap when it snows: ~0.03 ms at 4K.
#ifndef UNX_MATERIAL_SURFACE_LAYERS_HLSLI
#define UNX_MATERIAL_SURFACE_LAYERS_HLSLI
#include "Passes/Decal/SurfaceState.hlsli"
#include "Passes/Atmosphere/WeatherField.hlsli"

#define SL_SNOW_EXTINCTION 0.01   // m: e-folding depth of the substrate's visibility under snow (visible light in fresh snow)
#define SL_SNOW_FULL_DEG 40.0     // snow lies fully on faces up to this angle from up ...
#define SL_SNOW_NONE_DEG 60.0     // ... and not at all from this one (angle of repose of dry snow)
#define SL_WET_INTERNAL 0.472     // p: the share of the substrate's light the film's surface returns (water, n = 1.33)
#define SL_WET_ROUGHNESS 0.08     // the lobe's roughness under a full film

struct SurfaceLayerInputs
{
    uint surfaceConstants, surfaceTable, surfacePool;  // E's field (SURFACE_NONE: none)
    uint weather;                                       // S's weather record SRV (UNX_NONE: none)
};

struct SurfaceLayerMaterial
{
    float3 baseColor;
    float roughness, metallic;
    float3 normal;
    float variance;
};

void slBlend(inout SurfaceLayerMaterial m, float a, float3 baseColor, float roughness, bool thick, float3 geometricNormal, float geometricVariance)
{
    if (!(a > 0)) return;
    m.baseColor = lerp(m.baseColor, baseColor, a);
    m.roughness = lerp(m.roughness, roughness, a);
    m.metallic = lerp(m.metallic, 0.0f, a);
    if (thick)
    {
        m.normal = normalize(lerp(m.normal, geometricNormal, a));
        m.variance = lerp(m.variance, geometricVariance, a);
    }
}

// The layers at a world position (world: camera position + camera-relative hit). geometricNormal: unit, the side the pixel
// shades; up: world +y.
void surfaceLayersApply(SurfaceLayerInputs in_, float3 world, float3 geometricNormal, float geometricVariance, inout SurfaceLayerMaterial m)
{
    SurfaceSample st = (SurfaceSample)0;
    if (in_.surfaceConstants != SURFACE_NONE)
    {
        SurfaceContext c;
        c.constants = in_.surfaceConstants;
        c.table = in_.surfaceTable;
        c.pool = in_.surfacePool;
        st = surfaceStateAt(c, world);
    }
    float snow = st.snow, wet = saturate(st.wet);
    if (in_.weather != 0xFFFFFFFFu)
    {
        const WeatherRecord w = weatherLoad(in_.weather);
        if (w.snowDepth > 0 || w.wetness > 0)
        {
            const float exposure = rainExposure(in_.weather, world);
            snow += exposure * w.snowDepth;
            wet = max(wet, saturate(w.wetness) * exposure);
        }
    }
    float under = 0;
#ifndef UNX_LAYERS_WITHOUT_SHORE
    if (in_.weather != 0xFFFFFFFFu) wet = max(wet, shoreWetness(in_.weather, world, under));
#endif
    if (wet > 0)
    {
        const float3 a = m.baseColor;
        const float3 film = 0.98f * a * (1.0f - SL_WET_INTERNAL) / (1.0f - SL_WET_INTERNAL * a);
        m.baseColor = lerp(a, lerp(film, a, m.metallic), wet);
        m.roughness = lerp(m.roughness, min(m.roughness, SL_WET_ROUGHNESS), wet * wet * (1.0f - under));
    }
    slBlend(m, saturate(st.blood), float3(0.30f, 0.012f, 0.010f), 0.2f, false, geometricNormal, geometricVariance);
    slBlend(m, saturate(st.scorch), float3(0.03f, 0.03f, 0.03f), 0.9f, false, geometricNormal, geometricVariance);
    slBlend(m, saturate(st.dust), float3(0.42f, 0.37f, 0.31f), 0.95f, false, geometricNormal, geometricVariance);
    slBlend(m, saturate(st.frost), float3(0.72f, 0.76f, 0.80f), 0.55f, false, geometricNormal, geometricVariance);
    if (snow > 0)
    {
        const float cosFull = cos(radians(SL_SNOW_FULL_DEG)), cosNone = cos(radians(SL_SNOW_NONE_DEG));
        const float slope = saturate((geometricNormal.y - cosNone) / (cosFull - cosNone));
        const float a = (1.0f - exp(-snow / SL_SNOW_EXTINCTION)) * slope;
        slBlend(m, a, float3(0.90f, 0.91f, 0.93f), 0.65f, true, geometricNormal, geometricVariance);
    }
}
#endif
