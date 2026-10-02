// unx-kernel: cs_6_6 main
// m.tsr.resolve (Tsr.hlsli; the reference's TSRResolveHistory): the history above the output resolution
// (output.upscale_tsr_history_percent) filtered down to the output. Per output pixel the 4 x 4 history pixels around its
// centre under a Mitchell-Netravali kernel (B = C = 1 / 3) of the distance in history pixels, tone-weighted (a highlight
// does not spread over its neighbours), clamped to the 16 samples' range (the kernel's negative lobes). At 200 % the
// centre lies on a history pixel corner: the weights are those of the reference's 2 x 2 decimation.
// P[0] = { history SRV (RGBA16F: rgb exposed linear, a = validity), output UAV (RGBA16F), output width, height }
// P[1] = { history width, height, 0, 0 }
#include "Passes/Shading/Tsr.hlsli"

float mitchellNetravali(float d)
{
    const float b = 1.0 / 3.0, c = 1.0 / 3.0;
    if (d < 1) return ((12.0 - 9.0 * b - 6.0 * c) * d * d * d + (-18.0 + 12.0 * b + 6.0 * c) * d * d + (6.0 - 2.0 * b)) / 6.0;
    if (d < 2) return ((-b - 6.0 * c) * d * d * d + (6.0 * b + 30.0 * c) * d * d + (-12.0 * b - 48.0 * c) * d + (8.0 * b + 24.0 * c)) / 6.0;
    return 0;
}

[numthreads(8, 8, 1)]
void main(uint2 o : SV_DispatchThreadID)
{
    const uint2 outSize = P[0].zw;
    if (any(o >= outSize)) return;
    Texture2D<float4> history = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].y];
    const int2 historySize = int2(P[1].xy);
    // the output pixel's centre in history pixels, and the centre of the kernel's first history pixel
    const float2 centre = (float2(o) + 0.5) * float2(historySize) / float2(outSize);
    const float2 first = floor(centre - 1.5) + 0.5;
    float3 sum = 0, lo = 0, hi = 0;
    float weightSum = 0, validity = 0, kernelSum = 0;
    [unroll] for (uint y = 0; y < 4; ++y)
        [unroll] for (uint x = 0; x < 4; ++x)
        {
            const float2 at = first + float2(x, y);
            float4 h = history.Load(int3(clamp(int2(at), 0, historySize - 1), 0));
            h = all(isfinite(h)) ? max(h, 0.0) : float4(0, 0, 0, 0);
            const float2 d = abs(at - centre);
            const float kernel = mitchellNetravali(d.x) * mitchellNetravali(d.y);
            const float weight = kernel * tsrHdrWeight(h.rgb);
            sum += h.rgb * weight;
            weightSum += weight;
            validity += h.a * kernel;
            kernelSum += kernel;
            lo = x + y == 0 ? h.rgb : min(lo, h.rgb);
            hi = x + y == 0 ? h.rgb : max(hi, h.rgb);
        }
    float3 result = weightSum > 0 ? sum / weightSum : lo;
    result = clamp(result, lo, hi);
    result = all(isfinite(result)) ? clamp(result, 0.0, 65504.0) : float3(0, 0, 0);
    output[o] = float4(result, saturate(kernelSum > 0 ? validity / kernelSum : 0.0));
}
