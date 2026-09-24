// unx-kernel: cs_6_6 main
// GI tests only: the M-facing APIs at every probe's own pixel -> two float4: (screenProbeIrradiance rgb, occlusion) and
// (screenProbeRadiance rgb for the mirrored view direction and a 0.4 rad cone, 0); w = -1 where the pixel shows sky.
// P[0] = { probes SRV, depth SRV, gbuffer SRV, output UAV }, P[1] = { probesX, probesY, width, height }; b1 = the view.
#include "GBuffer.hlsli"
#include "Passes/GI/ScreenProbes.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 probe : SV_DispatchThreadID)
{
    if (probe.x >= P[1].x || probe.y >= P[1].y) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[0].w];
    const uint2 pixel = min(probe * 8 + 4, P[1].zw - 1);
    const float d = depth.Load(int3(pixel, 0));
    const uint index = 2 * (probe.y * P[1].x + probe.x);
    if (d <= 0)
    {
        output[index] = float4(0, 0, 0, -1);
        output[index + 1] = float4(0, 0, 0, -1);
        return;
    }
    const float3 n = decodeGBuffer(gbuffer.Load(int3(pixel, 0))).normal;
    ProbeSrvs s = { P[0].x, P[0].x, 0, 0 };
    output[index] = screenProbeIrradiance(s, pixel, n, linearDepth(d));
    const float3 v = normalize(g_cameraPosition - worldFromDepth(float2(pixel), d));
    output[index + 1] = float4(screenProbeRadiance(s, pixel, n, linearDepth(d), reflect(-v, n), 0.4), 0);
}
