// unx-kernel: cs_6_6 main
// Water stage 3 (WaterSurface.hlsli WaterRayTerms): one thread per sample of the band. When R traced one of its jobs
// (result alpha 1) the sample's value is rebuilt as base + per term (traced ? weight x result : fallback), all fp32 (the
// ray's radiance in place of stage 1's stand-in; untraced samples keep their stored value);
// band A's radiance at the pixel (ViewResources::bandARadiance, what the composites put behind fragments) is updated
// and the shaded colour rewritten with FX's particle layer over it (shParticles), as WaterInterior.hlsl writes them.
// P[0] samples SRV (raw: head { count, capped count }, then 80 B per sample: pixel x | y << 16, reflection job,
//      refraction job (UNX_NONE: none), 0; reflection weight xyz, fallback xyz; refraction weight xyz, fallback xyz;
//      base xyz, 0),
//      results SRV (raw, 8 B per job: RGBA16F), bandARadiance UAV, colour UAV
// P[1] particle layer SRV, particle edges SRV (UNX_NONE: none), statistics UAV (raw; UNX_NONE), coverageRecordRadiance
//      UAV (raw, f16 x 4 per record: a target with WATER_RAY_RECORD updates that record instead of a pixel)
#include "WaterSurface.hlsli"
#include "WaterLinear.hlsli"
#include "Passes/Shading/CoverageSpecial.hlsli"

float4 waterRayResult(ByteAddressBuffer results, uint job)
{
    const uint2 h = results.Load2(8 * job);
    return float4(f16tof32(h.x), f16tof32(h.x >> 16), f16tof32(h.y), f16tof32(h.y >> 16));
}

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint i = waterLinear(group, thread, 64);
    ByteAddressBuffer samples = ResourceDescriptorHeap[P[0].x];
    if (i >= samples.Load(4)) return;
    ByteAddressBuffer results = ResourceDescriptorHeap[P[0].y];
    const uint at = 16 + WATER_RAY_SAMPLE_BYTES * i;
    const uint4 head = samples.Load4(at);
    const float4 a = asfloat(samples.Load4(at + 16)), b = asfloat(samples.Load4(at + 32)), c = asfloat(samples.Load4(at + 48));
    const float3 reflectWeight = a.xyz, reflectFallback = float3(a.w, b.xy), refractWeight = float3(b.zw, c.x), refractFallback = c.yzw;
    float3 value = asfloat(samples.Load3(at + 64)) + reflectFallback + refractFallback;
    uint traced = 0;
    if (head.y != UNX_NONE)
    {
        const float4 r = waterRayResult(results, head.y);
        if (r.a > 0.5) { value += reflectWeight * r.rgb - reflectFallback; ++traced; }
    }
    if (head.z != UNX_NONE)
    {
        const float4 r = waterRayResult(results, head.z);
        if (r.a > 0.5) { value += refractWeight * r.rgb - refractFallback; ++traced; }
    }
    if (traced == 0) return;
    value = max(value, 0);  // (fp32 roundings of the base)
    if ((head.x & WATER_RAY_RECORD) != 0)
    {
        RWByteAddressBuffer records = ResourceDescriptorHeap[P[1].w];
        const uint element = head.x & ~WATER_RAY_RECORD;
        records.Store2(element * 8, covPackRadiance(value));
    }
    else
    {
        const uint2 pixel = uint2(head.x & 0xFFFFu, head.x >> 16);
        RWTexture2D<float4> bandARadiance = ResourceDescriptorHeap[P[0].z];
        const float3 radiance = value;
        bandARadiance[pixel] = float4(radiance, 1);
        RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].w];
        colour[pixel] = float4(shParticles(radiance, pixel, P[1].x, P[1].y), 1);
    }
    if (P[1].z != UNX_NONE)
    {
        RWByteAddressBuffer statistics = ResourceDescriptorHeap[P[1].z];
        statistics.InterlockedAdd(4 * WATER_STAT_TRACED, traced);
    }
}
