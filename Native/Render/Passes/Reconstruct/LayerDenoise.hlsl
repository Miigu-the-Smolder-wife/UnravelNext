// unx-kernel: cs_6_6 main
// Per-frame spatial reconstruction of the reflection layers (RENDERER_REDESIGN_V2 1.2; LayerCommon.hlsli): one level of an
// a-trous wavelet filter (5 x 5 B3 spline, tap spacing 'step' units; the levels run with steps 1, 2, 4: +-14 units), one
// thread per pixel of the view. The tap unit:
//   M pixels                1 px (a 29 px support);
//   G pixels                max(the pixel's sample spacing s, blur_px / 14) px, at most 8. The independent estimates of
//                           interpolated G pixels lie s apart (the pixels between are bilinear mixes of the same four
//                           samples: taps closer than s average copies, not samples - measured, the first hardware run:
//                           13 samples in the 29 px support at s = 8). And the filter's radius is the lobe footprint
//                           blur_px (design 1.2), whatever the spacing: before a hit distance is known (the first frame
//                           after a cut or of a newly seen surface: ReflectionClassify's history is 0) every G pixel has
//                           its own samples, s = 1, and the +-14 px support covered a wide lobe's footprint only in
//                           part. With the unit blur_px / 14 the three levels reach +-blur_px; a pixel then averages
//                           the samples of its own residue class of the unit lattice (up to 29 x 29 of them), its
//                           neighbours those of theirs.
// Weights are geometric only, the same for both layers of a pixel:
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
// No value (luminance) weights: the G layers are band-limited by the lobe footprint the Gaussian already bounds, so a
// value difference between accepted taps is noise, and the noise here is heavy-tailed (a lobe that meets a lamp's
// hotspot with one ray in hundreds): variance-guided value weights kept every such sample as a hard-edged square of the
// level's footprint (measured, the first and second hardware runs' frame 0: Results/Local/Refl/cmp_v2/pillar_refl_f0.png).
// For M the cost is that edges of the hits' stochastic lighting (a local light's shadow seen in a mirror) are averaged
// over the support; with D-1 (deterministic local lights at hits) those leave the layer.
// Data: a pixel whose hits found no cache data (LayerCommon.hlsli guide bit; after a cut, before the hit cells' first
// update) holds no estimate of its stochastic layer: as a tap it weighs 0 there, and as the centre it takes its
// neighbours' weighted mean when any has data (its own value otherwise). Its residual is filtered as any other.
// Unbiased where the layer is constant over the accepted taps; elsewhere the bias is that of averaging the hit lighting
// over the filter's footprint on the reflected surface.
// P[0] = { stochastic in SRV, residual in SRV, guide SRV, step }, P[1] = { stochastic out UAV, residual out UAV, width, height }
// P[2] = { asuint(focal length px), 0, 0, 0 }; frame constants b1 = main view.
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
    const float4 s0 = inS.Load(int3(pixel, 0));
    float4 r0 = 0;
    if (!mirror) r0 = inR.Load(int3(pixel, 0));
    const float image0 = layerImageDepth(c);
    const float NoV = saturate(dot(c.normal, normalize(g_cameraPosition - c.position)));
    const float blur = c.hitDistance / max(c.linearZ, 1e-4) * reflectionLobeHalfAngle(c.roughness, NoV) * asfloat(P[2].x);
    const float sigmaR = max(0.5 * blur, 0.5), sigmaS = max(0.5 * blur, 6.0);
    const int unit = mirror ? 1 : (int)clamp(max((float)(1u << (g0.z & 3u)), floor(blur / 14.0)), 1.0, 8.0);
    const int step = (int)P[0].w * unit;
    float3 sumS = 0, sumR = 0;
    float wS = 0, wR = 0;
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
            if (!centre)
            {
                const float offset = length(float2(dx, dy)) * step;
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
            if (w > 0 && !layerNoData(gq))
            {
                float3 s = s0.rgb;
                if (!centre) s = inS.Load(int3(q, 0)).rgb;
                sumS += w * s;
                wS += w;
            }
            if (wResidual > 0)
            {
                float3 r = r0.rgb;
                if (!centre) r = inR.Load(int3(q, 0)).rgb;
                sumR += wResidual * r;
                wR += wResidual;
            }
        }
    }
    // (the centre tap always counts for the residual: wR > 0 for G; the stochastic layer has wS = 0 when the pixel and
    // every accepted tap lack data: its own value stays)
    outS[pixel] = float4(wS > 0 ? sumS / wS : s0.rgb, 0);
    if (!mirror) outR[pixel] = float4(sumR / wR, 0);
}
