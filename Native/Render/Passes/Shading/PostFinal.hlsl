// unx-kernel: cs_6_6 main
// Post chain, final pass (Post.cpp): exposed HDR (with the scene colour fringe and the sharpen) -> local exposure ->
// bloom mix -> lens flares -> natural vignetting -> tone curve (or the combined grading LUT: grading and curve in one
// lookup) -> grading LUT -> grain -> sRGB OETF -> 10-bit triangular dither -> the display output (RGB10A2).
// P[0] = { HDR SRV, bloom SRV (half resolution; UNX_NONE: off), output UAV, LUT SRV (UNX_NONE: none) },
// P[1] = { asfloat bloom strength, asfloat vignette, asfloat grain, frame index }, P[2] = { width, height, asfloat display
// peak, tone curve (0 film shFilm, 1 PBR Neutral) }, P[3] = { exposure correction SRV (raw: float c; UNX_NONE: none: a
// snap frame's own metering, Exposure.cpp), white balance on (v1.91: P[4..6].xyz = the rows of the 3 x 3 Bradford
// adaptation of the camera's white point to D65 in linear Rec.709, Post.cpp whiteBalanceMatrix; 0 = no multiply),
// HDR encoding, asuint(paper white, cd/m2) }: peak 0 = SDR (above); peak >= 1 = an HDR display (peak over paper
// white): the curve generalised to that peak,
// the LUT on its output over the peak, grain, then linear light with 1 = paper white (RGBA16F output, no OETF, no dither).
// The HDR encoding (FrameContext::displayEncoding; bit 8: the output is a 10-bit UNORM texture):
//   0  that linear Rec.709 light, 1 = paper white (the host encodes it);
//   1  scRGB: linear Rec.709, 1 = 80 cd/m2 (an R16G16B16A16 FLOAT swap chain in DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
//   2  HDR10: Rec.2020 primaries under the ST 2084 curve of the absolute luminance (DXGI_COLOR_SPACE_RGB_FULL_G2084_
//      NONE_P2020), into the swap chain's R10G10B10A2 (a triangular dither of one code) or a float texture.
// Local exposure (shading.post_local_exposure, LocalExposure.hlsli), first of all: P[7] = { grid SRV (UNX_NONE: off),
// blurred SRV, asuint(uv scale x), asuint(uv scale y) }, P[8] = { asuint(highlight), asuint(shadow), asuint(detail),
// asuint(blend) }, P[9].x = asuint(log2 middle grey). The bloom tail is of the image with it (PostDownsample.hlsl).
// P[10] = { asuint(sharpen / 6) (shading.post_sharpen; 0: off), asuint(fringe scale of red), asuint(fringe scale of
// green) (shading.post_fringe; both 0: off), asuint(fringe start) }:
//   fringe    the scene colour fringe (the reference's chromatic aberration in the tonemapper): red and green are read
//             nearer the image centre than blue - per axis, by their scale x the distance beyond the start offset -
//             as a lens focuses the longer wavelengths at a smaller magnification;
//   sharpen   the tonemapper's sharpen: the pixel less the mean of its four neighbours, added back x 4 x sharpen / 6,
//             less next to very bright content (the mask 1 - the largest luminance step to a neighbour, exposed).
// P[11] = { lens flare SRV (RGBA16F, quarter resolution: PostFlare.hlsl; UNX_NONE: off), combined grading LUT SRV
// (Texture3D RGBA16F: PostGradeLut.hlsl; UNX_NONE: the curve is evaluated here), its size, 0 }.
// Frame constants of the view (its projection gives the field angle).
#include "Bindless.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/LocalExposure.hlsli"

// SMPTE ST 2084's inverse EOTF: a luminance over 10 000 cd/m2 to the code value.
float3 st2084(float3 y)
{
    const float m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
    const float3 p = pow(saturate(y), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

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
    const float2 fringe = asfloat(P[10].yz);
    if (fringe.x > 0 || fringe.y > 0)
    {
        const float2 ndc = (float2(id) + 0.5) / float2(P[2].xy) * 2 - 1;
        const float2 beyond = sign(ndc) * saturate(abs(ndc) - asfloat(P[10].w));
        e.r = hdr.SampleLevel(g_linearClamp, (ndc - beyond * fringe.x) * 0.5 + 0.5, 0).r;
        e.g = hdr.SampleLevel(g_linearClamp, (ndc - beyond * fringe.y) * 0.5 + 0.5, 0).g;
    }
    const float sharpen = asfloat(P[10].x);
    if (sharpen > 0)
    {
        const int2 last = int2(P[2].xy) - 1;
        const float3 c1 = hdr.Load(int3(max(int2(id) - int2(1, 0), 0), 0)).rgb, c2 = hdr.Load(int3(min(int2(id) + int2(1, 0), last), 0)).rgb;
        const float3 c3 = hdr.Load(int3(max(int2(id) - int2(0, 1), 0), 0)).rgb, c4 = hdr.Load(int3(min(int2(id) + int2(0, 1), last), 0)).rgb;
        const float3 y = float3(0.3, 0.59, 0.11);
        const float4 steps = abs(dot(e, y) - float4(dot(c1, y), dot(c2, y), dot(c3, y), dot(c4, y)));
        const float edgeMask = saturate(1.0 - correctionFactor * max(max(steps.x, steps.y), max(steps.z, steps.w)));
        e -= (c1 + c2 + c3 + c4 - 4.0 * e) * (edgeMask * sharpen);
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
    if (P[11].x != UNX_NONE)
    {
        // image-based lens flares: stray light the lens adds (ghosts of the bright parts, PostFlare.hlsl)
        Texture2D<float4> flare = ResourceDescriptorHeap[P[11].x];
        e += flare.SampleLevel(g_linearClamp, (float2(id) + 0.5) / float2(P[2].xy), 0).rgb;
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
    if (P[11].y != UNX_NONE)
    {
        // the combined LUT (PostGradeLut.hlsl): the grading and the curve at the scene colour's log2 code (14 stops
        // around grey 0.18 at 444 / 1023, 0 at code 0), the value's square root stored
        Texture3D<float4> grade = ResourceDescriptorHeap[P[11].y];
        const float n = (float)P[11].z;
        const float3 code = saturate(log2(max(e, 0.0) + 0.18 * exp2(-444.0 / 1023.0 * 14.0)) / 14.0 - log2(0.18) / 14.0 + 444.0 / 1023.0);
        const float3 v = grade.SampleLevel(g_linearClamp, code * ((n - 1.0) / n) + 0.5 / n, 0).rgb;
        d = saturate(v * v);
    }
    else if (P[2].w == 1u) d = hdrDisplay ? saturate(shPbrNeutralPeak(max(e, 0.0), peak) / peak) : saturate(shPbrNeutral(max(e, 0.0)));
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
        float3 light = d * range;  // linear Rec.709, 1 = paper white
        const uint encoding = P[3].z & 0xFFu;
        const float paperWhite = asfloat(P[3].w);
        if (encoding == 1u) light *= paperWhite / 80.0;
        else if (encoding == 2u)
        {
            // Rec.709 to Rec.2020 primaries (ITU-R BT.2087; both D65), the absolute luminance, the curve
            const float3 wide = float3(dot(light, float3(0.6274, 0.3293, 0.0433)), dot(light, float3(0.0691, 0.9195, 0.0114)),
                                       dot(light, float3(0.0164, 0.0880, 0.8956)));
            light = st2084(max(wide, 0.0) * (paperWhite / 10000.0));
            if ((P[3].z & 0x100u) != 0)
            {
                const float e10 = hashUnit(uint3(id, P[1].w ^ 0x5bd1e995u)) + hashUnit(uint3(id.yx, P[1].w + 104729u)) - 1.0;
                light = round(saturate(light + e10 / 1023.0) * 1023.0) / 1023.0;
            }
        }
        linearOutput[id] = float4(light, 1);
        return;
    }
    float3 o = float3(shSrgbOetf(d.r), shSrgbOetf(d.g), shSrgbOetf(d.b));
    const float n = hashUnit(uint3(id, P[1].w ^ 0x5bd1e995u)) + hashUnit(uint3(id.yx, P[1].w + 104729u)) - 1.0;
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].z];
    // triangular dither of one 10-bit step, rounded here to the nearest code: the hardware's float -> UNORM conversion
    // may be off by up to 0.6 ULP (PostTests measured 3 % of channels one code low), which biases the dither
    output[id] = float4(round(saturate(o + n / 1023.0) * 1023.0) / 1023.0, 1);
}
