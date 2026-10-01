// The surface under a pixel for the screen-probe gather (gi.lumen): position, the shading normal turned to the viewer
// (as M shades it: GiScreenInputs.hlsli), linear view depth, and the same point one frame ago.
// P[0].x = depth SRV, P[0].y = G-buffer SRV, P[0].z = vis id SRV (0xFFFFFFFF: none), P[0].w = visible clusters SRV.
#ifndef UNX_GI_LUMEN_SURFACE_HLSLI
#define UNX_GI_LUMEN_SURFACE_HLSLI
#include "Passes/GI/Lumen/LgCommon.hlsli"
#include "Passes/GI/GiScreenInputs.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"

struct LgSurface
{
    bool valid;
    float3 position;
    float3 normal;
    float depth;      // linear view depth (m)
    float roughness;  // perceptual
    float3 baseColor;
};
LgSurface lgSurface(uint2 pixel)
{
    LgSurface s = (LgSurface)0;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].y];
    const float d = depth[pixel];
    const uint2 packed = gbuffer[pixel];
    s.valid = giScreenInputs(pixel, d, packed, s.position, s.normal);
    if (!s.valid) return s;
    s.depth = linearDepth(d);
    const GBufferSample g = decodeGBuffer(packed);
    s.roughness = g.roughness;
    s.baseColor = g.baseColor;
    return s;
}
// The surface point's world position one frame ago (its own motion: GiScreenHistory.hlsli); the point itself without a
// vis buffer.
float3 lgPreviousPosition(uint2 pixel, LgSurface s)
{
    if (P[0].z == 0xFFFFFFFFu) return s.position;
    float3 prevP, prevN;
    uint instance;
    giPreviousSurface(P[0].z, P[0].w, pixel, s.position, s.normal, prevP, prevN, instance);
    return prevP;
}
// Plane weight of a probe at 'probePosition' for a surface point: exp2(scale x (distance to the point's plane / depth)^2).
float lgPlaneWeight(float4 plane, float depth, float3 probePosition, float scale)
{
    const float distance = abs(dot(float4(probePosition, -1), plane));
    const float relative = distance / depth;
    return exp2(scale * relative * relative);
}
#endif
