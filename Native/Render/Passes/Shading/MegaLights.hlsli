// Stochastic direct light of the local lights (shading.mega_lights; owner A). The structure, the pass order and the
// default numbers follow Unreal Engine's MegaLights (ue6-main, Renderer/Private/MegaLights and Shaders/Private/MegaLights,
// read on 2026-10-01); the code is ours (no Epic code is copied). Passes of the main view, before the shading group:
//   m.ml.tiles     the downsampled tiles that hold a surface (the sample kernel's dispatch list);
//   m.ml.sample    per downsampled pixel (2 x 2 by default, one jittered pixel of the block per frame): the froxel list's
//                  lights weighed by log2(1 + unshadowed luminance x exposure) (lights hidden in the previous frame's tile
//                  weigh less), a stratified weighted reservoir picks N lights (4 by default);
//   m.ml.trace     one shadow ray per sample toward its point on the light (DispatchRays; the R track's ray scene);
//   m.ml.shade     per pixel (ShadeOpaque.hlsl compiled with MEGA_LIGHTS = 1): one of the 4 downsampled pixels around it,
//                  chosen at random by bilinear x plane x normal weights; its visible samples' lights are shaded with the
//                  pixel's own model, diffuse and specular apart, divided by the modulation factors;
//   m.ml.hash      the lights that were visible / hidden in each 8 x 8 tile, as small bit sets, for the next frame's sample;
//   m.ml.temporal  reprojected history, clamped to the neighbourhood, at most 12 frames (less where few lights decide);
//   m.ml.spatial   variance-guided filter (disk of 8 px), wider for 2 frames after a disocclusion; modulation back;
// the shading kernels then skip their local-light loop and add the result. The air: s.ml.volume
// (Passes/Atmosphere/MegaLightsVolume.hlsl) samples the froxels' lights the same way and S's integration adds it. Points
// off the screen: MegaLightsSampling.hlsli (the point, the target weight, the reservoir) and MegaLightsWorld.hlsli (R's
// world light grid, the shadow ray).
// Where it gives up accuracy for cost (listed for the user in Docs/Status/UNREAL_COMPARISON_LIGHTS_KO.md 5): N samples
// per downsampled pixel, the per-sample weight cap, the smooth cut of contributions under the minimum sample weight, the
// binary visibility of one ray, the history length and the filter radius. All are keys of shading.toml with Unreal's
// defaults.
#ifndef UNX_MEGA_LIGHTS_HLSLI
#define UNX_MEGA_LIGHTS_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "GBuffer.hlsli"
#include "MaterialModel.hlsli"

#define ML_LIGHT_NONE 0x7FFFu
#define ML_MAX_SAMPLES 4u
#define ML_HASH_TILE 8u       // pixels: one visible / hidden set per 8 x 8 tile
#define ML_HASH_WORDS 6u      // 4 words visible, 2 words hidden

// One light sample (R32G32_UINT texel): x = light (15 bits) | needs a ray (bit 15) | point on the light u (6 bits) | v
// (6 bits) | guided as visible (bit 28) | merged into the next sample of the same light (bit 29) | visible (bit 31);
// y = the weight, sum of the candidates' weights over this light's (1 / (N x probability) before the N).
struct MlSample
{
    uint light;
    bool needsRay;
    float2 uv;
    bool guidedVisible;
    bool merged;
    bool visible;
    float weight;
};
MlSample mlNoSample()
{
    MlSample s;
    s.light = ML_LIGHT_NONE;
    s.needsRay = false;
    s.uv = 0.5;
    s.guidedVisible = false;
    s.merged = false;
    s.visible = false;
    s.weight = 0;
    return s;
}
uint2 mlPack(MlSample s)
{
    uint x = s.light & 0x7FFFu;
    x |= s.needsRay ? 0x8000u : 0u;
    x |= uint(saturate(s.uv.x) * 63.0 + 0.5) << 16;
    x |= uint(saturate(s.uv.y) * 63.0 + 0.5) << 22;
    x |= s.guidedVisible ? (1u << 28) : 0u;
    x |= s.merged ? (1u << 29) : 0u;
    x |= s.visible ? (1u << 31) : 0u;
    return uint2(x, asuint(s.weight));
}
MlSample mlUnpack(uint2 v)
{
    MlSample s;
    s.light = v.x & 0x7FFFu;
    s.needsRay = (v.x & 0x8000u) != 0;
    s.uv = float2((v.x >> 16) & 63u, (v.x >> 22) & 63u) / 63.0;
    s.guidedVisible = (v.x & (1u << 28)) != 0;
    s.merged = (v.x & (1u << 29)) != 0;
    s.visible = (v.x & (1u << 31)) != 0;
    s.weight = asfloat(v.y);
    return s;
}

// Sample layout of N samples per downsampled pixel: 1 -> 1 x 1, 2 -> 2 x 1, 4 -> 2 x 2.
uint2 mlSampleGrid(uint n) { return uint2(n >= 2 ? 2 : 1, n >= 4 ? 2 : 1); }
uint2 mlSampleCoord(uint2 ds, uint n, uint i)
{
    const uint2 g = mlSampleGrid(n);
    return ds * g + uint2(i % g.x, i / g.x);
}

// The full-resolution pixel a downsampled pixel stands on this frame: for factor 2 the block's four pixels in turn over
// four frames, neighbouring blocks out of phase (a 4-rooks pattern).
uint2 mlJitter(uint2 ds, uint factor, uint frame)
{
    if (factor < 2) return 0;
    const uint i = (ds.x & 1u) + ds.y * 2u + frame;
    return uint2((i & 2u) ? 1u : 0u, (i & 1u) ? 0u : 1u);
}
uint2 mlFullPixel(uint2 ds, uint factor, uint frame)
{
    return min(ds * factor + mlJitter(ds, factor, frame), uint2(g_viewWidth, g_viewHeight) - 1);
}

// Random numbers: interleaved gradient noise moved by the frame (64-frame cycle) and by a per-use salt.
float mlNoise(uint2 p, uint frame, uint salt)
{
    const float2 q = float2(p) + 5.588238 * float(frame & 63u) + float2(47.0, 17.0) * float(salt);
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}
uint mlHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float mlLuminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Smooth cut of what lies under the minimum sample weight t: 1 well above it, 0 at and below it.
float mlFalloffMask(float x, float t)
{
    if (!(x > 0)) return 0;
    const float r = t / x, r2 = r * r, k = saturate(1 - r2 * r2);
    return k * k;
}

// The tile sets: a light sets two bits of the 128-bit visible set (or of the 64-bit hidden set); a light is in the set when
// both are set (false positives only: a hidden light taken as visible is sampled as before).
bool mlInVisible(uint4 set, uint light)
{
    const uint h = mlHash(light);
    const uint w0 = h & 3u, w1 = (h >> 2) & 3u, b0 = (h >> 4) & 31u, b1 = (h >> 9) & 31u;
    return ((set[w0] >> b0) & 1u) != 0 && ((set[w1] >> b1) & 1u) != 0;
}
void mlMarkVisible(inout uint4 set, uint light)
{
    const uint h = mlHash(light);
    set[h & 3u] |= 1u << ((h >> 4) & 31u);
    set[(h >> 2) & 3u] |= 1u << ((h >> 9) & 31u);
}
bool mlInHidden(uint2 set, uint light)
{
    const uint h = mlHash(light);
    const uint w0 = h & 1u, w1 = (h >> 1) & 1u, b0 = (h >> 2) & 31u, b1 = (h >> 7) & 31u;
    return ((set[w0] >> b0) & 1u) != 0 && ((set[w1] >> b1) & 1u) != 0;
}
void mlMarkHidden(inout uint2 set, uint light)
{
    const uint h = mlHash(light);
    set[h & 1u] |= 1u << ((h >> 2) & 31u);
    set[(h >> 1) & 1u] |= 1u << ((h >> 7) & 31u);
}

// What the lighting is divided by before the filters and multiplied by after them (the surface detail that is not noise):
// diffuse by the diffuse colour, specular by the lobe's directional albedo, both kept away from 0 so that neighbours of
// another material do not blow up. The same inputs in m.ml.shade and m.ml.spatial: G-buffer base colour and roughness,
// the material word's metallic, the material's specular, n.v of the normal turned to the viewer.
float3 mlDiffuseFactor(float3 baseColor, float metallic) { return lerp(0.04, 1.0, baseColor * (1 - metallic)); }
float3 mlSpecularFactor(float3 baseColor, float metallic, float specular, float roughness, float NoV)
{
    // the base lobe's directional albedo with multiple scattering (ShadingCommon.hlsli shSpecularAlbedo without the film)
    const float3 f0 = lerp((0.08 * specular).xxx, baseColor, metallic);
    const float2 ab = modelSpecularAlbedo(max(NoV, 1e-4), roughness);
    const float e = ab.x + ab.y;
    return lerp(0.02, 1.0, (f0 * ab.x + ab.y) * (1 + f0 * (1 / e - 1)));
}

float3 mlToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 mlFromYCoCg(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }

// The history length a shading confidence allows (the confidence is the mean probability mass of the pixel's visible
// samples: near 1 one or two lights decide the pixel and there is little to average).
float mlConfidenceFrames(float confidence, float maxFrames) { return confidence > 0 ? 1.0 / (confidence * 0.5) : maxFrames; }
#endif
