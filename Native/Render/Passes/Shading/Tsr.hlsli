// Temporal super resolution (output.upscale_tsr; Upscale.cpp): the structure, rules and numbers of Unreal Engine's TSR
// (ue6-main Engine/Shaders/Private/TemporalSuperResolution/*, PostProcess/TemporalSuperResolution.cpp, read 2026-10-02 as
// a reference; the code is ours). Per frame, at the internal resolution unless stated:
//   m.upscale.motion    each sample's reprojection vector and its point's previous view depth (UpscaleMotion.hlsl);
//   m.tsr.dilate        the closest depth of the 3 x 3 neighbourhood and its vector (edges take the foreground's), the
//                       depth error of the neighbourhood's slope, the reprojection edge (how much the dilated vector
//                       differs from the pixel's own), and every pixel scattered to where it was in the previous frame
//                       with its previous depth: the closest occluder there among this frame's surfaces. With
//                       output.upscale_tsr_reprojection_field the reprojection field: besides the vector its jacobian
//                       (how the vector changes across the pixel, from the neighbours on the pixel's own surface) and,
//                       on an edge whose two sides move apart, the boundary - where in the pixel the foreground ends,
//                       found by following the depth edge both ways as the spatial anti-aliaser follows a luma edge;
//   m.tsr.decimate      parallax disocclusion (something closer landed where this pixel was: it was hidden then), the
//                       previous guide - a low-resolution copy of the history in a perceptual space - reprojected, the
//                       reprojection edge over the dilated vectors;
//   m.tsr.flicker       the flickering heuristic: each pixel's luma followed over time; a gradient that flips its sign
//                       every frame on a still surface is the jitter beating against a pattern finer than the pixels,
//                       and its amplitude is the band inside which the rejection lets the history be;
//   m.tsr.reject        the shading rejection: input and reprojected guide compared at low frequency after each was
//                       clamped into the other's 3 x 3 range (their aliasing differs every frame and is no change of
//                       shading); what the comparison's clamp box removes from the filtered guide, over the larger of
//                       the two signals' difference and the box, is the rejection. Also the guide of this frame, the
//                       LDR luma and mask of the spatial anti-aliaser;
//   m.tsr.aa            spatial anti-aliasing of the pixels whose history is rejected or missing: the edge through the
//                       pixel is followed both ways (8 steps) and the pixel's sample position moves across it;
//   m.upscale           (output resolution) the history update: 5 input samples around the output pixel under a kernel
//                       as wide as an input pixel while the history is missing or rejected and as an output pixel
//                       while it refines; the history reprojected (Catmull-Rom), clamped to the samples' range only as
//                       far as the rejection says, its weight a validity in [0, 1] of 16 samples, held down where the
//                       pixel moves (4 samples at one output pixel a frame) and where it was rejected (2 samples).
// The history update reads the field per output pixel: on a boundary the vector of the side the output pixel lies on,
// and that vector carried to the output pixel's own position by the jacobian (a turn or a zoom reprojects every output
// pixel to its own place instead of all of an input pixel's to one offset).
// Not here yet: history resurrection, thin geometry detection, a history above the output resolution, lens distortion.
// The flickering heuristic follows the final scene colour (the reference: the colour before translucency, and less by
// what translucency changed).
#ifndef UNX_TSR_HLSLI
#define UNX_TSR_HLSLI
#include "Bindless.hlsli"

// The reference's settings (r.TSR.History.SampleCount 16, ShadingRejection.SampleCount 2, Velocity.WeightClampingSampleCount
// 4, Velocity.WeightClampingPixelSpeed 1; history at the output resolution).
#define TSR_HISTORY_SAMPLES 16.0
#define TSR_HYSTERESIS (1.0 / TSR_HISTORY_SAMPLES)
#define TSR_WEIGHT_CLAMPING_REJECTION (1.0 - 2.0 * TSR_HYSTERESIS)
#define TSR_WEIGHT_CLAMPING_SPEED_AMPLITUDE (1.0 - 4.0 * TSR_HYSTERESIS)
#define TSR_INV_WEIGHT_CLAMPING_PIXEL_SPEED 1.0
#define TSR_AA_MIN_LUMINANCE 0.05
#define TSR_QUANTIZATION_ERROR (0.5 / 1024.0)  // a 10-bit guide

// Colour spaces. Linear: the scene colour as the upscale gets it (exposed linear radiance).
//   guide         x / (x + 0.17): what the guide history stores (10-bit unorm);
//   measurement   guide^2: where the shading rejection compares input and guide;
//   accumulation  x / (x + 1): where the guide is blended.
float3 tsrLinearToGuide(float3 c) { return c / (c + 0.17); }
float3 tsrGuideToLinear(float3 g) { return g * min(0.17 / max(1.0 - g, 1e-6), 65504.0); }
float3 tsrGuideToMeasure(float3 g) { return g * g; }
float3 tsrLinearToMeasure(float3 c) { return tsrGuideToMeasure(tsrLinearToGuide(c)); }
float3 tsrToAccumulation(float3 c) { return c / (c + 1.0); }
float3 tsrFromAccumulation(float3 a) { return a * min(1.0 / max(1.0 - a, 1e-6), 65504.0); }

// Tone weights (Karis): 4 x luma and 1 / (4 + 4 Y).
float tsrLuma4(float3 c) { return c.g * 2.0 + (c.r + c.b); }
float tsrHdrWeightY(float luma4) { return max(1e-4, 1.0 / (luma4 + 4.0)); }
float tsrHdrWeight(float3 c) { return tsrHdrWeightY(tsrLuma4(c)); }

// Weight of a sample 'pixelDelta' input pixels from the kernel's centre: 1 - 1.9 x^2 + 0.9 x^4 with x = upscaleFactor x
// distance (1 output pixel wide at upscaleFactor = output / input size).
float tsrSampleWeight(float upscaleFactor, float2 pixelDelta, float minimalContribution)
{
    const float x2 = saturate(upscaleFactor * upscaleFactor * dot(pixelDelta, pixelDelta));
    return saturate((0.9 * x2 - 1.9) * x2 + (1.0 + minimalContribution));
}

// The spatial anti-aliaser's sample offset: 4 bits a channel over +- 0.5 input pixels.
uint tsrEncodeAaOffset(float2 texelOffset)
{
    const int2 q = clamp(int2(round(texelOffset * 14.0)) + 8, 1, 15);
    return (uint)q.x | ((uint)q.y << 4);
}
float2 tsrDecodeAaOffset(uint encoded) { return (float2(encoded & 15u, (encoded >> 4) & 15u) - 8.0) / 14.0; }

// The four texels around a position (texel units, centres at + 0.5) and their bilinear weights.
struct TsrBilinear
{
    int2 base;
    float2 f;
};
TsrBilinear tsrBilinear(float2 texelPosition)
{
    TsrBilinear b;
    const float2 p = texelPosition - 0.5;
    b.base = int2(floor(p));
    b.f = p - floor(p);
    return b;
}
int2 tsrBilinearTexel(TsrBilinear b, uint i) { return b.base + int2(i & 1u, i >> 1); }
float tsrBilinearWeight(TsrBilinear b, uint i) { return ((i & 1u) ? b.f.x : 1.0 - b.f.x) * ((i >> 1) ? b.f.y : 1.0 - b.f.y); }

// The closest occluder scatter (m.tsr.dilate -> m.tsr.decimate): previous device depth as a half in the high 14 bits'
// place and the pixel's reprojection vector in polar form below it (5 bits of angle, 13 of length at 1/32 pixel), so the
// largest value is the closest surface and brings its vector.
#define TSR_HOLE_BITS 18u
#define TSR_HOLE_LENGTH_BITS 13u
#define TSR_HOLE_ANGLE_BITS 5u
#define TSR_HOLE_LENGTH_PRECISION 32.0
uint tsrEncodeHoleVelocity(float2 pixelVelocity)
{
    const float angle = atan2(pixelVelocity.y, pixelVelocity.x);
    const float len = length(pixelVelocity);
    const uint a = (uint)(int)round(angle * (0.5 * 32.0 / 3.14159265) + 0.5 * 32.0) & 31u;
    const uint l = min((uint)ceil(len * TSR_HOLE_LENGTH_PRECISION), (1u << TSR_HOLE_LENGTH_BITS) - 1u);
    return (a << TSR_HOLE_LENGTH_BITS) | l;
}
void tsrDecodeHoleVelocity(uint encoded, out float angle, out float len)
{
    len = (float)(encoded & ((1u << TSR_HOLE_LENGTH_BITS) - 1u)) / TSR_HOLE_LENGTH_PRECISION;
    angle = (float)((encoded >> TSR_HOLE_LENGTH_BITS) & 31u) * (2.0 * 3.14159265 / 32.0) - 3.14159265;
}
float tsrMaxHoleLength() { return ((float)(1u << TSR_HOLE_LENGTH_BITS) - 1.5) / TSR_HOLE_LENGTH_PRECISION; }

// The reprojection field (m.tsr.dilate -> m.upscale; the reference's TSRReprojectionField.ush), RGBA32_UINT per internal
// pixel: x = the jacobian, y = the dilation's offset and the boundary, z = the closest device depth's bits.
//   jacobian   rows d(vector) / dx and d(vector) / dy in input pixels per input pixel, 8 bits a component over +- 2 on
//              a square-root scale (small values keep their precision);
//   offset     the neighbour the dilated vector came from, [-1, 1]^2 in 4 bits;
//   boundary   the vector from the edge's line through the pixel to the pixel's corner deepest in the foreground, 11 bits
//              a component over [-1, 1] input pixels. (0, 1) with any offset: the whole pixel is the foreground's.
#define TSR_JACOBIAN_RANGE 2.0
uint tsrEncodeJacobian(float2 dx, float2 dy)
{
    float4 j = clamp(float4(dx, dy), -TSR_JACOBIAN_RANGE, TSR_JACOBIAN_RANGE);
    j = sign(j) * sqrt(abs(j)) * sqrt(TSR_JACOBIAN_RANGE);
    const uint4 q = uint4(j * (127.0 / TSR_JACOBIAN_RANGE) + 127.5);
    return q.x | (q.y << 8) | (q.z << 16) | (q.w << 24);
}
void tsrDecodeJacobian(uint encoded, out float2 dx, out float2 dy)
{
    float4 j = float4(encoded & 255u, (encoded >> 8) & 255u, (encoded >> 16) & 255u, encoded >> 24) * (TSR_JACOBIAN_RANGE / 127.0) - TSR_JACOBIAN_RANGE;
    j = j * abs(j) / TSR_JACOBIAN_RANGE;
    dx = j.xy;
    dy = j.zw;
}
// How much the reprojection magnifies the history (>= 1), from the jacobian alone.
float tsrJacobianUpscale(float2 dx, float2 dy)
{
    const float2 e = float2(1, 0) + dx, s = float2(0, 1) + dy;
    return sqrt(max(dot(e, e), 1.0) * max(dot(s, s), 1.0));
}
uint tsrEncodeBoundary(int2 offset, float2 boundary)
{
    const int2 bits = clamp(int2(round(boundary * 1023.0)) + 1024, 1, 2047);
    return (uint)(offset.x + 1) | ((uint)(offset.y + 1) << 2) | ((uint)bits.x << 4) | ((uint)bits.y << 15);
}
int2 tsrDecodeBoundaryOffset(uint encoded) { return int2(encoded & 3u, (encoded >> 2) & 3u) - 1; }
float2 tsrDecodeBoundary(uint encoded)
{
    return clamp(float2(int2((encoded >> 4) & 2047u, (encoded >> 15) & 2047u) - 1024) / 1023.0, -1.0, 1.0);
}
// The field's code of a pixel with no offset and the whole pixel inside (nothing to choose).
#define TSR_NO_BOUNDARY (5u | (1024u << 4) | (2047u << 15))
// Whether an output pixel at dKO (input pixels from the input pixel's centre), outputPixel input pixels wide, reaches
// over the boundary into the foreground's side.
bool tsrInsideBoundary(float2 dKO, float2 boundary, float outputPixel)
{
    const float2 toCorner = float2(boundary.x < 0 ? -0.5 : 0.5, boundary.y < 0 ? -0.5 : 0.5);  // (-0: positive, as the store rounds)
    const float2 fromBoundary = boundary - toCorner + dKO + toCorner * outputPixel;
    return dot(fromBoundary, boundary) > 0;
}

// Catmull-Rom of t at uv (size texels): the 4 x 4 texels' separable weights as 5 bilinear taps (the corner taps left out).
float4 tsrCatmullRom(Texture2D<float4> t, float2 uv, float2 size)
{
    const float2 pos = uv * size - 0.5;
    const float2 f0 = floor(pos), f = pos - f0;
    const float2 w0 = f * (-0.5 + f * (1 - 0.5 * f));
    const float2 w1 = 1 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (f0 - 0.5) / size, t3 = (f0 + 2.5) / size, t12 = (f0 + 0.5 + w2 / w12) / size;
    const float wa = w12.x * w0.y, wb = w0.x * w12.y, wc = w12.x * w12.y, wd = w3.x * w12.y, we = w12.x * w3.y;
    const float4 r = t.SampleLevel(g_linearClamp, float2(t12.x, t0.y), 0) * wa + t.SampleLevel(g_linearClamp, float2(t0.x, t12.y), 0) * wb +
                     t.SampleLevel(g_linearClamp, t12, 0) * wc + t.SampleLevel(g_linearClamp, float2(t3.x, t12.y), 0) * wd +
                     t.SampleLevel(g_linearClamp, float2(t12.x, t3.y), 0) * we;
    return r / (wa + wb + wc + wd + we);
}

#endif
