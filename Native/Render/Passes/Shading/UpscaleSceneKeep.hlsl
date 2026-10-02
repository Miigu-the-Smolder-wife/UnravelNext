// unx-kernel: cs_6_6 main
// m.scenecolor (output.screen_trace_source = 0): the opaque scene colour kept for the next frame's screen-space traces
// (Passes/Reflection/ScreenTrace.hlsli sctPreviousColour): rgb = the colour, a = the frame's device depth at the pixel
// (the history depth test at a trace's hit: was the point visible in that frame).
// The colour is the lit opaque image as the shading group leaves it - before water, the coverage layers, glass and
// everything after them - with the air between the camera and the surface taken off again (ShadeOpaque applies S's air
// volume in the kernel; the same lookup here): a trace's hit wants the surface's own radiance, and the air on the camera's
// path is not the air on the ray. The reference's default source is the same image (r.Lumen.ScreenTracingSource 0: scene
// colour without translucency, extracted ahead of the fog).
// P[0] = { lit colour SRV (exposed linear), depth SRV, kept UAV (RGBA16F), 0 }, P[1] = { width, height, 0, 0 }
// P[2] = { atmosphere transmittance, multi-scatter, the view's air volume (UNX_NONE: no air applied), 0 }
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= P[1].xy)) return;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> kept = ResourceDescriptorHeap[P[0].z];
    float3 c = colour.Load(int3(pixel, 0)).rgb;
    const float d = depth.Load(int3(pixel, 0));
    if (P[2].z != UNX_NONE && d > 0)  // (reverse depth: 0 = the sky, which carries no surface)
    {
        AtmosphereSrvs atm;
        atm.transmittance = P[2].x;
        atm.multiScatter = P[2].y;
        atm.skyView = UNX_NONE;
        atm.aerial = P[2].z;
        float3 inscatter, transmittance;
        atmosphereAerial(atm, (float2(pixel) + 0.5) / float2(g_viewWidth, g_viewHeight), linearDepth(d), inscatter, transmittance);
        // (a surface behind air that passes under 2 % is not seen: its value is held to 50 x what arrived)
        c = max(c - inscatter * g_exposure, 0.0) / max(transmittance, 0.02);
    }
    kept[pixel] = float4(c, d);
}
