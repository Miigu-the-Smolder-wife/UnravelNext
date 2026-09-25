// unx-kernel: cs_6_6 main
// The value of each reflection job whose rays went through the rays buffer (ReflectionRay.hlsli: results[job] = { first
// slot, REFL_JOB_SPLIT }), from its rays' shaded values, as ReflectionTrace's inline path: M = the sample; G = gbar +
// mean(L_i - g_i) with the screen-probe control variate in the replayed direction; no unmasked sample: the cache's lobe
// value. One thread per job (the job count is in the rays header). Root constants: ReflectionRay.hlsli.
#define SKY 1  // no sky lookups here
#include "Passes/Reflection/ReflectionRay.hlsli"

[numthreads(64, 1, 1)]
void main(uint job : SV_DispatchThreadID)
{
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
    float3 sum = 0;
    float distSum = 0;
    uint valid = 0;
    [loop] for (uint i = 0; i < j.rays; ++i)
    {
        float3 dir;
        if (!reflNextDirection(j, seed, dir)) continue;
        const uint4 v = rays.Load4(reflRaysValueOffset(capacity, marker.x + i));
        const float3 L = float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)) / REFL_STORE_SCALE;
        const float3 g = j.mode == REFL_G ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, dir, 0.1763, P[3].w) : 0;
        sum += L - g;
        distSum += f16tof32(v.y >> 16);
        ++valid;
    }
    const float3 gbar = j.mode == REFL_G || valid == 0 ? giProbeFootprintRadiance(probeTexture, footprint, probeCount, reflect(-j.s.view, j.s.normal), j.lobe, P[3].w) : 0;
    results[job] = reflPackResult(max((valid > 0 ? sum / valid : 0) + gbar, 0.0), valid > 0 ? distSum / valid : 0);
}
