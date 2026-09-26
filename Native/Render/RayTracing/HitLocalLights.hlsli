// Local lights at ray hits (FEATURES_GAME 12, B2): one light per hit by next-event estimation with the reference path
// tracer's own estimator code (Reference/GpuTracer/shared/Lights.hlsli: range window, importance, the candidate choice
// over RayScene's uniform cell grid, per-type sampling - solid angle for spheres, area for rects / disks / tubes, delta for
// point and spot lights), so the renderer's hits and the reference agree term by term. The light data is RayScene's
// local-light grid (RtSceneSrvs.pad = its raw SRV, 0xFFFFFFFF: no local lights): header (RtLightGrid, then the offsets of
// the lights, cell starts and cell lights), the lights as RtLight.
// Estimate of the hit's outgoing radiance from local lights: f(v, wi) L cos / (pdf x P(light)), times the light's
// visibility (one shadow ray; lights that cast no shadow: 1, as in the reference). Unbiased for the sum over the cell's
// lights; the hit's history (GI texels, reflection time integration) averages the one-sample noise.
#ifndef UNX_RT_HIT_LOCAL_LIGHTS_HLSLI
#define UNX_RT_HIT_LOCAL_LIGHTS_HLSLI
#include "RayTracing/RayScene.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "../../../Reference/GpuTracer/shared/Lights.hlsli"

static uint g_rtLightData = 0xFFFFFFFFu;
RtLight rtLightFetch(uint i)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[g_rtLightData];
    return b.Load<RtLight>(b.Load(48) + i * 96);
}
uint rtLightCellStart(uint cell)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[g_rtLightData];
    return b.Load(b.Load(52) + cell * 4);
}
uint rtLightCellLight(uint k)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[g_rtLightData];
    return b.Load(b.Load(56) + k * 4);
}

// One light sample at x: the light (index, whether it casts shadows), the direction and distance to the sampled point,
// and L / (pdf P(light)) - the estimate's weight before the BRDF, cosine and visibility. valid = false: no light's range
// reaches x (or the sample has no contribution).
struct RtLocalSample
{
    bool valid;
    bool castShadow;
    float3 wi;
    float distance;
    float3 weight;
};
RtLocalSample rtLocalLightSample(RtSceneSrvs scene, float3 x, float u0, float u1, float u2)
{
    RtLocalSample o = (RtLocalSample)0;
    if (scene.pad == 0xFFFFFFFFu) return o;
    g_rtLightData = scene.pad;
    ByteAddressBuffer b = ResourceDescriptorHeap[scene.pad];
    const RtLightGrid grid = b.Load<RtLightGrid>(0);
    const uint cell = rtLightCell(grid, x);
    const float total = rtLightTotal(cell, x);
    if (!(total > 0)) return o;
    float probability;
    const uint li = rtLightChoose(cell, x, total, u0, probability);
    if (li == ~0u || !(probability > 0)) return o;
    const RtLight l = rtLightFetch(li);
    RtLightSample s;
    if (!rtLightSample(l, x, u1, u2, s) || !(s.pdf > 0)) return o;
    o.valid = true;
    o.castShadow = l.castShadow != 0;
    o.wi = s.wi;
    o.distance = s.distance;
    o.weight = s.L / (s.pdf * probability);
    return o;
}

// The model's BRDF x cosine toward wi for the hit (INTERFACES 8.1: diffuse albedo / pi, the GGX lobe with compensation,
// foliage transmission from behind). diffuseOnly: the GI cache's hit shading (its outgoing radiance is diffuse).
float3 rtLocalLightBrdfCos(GpuMaterial m, float3 n, float3 v, float3 wi, bool diffuseOnly)
{
    ModelSurface s;
    s.cls = m.classFlags & 0xFFu;
    s.baseColor = m.baseColor;
    s.roughness = m.roughness;
    s.metallic = m.metallic;
    s.specular = m.specular;
    s.transmission = m.transmission;
    const float NoL = dot(n, wi);
    const float3 albedo = s.baseColor * ((1 - s.metallic) / MODEL_PI);
    const bool foliage = s.cls == MATERIAL_FOLIAGE;
    if (NoL <= 0) return foliage ? albedo * s.transmission * -NoL : 0;
    const float3 diffuse = (foliage ? albedo * (1 - s.transmission) : albedo) * NoL;
    if (diffuseOnly) return diffuse;
    const float NoV = max(dot(n, v), 1e-4);
    const float alpha = modelAlpha(s.roughness);
    const float3 f0 = modelF0(s);
    const float3 compensation = 1 + f0 * (1 / modelDirectionalAlbedo(NoV, s.roughness) - 1);
    return diffuse + shSpecular(f0, alpha, compensation, n, v, wi, NoV, NoL) * NoL;
}

// Shadow ray origin: the hit stepped off its surface on the side the light is on (geometric normal; leaves transmit),
// and the ray's length to just before the sampled point.
RayDesc rtLocalShadowRay(float3 position, float3 geometricNormal, RtLocalSample l, float bias)
{
    RayDesc r;
    const float side = dot(geometricNormal, l.wi) > 0 ? 1.0 : -1.0;
    r.Origin = position + side * geometricNormal * bias;
    r.Direction = l.wi;
    r.TMin = 0;
    r.TMax = max(l.distance * (1 - 1e-4) - 2 * bias, 0.0);
    return r;
}
#endif
