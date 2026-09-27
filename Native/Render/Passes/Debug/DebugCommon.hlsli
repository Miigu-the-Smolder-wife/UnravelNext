// Debug draw passes (E, A15): shared vertex format, projection and depth test. See DebugDraw.hlsli for the buffer.
// Root constants of the draw passes: P[0] = { primitives SRV (raw), scene depth SRV (R32_FLOAT, reversed Z;
// DEBUG_DRAW_OFF = no depth test), font atlas SRV (R8_UNORM with mips), atlas columns }, P[1] = { atlas rows, atlas
// cell height (texels, mip 0), atlas mips, 0 }.
#ifndef UNX_DEBUG_COMMON_HLSLI
#define UNX_DEBUG_COMMON_HLSLI
#include "Bindless.hlsli"
#include "Passes/Debug/DebugDraw.hlsli"

struct DebugVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;                         // glyphs: position in the cell (0..1)
    nointerpolation float4 segment : TEXCOORD1;    // lines: end points in pixels (a.xy, b.xy)
    nointerpolation float extra : TEXCOORD2;       // lines: half width (px); glyphs: cell height (px)
    nointerpolation uint color : TEXCOORD3;
    nointerpolation uint info : TEXCOORD4;         // flags (lines, triangles); glyphs: code | flags << 16
};

// Pixel of a clip-space point (w > 0), and the clip-space point of a pixel at a given clip z, w (the pixel's NDC scaled by
// w: perspective-correct interpolation of z / w along the primitive).
float2 debugPixelOf(float4 clip)
{
    const float2 ndc = clip.xy / clip.w;
    return float2((ndc.x * 0.5f + 0.5f) * g_viewWidth, (0.5f - 0.5f * ndc.y) * g_viewHeight);
}
float4 debugClipOf(float2 pixel, float z, float w)
{
    const float2 ndc = float2(pixel.x / g_viewWidth * 2 - 1, 1 - pixel.y / g_viewHeight * 2);
    return float4(ndc * w, z, w);
}
// Clip position of a primitive point: world (viewProj) or, with DEBUG_SCREEN, a pixel in front of everything.
float4 debugClip(float3 p, uint flags)
{
    if (flags & DEBUG_SCREEN) return debugClipOf(p.xy, 1, 1);
    return mul(g_viewProj, float4(p, 1));
}
// Opacity factor of the scene depth test at a pixel for a fragment of device depth z (reversed Z: visible when
// z (1 + 1e-3) >= scene, i.e. its view depth is at most 1.001 x the scene's).
float debugDepthFactor(float2 pixel, float z, uint flags)
{
    if (!(flags & DEBUG_DEPTH_TEST) || (flags & DEBUG_SCREEN) || P[0].y == DEBUG_DRAW_OFF) return 1;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    // (the scene depth can be smaller than the target: the temporal upscale's internal resolution, Upscale.cpp)
    uint dw, dh;
    depth.GetDimensions(dw, dh);
    const float scene = depth.Load(int3(min(uint2(pixel * float2(dw, dh) / float2(g_viewWidth, g_viewHeight)), uint2(dw, dh) - 1), 0));
    if (z * 1.001f >= scene) return 1;
    return (flags & DEBUG_XRAY) ? 0.25f : 0.0f;
}
DebugVertex debugDegenerate()
{
    DebugVertex v = (DebugVertex)0;
    v.position = float4(2, 2, 0.5f, 1);  // outside the view: no pixel
    return v;
}
#endif
