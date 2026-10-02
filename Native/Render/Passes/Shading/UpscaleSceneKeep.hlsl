// unx-kernel: cs_6_6 main
// m.upscale.scenecolor (output.screen_trace_source = 0): the scene colour the upscale is given, kept for the next frame's
// screen-space traces (Passes/Reflection/ScreenTrace.hlsli sctPreviousColour): rgb = the colour, a = the frame's device
// depth at the pixel (the history depth test at a trace's hit: was the point visible in that frame).
// P[0] = { scene colour SRV, depth SRV, kept UAV (RGBA16F), 0 }, P[1] = { width, height, 0, 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (any(pixel >= P[1].xy)) return;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> kept = ResourceDescriptorHeap[P[0].z];
    kept[pixel] = float4(colour.Load(int3(pixel, 0)).rgb, depth.Load(int3(pixel, 0)));
}
