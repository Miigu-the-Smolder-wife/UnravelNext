// unx-kernel: cs_6_6 main
// Store the bordered radiance and its irradiance in one probe-owned group.
// Each input texel enters group memory once; all 36 normals share it and read
// precomputed, full-precision cosine weights. Same samples and accumulation
// order as LumenRadianceCacheIrradiance, without its repeated direction math.
// P0={params SRV, state SRV, traces SRV, filtered radiance SRV}
// P1={radiance atlas UAV, irradiance atlas UAV, weights SRV, first trace}
#include "Bindless.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"
groupshared float4 radianceTile[256];
groupshared float4 irradianceTile[LRC_IRRADIANCE_RES * LRC_IRRADIANCE_RES];

uint2 borderedSource(uint2 texel, int n)
{
    int2 s = int2(texel) - 1;
    if (s.x < 0 || s.x >= n) { s.x = clamp(s.x, 0, n - 1); s.y = n - 1 - s.y; }
    if (s.y < 0 || s.y >= n) { s.y = clamp(s.y, 0, n - 1); s.x = n - 1 - s.x; }
    return uint2(clamp(s, 0, n - 1));
}

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const LrcParams p = lrcParams(P[0].x);
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
    const uint trace = group.z + P[1].w;
    if (trace >= state.Load(8)) return;
    ByteAddressBuffer traces = ResourceDescriptorHeap[P[0].z];
    const uint slot = traces.Load(trace * 16 + 12) & 0xFFFFFFu;
    Texture2D<float4> filtered = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<float4> atlas = ResourceDescriptorHeap[P[1].x];
    const uint res = p.probeResolution, samples = res * res;
    const uint2 base = uint2(trace % p.tempProbes, trace / p.tempProbes) * res;
    const uint2 output = lrcAtlasCoord(p, slot);
    for (uint i = lane; i < p.finalResolution * p.finalResolution; i += 64)
    {
        const uint2 at = uint2(i % p.finalResolution, i / p.finalResolution);
        const float4 value = filtered[base + borderedSource(at, int(res))];
        atlas[output * p.finalResolution + at] = value.a < 0.5 ? float4(0, 0, 0, 0) : float4(value.rgb, 1);
    }

    const uint normals = LRC_IRRADIANCE_RES * LRC_IRRADIANCE_RES;
    ByteAddressBuffer weights = ResourceDescriptorHeap[P[1].z];
    float3 sum = 0;
    float held = 0, total = 0;
    for (uint first = 0; first < samples; first += 256)
    {
        const uint count = min(256u, samples - first);
        for (uint i = lane; i < count; i += 64)
        {
            const uint sample = first + i;
            const float4 value = filtered[base + uint2(sample % res, sample / res)];
            radianceTile[i] = value.a < 0.5 ? float4(0, 0, 0, 0) : float4(value.rgb, 1);
        }
        GroupMemoryBarrierWithGroupSync();
        if (lane < normals)
            for (uint i = 0; i < count; ++i)
            {
                const float cosine = asfloat(weights.Load(((first + i) * normals + lane) * 4));
                if (!(cosine > 0)) continue;
                const float4 value = radianceTile[i];
                sum += value.rgb * cosine;
                held += value.a * cosine;
                total += cosine;
            }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane < normals)
    {
        float4 value = 0;
        if (held > 1e-4 && total > 0)
            value = float4(sum * (LRC_PI * LRC_IRRADIANCE_SCALE / (held * LRC_RADIANCE_SCALE)), saturate(held / total));
        irradianceTile[lane] = all(isfinite(value)) ? value : float4(0, 0, 0, 0);
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 at = uint2(lane % LRC_IRRADIANCE_BORDERED, lane / LRC_IRRADIANCE_BORDERED);
    const uint2 from = borderedSource(at, int(LRC_IRRADIANCE_RES));
    RWTexture2D<float4> irradiance = ResourceDescriptorHeap[P[1].y];
    irradiance[output * LRC_IRRADIANCE_BORDERED + at] = irradianceTile[from.x + from.y * LRC_IRRADIANCE_RES];
}
