// unx-kernel: cs_6_6 main
// R gate only: ProbeLookupBench.hlsl mode 16 (M's GI cache irradiance at every pixel, front side) through the group-resolved
// path (GiCacheTile.hlsli: the 8 x 8 group files its cells in groupshared, resolves each distinct cell once, then every
// pixel evaluates with the table; the same result as giCacheIrradianceScreen). Timed as bench.probe.cachetile against
// bench.probe.cache: the cost of the lookup as a pass of its own (the separate-pass variant writes this texture for M),
// to set against its cost inside M's shading kernel (R_STATUS_KO.md 0, GI tile path).
// P[0] = { 0, depth SRV, gbuffer SRV, output UAV }, P[1] = { 32, width, height, 0 }, P[2].x = GI cache SRV; b1 = the main view.
#include "GBuffer.hlsli"
#include "Passes/GI/GiCacheTile.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    // Every lane reaches every barrier: no return before the table is resolved.
    giCacheTileClear(lane);
    GroupMemoryBarrierWithGroupSync();
    const bool inside = all(pixel < P[1].yz);
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    const float d = inside ? depth.Load(int3(pixel, 0)) : 0;
    const bool valid = d > 0;
    float3 position = 0, n = float3(0, 1, 0);
    if (valid)
    {
        const GBufferSample g = decodeGBuffer(gbuffer.Load(int3(pixel, 0)));
        position = worldFromDepth(float2(pixel), d);
        const float3 v = normalize(g_cameraPosition - position);
        n = dot(g.normal, v) > 0 ? g.normal : -g.normal;
    }
    ByteAddressBuffer cache = ResourceDescriptorHeap[P[2].x];
    const GiHeader h = giHeader(cache);
    giCacheTileFile(h, valid, position, n);
    GroupMemoryBarrierWithGroupSync();
    giCacheTileResolve(cache, h, lane);
    GroupMemoryBarrierWithGroupSync();
    if (!inside) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    float4 result = 0;
    if (valid)
    {
        float weight;
        result = float4(giCacheIrradianceTile(cache, h, position, n, weight), weight);
    }
    output[pixel] = result;
}
