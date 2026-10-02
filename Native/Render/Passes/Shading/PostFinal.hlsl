// unx-kernel: cs_6_6 main
// Post chain, final pass (Post.cpp): exposed HDR -> bloom mix -> natural vignetting -> tone curve -> grading LUT ->
// grain -> sRGB OETF -> 10-bit triangular dither -> the display output (RGB10A2).
// P[0] = { HDR SRV, bloom SRV (half resolution; UNX_NONE: off), output UAV, LUT SRV (UNX_NONE: none) },
// P[1] = { asfloat bloom strength, asfloat vignette, asfloat grain, frame index }, P[2] = { width, height, asfloat display
// peak, tone curve (0 film shFilm, 1 PBR Neutral) }, P[3] = { exposure correction SRV (raw: float c; UNX_NONE: none: a
// snap frame's own metering, Exposure.cpp), white balance on (v1.91: P[4..6].xyz = the rows of the 3 x 3 Bradford
// adaptation of the camera's white point to D65 in linear Rec.709, Post.cpp whiteBalanceMatrix; 0 = no multiply),
// 0, 0 }: peak 0 = SDR (above); peak >= 1 = an HDR display (peak over paper
// white): the curve generalised to that peak,
// the LUT on its output over the peak, grain, then linear light with 1 = paper white (RGBA16F output, no OETF, no dither).
// Local exposure (shading.post_local_exposure, LocalExposure.hlsli), first of all: P[7] = { grid SRV (UNX_NONE: off),
// blurred SRV, asuint(uv scale x), asuint(uv scale y) }, P[8] = { asuint(highlight), asuint(shadow), asuint(detail),
// asuint(blend) }, P[9].x = asuint(log2 middle grey). The bloom tail is of the image with it (PostDownsample.hlsl).
// Frame constants of the view (its projection gives the field angle).
#include "Bindless.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/LocalExposure.hlsli"

float hashUnit(uint3 v)  // [0, 1), deterministic (PCG3D)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return (v.x >> 8) * (1.0 / 16777216.0);
}

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[2].xy)) return;
    Texture2D<float4> hdr = ResourceDescriptorHeap[P[0].x];
    float3 e = hdr[id].rgb;
    float correctionFactor = 1;
    if (P[3].x != UNX_NONE)
    {
        ByteAddressBuffer correction = ResourceDescriptorHeap[P[3].x];
        correctionFactor = asfloat(correction.Load(0));  // a snap frame exposed for itself (Exposure.cpp, ExposureMeter.hlsl)
    }
    if (P[7].x != UNX_NONE)
    {
        LeParams le;
        le.grid = P[7].x;
        le.blurred = P[7].y;
        le.uvScale = asfloat(P[7].zw);
        le.highlight = asfloat(P[8].x);
        le.shadow = asfloat(P[8].y);
        le.detail = asfloat(P[8].z);
        le.blend = asfloat(P[8].w);
        le.logMiddleGrey = asfloat(P[9].x);
        e *= leScale(le, e, (float2(id) + 0.5) / float2(P[2].xy), log2(max(correctionFactor, 1e-6)));
    }
    const float bloom = asfloat(P[1].x);
    if (P[0].y != UNX_NONE && bloom > 0)
    {
        Texture2D<float4> tail = ResourceDescriptorHeap[P[0].y];
        uint bw, bh;
        tail.GetDimensions(bw, bh);
        const float2 p = (float2(id) + 0.5) * 0.5 - 0.5;  // bilinear at this pixel's centre (half resolution)
        const int2 b = (int2)floor(p);
        const float2 f = p - b;
        const int2 hi = int2(bw, bh) - 1;
        const float3 t00 = tail.Load(int3(clamp(b, int2(0, 0), hi), 0)).rgb, t10 = tail.Load(int3(clamp(b + int2(1, 0), int2(0, 0), hi), 0)).rgb;
        const float3 t01 = tail.Load(int3(clamp(b + int2(0, 1), int2(0, 0), hi), 0)).rgb, t11 = tail.Load(int3(clamp(b + int2(1, 1), int2(0, 0), hi), 0)).rgb;
        e = lerp(e, lerp(lerp(t00, t10, f.x), lerp(t01, t11, f.x), f.y), bloom);  // PSF = (1 - s) delta + s tail
    }
    e *= correctionFactor;
    if (P[3].y != 0)
    {
        // v1.91 camera white balance: the scene's white (the illuminant the camera is set to) to the display's D65, a
        // linear 3 x 3 before the non-linear curve (defect queue 6: a D65-fixed display left tungsten rooms orange)
        const float3x3 wb = float3x3(asfloat(P[4].xyz), asfloat(P[5].xyz), asfloat(P[6].xyz));
        e = mul(wb, e);
    }
    const float vignette = asfloat(P[1].y);
    if (vignette > 0)
    {
        // cos^4 vignetting on a circle through the corners, whatever the aspect and the field of view (the reference's
        // VignetteSpace and cosine fourth law): the corners sit at radius sqrt(2), tan(angle) = radius x intensity
        const float2 ndc = (float2(id) + 0.5) / float2(P[2].xy) * 2 - 1;
        const float aspect = (float)P[2].y / (float)P[2].x;
        const float2 t = ndc * float2(1.0, aspect) * (1.4142136 / sqrt(1.0 + aspect * aspect)) * vignette;
        const float c2 = 1.0 / (1.0 + dot(t, t));
        e *= c2 * c2;
    }
    const float peak = asfloat(P[2].z);
    const bool hdrDisplay = peak > 0;
    const float range = hdrDisplay ? peak : 1.0;
    // d: the curve's output over the display's range (0..1), so the LUT and grain act the same in SDR and HDR
    float3 d;
    if (P[2].w == 1u) d = hdrDisplay ? saturate(shPbrNeutralPeak(max(e, 0.0), peak) / peak) : saturate(shPbrNeutral(max(e, 0.0)));
    else d = saturate(shFilm(max(e, 0.0), range) / range);
    if (P[0].w != UNX_NONE)
    {
        Texture3D<float4> lut = ResourceDescriptorHeap[P[0].w];
        uint lw, lh, ld;
        lut.GetDimensions(lw, lh, ld);
        const float3 c = d * float3(lw - 1, lh - 1, ld - 1);  // the .cube domain [0, 1] onto the first..last texel
        const int3 i0 = (int3)floor(c);
        const int3 i1 = min(i0 + 1, int3(lw, lh, ld) - 1);
        const float3 f = c - i0;
        float3 v = 0;
        [unroll] for (uint k = 0; k < 8; ++k)
        {
            const int3 i = int3((k & 1) ? i1.x : i0.x, (k & 2) ? i1.y : i0.y, (k & 4) ? i1.z : i0.z);
            const float w = ((k & 1) ? f.x : 1 - f.x) * ((k & 2) ? f.y : 1 - f.y) * ((k & 4) ? f.z : 1 - f.z);
            v += w * lut.Load(int4(i, 0)).rgb;
        }
        d = saturate(v);
    }
    const float grain = asfloat(P[1].z);
    if (grain > 0)
    {
        // luminance grain per pixel and frame, multiplicative in linear light: d (1 + n) with n triangular of relative
        // standard deviation 'grain' (< 0.41, so 1 + n > 0). Zero-mean in energy, black stays black and the hue is kept;
        // an additive grain clipped at 0 lifted the shadows (PostTests: +6.9 10-bit steps at 0.01).
        const float n = hashUnit(uint3(id, P[1].w)) + hashUnit(uint3(id, P[1].w + 7919u)) - 1.0;
        d = saturate(d * (1.0 + n * grain * 2.4494897));
    }
    if (hdrDisplay)
    {
        RWTexture2D<float4> linearOutput = ResourceDescriptorHeap[P[0].z];
        linearOutput[id] = float4(d * range, 1);
        return;
    }
    float3 o = float3(shSrgbOetf(d.r), shSrgbOetf(d.g), shSrgbOetf(d.b));
    const float n = hashUnit(uint3(id, P[1].w ^ 0x5bd1e995u)) + hashUnit(uint3(id.yx, P[1].w + 104729u)) - 1.0;
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].z];
    // triangular dither of one 10-bit step, rounded here to the nearest code: the hardware's float -> UNORM conversion
    // may be off by up to 0.6 ULP (PostTests measured 3 % of channels one code low), which biases the dither
    output[id] = float4(round(saturate(o + n / 1023.0) * 1023.0) / 1023.0, 1);
}
