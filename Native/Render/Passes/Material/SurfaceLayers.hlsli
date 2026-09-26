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
// Wetness (the water film: a dielectric coat over the darkened substrate) comes with M's clearcoat layer (A9) and is not
// applied here yet.
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
    float snow = st.snow;
    if (in_.weather != 0xFFFFFFFFu)
    {
        const WeatherRecord w = weatherLoad(in_.weather);
        if (w.snowDepth > 0) snow += rainExposure(in_.weather, world) * w.snowDepth;
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
