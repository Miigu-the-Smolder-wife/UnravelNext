// Local lights at ray hits (FEATURES_GAME 12, B2): one light per hit by next-event estimation with the reference path
// tracer's own estimator code (Reference/GpuTracer/shared/Lights.hlsli: range window, importance, the candidate choice
// over RayScene's uniform cell grid, per-type sampling - solid angle for spheres, area for rects / disks / tubes, delta for
// point and spot lights), so the renderer's hits and the reference agree term by term. The light data is RayScene's
// local-light grid (RtSceneSrvs.pad = its raw SRV, 0xFFFFFFFF: no local lights): header (RtLightGrid, then the offsets of
// the lights, cell starts and cell lights), the lights as RtLight.
// FX particle lights (A3) join the choice (rtFxWeight below).
// Estimate of the hit's outgoing radiance from local lights: f(v, wi) L cos / (pdf x P(light)), times the light's
// visibility (one shadow ray; lights that cast no shadow: 1, as in the reference). Unbiased for the sum over the cell's
// lights; the hit's history (GI texels, reflection time integration) averages the one-sample noise.
#ifndef UNX_RT_HIT_LOCAL_LIGHTS_HLSLI
#define UNX_RT_HIT_LOCAL_LIGHTS_HLSLI
#include "RayTracing/RayScene.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Material/MaterialTextures.hlsli"
#include "../../../Reference/GpuTracer/shared/Lights.hlsli"
#include "Passes/Lights/LightFunction.hlsli"

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

// FX particle lights (A3, S_STATUS_KO.md 10): gpu::Light point records at the scene light buffer's tail [N, N + F), no
// shadows, in groups of 32 consecutive lights (FxLightGroups.hlsl; the light data header's word 21, 0xFFFFFFFF: none):
// { F, G } then per group its bounding sphere (of the lights' range spheres), sum of phi = intensity x luminance, first
// light and count. At x a group weighs w_g = sum phi / max(|x - centre|^2, 1 m^2) inside its sphere and 0 outside (where
// none of its lights reaches: exact), W = sum w_g; the FX set is taken with probability W / (W_grid + W) (the grid's
// total is the same kind of quantity, intensity x luminance / d^2), a group with w_g / W, then one of its lights with
// its exact importance at x (rtLightImportance) over the group's sum. Every light that reaches x has a nonzero
// probability, so the estimate stays unbiased; the weights put the choices near x (a power-only choice starved near FX
// lights beside a bright far one: FxLightHits). Cost per hit: G sphere tests twice (sum, then choice) + 32 records;
// G <= 1024 at the 32,768-light limit.
RtLight rtFxLight(uint j)
{
    const GpuLight g = loadLight(g_lightCount + j);
    RtLight l;
    l.position = g.position;
    l.type = kRtLightPoint;
    l.forward = g.forward;
    l.intensity = g.intensity;
    l.right = g.right;
    l.range = max(g.range, 1e-3);
    l.up = cross(g.forward, g.right);
    l.spotScale = g.spotScale;
    l.color = g.color;
    l.spotOffset = g.spotOffset;
    l.size = g.size;
    l.castShadow = 0;
    l.pad = 0;
    return l;
}
float rtFxGroupWeight(ByteAddressBuffer f, uint g, float3 x)
{
    const float4 sphere = asfloat(f.Load4(16 + 32 * g));
    const float3 d = x - sphere.xyz;
    const float d2 = dot(d, d);
    return d2 < sphere.w * sphere.w ? asfloat(f.Load(32 + 32 * g)) / max(d2, 1.0) : 0.0;
}
// The FX set's weight W at x (0: none reaches) and the groups' SRV.
float rtFxWeight(ByteAddressBuffer header, float3 x, out uint groups)
{
    groups = header.Load(84);
    if (groups == 0xFFFFFFFFu) return 0;
    ByteAddressBuffer f = ResourceDescriptorHeap[groups];
    const uint G = f.Load(4);
    float W = 0;
    [loop] for (uint g = 0; g < G; ++g) W += rtFxGroupWeight(f, g, x);
    return W;
}
// A light j (0-based in the tail) with its probability given the FX set: w_g / W x importance_j / sum over the group;
// ~0u when the chosen group's lights do not reach x (the sample then has no contribution).
uint rtFxChoose(uint groups, float3 x, float W, float u, out float probability)
{
    ByteAddressBuffer f = ResourceDescriptorHeap[groups];
    const uint G = f.Load(4);
    probability = 0;
    const float target = u * W;
    float cum = 0, wg = 0;
    uint g = ~0u;
    [loop] for (uint k = 0; k < G; ++k)
    {
        const float w = rtFxGroupWeight(f, k, x);
        if (w <= 0) continue;
        cum += w;
        g = k, wg = w;
        if (cum > target) break;
    }
    if (g == ~0u) return ~0u;
    const uint2 range = f.Load2(32 + 32 * g + 4);  // first, count
    float total = 0;
    for (uint k = 0; k < range.y; ++k) total += rtLightImportance(rtFxLight(range.x + k), x);
    if (!(total > 0)) return ~0u;
    // The draw's remainder inside the chosen group's interval: uniform in [0, 1) given the group.
    const float t2 = saturate((target - (cum - wg)) / wg) * total;
    float c2 = 0;
    uint j = ~0u;
    float imp = 0;
    for (uint k = 0; k < range.y; ++k)
    {
        const float i = rtLightImportance(rtFxLight(range.x + k), x);
        if (i <= 0) continue;
        c2 += i;
        j = range.x + k, imp = i;
        if (c2 > t2) break;
    }
    probability = wg / W * imp / total;
    return j;
}

// One light sample at x: the light (index, whether it casts shadows), the direction and distance to the sampled point,
// and L / (pdf P(light)) - the estimate's weight before the BRDF, cosine and visibility. valid = false: no light's range
// reaches x (or the sample has no contribution). Point and spot lights carry E's light function (A8, LightFunction.hlsli;
// word 20 of the grid header, 0xFFFFFFFF: none) toward x, at the angular footprint footprintWidth / distance (the hit's ray
// cone width, or larger for coarse GI hits); it multiplies the weight (the light choice is unchanged: still unbiased).
struct RtLocalSample
{
    bool valid;
    bool castShadow;
    uint light;  // the scene light sampled
    float3 wi;
    float distance;
    float3 weight;
};
RtLocalSample rtLocalLightSample(RtSceneSrvs scene, float3 x, float u0, float u1, float u2, float footprintWidth)
{
    RtLocalSample o = (RtLocalSample)0;
    if (scene.pad == 0xFFFFFFFFu) return o;
    g_rtLightData = scene.pad;
    ByteAddressBuffer b = ResourceDescriptorHeap[scene.pad];
    const RtLightGrid grid = b.Load<RtLightGrid>(0);
    const uint cell = rtLightCell(grid, x);
    const float total = rtLightTotal(cell, x);
    uint cdf;
    const float fxW = rtFxWeight(b, x, cdf);
    if (!(total + fxW > 0)) return o;
    const float pFx = fxW / (total + fxW);
    float probability;
    uint li;
    RtLight l;
    if (u0 < pFx)
    {
        const uint j = rtFxChoose(cdf, x, fxW, u0 / pFx, probability);
        if (j == ~0u) return o;
        probability *= pFx;
        li = g_lightCount + j;
        l = rtFxLight(j);
    }
    else
    {
        li = rtLightChoose(cell, x, total, pFx > 0 ? (u0 - pFx) / (1 - pFx) : u0, probability);
        if (li == ~0u) return o;
        probability *= 1 - pFx;
        l = rtLightFetch(li);
    }
    if (!(probability > 0)) return o;
    RtLightSample s;
    if (!rtLightSample(l, x, u1, u2, s) || !(s.pdf > 0)) return o;
    o.valid = true;
    o.castShadow = l.castShadow != 0;
    o.light = li;
    o.wi = s.wi;
    o.distance = s.distance;
    o.weight = s.L / (s.pdf * probability);
    if (l.type == kRtLightPoint || l.type == kRtLightSpot)
        o.weight *= lightFunction(b.Load(80), li, l.forward, l.right, -s.wi, footprintWidth / max(s.distance, 1e-4), g_time);
    return o;
}

// The model's BRDF x cosine toward wi for the hit (INTERFACES 8.1: diffuse albedo / pi, the GGX lobe with compensation,
// foliage transmission from behind). diffuseOnly: Lambert hits (GiAnalytic's closed forms, gi.experiment_disable 1024).
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

// ---- Emissive triangles (RayScene's list, the light data header's word 15): next-event estimation for the GI cache's
// update rays with multiple importance sampling against their texel sampling (FEATURES_GAME 12 (ii)). A triangle is chosen
// by its running weight (emission luminance x world area at build), a point on it uniformly by area; the pdf in solid
// angle at x is P(triangle) / area_now x d^2 / |cos at the emitter|, the same function the MIS weight of a texel ray that
// hits the triangle uses (rtEmissivePdf). Front faces emit; two-sided materials both.
uint rtEmissiveList(RtSceneSrvs scene)
{
    if (scene.pad == 0xFFFFFFFFu) return 0xFFFFFFFFu;
    ByteAddressBuffer b = ResourceDescriptorHeap[scene.pad];
    return b.Load(60);
}
struct RtEmissiveTriangle
{
    float3 p0, p1, p2;
    float2 uv0, uv1, uv2;
    GpuMaterial m;
};
RtEmissiveTriangle rtEmissiveTriangleOf(uint sceneInstance, uint meshTriangle, uint submesh)
{
    RtEmissiveTriangle o;
    const GpuInstance inst = loadInstance(sceneInstance);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const uint3 idx = loadTriangle(mesh, meshTriangle);
    const VertexData a = loadVertex(mesh, idx.x), b = loadVertex(mesh, idx.y), c = loadVertex(mesh, idx.z);
    o.p0 = transformPoint(inst.objectToWorld, a.position);
    o.p1 = transformPoint(inst.objectToWorld, b.position);
    o.p2 = transformPoint(inst.objectToWorld, c.position);
    o.uv0 = a.uv;
    o.uv1 = b.uv;
    o.uv2 = c.uv;
    o.m = loadMaterial(instanceMaterial(inst, loadSubmesh(mesh.submeshOffset + submesh), submesh));
    return o;
}
// Emitted radiance at uv (emissive x its texture at level 0).
float3 rtEmissionAt(GpuMaterial m, float2 uv)
{
    float3 e = m.emissive;
    if (m.emissiveTexture != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[m.emissiveTexture];
        e *= (m.textureClamp & MATERIAL_TEXTURE_EMISSIVE) ? t.SampleLevel(g_anisoClamp, uv, 0).rgb : t.SampleLevel(g_anisoWrap, uv, 0).rgb;
    }
    return e;
}
// Solid-angle pdf of drawing the point seen from x at distance 'distance' along wi on the given triangle (entry e).
float rtEmissivePdfOf(ByteAddressBuffer list, uint e, RtEmissiveTriangle tri, float3 wi, float distance)
{
    const float total = asfloat(list.Load(8));
    const float cum = asfloat(list.Load(16 + e * 16 + 12)), prev = e > 0 ? asfloat(list.Load(16 + (e - 1) * 16 + 12)) : 0.0;
    const float3 c = cross(tri.p1 - tri.p0, tri.p2 - tri.p0);
    const float area = 0.5 * length(c);
    const float cosL = abs(dot(c / max(2 * area, 1e-30), wi));
    if (!(area > 0) || !(cosL > 0) || !(total > 0)) return 0;
    return (cum - prev) / total / area * distance * distance / cosL;
}
// pdf for a texel ray from x that hit triangle 'primitive' of geometry g of scene instance 'sceneInstance' (0 when the
// instance has no emissive entries or the list is absent).
float rtEmissivePdf(RtSceneSrvs scene, uint sceneInstance, RtGeometry g, uint primitive, float3 wi, float distance)
{
    const uint listSrv = rtEmissiveList(scene);
    if (listSrv == 0xFFFFFFFFu) return 0;
    ByteAddressBuffer list = ResourceDescriptorHeap[listSrv];
    if (sceneInstance >= list.Load(4)) return 0;
    const uint first = list.Load(list.Load(12) + sceneInstance * 4);
    if (first == 0xFFFFFFFFu || (g.flags & RT_GEOMETRY_PROXY_INDICES) != 0) return 0;
    const GpuMesh mesh = loadMesh(loadInstance(sceneInstance).mesh);
    const uint meshTriangle = (g.indexOffset - mesh.indexOffset) / 3 + primitive;
    const uint e = first + meshTriangle;
    const uint4 entry = list.Load4(16 + e * 16);
    return rtEmissivePdfOf(list, e, rtEmissiveTriangleOf(entry.x, entry.y, entry.z), wi, distance);
}
struct RtEmissiveSample
{
    bool valid;
    float3 wi;
    float distance;
    float3 L;   // emitted radiance toward x
    float pdf;  // solid angle, choice included
};
RtEmissiveSample rtEmissiveSample(RtSceneSrvs scene, float3 x, float u0, float u1, float u2)
{
    RtEmissiveSample o = (RtEmissiveSample)0;
    const uint listSrv = rtEmissiveList(scene);
    if (listSrv == 0xFFFFFFFFu) return o;
    ByteAddressBuffer list = ResourceDescriptorHeap[listSrv];
    const uint count = list.Load(0);
    const float target = u0 * asfloat(list.Load(8));
    // First entry whose running weight exceeds the target (binary search, at most 32 steps).
    uint lo = 0, hi = count;
    [loop] for (uint step = 0; step < 32 && lo < hi; ++step)
    {
        const uint mid = (lo + hi) / 2;
        if (asfloat(list.Load(16 + mid * 16 + 12)) > target) hi = mid;
        else lo = mid + 1;
    }
    const uint e = min(lo, count - 1);
    const uint4 entry = list.Load4(16 + e * 16);
    const RtEmissiveTriangle tri = rtEmissiveTriangleOf(entry.x, entry.y, entry.z);
    const float su = sqrt(u1), b0 = 1 - su, b1 = u2 * su, b2 = 1 - b0 - b1;
    const float3 p = tri.p0 * b0 + tri.p1 * b1 + tri.p2 * b2;
    const float3 d = p - x;
    const float d2 = dot(d, d);
    if (!(d2 > 0)) return o;
    o.distance = sqrt(d2);
    o.wi = d / o.distance;
    const float3 c = cross(tri.p1 - tri.p0, tri.p2 - tri.p0);
    if (dot(c, o.wi) >= 0 && (tri.m.classFlags & MATERIAL_TWO_SIDED) == 0) return o;  // its back face toward x
    o.pdf = rtEmissivePdfOf(list, e, tri, o.wi, o.distance);
    if (!(o.pdf > 0)) return o;
    o.L = rtEmissionAt(tri.m, tri.uv0 * b0 + tri.uv1 * b1 + tri.uv2 * b2);
    o.valid = any(o.L > 0);
    return o;
}
#endif
