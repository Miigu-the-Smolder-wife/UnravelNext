// unx-kernel: cs_6_6 main
// GI tests only (GiAnalytic): screenProbeGatherTile (records from the tile's groupshared copy, GI_PROBE_TILE_CACHE)
// against screenProbeGather at every pixel of the view, front and back irradiance, occlusion and K radiance at two cone
// widths (two map levels): every value must be bit-identical. One group per 8 x 8 tile; alternate tiles are filled
// with giProbeTileLoad and with giProbeTileFetch + giProbeTileStore.
// P[0] = { probes SRV, depth SRV, gbuffer SRV, result UAV (raw) }, P[1] = { 0, 0, width, height }, P[2].x = maps atlas SRV;
// b1 = the view. Result: { pixels compared, mismatches, recorded, 0 } then the first 4 mismatches, 64 B each:
// { pixel, cone index, field mask (1 irradiance, 2 occlusion, 4 back, 8 radiance), 0, texture path rgb + occlusion,
//   tile path rgb + occlusion, texture radiance rgb, tile radiance... } (see GiAnalytic).
#define GI_PROBE_TILE_CACHE
#include "GBuffer.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"

uint differing(ScreenProbeLighting a, ScreenProbeLighting b)
{
    return (any(asuint(a.irradiance) != asuint(b.irradiance)) ? 1u : 0u) | (asuint(a.occlusion) != asuint(b.occlusion) ? 2u : 0u) |
           (any(asuint(a.irradianceBack) != asuint(b.irradianceBack)) ? 4u : 0u) | (any(asuint(a.radiance) != asuint(b.radiance)) ? 8u : 0u);
}

[numthreads(8, 8, 1)]
void main(uint2 tile : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const ProbeSrvs s = { P[0].x, P[0].x, P[2].x, 0 };
    // Alternate tiles fill the cache with the split form (giProbeTileFetch / giProbeTileStore, counts from the view size).
    if (((tile.x ^ tile.y) & 1) != 0)
        giProbeTileStore(lane, giProbeTileFetch(s, tile, lane, int2((P[1].zw + 7) / 8)));
    else
        giProbeTileLoad(s, tile, lane);
    GroupMemoryBarrierWithGroupSync();
    const uint2 pixel = tile * 8 + local;
    if (any(pixel >= P[1].zw)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    const float d = depth.Load(int3(pixel, 0));
    if (d <= 0) return;
    const float3 n = decodeGBuffer(gbuffer.Load(int3(pixel, 0))).normal;
    const float3 world = worldFromDepth(float2(pixel), d);
    const float3 dir = reflect(-normalize(g_cameraPosition - world), n);
    const float lin = linearDepth(d);
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[0].w];
    uint previous;
    result.InterlockedAdd(0, 1u, previous);
    [unroll] for (uint c = 0; c < 2; ++c)
    {
        const float cone = c == 0 ? 0.4 : 0.12;
        const ScreenProbeLighting a = screenProbeGather(s, pixel, world, n, lin, true, true, dir, cone);
        const ScreenProbeLighting b = screenProbeGatherTile(s, tile, pixel, world, n, lin, true, true, dir, cone);
        const uint mask = differing(a, b);
        if (mask == 0) continue;
        result.InterlockedAdd(4, 1u, previous);
        uint slot;
        result.InterlockedAdd(8, 1u, slot);
        if (slot >= 4) continue;
        const uint o = 16 + slot * 64;
        result.Store4(o, uint4(pixel.x | (pixel.y << 16), c, mask, 0));
        result.Store4(o + 16, uint4(asuint(a.irradiance), asuint(a.occlusion)));
        result.Store4(o + 32, uint4(asuint(b.irradiance), asuint(b.occlusion)));
        result.Store4(o + 48, uint4(asuint(a.radiance.x), asuint(b.radiance.x), asuint(a.irradianceBack.x), asuint(b.irradianceBack.x)));
    }
}
