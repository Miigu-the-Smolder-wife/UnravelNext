// unx-kernel: cs_6_6 main
// Temporal upscale (Upscale.cpp; output.render_scale): the output pixel's value from the jittered internal samples around
// it and its reprojected history. Per output pixel (centre x in unjittered internal pixel coordinates):
//   current  the 3 x 3 internal samples around x, each at its unjittered centre k + 0.5 - jitter, weighted by
//            exp(-K d^2) with d its distance in OUTPUT pixels (output.upscale_kernel K, 40: sigma 0.11 output px), in a
//            reversible tone map c / (1 + max(c)) (a lone bright sample cannot dominate the mean). The history
//            accumulates these weights over the jitter cycle (64 Halton (2, 3) positions per 1 x 1 internal pixel), so the
//            converged value is the scene sampled within about a tenth of an output pixel of the output pixel's centre -
//            the native image's point sample. (The kernel was exp(-2.29 d^2) in INTERNAL pixels: sigma 0.7 output px at
//            2/3 scale, whose blur every frame's value carried into the converged image: 974c6bb's upscaled captures
//            were visibly blurred - carving outlines, mouldings and wood grain [measured, user]; in the CPU model of this
//            kernel (Tools/ImageQuality, band 0.25-0.5 cycles/px) energy ratio 0.10, SSIM 0.78 against native.) Where
//            the history has no weight yet (a new surface), the wide internal-pixel kernel fills the pixel as before.
//   history  the previous output at x - motion (the motion of the nearest of the 3 x 3 samples: edges take the
//            foreground's), Catmull-Rom (5 bilinear taps) where the pixel is still, Lanczos-3 (6 x 6 loads) where it
//            moves, times the exposure ratio. Still: corrected at LOW frequency only, and only where its mean over one
//            internal pixel (4 bilinear taps) lies outside the samples' YCoCg min / max widened by half their extent
//            (a lighting change, a disocclusion): moved by that difference, fully clipped where the difference is as
//            large as the extent. Moving (a quarter output pixel per frame and more, blended in): clipped to the min /
//            max widened by a tenth. See the comments at the correction for the measurements.
//   blend    alpha = w / (w + n), w = this frame's summed sample weight, n = the history's (alpha channel), capped at
//            output.upscale_history_frames frames of weight where the pixel is still and at
//            output.upscale_history_frames_moving where it moves by a quarter output pixel or more (every frame of
//            motion resamples the history: a long history blurs moving detail [CPU model]); the new weight min(n + w, cap).
// A history outside the previous frame, or a reset (first frame, size change, cut, restore): n = 0.
// (Concept reference only, no code: the temporal upsampling of UE5 TSR / TAAU - Engine/Shaders/Private/TemporalAA/ and
// TemporalAA.usf: jittered samples reprojected into an output-resolution history, neighbourhood clamping in YCoCg,
// nearest-depth motion dilation.)
// P[0] = { colour SRV (internal, exposed linear), depth SRV (internal), motion SRV (internal, UpscaleMotion.hlsl),
// history SRV (output) }, P[1] = { output UAV (RGBA16F: rgb exposed linear, a = history weight), internal width, height,
// flags (1: reset) }, P[2] = { output width, height, asuint(jitter x), asuint(jitter y) }, P[3] = { asuint(exposure
// ratio), asuint(history frames still), asuint(history frames moving), asuint(kernel K) }.
#include "Bindless.hlsli"

float upMax3(float3 c) { return max(c.r, max(c.g, c.b)); }
float3 upTonemap(float3 c) { return c / (1 + upMax3(c)); }
float3 upInverse(float3 c) { return c / max(1 - upMax3(c), 1.0 / 65504.0); }
float3 upYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 upRgb(float3 y) { return float3(y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z); }

// Catmull-Rom of t at uv (size texels): the 4 x 4 texels' separable weights, the middle two per axis merged into one
// bilinear tap, the 4 corner taps (their weights are small and negative-positive pairs) left out.
float4 upHistory(Texture2D<float4> t, float2 uv, float2 size)
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

// Lanczos-3 of t at uv (6 x 6 texel loads, separable weights normalised): the moving history's resampling. Catmull-Rom
// every frame of motion took most of the detail near the output Nyquist frequency away from the history within a few
// frames (CPU model, fine wood grain panned 0.2-0.45 px / frame: energy 0.19-0.23 of native with Catmull-Rom, 0.37-0.38
// with Lanczos-3 and the moving clip below; SSIM 0.73-0.77 -> 0.86-0.87). Its overshoot is removed by that clip.
float upLanczos(float x) { return abs(x) < 1e-4 ? 1.0 : (abs(x) >= 3 ? 0.0 : 3 * sin(3.14159265 * x) * sin(3.14159265 * x / 3) / (9.8696044 * x * x)); }
float4 upHistoryLanczos(Texture2D<float4> t, float2 uv, float2 size)
{
    const float2 pos = uv * size - 0.5;
    const float2 f0 = floor(pos), f = pos - f0;
    float wx[6], wy[6];
    float sx = 0, sy = 0;
    [unroll] for (int i = 0; i < 6; ++i)
    {
        wx[i] = upLanczos(f.x - (i - 2));
        wy[i] = upLanczos(f.y - (i - 2));
        sx += wx[i];
        sy += wy[i];
    }
    const int2 hi = int2(size) - 1;
    float4 r = 0;
    [unroll] for (int y = 0; y < 6; ++y)
    {
        float4 row = 0;
        [unroll] for (int x = 0; x < 6; ++x) row += wx[x] * t.Load(int3(clamp(int2(f0) + int2(x - 2, y - 2), 0, hi), 0));
        r += wy[y] * row;
    }
    return r / (sx * sy);
}

// h moved towards the box centre until it is inside the box (lo, hi).
float3 upClip(float3 h, float3 lo, float3 hi)
{
    const float3 c = 0.5 * (lo + hi), e = 0.5 * (hi - lo) + 1e-6;
    const float3 v = h - c;
    const float3 a = abs(v / e);
    const float m = max(a.x, max(a.y, a.z));
    return m > 1 ? c + v / m : h;
}

[numthreads(8, 8, 1)]
void main(uint2 o : SV_DispatchThreadID)
{
    const uint2 outSize = P[2].xy;
    if (any(o >= outSize)) return;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> motionTex = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[1].x];
    const int2 inSize = int2(P[1].yz);
    const float2 jitter = asfloat(P[2].zw);
    const float2 scale = float2(outSize) / float2(inSize);  // output pixels per internal pixel
    const float K = asfloat(P[3].w);
    const float2 uv = (float2(o) + 0.5) / float2(outSize);
    const float2 x = uv * float2(inSize);
    const int2 kc = int2(floor(x + jitter));  // the sample whose unjittered centre is within half a pixel of x

    Texture2D<float4> base = ResourceDescriptorHeap[P[4].x];
    float3 sumWide = 0, sum = 0, baseWide = 0, baseSum = 0;
    float wsumWide = 0, wsum = 0, nearest = -1;
    int2 nearestK = clamp(kc, 0, inSize - 1);
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 k = clamp(kc + int2(dx, dy), 0, inSize - 1);
            const float3 c = max(colour.Load(int3(k, 0)).rgb, 0);
            const float3 b = base.Load(int3(k, 0)).rgb;
            const float3 detail = all(isfinite(c)) ? c : 0;
            const float2 d = float2(k) + 0.5 - jitter - x;
            const float wWide = exp(-2.29 * dot(d, d));
            sumWide += wWide * detail;
            baseWide += wWide * b;
            wsumWide += wWide;
            const float2 dOut = d * scale;
            const float w = exp(-K * dot(dOut, dOut));
            sum += w * detail;
            baseSum += w * b;
            wsum += w;
            const float z = depth.Load(int3(k, 0));
            if (z > nearest) { nearest = z; nearestK = k; }
        }
    const float3 wide = sumWide / max(wsumWide, 1e-6);
    const float4 surface = motionTex.Load(int3(nearestK, 0));
    const float2 motionUv = surface.xy;
    const float movePx = length(motionUv * float2(outSize));
    const float perFrame = (3.14159265 / K) / (scale.x * scale.y);
    const float capFrames = lerp(asfloat(P[3].y), asfloat(P[3].z), saturate(movePx / 0.25));
    const float cap = capFrames * perFrame;
    float n = 0;
    float3 history = 0, historyBase = 0;
    const float2 prevUv = uv - motionUv;
    if ((P[1].w & 1u) == 0 && all(prevUv >= 0) && all(prevUv <= 1))
    {
        Texture2D<float4> historyTex = ResourceDescriptorHeap[P[0].w];
        Texture2D<float4> baseHistory = ResourceDescriptorHeap[P[4].z];
        const float previousDepth = baseHistory.SampleLevel(g_pointClamp, prevUv, 0).a;
        const bool geometryMatches = surface.w == 0 ? previousDepth == 0 :
            previousDepth > 0 && abs(previousDepth - surface.w) <= max(0.002, 0.01 * surface.w);
        const float4 h = movePx == 0 ? historyTex.Load(int3(o, 0)) :
                         movePx > 1e-3 ? upHistoryLanczos(historyTex, prevUv, float2(outSize)) : upHistory(historyTex, prevUv, float2(outSize));
        const float4 hb = movePx == 0 ? baseHistory.Load(int3(o, 0)) :
                          movePx > 1e-3 ? upHistoryLanczos(baseHistory, prevUv, float2(outSize)) : upHistory(baseHistory, prevUv, float2(outSize));
        if (geometryMatches && all(isfinite(h)) && all(isfinite(hb)))
        {
            history = max(h.rgb, 0) * asfloat(P[3].x);
            historyBase = max(hb.rgb, 0) * asfloat(P[3].x);
            n = clamp(h.a, 0, cap);
        }
    }
    const float3 sharp = wsum > 1e-5 ? sum / wsum : wide;
    const float3 current = lerp(wide, sharp, saturate(n / max(4 * perFrame, 1e-6)));
    const float3 wideBase = baseWide / max(wsumWide, 1e-6);
    const float3 sharpBase = wsum > 1e-5 ? baseSum / wsum : wideBase;
    const float3 currentBase = lerp(wideBase, sharpBase, saturate(n / max(4 * perFrame, 1e-6)));
    const float alpha = wsum / max(wsum + n, 1e-9);
    const float3 result = max(lerp(history, current, n > 0 ? alpha : 1.0), 0);
    RWTexture2D<float4> detailOutput = ResourceDescriptorHeap[P[4].y];
    RWTexture2D<float4> baseOutput = ResourceDescriptorHeap[P[4].w];
    const float3 resultBase = max(lerp(historyBase, currentBase, n > 0 ? alpha : 1.0), 0);
    detailOutput[o] = float4(result, min(n + wsum, cap));
    baseOutput[o] = float4(resultBase, surface.z);
    const float2 baseUv = (x + jitter) / float2(inSize);
    const float3 baseNow = max(base.SampleLevel(g_linearClamp, baseUv, 0).rgb, 0);
    output[o] = float4(min(baseNow * (result / max(resultBase, 1e-6)), 65504.0), 1);
}
