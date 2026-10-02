// unx-kernel: cs_6_6 main
// m.tsr.reject (Tsr.hlsli; the reference's TSRRejectShading, MeasureRejection): the shading rejection of the history,
// this frame's guide, and the spatial anti-aliaser's inputs. One group per 16 x 16 internal pixels; the chain of 3 x 3
// operators needs 7 more pixels on every side, so the group works on a 30 x 30 region held in group memory (colours as
// three 11 / 11 / 10-bit square roots: the guide's own precision), one stage after the other:
//   0  I = the input and H = the reprojected guide in the measurement space
//   1  H' = H clamped to I's 3 x 3 range, I' = I clamped to H's 3 x 3 range
//   2  C = I clamped to H''s 3 x 3 range (the input without what only its aliasing has), G = H clamped to I''s
//   3  FI = blur(C), FH = blur(G), V = min(|variation(|I - C|)|, |variation(C)|), B = the 3 x 3 range of C
//   3b (thin geometry, P[2].z: TsrThin.hlsl's relaxation weight w) L = FH clamped into the box between FI's 3 x 3 range
//      and FH's own, w of the way to FH's
//   4  the clamp box = the 3 x 3 range of FI - where w > 0 the 3 x 3 range of L: what of the history its neighbourhood
//      holds too, a thin feature a pixel aside of where this frame's samples found it - widened by the error
//      max(q, blur(V), B / 16) + q; E = |clamp(FH) - FH|, R = min over channels of 1 - E / max(|FI - FH|, B / 4 + q / 2)
//   5  the 3 x 3 medians of E and R
//   6  per pixel: the 3 x 3 maximum of the median E over the same denominator -> the rejection (1 = the history
//      holds), the 3 x 3 minimum of the median R -> how far the history may stay unclamped.
// Then, per pixel: the guide of this frame (the input blended into the reprojected guide: all of it where the pixel was
// disoccluded or rejected, a 1 / (1 + 16 scale^2) share where it holds), whether the spatial anti-aliaser runs (visible
// aliasing and a rejected or missing history), and the outputs.
// Resurrection (P[3].x; TsrResurrect.hlsl measured the kept frame's guide the same way): a pixel the kept frame is the
// closer one for, and whose rejection is better there by 0.1, takes the kept frame - its rejection and clamp disable,
// its guide as this frame's history, and bit 1 of the rejection's a: m.upscale then reads the kept frame's history. A
// resurrected pixel is not disoccluded for the update (its history is the kept frame's).
// P[0] = { colour SRV (internal, exposed linear), reprojected guide SRV (R10G10B10A2), decimate mask SRV (RG8),
//          rejection UAV (RGBA8: rejection, history clamp disable, validity decrease, a x 255: bit 0 not disoccluded,
//          bit 1 resurrected) }
// P[1] = { guide UAV (R10G10B10A2, next frame's history: guide colour, a = this frame's reprojection edge), AA input UAV
//          (RG8: LDR luma, mask), width, height }, P[2] = { asuint(theoretic blend factor), moire error SRV (R16F,
//          TsrFlicker.hlsl; UNX_NONE: no flickering heuristic), thin geometry relaxation SRV (R8, TsrThin.hlsl;
//          UNX_NONE: none), layers SRV (RGBA8, UpscaleMotion.hlsl; UNX_NONE: none) }: inside the moire error (x 3: luma to a
//          channel) the filtered guide is not clamped, and the denominator is at least the error's share of the channel.
//          The layers' rule: a pixel under a layer without a vector (g: particles, see-through fragments) or seen
//          through a tracked glass or water whose background moves otherwise (b) keeps the history's clamp by that
//          amount - the history cannot leave this frame's samples' range, no trail -, and the first also the validity's
//          decrease (a short history: the layer's own change shows at once)
// P[3] = { resurrection measure SRV (RGBA8, TsrResurrect.hlsl; UNX_NONE: no resurrection this frame), resurrected
//          guide SRV (R10G10B10A2), 0, 0 }
#include "Passes/Shading/Tsr.hlsli"

#define TILE 16
#define BORDER 7
#define SIDE (TILE + 2 * BORDER)
#define CELLS (SIDE * SIDE)

groupshared uint gA[CELLS];
groupshared uint gB[CELLS];
groupshared uint gC[CELLS];
groupshared uint gD[CELLS];
groupshared uint gE[CELLS];
groupshared uint gF[CELLS];
groupshared uint gG[CELLS];
groupshared uint gAlias[TILE * TILE];

uint pack(float3 c)
{
    const float3 s = sqrt(saturate(c));
    return (uint)(s.r * 2047.0 + 0.5) | ((uint)(s.g * 2047.0 + 0.5) << 11) | ((uint)(s.b * 1023.0 + 0.5) << 22);
}
float3 unpack(uint v)
{
    const float3 s = float3(v & 2047u, (v >> 11) & 2047u, v >> 22) * float3(1.0 / 2047.0, 1.0 / 2047.0, 1.0 / 1023.0);
    return s * s;
}
uint cellIndex(int2 c) { return (uint)(c.y * SIDE + c.x); }
bool inMargin(int2 c, int margin) { return all(c >= margin) && all(c < SIDE - margin); }
float min3(float3 v) { return min(v.x, min(v.y, v.z)); }

#define FOR_3X3(c, body)                                         \
    {                                                           \
        [unroll] for (int ny = -1; ny <= 1; ++ny)               \
            [unroll] for (int nx = -1; nx <= 1; ++nx)           \
            {                                                   \
                const uint ni = cellIndex(c + int2(nx, ny));     \
                const float nw = (nx == 0 ? 0.5 : 0.25) * (ny == 0 ? 0.5 : 0.25); \
                body                                            \
            }                                                   \
    }

void sortLmh(inout float4 a, inout float4 b, inout float4 c)
{
    const float4 lo = min(a, min(b, c)), hi = max(a, max(b, c));
    const float4 mid = a + b + c - lo - hi;
    a = lo;
    b = mid;
    c = hi;
}
float4 median3(float4 a, float4 b, float4 c) { return max(min(a, b), min(max(a, b), c)); }

[numthreads(TILE, TILE, 1)]
void main(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const int2 size = int2(P[1].zw);
    const int2 origin = int2(group) * TILE - BORDER;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> guide = ResourceDescriptorHeap[P[0].y];
    uint i;

    // 0: the region's input and guide in the measurement space
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 p = clamp(origin + int2(i % SIDE, i / SIDE), 0, size - 1);
        float3 c = colour.Load(int3(p, 0)).rgb;
        c = all(isfinite(c)) ? max(c, 0.0) : float3(0, 0, 0);
        gA[i] = pack(tsrLinearToMeasure(c));
        gB[i] = pack(tsrGuideToMeasure(guide.Load(int3(p, 0)).rgb));
    }
    GroupMemoryBarrierWithGroupSync();

    // 1: each clamped to the other's 3 x 3 range
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 1)) continue;
        float3 inputMin = 1, inputMax = 0, guideMin = 1, guideMax = 0;
        FOR_3X3(c, {
            const float3 a = unpack(gA[ni]);
            const float3 b = unpack(gB[ni]);
            inputMin = min(inputMin, a);
            inputMax = max(inputMax, a);
            guideMin = min(guideMin, b);
            guideMax = max(guideMax, b);
        })
        gC[i] = pack(clamp(unpack(gB[i]), inputMin, inputMax));
        gD[i] = pack(clamp(unpack(gA[i]), guideMin, guideMax));
        if (all(c >= BORDER) && all(c < BORDER + TILE))
            gAlias[(c.y - BORDER) * TILE + (c.x - BORDER)] = dot(inputMax - inputMin, float3(0.299, 0.587, 0.114)) > TSR_AA_MIN_LUMINANCE ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // 2: mutual annihilation
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 2)) continue;
        float3 aMin = 1, aMax = 0, bMin = 1, bMax = 0;
        FOR_3X3(c, {
            const float3 a = unpack(gC[ni]);
            const float3 b = unpack(gD[ni]);
            aMin = min(aMin, a);
            aMax = max(aMax, a);
            bMin = min(bMin, b);
            bMax = max(bMax, b);
        })
        gE[i] = pack(clamp(unpack(gA[i]), aMin, aMax));  // C: the clamped input
        gF[i] = pack(clamp(unpack(gB[i]), bMin, bMax));  // G: the clamped guide
    }
    GroupMemoryBarrierWithGroupSync();

    // 3: the filtered signals, the input's variation and range
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 3)) continue;
        float3 filteredInput = 0, filteredGuide = 0, sumC = 0, sumD = 0, cMin = 1, cMax = 0;
        FOR_3X3(c, {
            const float3 cc = unpack(gE[ni]);
            const float3 gg = unpack(gF[ni]);
            const float3 dd = abs(unpack(gA[ni]) - cc);
            filteredInput += cc * nw;
            filteredGuide += gg * nw;
            sumC += cc;
            sumD += dd;
            cMin = min(cMin, cc);
            cMax = max(cMax, cc);
        })
        const float3 centre = unpack(gE[i]);
        const float3 variationC = abs(1.125 * centre - 0.125 * sumC);
        const float3 variationD = abs(1.125 * abs(unpack(gA[i]) - centre) - 0.125 * sumD);
        gC[i] = pack(filteredInput);
        gD[i] = pack(filteredGuide);
        gB[i] = pack(min(variationC, variationD));
        gG[i] = pack(cMax - cMin);
    }
    GroupMemoryBarrierWithGroupSync();

    // 3b: the filtered guide clamped into the relaxed box (gA is free until the medians)
    const bool thin = P[2].z != 0xFFFFFFFFu;
    if (thin)
    {
        Texture2D<float> relaxationTexture = ResourceDescriptorHeap[P[2].z];
        for (i = lane; i < CELLS; i += TILE * TILE)
        {
            const int2 c = int2(i % SIDE, i / SIDE);
            if (!inMargin(c, 4)) continue;
            float3 inputMin = 1, inputMax = 0, guideMin = 1, guideMax = 0;
            FOR_3X3(c, {
                const float3 a = unpack(gC[ni]);
                const float3 b = unpack(gD[ni]);
                inputMin = min(inputMin, a);
                inputMax = max(inputMax, a);
                guideMin = min(guideMin, b);
                guideMax = max(guideMax, b);
            })
            float weight = relaxationTexture.Load(int3(clamp(origin + c, 0, size - 1), 0));
            weight = weight > 1.0 / 127.0 ? weight : 0.0;
            gA[i] = pack(clamp(unpack(gD[i]), lerp(inputMin, guideMin, weight), lerp(inputMax, guideMax, weight)));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // 4: the clamp box, what it removes from the filtered guide, the raw rejection
    const float q = TSR_QUANTIZATION_ERROR, filteringWeight = 0.25;
    const bool moire = P[2].y != 0xFFFFFFFFu;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 5)) continue;
        float3 blurredVariation = 0, boxMin = 1, boxMax = 0, relaxedMin = 1, relaxedMax = 0;
        FOR_3X3(c, {
            blurredVariation += unpack(gB[ni]) * nw;
            const float3 f = unpack(gC[ni]);
            boxMin = min(boxMin, f);
            boxMax = max(boxMax, f);
            const float3 l = unpack(gA[ni]);
            relaxedMin = min(relaxedMin, l);
            relaxedMax = max(relaxedMax, l);
        })
        if (thin)
        {
            Texture2D<float> relaxationTexture = ResourceDescriptorHeap[P[2].z];
            if (relaxationTexture.Load(int3(clamp(origin + c, 0, size - 1), 0)) > 1.0 / 127.0)
            {
                boxMin = relaxedMin;
                boxMax = relaxedMax;
            }
        }
        const float3 range = unpack(gG[i]);
        const float3 clampError = max(max(float3(q, q, q), blurredVariation), range * (filteringWeight * 0.25)) + q;
        const float3 filteredGuide = unpack(gD[i]), filteredInput = unpack(gC[i]);
        float3 lo = boxMin - clampError, hi = boxMax + clampError;
        float3 boxSize = range * filteringWeight + q * 2.0 * filteringWeight;
        float moireError = 0;
        if (moire)
        {
            Texture2D<float> moireTexture = ResourceDescriptorHeap[P[2].y];
            moireError = moireTexture.Load(int3(clamp(origin + c, 0, size - 1), 0));
            const float3 widened = 3.0 * moireError;
            const float3 stableLo = min(lo, hi - widened), stableHi = max(hi, lo + widened);
            lo = stableLo;
            hi = stableHi;
        }
        const float3 clamped = clamp(filteredGuide, lo, hi);
        const float3 energy = abs(clamped - filteredGuide);
        if (moire)
        {
            const float total = energy.r + energy.g + energy.b;
            boxSize = max(boxSize, (total > 0 ? energy / total : float3(0, 0, 0)) * (filteringWeight * 3.0 * moireError));
        }
        const float3 delta = max(abs(filteredInput - filteredGuide), boxSize);
        gE[i] = pack(energy);
        gF[i] = asuint(min3(saturate(1.0 - energy / delta)));
    }
    GroupMemoryBarrierWithGroupSync();

    // 5: 3 x 3 medians (columns sorted, then max of the lows, median of the mids, min of the highs, their median)
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 6)) continue;
        float4 lows[3], mids[3], highs[3];
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float4 column[3];
            [unroll] for (int y = -1; y <= 1; ++y)
            {
                const uint ni = cellIndex(c + int2(x, y));
                column[y + 1] = float4(unpack(gE[ni]), asfloat(gF[ni]));
            }
            sortLmh(column[0], column[1], column[2]);
            lows[x + 1] = column[0];
            mids[x + 1] = column[1];
            highs[x + 1] = column[2];
        }
        const float4 m = median3(max(lows[0], max(lows[1], lows[2])), median3(mids[0], mids[1], mids[2]), min(highs[0], min(highs[1], highs[2])));
        gA[i] = pack(m.rgb);
        gB[i] = asuint(m.a);
    }
    GroupMemoryBarrierWithGroupSync();

    // 6: this thread's pixel
    const int2 pixel = int2(group) * TILE + int2(local);
    if (any(pixel >= size)) return;
    const int2 cell = int2(local) + BORDER;
    float3 filteredEnergy = 0;
    float clampBlend = 1;
    FOR_3X3(cell, {
        filteredEnergy = max(filteredEnergy, unpack(gA[ni]));
        clampBlend = min(clampBlend, asfloat(gB[ni]));
    })
    const uint ci = cellIndex(cell);
    float3 boxSize = unpack(gG[ci]) * filteringWeight + q * 2.0 * filteringWeight;
    if (moire)
    {
        Texture2D<float> moireTexture = ResourceDescriptorHeap[P[2].y];
        const float3 energy = unpack(gE[ci]);
        const float total = energy.r + energy.g + energy.b;
        boxSize = max(boxSize, (total > 0 ? energy / total : float3(0, 0, 0)) * (filteringWeight * 3.0 * moireTexture.Load(int3(pixel, 0))));
    }
    const float3 delta = max(abs(unpack(gC[ci]) - unpack(gD[ci])), boxSize);
    float rejection = min3(saturate(1.0 - filteredEnergy / delta));

    Texture2D<float2> decimateMask = ResourceDescriptorHeap[P[0].z];
    const float2 mask = decimateMask.Load(int3(pixel, 0));
    const uint maskBits = (uint)round(mask.r * 255.0);
    const bool hidden = (maskBits & 3u) != 0;  // (off screen, a cut, or hidden in the previous frame)
    const float velocityEdge = mask.g;
    float guideUncertainty = 1;
    [unroll] for (int k = 0; k < 9; ++k)
        guideUncertainty = min(guideUncertainty, guide.Load(int3(clamp(pixel + int2(k % 3, k / 3) - 1, 0, size - 1), 0)).a);
    float3 history = tsrGuideToLinear(guide.Load(int3(pixel, 0)).rgb);

    // resurrection: the kept frame in place of the previous frame's history
    bool resurrected = false;
    if (P[3].x != 0xFFFFFFFFu)
    {
        Texture2D<float4> resurrection = ResourceDescriptorHeap[P[3].x];
        const float4 kept = resurrection.Load(int3(pixel, 0));
        resurrected = kept.b > 0.5 && (maskBits & 16u) == 0 && kept.r - rejection > 0.1;
        if (resurrected)
        {
            Texture2D<float4> keptGuide = ResourceDescriptorHeap[P[3].y];
            const float4 keptValue = keptGuide.Load(int3(pixel, 0));
            rejection = kept.r;
            clampBlend = kept.g;
            history = tsrGuideToLinear(keptValue.rgb);
            guideUncertainty = keptValue.a;
        }
    }
    const bool disoccluded = hidden && !resurrected;

    // this frame's guide
    float3 input = colour.Load(int3(pixel, 0)).rgb;
    input = all(isfinite(input)) ? max(input, 0.0) : float3(0, 0, 0);
    const float totalBlend = hidden ? 1.0 : saturate(1.0 - rejection * 4.0);
    const float blend = max(max(asfloat(P[2].x), 1.0 - rejection), totalBlend);
    const float3 mixed = lerp(tsrToAccumulation(history), tsrToAccumulation(input), blend);
    RWTexture2D<float4> guideOut = ResourceDescriptorHeap[P[1].x];
    guideOut[pixel] = float4(saturate(tsrLinearToGuide(tsrFromAccumulation(mixed))), velocityEdge);

    float animated = 0, seenThrough = 0;
    if (P[2].w != 0xFFFFFFFFu)
    {
        Texture2D<float4> layers = ResourceDescriptorHeap[P[2].w];
        const float4 layer = layers.Load(int3(pixel, 0));
        animated = layer.g;
        seenThrough = layer.b;
    }
    const float disableClamp = disoccluded ? 0.0 : min(clampBlend, min(velocityEdge, guideUncertainty)) * (1.0 - max(animated, seenThrough));
    const float increaseValidity = disoccluded ? 0.0 : clampBlend * (1.0 - animated);
    const bool antiAlias = gAlias[local.y * TILE + local.x] != 0 && (rejection < 0.25 || disoccluded);
    RWTexture2D<float4> rejectionOut = ResourceDescriptorHeap[P[0].w];
    // (the stores round down: a value is never raised by its 8 bits)
    rejectionOut[pixel] = float4(floor(float3(rejection, disableClamp, 1.0 - increaseValidity) * float3(255, 255, 255) + float3(0, 0, 0.999)) / 255.0,
                                 ((disoccluded ? 0.0 : 1.0) + (resurrected ? 2.0 : 0.0)) / 255.0);
    RWTexture2D<float2> aaInput = ResourceDescriptorHeap[P[1].y];
    const float luma = dot(min(input, 65504.0), float3(0.299, 0.587, 0.114));
    aaInput[pixel] = float2(luma / (0.5 + luma), antiAlias ? 1.0 : 0.0);
}
