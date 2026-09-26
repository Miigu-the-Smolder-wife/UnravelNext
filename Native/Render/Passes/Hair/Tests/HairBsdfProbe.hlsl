// unx-kernel: cs_6_6 main
// Hair BSDF test probe (HairTests.cpp): per configuration, 256 threads each draw P[0].z sample pairs and sum
//   uniform sphere directions: 4 pi kernel (albedo), 4 pi pdf (normalization), 4 pi pdf wi.x (sampler reference);
//   hairSample: weight (albedo by importance), wi.x (sampler moment).
// Config k (float4 x 3): (outgoing.xyz, h), (absorption.rgb, eta), (betaM, betaN, tilt, 0).
// P[0] = { configs SRV, results UAV (float4 x 4 per thread), samples per thread, config count }.
#include "Bindless.hlsli"
#include "Passes/Hair/HairBsdf.hlsli"

uint probeHash(uint v)
{
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; return v ^ (v >> 16);
}
float probeRandom(inout uint s)
{
    s = probeHash(s);
    return (s >> 8) * (1.0f / 16777216.0f);
}

[numthreads(256, 1, 1)]
void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const uint k = gid.x;
    if (k >= P[0].w) return;
    StructuredBuffer<float4> configs = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float4> results = ResourceDescriptorHeap[P[0].y];
    const float4 c0 = configs[3 * k], c1 = configs[3 * k + 1], c2 = configs[3 * k + 2];
    const float3 wo = normalize(c0.xyz);
    const HairDistribution d = hairDistribution(wo, c0.w, c1.w, c1.xyz, c2.x, c2.y, c2.z);
    uint seed = probeHash(k * 7919u + gtid.x * 104729u + 17u);
    float3 albedoU = 0, albedoI = 0;
    float pdfU = 0, momentU = 0, momentI = 0;
    for (uint n = 0; n < P[0].z; ++n)
    {
        // uniform direction
        const float z = 1 - 2 * probeRandom(seed), phi = 2 * kHairPi * probeRandom(seed), r = sqrt(max(0.0f, 1 - z * z));
        const float3 wi = float3(z, r * cos(phi), r * sin(phi));
        float pdf;
        const float3 f = hairEvaluate(d, wi, pdf);
        albedoU += 4 * kHairPi * f;
        pdfU += 4 * kHairPi * pdf;
        momentU += 4 * kHairPi * pdf * wi.x;
        // importance sample
        const float3 u = float3(probeRandom(seed), probeRandom(seed), probeRandom(seed));
        const HairSample s = hairSample(d, u);
        if (s.pdf > 0)
        {
            albedoI += s.weight;
            momentI += s.direction.x;
        }
    }
    const uint o = 4 * (k * 256 + gtid.x);
    results[o] = float4(albedoU, pdfU);
    results[o + 1] = float4(albedoI, momentU);
    results[o + 2] = float4(momentI, 0, 0, 0);
    results[o + 3] = 0;
}
