// unx-kernel: cs_6_6 main
// m.ml.temporal (MegaLights.hlsli): the temporal step of the stochastic local lights, per pixel, diffuse and specular
// apart (both exposed and divided by the modulation factors).
//   history   the pixel's surface point one frame ago (GiScreenHistory.hlsli: the vis buffer's triangle in its previous
//             vertices) in the previous view. A previous pixel holds the same surface when its stored view depth is
//             within threshold x depth / lerp(0.1, 1, n.v) of the point's and it has accumulated frames. All 12 pixels of
//             the bicubic footprint valid: Catmull-Rom (5 bilinear taps); else the valid ones of the 2 x 2, bilinear.
//   clamp     to mean +- scale x standard deviation of this frame's 5 x 5 neighbourhood (corners left out; YCoCg), the
//             centre always inside; the distance the history had to move, relative to the box, lowers the history
//             confidence (1 = untouched), and the clamp is softened by it.
//   length    n = min(n_prev + 1, min(lerp(min frames, max frames, history confidence), frames the shading confidence
//             allows)); value = lerp(history, now, 1 / n); the same for the luminance moments (mean, mean square).
// Pixels without a valid sample this frame pass their history on; pixels without history start at n = 1.
// P[0] = { resolved diffuse (a = shading confidence), resolved specular (a = valid), depth, G-buffer }
// P[1] = { width, height, flags (1: the previous frame's history is valid), exposure ratio now / previous (float) }
// P[2] = { previous diffuse, previous specular, previous moments, previous frame counts (R8_UINT, n x 8) }
// P[3] = { previous view depth (R32_FLOAT), vis id, visible clusters, material word }
// P[4] = { diffuse UAV, specular UAV (a = 1 where the pixel holds lighting), moments UAV (diffuse mean, mean square,
//          specular mean, mean square), frame counts UAV }
// P[5] = { view depth UAV (the next frame's key; 0 = no surface), history confidence UAV (R8G8_UNORM: diffuse, specular), 0, 0 }
// P[6] = { max frames, min frames on a history miss, history distance threshold, neighbourhood clamp scale } (floats)
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/GI/GiScreenHistory.hlsli"
#include "Passes/Shading/MegaLights.hlsli"

// Catmull-Rom of a history texture at continuous pixel position 'pos' (pixel centres at + 0.5) by 5 bilinear taps (the
// 4 corner taps of the 3 x 3 form are left out and the rest renormalised).
float3 mlCatmullRom(Texture2D<float4> t, float2 pos, float2 size)
{
    const float2 centre = floor(pos - 0.5) + 0.5, f = pos - centre;
    const float2 w0 = f * (-0.5 + f * (1 - 0.5 * f)), w1 = 1 + f * f * (-2.5 + 1.5 * f), w2 = f * (0.5 + f * (2 - 1.5 * f)), w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2, t0 = (centre - 1) / size, t3 = (centre + 2) / size, t12 = (centre + w2 / w12) / size;
    const float a = w12.x * w0.y, b = w0.x * w12.y, c = w12.x * w12.y, d = w3.x * w12.y, e = w12.x * w3.y;
    const float3 sum = t.SampleLevel(g_linearClamp, float2(t12.x, t0.y), 0).rgb * a + t.SampleLevel(g_linearClamp, float2(t0.x, t12.y), 0).rgb * b +
                       t.SampleLevel(g_linearClamp, float2(t12.x, t12.y), 0).rgb * c + t.SampleLevel(g_linearClamp, float2(t3.x, t12.y), 0).rgb * d +
                       t.SampleLevel(g_linearClamp, float2(t12.x, t3.y), 0).rgb * e;
    return max(sum / (a + b + c + d + e), 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    const uint2 size = P[1].xy;
    if (any(pixel >= size)) return;
    RWTexture2D<float4> outDiffuse = ResourceDescriptorHeap[P[4].x];
    RWTexture2D<float4> outSpecular = ResourceDescriptorHeap[P[4].y];
    RWTexture2D<float4> outMoments = ResourceDescriptorHeap[P[4].z];
    RWTexture2D<uint> outFrames = ResourceDescriptorHeap[P[4].w];
    RWTexture2D<float> outDepth = ResourceDescriptorHeap[P[5].x];
    RWTexture2D<float2> outConfidence = ResourceDescriptorHeap[P[5].y];
    Texture2D<uint> words = ResourceDescriptorHeap[P[3].w];
    if (mWordMaterial(words[pixel]) == M_MATERIAL_SKY)
    {
        outDiffuse[pixel] = 0;
        outSpecular[pixel] = 0;
        outMoments[pixel] = 0;
        outFrames[pixel] = 0;
        outDepth[pixel] = 0;
        outConfidence[pixel] = float2(1, 1);
        return;
    }
    Texture2D<float4> nowDiffuse = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> nowSpecular = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    const float4 centreD = nowDiffuse[pixel], centreS = nowSpecular[pixel];
    const bool nowValid = centreS.a > 0;
    const float linearZ = linearDepth(depthTex[pixel]);
    outDepth[pixel] = linearZ;
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 worldPos = g_cameraPosition + D * linearZ;
    const float3 v = -normalize(D);
    const float3 n = mNormalTowardsViewer(octDecode(gbuffer[pixel].x), v);
    const float NoV = dot(n, v);
    const float maxFrames = asfloat(P[6].x), minFrames = asfloat(P[6].y);

    // ---- history
    bool haveHistory = false;
    float3 histD = 0, histS = 0;
    float4 histM = 0;
    float histN = 0;
    if ((P[1].z & 1u) != 0)
    {
        float3 prevP, prevN;
        uint instance;
        giPreviousSurface(P[3].y, P[3].z, pixel, worldPos, n, prevP, prevN, instance);
        float2 pp;
        float prevDepth;
        if (giPreviousPixel(prevP, float2(size), pp, prevDepth) && all(pp >= 0) && all(pp < float2(size)))
        {
            Texture2D<float4> prevDiffuse = ResourceDescriptorHeap[P[2].x];
            Texture2D<float4> prevSpecular = ResourceDescriptorHeap[P[2].y];
            Texture2D<float4> prevMoments = ResourceDescriptorHeap[P[2].z];
            Texture2D<uint> prevFrames = ResourceDescriptorHeap[P[2].w];
            Texture2D<float> prevKeys = ResourceDescriptorHeap[P[3].x];
            const float tolerance = prevDepth * asfloat(P[6].z) / lerp(0.1, 1.0, saturate(NoV));
            const float2 x = pp - 0.5;
            const int2 i0 = int2(floor(x));
            const float2 f = x - floor(x);
            // the 4 x 4 footprint without its corners: every pixel the same surface with frames?
            bool all12 = true;
            float4 w = 0;
            uint4 frames4 = 0;
            for (int ty = -1; ty <= 2; ++ty)
                for (int tx = -1; tx <= 2; ++tx)
                {
                    if ((tx == -1 || tx == 2) && (ty == -1 || ty == 2)) continue;
                    const int2 c = i0 + int2(tx, ty);
                    bool ok = all(c >= 0) && all(c < int2(size));
                    uint frames = 0;
                    if (ok)
                    {
                        const float d = prevKeys[c];
                        frames = prevFrames[c];
                        ok = d > 0 && abs(d - prevDepth) < tolerance && frames > 0;
                    }
                    all12 = all12 && ok;
                    if (tx >= 0 && tx <= 1 && ty >= 0 && ty <= 1)
                    {
                        const uint k = uint(tx) + 2u * uint(ty);
                        const float bw = (tx == 0 ? 1 - f.x : f.x) * (ty == 0 ? 1 - f.y : f.y);
                        w[k] = ok && bw > 0.01 ? bw : 0;  // (a tap of little weight would dominate after the renormalisation)
                        frames4[k] = frames;
                    }
                }
            const float sum = w.x + w.y + w.z + w.w;
            if (sum > 0)
            {
                haveHistory = true;
                w /= sum;
                for (uint k = 0; k < 4; ++k)
                {
                    if (!(w[k] > 0)) continue;
                    const int2 c = i0 + int2(k & 1u, k >> 1);
                    histM += prevMoments[c] * w[k];
                    histN += (frames4[k] / 8.0) * w[k];
                    if (!all12)
                    {
                        histD += prevDiffuse[c].rgb * w[k];
                        histS += prevSpecular[c].rgb * w[k];
                    }
                }
                if (all12)
                {
                    histD = mlCatmullRom(prevDiffuse, pp, float2(size));
                    histS = mlCatmullRom(prevSpecular, pp, float2(size));
                }
                // the history in this frame's exposure
                const float ratio = asfloat(P[1].w);
                histD *= ratio;
                histS *= ratio;
                histM *= float4(ratio, ratio * ratio, ratio, ratio * ratio);
            }
        }
    }

    float3 diffuse = 0, specular = 0;
    float4 moments = 0;
    float count = 0;
    float2 historyConfidence = 1;
    if (haveHistory && nowValid)
    {
        // ---- this frame's neighbourhood (YCoCg)
        const float3 cD = mlToYCoCg(centreD.rgb), cS = mlToYCoCg(centreS.rgb);
        float3 sumD = cD, sumS = cS, sqD = 0, sqS = 0;
        float weight = 1;
        for (int oy = -2; oy <= 2; ++oy)
            for (int ox = -2; ox <= 2; ++ox)
            {
                if ((ox == 0 && oy == 0) || (abs(ox) == 2 && abs(oy) == 2)) continue;
                const int2 c = int2(pixel) + int2(ox, oy);
                if (any(c < 0) || any(c >= int2(size))) continue;
                const float4 s4 = nowSpecular[c];
                if (!(s4.a > 0)) continue;
                const float3 yd = mlToYCoCg(nowDiffuse[c].rgb), ys = mlToYCoCg(s4.rgb);
                sumD += yd;
                sumS += ys;
                sqD += (yd - cD) * (yd - cD);
                sqS += (ys - cS) * (ys - cS);
                weight += 1;
            }
        const float confidenceFrames = mlConfidenceFrames(centreD.a, maxFrames);
        // (a pixel few lights decide has little noise: its box may be wide, the history cannot ghost far)
        const float scale = asfloat(P[6].w) * (1 + 3 * saturate((confidenceFrames - 4.0) / (2.0 - 4.0)));
        const float3 meanD = sumD / weight, meanS = sumS / weight;
        const float3 sdD = sqrt(max(sqD / weight - (meanD - cD) * (meanD - cD), 0.0)), sdS = sqrt(max(sqS / weight - (meanS - cS) * (meanS - cS), 0.0));
        const float3 loD = min(meanD - scale * sdD, cD), hiD = max(meanD + scale * sdD, cD);
        const float3 loS = min(meanS - scale * sdS, cS), hiS = max(meanS + scale * sdS, cS);
        const float3 hD = mlToYCoCg(histD), hS = mlToYCoCg(histS);
        const float3 clD = clamp(hD, loD, hiD), clS = clamp(hS, loS, hiS);
        historyConfidence.x = saturate(1 - length(abs(clD - hD) / max((hiD - loD) * 0.5, 0.1)));
        historyConfidence.y = saturate(1 - length(abs(clS - hS) / max((hiS - loS) * 0.5, 0.1)));
        histD = max(mlFromYCoCg(lerp(clD, hD, historyConfidence.x)), 0.0);
        histS = max(mlFromYCoCg(lerp(clS, hS, historyConfidence.y)), 0.0);

        const float nD = min(histN + 1, min(lerp(minFrames, maxFrames, historyConfidence.x), confidenceFrames));
        const float nS = min(histN + 1, min(lerp(minFrames, maxFrames, historyConfidence.y), confidenceFrames));
        count = max(0.5 * (nD + nS), 1.0);
        const float lumD = mlLuminance(centreD.rgb), lumS = mlLuminance(centreS.rgb);
        diffuse = lerp(histD, centreD.rgb, 1 / max(nD, 1.0));
        specular = lerp(histS, centreS.rgb, 1 / max(nS, 1.0));
        moments.xy = lerp(histM.xy, float2(lumD, min(lumD * lumD, 60000.0)), 1 / max(nD, 1.0));
        moments.zw = lerp(histM.zw, float2(lumS, min(lumS * lumS, 60000.0)), 1 / max(nS, 1.0));
    }
    else if (haveHistory)
    {
        diffuse = histD;
        specular = histS;
        moments = histM;
        count = histN;
    }
    else if (nowValid)
    {
        const float lumD = mlLuminance(centreD.rgb), lumS = mlLuminance(centreS.rgb);
        diffuse = centreD.rgb;
        specular = centreS.rgb;
        moments = float4(lumD, min(lumD * lumD, 60000.0), lumS, min(lumS * lumS, 60000.0));
        count = 1;
    }
    if (any(isnan(diffuse)) || any(isinf(diffuse))) diffuse = 0;
    if (any(isnan(specular)) || any(isinf(specular))) specular = 0;
    if (any(isnan(moments)) || any(isinf(moments))) moments = 0;
    outDiffuse[pixel] = float4(diffuse, centreD.a);
    outSpecular[pixel] = float4(specular, count > 0 ? 1 : 0);
    outMoments[pixel] = moments;
    outFrames[pixel] = uint(min(count, 31.0) * 8 + 0.5);
    outConfidence[pixel] = historyConfidence;
}
