// unx-kernel: cs_6_6 main
// Temporal upscale (Upscale.cpp; output.render_height_max): the output pixel's value from the jittered internal samples
// around it and its reprojected history. Per output pixel (centre x in unjittered internal pixel coordinates):
//   current  the 3 x 3 internal samples around x, each at its unjittered centre k + 0.5 - jitter, weighted by a Gaussian
//            of its distance d (internal pixels): w = exp(-2.29 d^2) (a Blackman-Harris-like window), in a reversible
//            tone map c / (1 + max(c)) (a lone bright sample cannot dominate the mean);
//   history  the previous output at x - motion (the motion of the nearest of the 3 x 3 samples: edges take the
//            foreground's), Catmull-Rom (5 bilinear taps), times the exposure ratio, clipped to the samples' YCoCg
//            box (their mean +- 1 sigma, within their min / max) towards its centre;
//   blend    alpha = c / (c + n), c = the nearest sample's weight (how well this frame's samples cover x: 1 on a sample,
//            lower between them), n = the history's accumulated weight (alpha channel, at most
//            output.upscale_history_frames); the new weight min(n + c, max).
// A history outside the previous frame, or a reset (first frame, size change, cut, restore): n = 0.
// (Concept reference only, no code: the temporal upsampling of UE5 TSR / TAAU - Engine/Shaders/Private/TemporalAA/ and
// TemporalAA.usf: jittered samples reprojected into an output-resolution history, neighbourhood clamping in YCoCg,
// nearest-depth motion dilation, a Blackman-Harris sample window.)
// P[0] = { colour SRV (internal, exposed linear), depth SRV (internal), motion SRV (internal, UpscaleMotion.hlsl),
// history SRV (output) }, P[1] = { output UAV (RGBA16F: rgb exposed linear, a = history weight), internal width, height,
// flags (1: reset) }, P[2] = { output width, height, asuint(jitter x), asuint(jitter y) }, P[3] = { asuint(exposure
// ratio), asuint(maximum history weight), 0, 0 }.
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
    Texture2D<float2> motionTex = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[1].x];
    const int2 inSize = int2(P[1].yz);
    const float2 jitter = asfloat(P[2].zw);
    const float2 uv = (float2(o) + 0.5) / float2(outSize);
    const float2 x = uv * float2(inSize);
    const int2 kc = int2(floor(x + jitter));  // the sample whose unjittered centre is within half a pixel of x

    float3 sum = 0, m1 = 0, m2 = 0, lo = 1e30, hi = -1e30;
    float wsum = 0, wmax = 0, nearest = -1;
    int2 nearestK = clamp(kc, 0, inSize - 1);
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 k = clamp(kc + int2(dx, dy), 0, inSize - 1);
            float3 c = colour.Load(int3(k, 0)).rgb;
            c = all(isfinite(c)) ? upTonemap(max(c, 0)) : 0;
            const float3 y = upYCoCg(c);
            m1 += y;
            m2 += y * y;
            lo = min(lo, y);
            hi = max(hi, y);
            const float2 d = float2(k) + 0.5 - jitter - x;
            const float w = exp(-2.29 * dot(d, d));
            sum += w * c;
            wsum += w;
            wmax = max(wmax, w);
            const float z = depth.Load(int3(k, 0));  // reversed Z: the largest is the nearest
            if (z > nearest)
            {
                nearest = z;
                nearestK = k;
            }
        }
    const float3 current = sum / max(wsum, 1e-6);

    const float maxWeight = asfloat(P[3].y);
    float n = 0;
    float3 history = 0;
    const float2 prevUv = uv - motionTex.Load(int3(nearestK, 0));
    if ((P[1].w & 1u) == 0 && all(prevUv >= 0) && all(prevUv <= 1))
    {
        Texture2D<float4> historyTex = ResourceDescriptorHeap[P[0].w];
        const float4 h = upHistory(historyTex, prevUv, float2(outSize));
        if (all(isfinite(h)))
        {
            history = upTonemap(max(h.rgb * asfloat(P[3].x), 0));
            n = clamp(h.a, 0, maxWeight);
        }
    }
    const float3 mean = m1 / 9, sigma = sqrt(max(m2 / 9 - mean * mean, 0));
    const float3 boxLo = max(lo, mean - sigma), boxHi = min(hi, mean + sigma);
    history = upRgb(upClip(upYCoCg(history), boxLo, boxHi));

    const float alpha = wmax / (wmax + n);  // n = 0: the current samples alone
    const float3 result = max(lerp(history, current, alpha), 0);
    output[o] = float4(upInverse(result), min(n + wmax, maxWeight));
}
