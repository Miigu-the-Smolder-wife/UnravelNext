// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// Reflection rays (ARCHITECTURE 2.6 G and M paths; DispatchRays with alpha any-hit), one thread per job, dispatched
// indirectly with this frame's job count. The value is the lobe-normalised incident radiance (VNDF samples of the GGX
// lobe, so the normalised integral is the mean of the samples):
//   M job: one sample;
//   G job: reflection.g_rays_per_sample (4) samples with the screen-probe cache as control variate:
//          I = gbar + mean(L_i - g_i), g_i = screenProbeRadiance in direction w_i (texel cone), gbar = the same over the
//          whole lobe (reflectionLobeHalfAngle). The variance left is that of L - g: small where the cache is good.
// Radiance at a hit = emission + diffuse albedo / pi * (direct sun: one shadow ray in the solar disk + cache irradiance
// (trilinear, indirect + sky)); sky on a miss. Specular reflection at the hit point is not included (see R status).
//
// P[0] = { jobs SRV, results UAV (uint2 per job), mode SRV, probes SRV }
// P[1], P[2], P[3].xyz = sky and sun (GiSky.hlsli), ray length in P[1].w
// P[4] = { depth SRV, gbuffer SRV, GI cache UAV (raw), rays per G sample }, P[5] = { frame, 0, 0, 0 }
// P[6], P[7] = RtSceneSrvs. Frame constants b1 = main view.
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionInternal.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/GI/GiSky.hlsli"

// A reflection hit consumes the cache like a GI hit: its cell (the lobe's footprint there, 2 t tan(lobe), at least) is
// found or created and requested for update next frame (hit tier), so cells only reflections see are kept converged.
// The irradiance read starts at that level and climbs until an updated cell exists.
float3 reflHitRadiance(RtSceneSrvs scene, RWByteAddressBuffer cache, GiHeader h, RayDesc r, float coneTan, uint seed, out float hitDistance)
{
    const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_REFLECTION);
    if (hit.t < 0)
    {
        hitDistance = 65000;
        return giSkyRadiance(r.Direction);
    }
    hitDistance = hit.t;
    const RtSurface s = rtSurface(scene, hit, r.Origin, r.Direction);
    const GpuMaterial m = loadMaterial(s.material);
    if (!s.frontFace && (m.classFlags & MATERIAL_TWO_SIDED) == 0) return 0;
    const uint footprintLevel = giLevelForSize(h, 2 * hit.t * coneTan);
    bool created;
    const uint e = giFindOrCreate(cache, h, giSurfaceKey(h, s.position, s.normal, footprintLevel), s.position, s.normal, created);
    if (e != GI_ENTRY_PENDING)
    {
        giTouch(cache, h, e);
        giRequestHit(cache, h, e);
    }
    float w;
    const float3 indirect = giCacheIrradianceAt(cache, h, s.position, s.normal, footprintLevel, w);
    float3 sun = 0;
    const float3 l = normalize(g_sunDirection);
    const float cosSun = dot(s.normal, l);
    if (cosSun > 0)
    {
        const float3 e0 = giSunIlluminance(s.position);
        if (any(e0 > 0))
        {
            RayDesc sr;
            sr.Origin = s.position + s.normal * (1e-3 + 2e-4 * distance(s.position, g_cameraPosition));
            sr.Direction = giSunDirection(seed);
            sr.TMin = 0;
            sr.TMax = giRayLength();
            if (rtVisible(scene, sr, RT_MASK_REFLECTION)) sun = e0 * cosSun;
        }
    }
    return m.emissive + m.baseColor * (1 - m.metallic) / 3.14159265 * (indirect + sun);
}

[shader("raygeneration")]
void ReflectionTraceGen()
{
    const uint job = DispatchRaysIndex().x;
    StructuredBuffer<uint> jobs = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint2> results = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint> modes = ResourceDescriptorHeap[P[0].z];
    Texture2D<float> depth = ResourceDescriptorHeap[P[4].x];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[4].y];
    RWByteAddressBuffer cache = ResourceDescriptorHeap[P[4].z];
    const GiHeader h = giHeader(cache);
    const RtSceneSrvs scene = rtScene();
    const uint2 pixel = reflUnpackPixel(jobs[job]);
    const uint mode = reflMode(modes.Load(int3(pixel, 0)));
    const ReflSurface s = reflSurface(depth, gbuffer, pixel);
    const float alpha = max(s.roughness * s.roughness, 1e-4);
    const uint rays = mode == REFL_M ? 1u : P[4].w;
    const ProbeSrvs probes = { P[0].w, P[0].w, 0, 0 };
    const float lobe = reflectionLobeHalfAngle(s.roughness, dot(s.normal, s.view));
    uint seed = giRandom(pixel.x * 7919u + pixel.y * 104729u + P[5].x * 15485863u);
    float3 sum = 0;
    float distSum = 0;
    uint valid = 0;
    [loop] for (uint i = 0; i < rays; ++i)
    {
        const float2 u = float2(giUnit(seed), giUnit(seed + 1));
        seed = giRandom(seed + 2);
        const float3 dir = reflSampleGgx(s.normal, s.view, alpha, u);
        // Below the surface: a masked sample, outside both the numerator and the normaliser of the lobe average (the
        // specular directional albedo M applies carries that loss).
        if (dot(dir, s.normal) <= 0) continue;
        RayDesc r;
        r.Origin = s.position + s.normal * (1e-3 + 2e-4 * s.linearDepth);
        r.Direction = dir;
        r.TMin = 0;
        r.TMax = giRayLength();
        float d;
        const float3 L = reflHitRadiance(scene, cache, h, r, tan(lobe), seed, d);
        // Control variate for G: the cache's radiance in the same direction at texel resolution.
        const float3 g = mode == REFL_G ? screenProbeRadiance(probes, pixel, s.normal, s.linearDepth, dir, 0.1763) : 0;
        sum += L - g;
        distSum += d;
        ++valid;
    }
    // G: gbar + mean(L - g). M: the sample itself. No unmasked sample (a grazing single M sample): the cache's lobe value.
    const float3 gbar = mode == REFL_G || valid == 0 ? screenProbeRadiance(probes, pixel, s.normal, s.linearDepth, reflect(-s.view, s.normal), lobe) : 0;
    const float3 value = (valid > 0 ? sum / valid : 0) + gbar;
    results[job] = reflPackResult(max(value, 0.0), valid > 0 ? distSum / valid : 0);
}
