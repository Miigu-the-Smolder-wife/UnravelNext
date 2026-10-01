// Reconstruction layers (RENDERER_REDESIGN_V2 1.2, P2 "reconstruction A"): the stochastic lighting terms as layers at
// the internal resolution, with their modulation taken out, rebuilt every frame by a spatial filter guided by geometry
// and helped by a short history (LayerDenoise.hlsl, LayerTemporal.hlsl), then multiplied back (LayerCompose.hlsl).
// A frame without any history must already be clean: the history only removes what the spatial filter leaves.
//
// The reflection layers (written by ReflectionResolve, ReflectionInternal.hlsli):
//   stochastic  RGBA16F  rgb = L_rs: the hits' stochastic light over the hits' albedo (nits), a = the frames in its history (LayerTemporal; 0 before)
//   residual    RGBA16F  rgb = L_g: a G pixel's lobe estimate without the stochastic share (nits; 0 for M), a as above
//   guide       RGBA32UI x = receiver device depth (float bits)
//                        y = receiver normal oct 11 + 11 | roughness unorm7 << 22 | no cache data at the hits << 29 | mode << 30
//                        z = M: hit normal oct 8 + 8, G: log2 of the sample spacing | hit distance fp16 << 16 (M: its ray's;
//                            G: the samples' nearest)
//                        w = albedo (reflPackAlbedo: the demodulation key the composition multiplies back)
// mode: 0 = no layer value at the pixel (K, planar mirror, sky, no data), 1 = M (mirror: one ray), 2 = G (glossy lobe).
#ifndef UNX_LAYER_COMMON_HLSLI
#define UNX_LAYER_COMMON_HLSLI
#include "Passes/Reflection/ReflectionInternal.hlsli"

#define LAYER_MODE_NONE 0u
#define LAYER_MODE_M 1u
#define LAYER_MODE_G 2u

// Unit vector <-> octahedral 11 + 11 bits (0.1 deg steps: the receiver normal aims the hit point, layerHitPoint).
uint layerPackOct22(float3 n)
{
    const float3 a = n / max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    float2 o = a.xy;
    if (a.z < 0) o = (1 - abs(a.yx)) * float2(a.x >= 0 ? 1 : -1, a.y >= 0 ? 1 : -1);
    const uint2 q = uint2(round(saturate(o * 0.5 + 0.5) * 2047.0));
    return q.x | (q.y << 11);
}
float3 layerUnpackOct22(uint v)
{
    const float2 o = float2(v & 0x7FFu, (v >> 11) & 0x7FFu) / 2047.0 * 2 - 1;
    float3 n = float3(o, 1 - abs(o.x) - abs(o.y));
    const float t = saturate(-n.z);
    n.xy += float2(n.x >= 0 ? -t : t, n.y >= 0 ? -t : t);
    return normalize(n);
}
uint4 layerPackGuide(float deviceDepth, float3 normal, float roughness, uint mode, bool noData, uint hitNormalOct, float hitDistance, uint albedo)
{
    return uint4(asuint(deviceDepth), layerPackOct22(normal) | ((uint)round(saturate(roughness) * 127.0) << 22) | (noData ? 1u << 29 : 0u) | (mode << 30),
                 (hitNormalOct & 0xFFFFu) | (f32tof16(min(hitDistance, 65000.0)) << 16), albedo);
}

struct LayerGuide
{
    uint mode;
    float deviceDepth, linearZ;
    float3 position, normal;  // the receiver (world)
    float roughness;
    float3 hitNormal;
    float hitDistance;
    uint albedo;
};
uint layerMode(uint4 g) { return g.y >> 30; }
bool layerNoData(uint4 g) { return (g.y & (1u << 29)) != 0; }
LayerGuide layerGuide(uint4 g, uint2 pixel)
{
    LayerGuide o;
    o.mode = g.y >> 30;
    o.deviceDepth = asfloat(g.x);
    o.linearZ = linearDepth(max(o.deviceDepth, 1e-30));
    o.position = worldFromDepth(float2(pixel), o.deviceDepth);
    o.normal = layerUnpackOct22(g.y & 0x3FFFFFu);
    o.roughness = ((g.y >> 22) & 0x7Fu) / 127.0;
    o.hitNormal = reflUnpackOct16(g.z & 0xFFFFu);
    o.hitDistance = f16tof32(g.z >> 16);
    o.albedo = g.w;
    return o;
}
// An M pixel's unfolded path length (eye -> mirror -> hit): the depth of the reflected image behind the mirror. Along a
// mirror it changes as a directly seen surface's depth does, and jumps at the reflected objects' outlines.
float layerImageDepth(LayerGuide g) { return g.linearZ + g.hitDistance; }
float layerLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

#endif
