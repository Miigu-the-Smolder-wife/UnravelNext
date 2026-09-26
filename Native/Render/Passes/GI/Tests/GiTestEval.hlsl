// unx-kernel: cs_6_6 main
// GI tests only: the M-facing APIs at every probe's own pixel -> four float4: (screenProbeIrradiance rgb, occlusion),
// (screenProbeRadiance rgb for the mirrored view direction and a 0.4 rad cone, 0), (world position, 0), (normal, 0); the
// first two have w = -1 where the pixel shows sky. Position .w = the cache's irradiance at the point (the entries' 9 x 9
// maps, giCacheIrradianceAt, channel mean; -1 = no entry with data). P[2].y = the cache SRV (raw).
// P[2].z = GGX alpha bits (0 = off): normal .w = the K path by the lobe (screenProbeGatherLobe, channel mean) and radiance
// .w = the split-sum K path for the same lobe (mirror direction, reflectionLobeHalfAngle; channel mean), for the lobe check.
// P[2].z = 0: normal .w = M's per-pixel cache irradiance (giCacheIrradianceScreen, channel mean; -1 = none) and radiance
// .w = the same before the partner corners (giTestFillOnly: the missing weight from coarser levels), for the screen check.
// P[0] = { probes SRV, depth SRV, gbuffer SRV, output UAV }, P[1] = { probesX, probesY, width, height }, P[2].x = maps atlas SRV;
// b1 = the view.
#include "GBuffer.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"
#include "Passes/GI/GiCache.hlsli"

// The screen reconstruction before the partner corners (2026-09-26): each level's trilinear sum over the corners with
// data, the rest of the weight from the next coarser level.
float3 giTestFillOnly(ByteAddressBuffer b, GiHeader h, float3 worldPos, float3 normal, out float weight)
{
    uint level;
    const float beta = giLevelBand(h, worldPos, level);
    const uint nc = giNormalClass(normal);
    float3 result = 0;
    float remain = 1;
    [loop] for (uint k = 0; k < GI_FILL_LEVELS && remain > 1e-3 && level <= h.maxLevel; ++k, ++level)
    {
        float3 s = 0, unused = 0;
        float w = 0;
        giAccumulateLevel(b, h, worldPos, normal, normal, false, nc, level, s, unused, w);
        const float take = k == 0 ? 1 - beta : 1.0;
        result += (remain * take) * s;
        remain *= 1 - take * w;
    }
    weight = 1 - remain;
    return weight > 0 ? result / weight : 0;
}
#include "Passes/Reflection/Reflection.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 probe : SV_DispatchThreadID)
{
    if (probe.x >= P[1].x || probe.y >= P[1].y) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[0].w];
    const uint2 pixel = min(probe * 8, P[1].zw - 1);  // the pixel just below-right of the probe's corner
    const float d = depth.Load(int3(pixel, 0));
    const uint index = 4 * (probe.y * P[1].x + probe.x);
    if (d <= 0)
    {
        output[index] = float4(0, 0, 0, -1);
        output[index + 1] = float4(0, 0, 0, -1);
        return;
    }
    const float3 n = decodeGBuffer(gbuffer.Load(int3(pixel, 0))).normal;
    // M's lookup (screenProbeGather: one footprint, the K path from the hardware-filtered maps atlas).
    ProbeSrvs s = { P[0].x, P[0].x, P[2].x, 0 };
    const float3 world = worldFromDepth(float2(pixel), d);
    const float3 v = normalize(g_cameraPosition - world);
    const ScreenProbeLighting l = screenProbeGather(s, pixel, world, n, linearDepth(d), false, true, reflect(-v, n), 0.4);
    output[index] = float4(l.irradiance, l.occlusion);
    float lobe = 0, split = 0;
    const float alpha = asfloat(P[2].z);
    if (alpha > 0)
    {
        const ScreenProbeLighting k = screenProbeGatherLobe(s, pixel, world, n, linearDepth(d), false, true, v, alpha);
        lobe = (k.radiance.r + k.radiance.g + k.radiance.b) / 3;
        const ScreenProbeLighting q = screenProbeGather(s, pixel, world, n, linearDepth(d), false, true, reflect(-v, n), reflectionLobeHalfAngle(sqrt(alpha), dot(n, v)));
        split = (q.radiance.r + q.radiance.g + q.radiance.b) / 3;
    }
    if (alpha <= 0)
    {
        ByteAddressBuffer c = ResourceDescriptorHeap[P[2].y];
        const GiHeader hc = giHeader(c);
        float ws, wf;
        const float3 es = giCacheIrradianceScreen(c, hc, world, n, ws), ef = giTestFillOnly(c, hc, world, n, wf);
        lobe = ws > 0 ? (es.r + es.g + es.b) / 3 : -1.0;
        split = wf > 0 ? (ef.r + ef.g + ef.b) / 3 : -1.0;
    }
    output[index + 1] = float4(l.radiance, split);
    ByteAddressBuffer cache = ResourceDescriptorHeap[P[2].y];
    const GiHeader h = giHeader(cache);
    float weight;
    const float3 e = giCacheIrradianceAt(cache, h, world, n, 0, weight);
    output[index + 2] = float4(world, weight > 0 ? (e.r + e.g + e.b) / 3 : -1.0);
    output[index + 3] = float4(n, lobe);
}
