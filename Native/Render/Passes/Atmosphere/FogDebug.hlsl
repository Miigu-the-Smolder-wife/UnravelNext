// unx-kernel: cs_6_6 main
// s.fog.debug (atmosphere.fog.debug_view; the gate's capture layer "fog"): the fog alone as every pixel of the main view
// takes it - rgb = the radiance in-scattered between the camera and the pixel's surface x exposure, a = the
// transmittance (FogVolume.hlsli fogAt at the pixel's depth; sky pixels: the whole ray). An instrument: bands, leaks and
// the fog's share of a picture are read off this image and the depth layer instead of off the lit picture.
// P[0] = { depth SRV, output UAV (RGBA16F), 0, 0 }. Frame constants of the main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_viewWidth || id.y >= g_viewHeight) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].y];
    const float device = depth[id.xy];
    const float4 fog = fogAt((float2(id.xy) + 0.5) / float2(g_viewWidth, g_viewHeight), device <= 0 ? 3.0e38 : linearDepth(device));
    output[id.xy] = float4(fog.rgb * g_exposure, fog.a);
}
