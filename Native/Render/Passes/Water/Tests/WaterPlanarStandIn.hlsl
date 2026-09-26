// unx-kernel: cs_6_6 main
// Water calm-water test (WaterSurfaceTests 6): a stand-in for the reflection camera that FrameServices::renderView draws.
// Each of its pixels gets |r| x 20 + 1, r the pixel ray's direction from the mirrored eye - the mirror direction at the
// plane point - as exposed radiance, the value WaterFakeRays.hlsl gives a reflection job along r (ray results are
// exposed radiance too), and depth 0 (sky).
// P[0] = { colour UAV (RGBA16F), depth UAV (R32F), 0, 0 }; frame constants b1 = the reflection camera's.
#include "Bindless.hlsli"
#include "Passes/Common/Frame.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(g_viewWidth, g_viewHeight))) return;
    const float3 r = normalize(worldFromDepth(float2(id.xy), 1.0) - g_cameraPosition);
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    colour[id.xy] = float4(abs(r) * 20 + 1, 1);
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    depth[id.xy] = 0;
}
