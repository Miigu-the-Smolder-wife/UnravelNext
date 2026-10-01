// shading.mega_lights for points that are not screen pixels (owner A; consumer: S2's world-space direct-light store, the
// counterpart of Unreal's stochastic surface-cache direct lighting): light samples at a world point from R's world light
// grid, and the shadow ray of a sample. For ray generation libraries: include RayTracing/RayShaders.hlsli and
// RayTracing/HitLocalLights.hlsli first (the grid's fetchers and rtVisible).
//   MlPoint p = mlPointLambert(position, normal, albedo);            // or mlPointOf(surface, ...) with a viewer
//   MlWorldSamples s = mlWorldSamples(scene, p, 4, u0, minWeight, exposure, stableMask);
//   for i < s.count: if (mlSampleVisible(scene, position, normal, s.light[i], uv_i, bias, normalBias, endBias))
//       radiance += mlLightUnshadowed(p, loadLight(s.light[i]), s.light[i], stableMask) * s.weight[i];   // weight = 1 / (N x probability)
// (consecutive strata often hold the same light: one ray may serve them when the light is not an area light.)
#ifndef UNX_MEGA_LIGHTS_WORLD_HLSLI
#define UNX_MEGA_LIGHTS_WORLD_HLSLI
#include "Passes/Shading/MegaLightsSampling.hlsli"

struct MlWorldSamples
{
    uint count;                    // samples with a light (<= N)
    uint light[ML_MAX_SAMPLES];    // scene light index
    float weight[ML_MAX_SAMPLES];  // sum of the offered weights / (the light's weight x N)
    bool castShadow[ML_MAX_SAMPLES];
};

// N samples (1, 2 or 4) at the point from the lights of its cell of R's light grid (every light whose range reaches the
// cell; FX particle lights are not in the grid). exposure: the scale that makes the minimum sample weight meaningful
// (g_exposure for values that reach the screen).
MlWorldSamples mlWorldSamples(RtSceneSrvs scene, MlPoint p, uint n, float u0, float minWeight, float exposure, uint stableMask)
{
    MlWorldSamples o;
    o.count = 0;
    uint i;
    for (i = 0; i < ML_MAX_SAMPLES; ++i)
    {
        o.light[i] = ML_LIGHT_NONE;
        o.weight[i] = 0;
        o.castShadow[i] = false;
    }
    if (scene.pad == 0xFFFFFFFFu) return o;
    g_rtLightData = scene.pad;
    ByteAddressBuffer b = ResourceDescriptorHeap[scene.pad];
    const RtLightGrid grid = b.Load<RtLightGrid>(0);
    const uint cell = rtLightCell(grid, p.offset + g_cameraPosition);
    if (cell == ~0u) return o;
    MlReservoir r = mlReservoirBegin(u0, n);
    const uint k1 = rtLightCellStart(cell + 1);
    for (uint k = rtLightCellStart(cell); k < k1; ++k)
    {
        const uint li = rtLightCellLight(k);
        const float w = mlTargetWeight(mlLuminance(mlLightUnshadowed(p, loadLight(li), li, stableMask)) * exposure, minWeight);
        if (w > 0) mlOffer(r, n, w, li, true);
    }
    for (i = 0; i < n; ++i)
    {
        if (r.light[i] == ML_LIGHT_NONE) continue;
        o.light[o.count] = r.light[i];
        o.weight[o.count] = r.sum / (r.weight[i] * n);
        o.castShadow[o.count] = lightCastsShadow(loadLight(r.light[i]));
        ++o.count;
    }
    return o;
}

RtLight mlRtLight(GpuLight g)
{
    RtLight l;
    l.position = g.position;
    l.type = lightType(g);
    l.forward = g.forward;
    l.intensity = g.intensity;
    l.right = g.right;
    l.range = max(g.range, 1e-3);
    l.up = cross(g.forward, g.right);
    l.spotScale = g.spotScale;
    l.color = g.color;
    l.spotOffset = g.spotOffset;
    l.size = g.size;
    l.castShadow = lightCastsShadow(g) ? 1u : 0u;
    l.pad = 0;
    return l;
}

// One shadow ray from the point (x world, n its normal) toward the sample's point on the light ((u, v): the centre of point
// and spot lights whatever they are): false when blocked or when that point sends nothing to x. Shadow casters only
// (RT_MASK_SHADOW); the ray ends before the light by the light's own end bias (scene::Light::rayEndBias, as Unreal's
// per-light Ray End Bias: a light inside a housing or a trough) or, when it has none, by endBias (the engine's default).
bool mlSampleVisible(RtSceneSrvs scene, float3 x, float3 n, uint light, float2 uv, float bias, float normalBias, float endBias)
{
    const GpuLight g = loadLight(light);
    endBias = lightRayEndBias(g, endBias);
    RtLightSample ls;
    if (!rtLightSample(mlRtLight(g), x, uv.x, uv.y, ls)) return false;
    RayDesc ray;
    ray.Origin = x + n * (dot(n, ls.wi) < 0 ? -normalBias : normalBias);
    ray.Direction = ls.wi;
    ray.TMin = bias;
    ray.TMax = max(bias, ls.distance - endBias);
    return rtVisible(scene, ray, RT_MASK_SHADOW);
}
#endif
