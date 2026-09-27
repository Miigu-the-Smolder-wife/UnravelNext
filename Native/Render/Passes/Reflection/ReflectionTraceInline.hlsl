// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1 JOB=1,2
// The reflection jobs whose rays did not fit this frame's rays buffer (ReflectionTrace marked them REFL_JOB_INLINE):
// traced, shaded and combined in the ray generation shader, one thread per job of the frame (dispatched indirectly with
// the job count; other jobs return at once). JOB = the job mode this library handles (1 REFL_M, 2 REFL_G; a compile
// constant, so each library holds one mode's code and stays under the kernel size limit); both run every frame. The same value as the split passes (ReflectionHit.hlsli), only slower.
// Root constants: ReflectionRay.hlsli.
#define SHADOW_RESIDENCY_LOOP 1  // (the overflow path is at the DXIL limit: ShadowVisibility.hlsli's loop form)
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "Passes/Reflection/ReflectionHit.hlsli"

// The inline path of one job (the rays buffer is full): trace, shade and combine in this thread.
void reflTraceInline(ReflJob j, uint job, RtSceneSrvs scene, RWByteAddressBuffer cache, GiHeader h)
{
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint4> probeTexture = ResourceDescriptorHeap[P[0].w];
    float probeSpacing;
    int2 probeCount;
    const GiProbeFootprint footprint = giProbeFootprint(probeTexture, j.pixel, j.s.normal, j.s.linearDepth, probeSpacing, probeCount);
    uint seed = j.seed;
    float3 sumL = 0, sumG = 0;
    float nearest = 65000;  // the lobe's nearest hit (as ReflectionCombine)
    uint valid = 0;
    float motion = 0;
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
        float hitMotion;
        const float3 L = reflHitRadiance(scene, cache, h, r, j.coneWidth, j.coneSpread, seed, d, hitMotion);
        motion = max(motion, hitMotion);
        const float3 g = j.mode == REFL_G ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, dir, 0.1763, P[3].w) : 0;
        sumL += L;
        sumG += g;
        nearest = min(nearest, d);
        ++valid;
    }
    const float3 gbar = j.mode == REFL_G ? reflLobeControl(j, probeTexture, footprint, probeCount)
                                         : valid == 0 ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, reflect(-j.s.view, j.s.normal), j.lobe, P[3].w) : 0;
    results[job] = reflPackResult(reflLobeEstimate(sumL, sumG, valid, gbar), valid > 0 ? nearest : 0, motion);
    // Diagnostics: G samples and those estimated by the ratio branch, one atomic per wave (GI header).
    const uint gSamples = WaveActiveCountBits(j.mode == REFL_G), gRatio = WaveActiveCountBits(j.mode == REFL_G && reflLobeRatio(sumL, sumG, valid, gbar));
    if (WaveIsFirstLane() && gSamples)
    {
        RWByteAddressBuffer statCache = ResourceDescriptorHeap[P[4].z];
        statCache.InterlockedAdd(GI_H_STAT_G_SAMPLES, gSamples);
        if (gRatio) statCache.InterlockedAdd(GI_H_STAT_G_RATIO, gRatio);
    }
    if (j.mode == REFL_G && valid > 0)
    {
        const float lumL = dot(sumL, float3(0.2126, 0.7152, 0.0722)), lumG = dot(sumG, float3(0.2126, 0.7152, 0.0722));
        const uint bin = lumG > 1e-8 ? (uint)clamp(floor(log2(max(lumL, 1e-30) / lumG)) + 4, 0.0, (float)GI_G_HIST_BINS - 1) : GI_G_HIST_BINS - 1;
        RWByteAddressBuffer histCache = ResourceDescriptorHeap[P[4].z];
        histCache.InterlockedAdd(GI_H_STAT_G_HIST + bin * 4, 1u);
    }
}

[shader("raygeneration")]
void ReflectionTraceInlineGen()
{
    const uint job = DispatchRaysIndex().x;
    RWStructuredBuffer<uint3> results = ResourceDescriptorHeap[P[0].y];
    if (results[job].y != REFL_JOB_INLINE) return;
    ReflJob j = reflLoadJob(job);
    if (j.mode != JOB) return;
    j.mode = JOB;  // the compile constant: the other mode's branches fold away
    RWByteAddressBuffer cache = ResourceDescriptorHeap[P[4].z];
    const GiHeader h = giHeader(cache);
    reflTraceInline(j, job, rtScene(), cache, h);
}
