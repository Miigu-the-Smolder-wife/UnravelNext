// unx-kernel: cs_6_6 main
// GI tests only: the M-facing APIs at every probe's own pixel -> four float4: (screenProbeIrradiance rgb, occlusion),
// (screenProbeRadiance rgb for the mirrored view direction and a 0.4 rad cone, 0), (world position, 0), (normal, 0); the
// first two have w = -1 where the pixel shows sky.
// P[0] = { probes SRV, depth SRV, gbuffer SRV, output UAV }, P[1] = { probesX, probesY, width, height }, P[2].x = maps atlas SRV;
// b1 = the view.
#include "GBuffer.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"

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
    output[index + 1] = float4(l.radiance, 0);
    output[index + 2] = float4(world, 0);
    output[index + 3] = float4(n, 0);
}
