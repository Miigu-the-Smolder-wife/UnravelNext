// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// Reflection rays (ARCHITECTURE 2.6 G and M paths; DispatchRays with alpha any-hit), one thread per job, dispatched
// indirectly with this frame's job count. The value is the lobe-normalised incident radiance (VNDF samples of the GGX
// lobe, so the normalised integral is the mean of the samples):
//   M job: one sample;
//   G job: reflection.g_rays_per_sample (4) samples with the screen-probe cache as control variate g_i =
//          screenProbeRadiance in direction w_i (texel cone), gbar = its mean over the lobe (reflLobeControl, quadrature):
//          reflLobeEstimate (difference estimator where the rays are as bright as the cache, ratio estimator where darker).
// Traversal only (ARCHITECTURE 2.6 revision 1): the job's rays get slots in the rays buffer and their hit records; the
// hit shading runs in compute (ReflectionShade.hlsl, then ReflectionShadow for off-screen sun visibility, then
// ReflectionCombine for the job's value). A job whose rays do not fit this frame's capacity is traced, shaded and
// combined here (ReflectionHit.hlsli): the same value, only slower. Root constants: ReflectionRay.hlsli.
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "Passes/Reflection/ReflectionHit.hlsli"

// The inline path of one job (the rays buffer is full): trace, shade and combine in this thread.
void reflTraceInline(ReflJob j, uint job, RtSceneSrvs scene, RWByteAddressBuffer cache, GiHeader h)
{
    RWStructuredBuffer<uint2> results = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint4> probeTexture = ResourceDescriptorHeap[P[0].w];
    float probeSpacing;
    int2 probeCount;
    const GiProbeFootprint footprint = giProbeFootprint(probeTexture, j.pixel, j.s.normal, j.s.linearDepth, probeSpacing, probeCount);
    uint seed = j.seed;
    float3 sumL = 0, sumG = 0;
    float distSum = 0;
    uint valid = 0;
    [loop] for (uint i = 0; i < j.rays; ++i)
    {
        float3 dir;
        if (!reflNextDirection(j, seed, dir)) continue;
        RayDesc r;
        r.Origin = reflRayOrigin(j.s);
        r.Direction = dir;
        r.TMin = 0;
        r.TMax = giRayLength();
        float d;
        const float3 L = reflHitRadiance(scene, cache, h, r, j.coneWidth, j.coneSpread, seed, d);
        const float3 g = j.mode == REFL_G ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, dir, 0.1763, P[3].w) : 0;
        sumL += L;
        sumG += g;
        distSum += d;
        ++valid;
    }
    const float3 gbar = j.mode == REFL_G ? reflLobeControl(j, probeTexture, footprint, probeCount)
                                         : valid == 0 ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, reflect(-j.s.view, j.s.normal), j.lobe, P[3].w) : 0;
    results[job] = reflPackResult(reflLobeEstimate(sumL, sumG, valid, gbar), valid > 0 ? distSum / valid : 0);
    // Diagnostics: G samples and those estimated by the ratio branch, one atomic per wave (GI header).
    const uint gSamples = WaveActiveCountBits(j.mode == REFL_G), gRatio = WaveActiveCountBits(j.mode == REFL_G && reflLobeRatio(sumL, sumG, valid));
    if (WaveIsFirstLane() && gSamples)
    {
        RWByteAddressBuffer statCache = ResourceDescriptorHeap[P[4].z];
        statCache.InterlockedAdd(GI_H_STAT_G_SAMPLES, gSamples);
        if (gRatio) statCache.InterlockedAdd(GI_H_STAT_G_RATIO, gRatio);
    }
}

[shader("raygeneration")]
void ReflectionTraceGen()
{
    const uint job = DispatchRaysIndex().x;
    RWStructuredBuffer<uint2> results = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer cache = ResourceDescriptorHeap[P[4].z];
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    const GiHeader h = giHeader(cache);
    const RtSceneSrvs scene = rtScene();
    const ReflJob j = reflLoadJob(job);
    const uint capacity = rays.Load(4);
    uint base;
    rays.InterlockedAdd(0, j.rays, base);
    if (base + j.rays > capacity)
    {
        reflTraceInline(j, job, scene, cache, h);
        return;
    }
    uint seed = j.seed;
    [loop] for (uint i = 0; i < j.rays; ++i)
    {
        const uint slot = base + i;
        rays.Store(reflRaysJobOffset(capacity, slot), job | (i << 28));
        float3 dir;
        if (!reflNextDirection(j, seed, dir))
        {
            rays.Store4(reflRaysHitOffset(slot), uint4(REFL_RAY_NONE, 0, 0, 0));
            continue;
        }
        RayDesc r;
        r.Origin = reflRayOrigin(j.s);
        r.Direction = dir;
        r.TMin = 0;
        r.TMax = giRayLength();
        const RtHit hit = rtTraceClosest(scene, r, RAY_FLAG_NONE, RT_MASK_REFLECTION);
        rays.Store4(reflRaysHitOffset(slot), hit.t < 0 ? uint4(REFL_RAY_MISS, 0, 0, 0)
                                                        : uint4(hit.instance | (hit.frontFace << 31), (hit.geometry << 24) | hit.primitive,
                                                                reflPackBarycentrics(hit.barycentrics), asuint(hit.t)));
    }
    results[job] = uint2(base, REFL_JOB_SPLIT);
}
