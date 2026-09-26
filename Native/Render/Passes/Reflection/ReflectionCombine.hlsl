// unx-kernel: cs_6_6 main
// The value of each reflection job whose rays went through the rays buffer (ReflectionRay.hlsli: results[job] = { first
// slot, REFL_JOB_SPLIT }), from its rays' shaded values, as ReflectionTrace's inline path: M = the sample; G =
// reflLobeEstimate with the screen-probe control variate in the replayed direction; no unmasked sample: the cache's lobe
// value. One thread per job (the job count is in the rays header). Root constants: ReflectionRay.hlsli.
#define SKY 1  // no sky lookups here
#include "Passes/Reflection/ReflectionRay.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint job = (group.y * 65535u + group.x) * 64u + lane;  // 2D dispatch (ReflectionRayArgs)
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    if (job >= rays.Load(12)) return;
    RWStructuredBuffer<uint2> results = ResourceDescriptorHeap[P[0].y];
    const uint2 marker = results[job];
    if (marker.y != REFL_JOB_SPLIT) return;  // traced and combined inline
    const uint capacity = rays.Load(4);
    const ReflJob j = reflLoadJob(job);
    Texture2D<uint4> probeTexture = ResourceDescriptorHeap[P[0].w];
    float probeSpacing;
    int2 probeCount;
    const GiProbeFootprint footprint = giProbeFootprint(probeTexture, j.pixel, j.s.normal, j.s.linearDepth, probeSpacing, probeCount);
    uint seed = j.seed;
    float3 sumL = 0, sumG = 0;
    float distSum = 0;
    uint valid = 0;
    bool moving = false;
    [loop] for (uint i = 0; i < j.rays; ++i)
    {
        float3 dir;
        if (!reflNextDirection(j, seed, dir)) continue;
        const uint4 v = rays.Load4(reflRaysValueOffset(capacity, marker.x + i));
        const float3 L = float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)) / REFL_STORE_SCALE;
        const float3 g = j.mode == REFL_G ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, dir, 0.1763, P[3].w) : 0;
        sumL += L;
        sumG += g;
        distSum += f16tof32(v.y >> 16);
        moving = moving || ((v.w >> 17) & 1u) != 0;  // ReflectionShadeRays: the ray hit moving geometry
        ++valid;
    }
    const float3 gbar = j.mode == REFL_G ? reflLobeControl(j, probeTexture, footprint, probeCount)
                                         : valid == 0 ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, reflect(-j.s.view, j.s.normal), j.lobe, P[3].w) : 0;
    results[job] = reflPackResult(reflLobeEstimate(sumL, sumG, valid, gbar), valid > 0 ? distSum / valid : 0, moving);
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
        RWByteAddressBuffer histCache = ResourceDescriptorHeap[P[4].z];
        if (lumG > 1e-8)
            histCache.InterlockedAdd(GI_H_STAT_G_HIST + (uint)clamp(floor(log2(max(lumL, 1e-30) / lumG)) + 4, 0.0, (float)GI_G_HIST_BINS - 1) * 4, 1u);
        else
            histCache.InterlockedAdd(GI_H_STAT_G_ZERO, 1u);
    }
}
