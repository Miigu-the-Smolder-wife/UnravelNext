// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.temporal: the pixels' history. Thread = pixel. The pixel's own surface point one frame ago
// (GiScreenHistory.hlsli) in the previous view, bilinear over the 4 history pixels around it; a history pixel counts
// when it held the same surface: its stored point lies within P[3].y x depth of the pixel's previous plane.
//   frames: N = min(weighted mean of the 4 (N_i + 1), N_max) (P[3].x, 10);
//   fast update: f = saturate(moving share / P[3].z (0.1)), f = saturate(min((f - 0.2) / 0.8, P[3].w (0.9))), kept at least
//   at the history's f; N = min(N, (1 - f) x N_max): where the lighting moves the history shortens to one frame;
//   blend: value = lerp(history, new, 1 / (1 + N)); a pixel without lighting this frame (no probe) with a history
//   takes 1 / (1 + 4 N). The new value of a pixel with N < 4 (no or a short history): this frame's values' mean over
//   the pixel's surface around it (5 x 5 taps).
// Output: diffuse RGBA16F = irradiance x exposure, a = N + 1 (0 = no surface; M's a > 0 test and LgScreenData's
// disocclusion test read it); rough specular RGBA16F, a = f; keys R32G32_UINT = { device depth bits, normal (2 x 15
// bit octahedral) }, 0 = no surface.
// P[0] = LgSurface inputs, P[1] = { new diffuse SRV, new specular SRV, diffuse UAV, specular UAV },
// P[2] = { keys UAV, previous diffuse SRV, previous specular SRV, previous keys SRV }, P[3] = { N_max (float), distance
// threshold (float), fraction for fast mode (float), max fast amount (float) }, P[4..7] = the previous frame's inverse
// view-projection (rows), P[9].z bit 16 = history valid, P[10].x.. as the common block; P[11].z = exposure ratio (this
// / previous, float). b1 = the view.
// Foliage's back-side irradiance (LgIntegrate.hlsl) takes the diffuse's blend: P[10].z = its frame value SRV (0xFFFFFFFF:
// none), P[10].w = its history SRV (0xFFFFFFFF: none yet), P[11].x = its output UAV.
// A foliage pixel (a Foliage or Subsurface material: the reference's pixel with backface diffuse) keeps its history over
// a wider distance - P[11].w (float; the reference's Temporal.DistanceThresholdForFoliage 0.03: foliage is full of edges
// and discontinuities) in place of P[3].y; P[11].y = M's material word SRV (0xFFFFFFFF: none - every pixel P[3].y).
// P[9].z bits 24-31 (the reference's Temporal.RejectBasedOnNormal, off by default there and here: 0): the cosine x 255
// of the widest angle between a history pixel's normal and the pixel's previous normal that still counts (its
// Temporal.NormalThreshold, 45 degrees) - less streaking where surfaces meet (a character's feet), at the filter's cost.
#define UNX_CLUSTER_STREAM 1  // (a vis id's triangle from its cluster's stream when it is compressed: ClusterStream.hlsli)
#include "Passes/GI/Lumen/LgSurface.hlsli"
#include "Scene.hlsli"

uint lgPackKeyNormal(float3 n)
{
    const float2 e = lgEncodeNormal(n);
    return (uint)round(e.x * 32767.0) | ((uint)round(e.y * 32767.0) << 15);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= lgViewSize())) return;
    Texture2D<float4> newDiffuse = ResourceDescriptorHeap[P[1].x];
    Texture2D<float4> newSpecular = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float4> diffuseOut = ResourceDescriptorHeap[P[1].z];
    RWTexture2D<float4> specularOut = ResourceDescriptorHeap[P[1].w];
    RWTexture2D<uint2> keysOut = ResourceDescriptorHeap[P[2].x];
    const LgSurface s = lgSurface(id.xy);
    const bool backfaceOn = P[10].z != 0xFFFFFFFFu;
    if (!s.valid)
    {
        diffuseOut[id.xy] = 0;
        specularOut[id.xy] = 0;
        keysOut[id.xy] = uint2(0, 0);
        if (backfaceOn)
        {
            RWTexture2D<float4> backfaceOut = ResourceDescriptorHeap[P[11].x];
            backfaceOut[id.xy] = 0;
        }
        return;
    }
    float3 outBackface = 0;
    if (backfaceOn)
    {
        Texture2D<float4> newBackface = ResourceDescriptorHeap[P[10].z];
        outBackface = newBackface[id.xy].rgb;
    }
    const float3 freshBackface = outBackface;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    keysOut[id.xy] = uint2(asuint(depth[id.xy]), lgPackKeyNormal(s.normal));
    const float4 fresh = newDiffuse[id.xy];
    const float3 freshSpecular = newSpecular[id.xy].rgb;
    const bool lit = fresh.a > 0;
    const float moving = abs(fresh.a);
    const float maxFrames = asfloat(P[3].x);
    float distanceThreshold = asfloat(P[3].y);
    if (P[11].y != 0xFFFFFFFFu)
    {
        Texture2D<uint> words = ResourceDescriptorHeap[P[11].y];
        const uint cls = materialClass(loadMaterial(words.Load(int3(id.xy, 0)) & 0xFFFFu));
        if (cls == MATERIAL_FOLIAGE || cls == MATERIAL_SUBSURFACE) distanceThreshold = asfloat(P[11].w);
    }
    const float rejectNormalCos = (P[9].z >> 24) / 255.0;  // (0: no rejection by the normal)
    float3 outDiffuse = fresh.rgb, outSpecular = freshSpecular;
    float frames = 0, fast = 0;
    bool hasHistory = false;
    float3 historyD = 0, historyS = 0;
    float historyAlpha = 1;
    if (lgHistoryValid())
    {
        float3 prevP, prevN;
        uint instance;
        if (P[0].z != 0xFFFFFFFFu) giPreviousSurface(P[0].z, P[0].w, id.xy, s.position, s.normal, prevP, prevN, instance);
        else
        {
            prevP = s.position;
            prevN = s.normal;
        }
        float2 prevPixel;
        float prevDepth;
        if (giPreviousPixel(prevP, (float2)lgViewSize(), prevPixel, prevDepth))
        {
            Texture2D<float4> prevDiffuse = ResourceDescriptorHeap[P[2].y];
            Texture2D<float4> prevSpecular = ResourceDescriptorHeap[P[2].z];
            Texture2D<uint2> prevKeys = ResourceDescriptorHeap[P[2].w];
            const float4x4 invPrev = float4x4(asfloat(P[4]), asfloat(P[5]), asfloat(P[6]), asfloat(P[7]));
            const float2 base = floor(prevPixel - 0.5);
            const float2 f = prevPixel - 0.5 - base;
            const float4 bilinear = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
            const float4 plane = float4(prevN, dot(prevP, prevN));
            float3 sumDiffuse = 0, sumSpecular = 0, sumBackface = 0;
            float sumFrames = 0, sumFast = 0, weight = 0;
            [unroll] for (uint c = 0; c < 4; ++c)
            {
                const int2 q = (int2)base + int2(c & 1u, c >> 1);
                if (any(q < 0) || any(q >= (int2)lgViewSize())) continue;
                const uint2 key = prevKeys[q];
                if (key.x == 0) continue;
                const float2 ndc = float2((q.x + 0.5) / lgViewSize().x * 2 - 1, 1 - (q.y + 0.5) / lgViewSize().y * 2);
                const float4 hp = mul(invPrev, float4(ndc, asfloat(key.x), 1));
                const float3 prevPoint = hp.xyz / hp.w;
                if (abs(dot(float4(prevPoint, -1), plane)) > distanceThreshold * max(prevDepth, 1e-3)) continue;
                if (rejectNormalCos > 0 && dot(lgDecodeNormal(float2(key.y & 0x7FFFu, (key.y >> 15) & 0x7FFFu) / 32767.0), prevN) < rejectNormalCos) continue;
                const float4 d = prevDiffuse[q];
                if (!(d.a > 0)) continue;
                const float4 sp = prevSpecular[q];
                const float w = bilinear[c];
                sumDiffuse += d.rgb * w;
                sumSpecular += sp.rgb * w;
                if (backfaceOn && P[10].w != 0xFFFFFFFFu)
                {
                    Texture2D<float4> prevBackface = ResourceDescriptorHeap[P[10].w];
                    sumBackface += prevBackface[q].rgb * w;
                }
                sumFrames += d.a * w;  // (N_i + 1)
                sumFast += sp.a * w;
                weight += w;
            }
            if (weight > 1e-4)
            {
                const float ratio = asfloat(P[11].z);
                const float3 historyDiffuse = sumDiffuse / weight * ratio, historySpecular = sumSpecular / weight * ratio;
                frames = min(sumFrames / weight, maxFrames);
                fast = saturate(moving / max(asfloat(P[3].z), 1e-4));
                fast = saturate(min((fast - 0.2) / 0.8, asfloat(P[3].w)));
                const float outFast = fast;
                fast = max(fast, min(sumFast / weight, asfloat(P[3].w)));
                frames = min(frames, (1 - fast) * maxFrames);
                float alpha = 1 / (1 + frames);
                if (!lit && frames >= 1) alpha = 1 / (1 + 4 * frames);
                hasHistory = true;
                historyD = historyDiffuse;
                historyS = historySpecular;
                historyAlpha = alpha;
                if (backfaceOn && P[10].w != 0xFFFFFFFFu) outBackface = lerp(sumBackface / weight * ratio, freshBackface, alpha);
                fast = outFast;
            }
        }
    }
    // A pixel whose history is short (a cut's first frames, a disocclusion: under 4 frames) would show its one frame's
    // estimate at a large share - a jittered place among its probes: its new value is the mean of this frame's estimates
    // on its own surface around it (5 x 5 taps 2 pixels apart: the same plane within the history's distance threshold,
    // the normal within 25 degrees). The batch of 2026-10-02: the cut's first frame alone came out clean (bath, the GI
    // layer's pixel noise f600 0.075 -> 0.020) and its second (history of one frame) stood above it (0.033).
    float3 meanDiffuse = fresh.rgb, meanSpecular = freshSpecular;
    if (frames < 4 && lit)
    {
        const float4 plane = float4(s.normal, dot(s.position, s.normal));
        float3 sumDiffuse = fresh.rgb, sumSpecular = freshSpecular;
        float weight = 1;
        [loop] for (int ty = -2; ty <= 2; ++ty)
            [loop] for (int tx = -2; tx <= 2; ++tx)
            {
                if (tx == 0 && ty == 0) continue;
                const int2 q = (int2)id.xy + int2(tx, ty) * 2;
                if (any(q < 0) || any(q >= (int2)lgViewSize())) continue;
                const float4 d = newDiffuse[q];
                if (!(d.a > 0)) continue;
                const LgSurface n = lgSurface((uint2)q);
                if (!n.valid || dot(n.normal, s.normal) < 0.9) continue;
                if (abs(dot(float4(n.position, -1), plane)) > distanceThreshold * max(s.depth, 1e-3)) continue;
                sumDiffuse += d.rgb;
                sumSpecular += newSpecular[q].rgb;
                weight += 1;
            }
        meanDiffuse = sumDiffuse / weight;
        meanSpecular = sumSpecular / weight;
    }
    outDiffuse = hasHistory ? lerp(historyD, meanDiffuse, historyAlpha) : meanDiffuse;
    outSpecular = hasHistory ? lerp(historyS, meanSpecular, historyAlpha) : meanSpecular;
    diffuseOut[id.xy] = float4(max(outDiffuse, 0.0), frames + 1);
    specularOut[id.xy] = float4(max(outSpecular, 0.0), fast);
    if (backfaceOn)
    {
        RWTexture2D<float4> backfaceOut = ResourceDescriptorHeap[P[11].x];
        backfaceOut[id.xy] = float4(max(outBackface, 0.0), 1);
    }
}
