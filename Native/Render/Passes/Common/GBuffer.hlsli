// G-buffer (INTERFACES_KO.md 7.2; ARCHITECTURE 2.2). Owner: M. Readers: S (normal for shadow offsets), R (normal,
// roughness, depth for GI probes and reflection classification), M (shading).
//   RG32_UINT, 8 B/pixel:
//     .x  shading normal (world), octahedral snorm16 x 2
//     .y  base colour sRGB8 x 3 (bits 0..23) | perceptual roughness unorm8 (bits 24..31)
//   Metallic, specular, material class and flags come from the material table through the vis id (never stored
//   per pixel). Per-pixel metallic/occlusion maps are re-evaluated by M's shading kernel from the vis id.
#ifndef UNX_GBUFFER_HLSLI
#define UNX_GBUFFER_HLSLI
#include "Scene.hlsli"

struct GBufferSample
{
    float3 normal;     // world, unit
    float3 baseColor;  // linear
    float roughness;   // perceptual
};

float srgbToLinear(float c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float linearToSrgb(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

uint2 encodeGBuffer(GBufferSample s)
{
    const uint r = uint(round(saturate(linearToSrgb(s.baseColor.r)) * 255.0));
    const uint g = uint(round(saturate(linearToSrgb(s.baseColor.g)) * 255.0));
    const uint b = uint(round(saturate(linearToSrgb(s.baseColor.b)) * 255.0));
    const uint rough = uint(round(saturate(s.roughness) * 255.0));
    return uint2(octEncode(s.normal), r | (g << 8) | (b << 16) | (rough << 24));
}

GBufferSample decodeGBuffer(uint2 packed)
{
    GBufferSample s;
    s.normal = octDecode(packed.x);
    s.baseColor = float3(srgbToLinear((packed.y & 0xFFu) / 255.0), srgbToLinear(((packed.y >> 8) & 0xFFu) / 255.0), srgbToLinear(((packed.y >> 16) & 0xFFu) / 255.0));
    s.roughness = (packed.y >> 24) / 255.0;
    return s;
}

#endif
