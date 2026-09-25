// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// Reflection rays (ARCHITECTURE 2.6 G and M paths; DispatchRays with alpha any-hit), one thread per job, dispatched
// indirectly with this frame's job count. The value is the lobe-normalised incident radiance (VNDF samples of the GGX
// lobe, so the normalised integral is the mean of the samples):
//   M job: one sample;
//   G job: reflection.g_rays_per_sample (4) samples with the screen-probe cache as control variate:
//          I = gbar + mean(L_i - g_i), g_i = screenProbeRadiance in direction w_i (texel cone), gbar = the same over the
//          whole lobe (reflectionLobeHalfAngle). The variance left is that of L - g: small where the cache is good.
// Radiance at a hit: RayTracing/HitShading.hlsli (model v1 with the direct view's sun terms; one shadow ray in the solar
// disk; indirect diffuse = cache irradiance, indirect specular = cache radiance from the hit's mirror direction); sky on
// a miss.
//
// P[0] = { jobs SRV, results UAV (uint2 per job), mode SRV, probes SRV }
// P[1], P[2], P[3].xyz = sky and sun (GiSky.hlsli), ray length in P[1].w, P[3].w = view.screenProbeMaps SRV
// P[4] = { depth SRV, gbuffer SRV, GI cache UAV (raw), rays per G sample }, P[5] = { frame | experiment << 24, specular albedo LUT SRV, ShadowSrvs buffer (ReflectionHit.hlsli), exact set counts }
// P[6], P[7] = RtSceneSrvs. Frame constants b1 = main view.
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionInternal.hlsli"
#include "Passes/GI/GiInternal.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/Reflection/ReflectionHit.hlsli"

#define REFL_SAMPLE_ATTEMPTS 8u

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
    // The screen-probe footprint once per job (the G control variates and gbar share it); the K-path maps from the
    // hardware-filtered atlas (P[3].w).
    Texture2D<uint4> probeTexture = ResourceDescriptorHeap[P[0].w];
    float probeSpacing;
    int2 probeCount;
    const GiProbeFootprint footprint = giProbeFootprint(probeTexture, pixel, s.normal, s.linearDepth, probeSpacing, probeCount);
    const float lobe = reflectionLobeHalfAngle(s.roughness, dot(s.normal, s.view));
    uint seed = giRandom(pixel.x * 7919u + pixel.y * 104729u + (P[5].x & 0xFFFFFFu) * 15485863u);
    float3 sum = 0;
    float distSum = 0;
    uint valid = 0;
    [loop] for (uint i = 0; i < rays; ++i)
    {
        // The lobe average is over unmasked directions (the specular directional albedo M applies carries the masked
        // loss), so a direction below the surface is redrawn: rejection sampling draws exactly the unmasked part of the
        // VNDF distribution. Only when all REFL_SAMPLE_ATTEMPTS draws are masked (extreme grazing) is the sample dropped.
        float3 dir = 0;
        bool found = false;
        [loop] for (uint attempt = 0; attempt < REFL_SAMPLE_ATTEMPTS && !found; ++attempt)
        {
            const float2 u = float2(giUnit(seed), giUnit(seed + 1));
            seed = giRandom(seed + 2);
            dir = reflSampleGgx(s.normal, s.view, alpha, u);
            found = dot(dir, s.normal) > 0;
        }
        if (!found) continue;
        RayDesc r;
        r.Origin = s.position + s.normal * (1e-3 + 2e-4 * s.linearDepth);
        r.Direction = dir;
        r.TMin = 0;
        r.TMax = giRayLength();
        float d;
        const float3 L = reflHitRadiance(scene, cache, h, r, tan(lobe), seed, d);
        // Control variate for G: the cache's radiance in the same direction at texel resolution.
        const float3 g = mode == REFL_G ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, dir, 0.1763, P[3].w) : 0;
        sum += L - g;
        distSum += d;
        ++valid;
    }
    // G: gbar + mean(L - g). M: the sample itself. No unmasked sample (a grazing single M sample): the cache's lobe value.
    const float3 gbar = mode == REFL_G || valid == 0 ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, reflect(-s.view, s.normal), lobe, P[3].w) : 0;
    const float3 value = (valid > 0 ? sum / valid : 0) + gbar;
    results[job] = reflPackResult(max(value, 0.0), valid > 0 ? distSum / valid : 0);
}
