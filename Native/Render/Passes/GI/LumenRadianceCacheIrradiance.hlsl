// unx-kernel: cs_6_6 main
// r.gi.rc.irradiance (LumenRadianceCache.hlsli, lumen.hit_indirect_radiance_cache): the irradiance map of every probe
// traced this frame, from the probe's radiance as the store left it in the cache's atlas. One group of 8 x 8 threads per
// traced probe (the pass runs on the store's dispatch arguments: of each probe's groups only the first works). The 36
// interior texels each stand for a normal of the 6 x 6 equal-area map and sum the probe's radiance texels (at most
// 64 x 64: lumen.radiance_cache_probe_resolution) over that normal's hemisphere with the cosine; the equal-area map gives
// every radiance texel the same solid angle, so
//     E(n) = pi x sum(L cos) / sum(cos over the texels that hold radiance)
// - the texels that hold nothing (probe occlusion: the probe's line to the ray's start was blocked) are left out and the
// rest stands for them. Stored rgb = E x LRC_IRRADIANCE_SCALE, a = the share of the hemisphere's cosine that held
// radiance (0: nothing - the reader skips the probe for that normal). The border texels repeat the interior across the
// map's edges as the radiance atlas' do, for bilinear lookups.
// P[0] = { parameters SRV, state SRV, trace records SRV, cache atlas SRV }
// P[1] = { irradiance atlas UAV (RGBA16F), the dispatch's first trace record, 0, 0 }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"

groupshared float4 g_irradiance[LRC_IRRADIANCE_RES * LRC_IRRADIANCE_RES];

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID)
{
    const LrcParams p = lrcParams(P[0].x);
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
    const uint trace = gid.z + P[1].y;
    const bool work = gid.x == 0 && gid.y == 0 && trace < state.Load(8);  // uniform over the group
    uint slot = 0;
    if (work)
    {
        ByteAddressBuffer traces = ResourceDescriptorHeap[P[0].z];
        slot = traces.Load(trace * 16 + 12) & 0xFFFFFFu;
    }
    if (work && all(tid.xy < LRC_IRRADIANCE_RES))
    {
        Texture2D<float4> atlas = ResourceDescriptorHeap[P[0].w];
        const uint res = p.probeResolution;
        const uint2 base = lrcAtlasCoord(p, slot) * p.finalResolution + 1;
        const float3 normal = lrcUvToDirection((float2(tid.xy) + 0.5) / float(LRC_IRRADIANCE_RES));
        float3 sum = 0;
        float held = 0, total = 0;
        for (uint y = 0; y < res; ++y)
            for (uint x = 0; x < res; ++x)
            {
                const float c = dot(lrcUvToDirection((float2(x, y) + 0.5) / float(res)), normal);
                if (!(c > 0)) continue;
                const float4 v = atlas[base + uint2(x, y)];  // (rgb x a, a)
                sum += v.rgb * c;
                held += v.a * c;
                total += c;
            }
        float4 e = float4(0, 0, 0, 0);
        if (held > 1e-4 && total > 0) e = float4(sum * (LRC_PI * LRC_IRRADIANCE_SCALE / (held * LRC_RADIANCE_SCALE)), saturate(held / total));
        if (any(isnan(e)) || any(isinf(e))) e = float4(0, 0, 0, 0);
        g_irradiance[tid.x + tid.y * LRC_IRRADIANCE_RES] = e;
    }
    GroupMemoryBarrierWithGroupSync();
    if (!work) return;
    // the interior texel a bordered texel shows (LumenRadianceCacheFilter.hlsl's store: across an edge of the map the
    // neighbour is the same edge's texel mirrored about the edge's centre)
    int2 s = int2(tid.xy) - 1;
    const int n = int(LRC_IRRADIANCE_RES);
    if (s.x < 0 || s.x >= n)
    {
        s.x = clamp(s.x, 0, n - 1);
        s.y = n - 1 - s.y;
    }
    if (s.y < 0 || s.y >= n)
    {
        s.y = clamp(s.y, 0, n - 1);
        s.x = n - 1 - s.x;
    }
    s = clamp(s, 0, n - 1);
    RWTexture2D<float4> irradiance = ResourceDescriptorHeap[P[1].x];
    irradiance[lrcAtlasCoord(p, slot) * LRC_IRRADIANCE_BORDERED + tid.xy] = g_irradiance[s.x + s.y * n];
}
