// unx-kernel: cs_6_6 main
// Per-frame spatial reconstruction of the reflection layers (RENDERER_REDESIGN_V2 1.2; LayerCommon.hlsli): one level of an
// a-trous wavelet filter (5 x 5 B3 spline, tap spacing 'step' units; the levels run with steps 1, 2, 4), one thread per
// pixel of the view. The unit is the pixel's sample spacing: 1 px for M pixels and G pixels with their own samples, the G
// grid's spacing s (2, 4, 8 px) for interpolated G pixels - their independent estimates lie s apart (the pixels between
// are bilinear mixes of the same four samples), so taps closer than s would average copies, not samples, and the level 0
// spread would not be the samples' (a 29 px support holds 13 samples at s = 8; 29 s holds what the lobe footprint allows).
// Both layers of a pixel are filtered with the same geometric weights:
//   M pixels (mirror): the guide is the hit geometry, never the mirror's own surface detail - taps of M pixels whose
//     reflected image lies at the same depth (eye -> mirror -> hit path length within 1 % + 0.4 % per pixel of distance)
//     and whose hits face the same way (hit normals, power 8). Only the hits' stochastic light over their albedo is
//     averaged, among neighbouring pixels of the same reflected surface: the reflected image's identity (outlines,
//     textures, emission, sun) is not in the layer and passes unfiltered.
//   G pixels (glossy): taps of G pixels on the pixel's tangent plane (2 % of the depth), with its normal (power 8), its
//     roughness and a nearest-hit distance within a factor 4, inside the lobe's footprint: a Gaussian over the tap
//     distance of sigma blur_px / 2 for the residual (it is band-limited by the lobe, and a wider average would blur the
//     reflection the lobe resolves) and of max(blur_px / 2, 6 px) for the stochastic layer (the hits' lighting over their
//     albedo varies no faster than under a mirror, whose layer the full support averages).
// Data: a pixel whose hits found no cache data (LayerCommon.hlsli guide bit; after a cut, before the hit cells' first
// update) holds no estimate of its stochastic layer: as a tap it weighs 0 there, and as the centre it takes its
// neighbours' weighted mean when any has data (its own value otherwise). Its residual is filtered as any other.
// Value weights per layer: exp(-|luminance difference| / (4 sigma)), sigma = the pixel's standard deviation in the layer's alpha.
// Level 0 has no sigma yet: it filters by geometry alone and writes that of its weighted mean from the taps' own spread
// (variance = sample variance x sum w^2 / (sum w)^2); later levels read it and propagate sum w^2 var / (sum w)^2. Noisy
// neighbourhoods are averaged widely, converged ones keep their gradients (the history lowers the variance: LayerTemporal).
// Unbiased where the layer is constant over the accepted taps; elsewhere the bias is that of averaging the hit lighting
// over the filter's footprint on the reflected surface.
// P[0] = { stochastic in SRV, residual in SRV, guide SRV, step }, P[1] = { stochastic out UAV, residual out UAV, width, height }
// P[2] = { asuint(focal length px), level (0: no variance in the input), 0, 0 }; frame constants b1 = main view.
#include "Passes/Reconstruct/LayerCommon.hlsli"

static const float kB3[5] = { 1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0 };

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint2 size = P[1].zw;
    if (any(pixel >= size)) return;
    Texture2D<uint4> guides = ResourceDescriptorHeap[P[0].z];
    const uint4 g0 = guides.Load(int3(pixel, 0));
    if (layerMode(g0) == LAYER_MODE_NONE) return;
    Texture2D<float4> inS = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> inR = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> outS = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float4> outR = ResourceDescriptorHeap[P[1].y];
    const LayerGuide c = layerGuide(g0, pixel);
    const bool mirror = c.mode == LAYER_MODE_M;
    const int step = (int)P[0].w << (mirror ? 0u : (g0.z & 3u));  // (G: in units of its sample spacing)
    const bool first = P[2].y == 0;
    const float4 s0 = inS.Load(int3(pixel, 0));
    const float4 r0 = mirror ? float4(0, 0, 0, 0) : inR.Load(int3(pixel, 0));
    const float lumS0 = layerLuminance(s0.rgb), lumR0 = layerLuminance(r0.rgb);
    const bool noData0 = layerNoData(g0);  // (its own value is no reference for the value weights)
    const float toleranceS = 4 * max(s0.a, 0.0) + 1e-4 * abs(lumS0) + 1e-6;
    const float toleranceR = 4 * max(r0.a, 0.0) + 1e-4 * abs(lumR0) + 1e-6;
    const float image0 = layerImageDepth(c);
    const float NoV = saturate(dot(c.normal, normalize(g_cameraPosition - c.position)));
    const float blur = c.hitDistance / max(c.linearZ, 1e-4) * reflectionLobeHalfAngle(c.roughness, NoV) * asfloat(P[2].x);
    const float sigmaR = max(0.5 * blur, 0.5), sigmaS = max(0.5 * blur, 6.0);
    float3 sumS = 0, sumR = 0;
    float wS = 0, wR = 0, w2S = 0, w2R = 0;    // sums of weights and (level 0) of squared weights
    float m1S = 0, m2S = 0, m1R = 0, m2R = 0;  // level 0: luminance moments; later levels (m1): sum w^2 var
    [loop] for (int dy = -2; dy <= 2; ++dy)
    {
        [loop] for (int dx = -2; dx <= 2; ++dx)
        {
            const int2 q = int2(pixel) + int2(dx, dy) * step;
            if (any(q < 0) || any(q >= int2(size))) continue;
            const bool centre = dx == 0 && dy == 0;
            uint4 gq = g0;
            if (!centre) gq = guides.Load(int3(q, 0));
            if (layerMode(gq) != c.mode) continue;
            float w = kB3[dx + 2] * kB3[dy + 2];
            float wResidual = mirror ? 0 : w;
            const float offset = length(float2(dx, dy)) * step;
            if (!centre)
            {
                if (mirror)
                {
                    const float imageQ = linearDepth(max(asfloat(gq.x), 1e-30)) + f16tof32(gq.z >> 16);
                    const float agree = saturate(dot(c.hitNormal, reflUnpackOct16(gq.z & 0xFFFFu)));
                    const float agree2 = agree * agree, agree4 = agree2 * agree2;
                    w *= exp(-abs(imageQ - image0) / (image0 * (0.01 + 0.004 * offset))) * agree4 * agree4;
                }
                else
                {
                    const LayerGuide t = layerGuide(gq, uint2(q));
                    const float plane = saturate(1 - abs(dot(c.normal, t.position - c.position)) / max(c.linearZ, 1e-4) / 0.02);
                    const float agree = saturate(dot(c.normal, t.normal));
                    const float agree2 = agree * agree, agree4 = agree2 * agree2;
                    const float nearer = min(c.hitDistance, t.hitDistance), farther = max(c.hitDistance, t.hitDistance);
                    w *= plane * plane * agree4 * agree4 * saturate(1 - abs(c.roughness - t.roughness) * 4) * saturate(4 * nearer / max(farther, 1e-6));
                    wResidual = w * exp(-0.5 * offset * offset / (sigmaR * sigmaR));
                    w *= exp(-0.5 * offset * offset / (sigmaS * sigmaS));
                }
            }
            if (!(w > 0)) continue;
            if (!layerNoData(gq))
            {
                float4 s = s0;
                if (!centre) s = inS.Load(int3(q, 0));
                const float lumS = layerLuminance(s.rgb);
                const float ws = first || noData0 ? w : w * exp(-abs(lumS - lumS0) / toleranceS);
                sumS += ws * s.rgb;
                wS += ws;
                if (first)
                {
                    w2S += ws * ws;
                    m1S += ws * lumS;
                    m2S += ws * lumS * lumS;
                }
                else m1S += ws * ws * s.a * s.a;
            }
            if (wResidual > 0)
            {
                float4 r = r0;
                if (!centre) r = inR.Load(int3(q, 0));
                const float lumR = layerLuminance(r.rgb);
                const float wr = first ? wResidual : wResidual * exp(-abs(lumR - lumR0) / toleranceR);
                sumR += wr * r.rgb;
                wR += wr;
                if (first)
                {
                    w2R += wr * wr;
                    m1R += wr * lumR;
                    m2R += wr * lumR * lumR;
                }
                else m1R += wr * wr * r.a * r.a;
            }
        }
    }
    // (the centre tap always counts for the residual: wR > 0 for G; the stochastic layer has wS = 0 when the pixel and
    // every accepted tap lack data: its own value stays, sigma 0)
    if (wS > 0)
    {
        const float varS = first ? max(m2S / wS - (m1S / wS) * (m1S / wS), 0.0) * w2S / (wS * wS) : m1S / (wS * wS);
        outS[pixel] = float4(sumS / wS, min(sqrt(varS), 65504.0));
    }
    else outS[pixel] = float4(s0.rgb, 0);
    if (!mirror)
    {
        const float varR = first ? max(m2R / wR - (m1R / wR) * (m1R / wR), 0.0) * w2R / (wR * wR) : m1R / (wR * wR);
        outR[pixel] = float4(sumR / wR, min(sqrt(varR), 65504.0));
    }
}
