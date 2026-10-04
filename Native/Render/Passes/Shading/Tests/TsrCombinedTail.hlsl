// unx-kernel: cs_6_6 main
// Test-only combined analysis. P0-3=rejection; P4={lumaPrefix,history,info,reset}; P5={moireOut,historyOut,0,0}.
// 6*34*34 uint scratch +20*20 uint moire =29344 bytes (<32KiB).
#include "Passes/Shading/Tsr.hlsli"
#include "Passes/Common/Frame.hlsli"
#include "Passes/Shading/TsrRejectCodes.hlsli"
groupshared uint gWork[6936];
groupshared uint gMoire[400];
#define TILE 16
#define BORDER 9
#define THIN_GRADIENT 0.00007  // (the reference's ValidGeometryGradientThreshold)
#define SIDE (TILE + 2 * BORDER)
#define CELLS (SIDE * SIDE)
#define MIN_BLEND 0.05
#define ENCODING_ERROR (1.0 / 127.0)
#define MAX_COUNT 20.0


uint pack2(float a, float b) { return (uint)(saturate(a) * 65535.0 + 0.5) | ((uint)(saturate(b) * 65535.0 + 0.5) << 16); }
float2 unpack2(uint v) { return float2(v & 0xFFFFu, v >> 16) * (1.0 / 65535.0); }
uint2 codes2(uint v) { return uint2(v & 0xFFFFu, v >> 16); }
uint packCodes2(uint2 v) { return v.x | (v.y << 16); }
uint flickerCellIndex(int2 c) { return (uint)(c.y * SIDE + c.x); }
bool flickerInMargin(int2 c, int margin) { return all(c >= margin) && all(c < SIDE - margin); }

void sort3(inout float2 a, inout float2 b, inout float2 c)
{
    const float2 lo = min(a, min(b, c)), hi = max(a, max(b, c));
    const float2 mid = a + b + c - lo - hi;
    a = lo;
    b = mid;
    c = hi;
}
float2 median3(float2 a, float2 b, float2 c) { return max(min(a, b), min(max(a, b), c)); }

void flicker(uint2 group, uint lane)
{
    const int2 size = int2(P[1].zw);
    const int2 origin = int2(group) * TILE - BORDER;
    Texture2D<float4> historyTexture = ResourceDescriptorHeap[P[4].y];
    const float q = TSR_QUANTIZATION_ERROR, filteringWeight = 0.25;
    uint i;

    Texture2D<uint4> prepared = ResourceDescriptorHeap[P[4].x];
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        const uint4 value = prepared.Load(int3(clamp(origin + c + 9, 0, size + 17), 0));
        gWork[i] = value.x; gWork[3468 + i] = value.y; gWork[1156 + i] = value.z;
    }
    GroupMemoryBarrierWithGroupSync();

    // 3: the filtered signals and the clamped input's range
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!flickerInMargin(c, 1)) continue;
        float2 blurred = 0;
        float lo = 1, hi = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const int2 o = int2(k % 3, k / 3) - 1;
            const float2 v = unpack2(gWork[3468 + (flickerCellIndex(c + o))]);
            blurred += v * ((o.x == 0 ? 0.5 : 0.25) * (o.y == 0 ? 0.5 : 0.25));
            lo = min(lo, v.x);
            hi = max(hi, v.x);
        }
        gWork[4624 + (i)] = pack2(blurred.x, blurred.y);
        gWork[1156 + (i)] = (gWork[1156 + (i)] & 0xFFFFu) | (pack2(0, hi - lo) & 0xFFFF0000u);
    }
    GroupMemoryBarrierWithGroupSync();

    // 3b: the filtered history clamped into the box relaxed by the thin geometry's weight (gAB is free until stage 6)
    const bool thin = P[2].z != UNX_NONE;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!flickerInMargin(c, 2)) continue;
        float2 lo = 1, hi = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const float2 v = unpack2(gWork[4624 + (flickerCellIndex(c + int2(k % 3, k / 3) - 1))]);
            lo = min(lo, v);
            hi = max(hi, v);
        }
        float weight = 0;
        if (thin)
        {
            Texture2D<float> relaxationTexture = ResourceDescriptorHeap[P[2].z];
            weight = relaxationTexture.Load(int3(clamp(origin + c, 0, size - 1), 0));
            weight = weight > 1.0 / 127.0 ? weight : 0.0;
        }
        gWork[2312 + (i)] = pack2(clamp(unpack2(gWork[4624 + (i)]).y, lerp(lo.x, lo.y, weight), lerp(hi.x, hi.y, weight)), weight);
    }
    GroupMemoryBarrierWithGroupSync();

    // 4: the clamp box, the energy it removes from the filtered history, the raw rejection
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!flickerInMargin(c, 3)) continue;
        const bool relaxed = unpack2(gWork[2312 + (i)]).y > 0;
        float lo = 1, hi = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const uint ni = flickerCellIndex(c + int2(k % 3, k / 3) - 1);
            const float v = relaxed ? unpack2(gWork[2312 + (ni)]).x : unpack2(gWork[4624 + (ni)]).x;
            lo = min(lo, v);
            hi = max(hi, v);
        }
        const float range = unpack2(gWork[1156 + (i)]).y;
        const float clampError = max(q, range * (filteringWeight * 0.25)) + q;
        const float2 filtered = unpack2(gWork[4624 + (i)]);
        const float clamped = clamp(filtered.y, lo - clampError, hi + clampError);
        const float delta = max(abs(filtered.x - filtered.y), range * filteringWeight + q * 2.0 * filteringWeight);
        const float energy = abs(clamped - filtered.y);
        gWork[5780 + (i)] = pack2(energy, saturate(1.0 - energy / delta));
    }
    GroupMemoryBarrierWithGroupSync();

    // 5: 3 x 3 medians
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!flickerInMargin(c, 4)) continue;
        float2 lows[3], mids[3], highs[3];
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 a = unpack2(gWork[5780 + (flickerCellIndex(c + int2(x, -1)))]), b = unpack2(gWork[5780 + (flickerCellIndex(c + int2(x, 0)))]), d = unpack2(gWork[5780 + (flickerCellIndex(c + int2(x, 1)))]);
            sort3(a, b, d);
            lows[x + 1] = a;
            mids[x + 1] = b;
            highs[x + 1] = d;
        }
        const float2 m = median3(max(lows[0], max(lows[1], lows[2])), median3(mids[0], mids[1], mids[2]), min(highs[0], min(highs[1], highs[2])));
        // Stage 3 consumed the clamps; its barrier precedes this reuse.
        gWork[3468 + (i)] = pack2(m.x, m.y);
    }
    GroupMemoryBarrierWithGroupSync();

    // 6: the rejection per cell, the history update it would cause, the gradient and whether it flickers
    const bool cut = (P[4].w & 1u) != 0;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!flickerInMargin(c, 5)) continue;
        const bool relaxed = unpack2(gWork[2312 + (i)]).y > 0;
        float filteredEnergy = 0, clampBlend = 1;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const float2 v = unpack2(gWork[3468 + (flickerCellIndex(c + int2(k % 3, k / 3) - 1))]);
            filteredEnergy = max(filteredEnergy, v.x);
            clampBlend = min(clampBlend, v.y);
        }
        const float2 filtered = unpack2(gWork[4624 + (i)]);
        const float2 gradientRange = unpack2(gWork[1156 + (i)]);
        const float delta = max(abs(filtered.x - filtered.y), gradientRange.y * filteringWeight + q * 2.0 * filteringWeight);
        const float rejection = saturate(1.0 - filteredEnergy / delta);
        const float2 own = unpack2(gWork[0 + (i)]);
        const float input = own.x, previous = own.y;
        // (the plus-shaped range of the input)
        const float e = unpack2(gWork[0 + (flickerCellIndex(c + int2(1, 0)))]).x, w = unpack2(gWork[0 + (flickerCellIndex(c + int2(-1, 0)))]).x;
        const float s = unpack2(gWork[0 + (flickerCellIndex(c + int2(0, 1)))]).x, n = unpack2(gWork[0 + (flickerCellIndex(c + int2(0, -1)))]).x;
        const float clampedPrevious = clamp(previous, min(input, min(min(e, w), min(s, n))), max(input, max(max(e, w), max(s, n))));
        const float finalPrevious = lerp(clampedPrevious, previous, clampBlend);
        const float updated = lerp(finalPrevious, input, max(1.0 - rejection, MIN_BLEND));
        const float ghosting = lerp(previous, input, MIN_BLEND);
        const float gradient = updated - ghosting;
        const float previousGradient = gradientRange.x * (255.0 / 127.0) - 1.0;
        const bool sameSign = gradient * previousGradient > 0;
        bool withinError = abs(gradient) < ENCODING_ERROR || abs(previousGradient) < ENCODING_ERROR;
        // (thin geometry: its residual flicker is that small)
        if (relaxed && (abs(gradient) > THIN_GRADIENT || abs(previousGradient) > THIN_GRADIENT)) withinError = false;
        const float flicker = sameSign || withinError || cut ? 0.0 : 1.0;
        gWork[2312 + (i)] = pack2(min(abs(previousGradient), abs(gradient)) * flicker, flicker);
        // Stage 5 consumed raw energy/rejection. Keep medians in gCD intact
        // while adjacent cells still read them in this stage.
        gWork[5780 + (i)] = pack2(updated, gradient * 0.5 + 0.5);
    }
    GroupMemoryBarrierWithGroupSync();

    // 7: this thread's pixel
    for (uint oi = lane; oi < 400; oi += TILE * TILE)
    {
    const int2 outputLocal = int2(oi % 20, oi / 20) - 2;
    const int2 pixel = int2(group) * TILE + outputLocal;
    if (any(pixel < 0) || any(pixel >= size)) continue;
    const int2 cell = outputLocal + BORDER;
    float2 contribution = 0;  // the gradient variation and the flicker, dilated over 5 x 5
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x) contribution = max(contribution, unpack2(gWork[2312 + (flickerCellIndex(cell + int2(x, y)))]));
    const uint ci = flickerCellIndex(cell);
    const float2 state = unpack2(gWork[5780 + (ci)]);
    const float updated = state.x, gradient = state.y * 2.0 - 1.0;
    const float flicker = unpack2(gWork[2312 + (ci)]).y;
    const float4 history = historyTexture.Load(int3(pixel, 0));
    Texture2D<float2> decimateMask = ResourceDescriptorHeap[P[0].z];
    Texture2D<float4> info = ResourceDescriptorHeap[P[4].z];
    const bool disoccluded = ((uint)round(decimateMask.Load(int3(pixel, 0)).r * 255.0) & 3u) != 0;
    const float infoA = info.Load(int3(pixel, 0)).a;
    const float still = 1.0 - saturate((infoA - (infoA > 0.75 ? 1.0 : 0.0)) * 2.0);
    const float previousGradient = history.g * (255.0 / 127.0) - 1.0;
    float newGradient = previousGradient * (1.0 - MIN_BLEND) * (1.0 - flicker) + gradient;
    float variation = history.b * (1.0 - MIN_BLEND) * still + contribution.x;
    float count = history.a * MAX_COUNT * (1.0 - MIN_BLEND) * still + contribution.y;
    if (disoccluded)
    {
        newGradient = 0;
        variation = 0;
        count = 0;
    }
    const float quantizedCount = floor(count * (255.0 / MAX_COUNT)) * (MAX_COUNT / 255.0);
    variation = count > 0 ? variation * quantizedCount / count : 0.0;
    // (the flicker's period in frames: 2, longer below 60 Hz; the count fades the error in past half of 1 - 0.95^period;
    // a thin region's small gradients - the stage 6 rule again, at the pixel - at three times the period's count)
    float period = 2.0 / max(g_deltaTime * 60.0, 1.0);
    if (thin)
    {
        Texture2D<float> relaxationTexture = ResourceDescriptorHeap[P[2].z];
        const bool small = abs(gradient) < ENCODING_ERROR || abs(previousGradient) < ENCODING_ERROR;
        const bool present = abs(gradient) > THIN_GRADIENT || abs(previousGradient) > THIN_GRADIENT;
        if (relaxationTexture.Load(int3(pixel, 0)) > 1.0 / 127.0 && small && present) period *= 3.0;
    }
    const float fadeIn = saturate(count * (1.0 - pow(1.0 - MIN_BLEND, period)) - 0.5);
    const float error = count > 0 ? (abs(variation / count) + count * ENCODING_ERROR) * fadeIn : 0.0;
    // Match the original R16_FLOAT store/load boundary exactly.
    gMoire[oi] = f32tof16(saturate(error * still));
    if (any(outputLocal < 0) || any(outputLocal >= TILE)) continue;
    RWTexture2D<float> errorOut = ResourceDescriptorHeap[P[5].x];
    errorOut[pixel] = saturate(error * still);
    RWTexture2D<float4> historyOut = ResourceDescriptorHeap[P[5].y];
    historyOut[pixel] = float4(sqrt(saturate(updated)), saturate(clamp(newGradient, -1.0, 1.0) * (127.0 / 255.0) + 127.0 / 255.0), saturate(variation),
                               saturate(quantizedCount / MAX_COUNT));
    }
}

#undef BORDER
#undef SIDE
#undef CELLS
float readMoire(int2 pixel, uint2 group)
{
    const int2 local = pixel - int2(group) * 16 + 2;
    return f16tof32(gMoire[local.y * 20 + local.x]);
}
#ifndef TSR_REJECT_FROM_PREFIX
#define TSR_REJECT_FROM_PREFIX 1
#endif
#define PREFIX_STAGES (2 * TSR_REJECT_FROM_PREFIX)
#define BORDER (7 - PREFIX_STAGES)
#define SIDE (TILE + 2 * BORDER)
#define CELLS (SIDE * SIDE)


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

void reject(uint2 group, uint2 local, uint lane)
{
    const int2 size = int2(P[1].zw);
    const int2 origin = int2(group) * TILE - BORDER;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> guide = ResourceDescriptorHeap[P[0].y];
    uint i;

    // P[3].z: production TsrRejectPrefix output {clamped input, clamped
    // guide, original input, alias}. Its five pixels of real overscan and
    // two-pixel input halo retain the original seven-pixel support.
    Texture2D<uint4> prepared = ResourceDescriptorHeap[P[3].z];
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        const uint4 value = prepared.Load(int3(clamp(origin + c + 5, 0, size + 9), 0));
        gWork[2704 + (i)] = value.x; gWork[3380 + (i)] = value.y; gWork[0 + (i)] = value.z;
        if (all(c >= BORDER) && all(c < BORDER + TILE))
            gWork[4732 + ((c.y - BORDER) * TILE + c.x - BORDER)] = value.w;
    }
    GroupMemoryBarrierWithGroupSync();
    // 3: the filtered signals, the input's variation and range
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 3 - PREFIX_STAGES)) continue;
        float3 filteredInput = 0, filteredGuide = 0, sumC = 0, sumD = 0, cMin = 1, cMax = 0;
        FOR_3X3(c, {
            const float3 cc = unpack(gWork[2704 + (ni)]);
            const float3 gg = unpack(gWork[3380 + (ni)]);
            const float3 dd = abs(unpack(gWork[0 + (ni)]) - cc);
            filteredInput += cc * nw;
            filteredGuide += gg * nw;
            sumC += cc;
            sumD += dd;
            cMin = min(cMin, cc);
            cMax = max(cMax, cc);
        })
        const float3 centre = unpack(gWork[2704 + (i)]);
        const float3 variationC = abs(1.125 * centre - 0.125 * sumC);
        const float3 variationD = abs(1.125 * abs(unpack(gWork[0 + (i)]) - centre) - 0.125 * sumD);
        gWork[1352 + (i)] = pack(filteredInput);
        gWork[2028 + (i)] = pack(filteredGuide);
        gWork[676 + (i)] = pack(min(variationC, variationD));
        gWork[4056 + (i)] = pack(cMax - cMin);
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
            if (!inMargin(c, 4 - PREFIX_STAGES)) continue;
            float3 inputMin = 1, inputMax = 0, guideMin = 1, guideMax = 0;
            FOR_3X3(c, {
                const float3 a = unpack(gWork[1352 + (ni)]);
                const float3 b = unpack(gWork[2028 + (ni)]);
                inputMin = min(inputMin, a);
                inputMax = max(inputMax, a);
                guideMin = min(guideMin, b);
                guideMax = max(guideMax, b);
            })
            float weight = relaxationTexture.Load(int3(clamp(origin + c, 0, size - 1), 0));
            weight = weight > 1.0 / 127.0 ? weight : 0.0;
            gWork[0 + (i)] = pack(clamp(unpack(gWork[2028 + (i)]), lerp(inputMin, guideMin, weight), lerp(inputMax, guideMax, weight)));
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // 4: the clamp box, what it removes from the filtered guide, the raw rejection
    const float q = TSR_QUANTIZATION_ERROR, filteringWeight = 0.25;
    const bool moire = P[2].y != 0xFFFFFFFFu;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 5 - PREFIX_STAGES)) continue;
        float3 blurredVariation = 0, boxMin = 1, boxMax = 0, relaxedMin = 1, relaxedMax = 0;
        FOR_3X3(c, {
            blurredVariation += unpack(gWork[676 + (ni)]) * nw;
            const float3 f = unpack(gWork[1352 + (ni)]);
            boxMin = min(boxMin, f);
            boxMax = max(boxMax, f);
            const float3 l = unpack(gWork[0 + (ni)]);
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
        const float3 range = unpack(gWork[4056 + (i)]);
        const float3 clampError = max(max(float3(q, q, q), blurredVariation), range * (filteringWeight * 0.25)) + q;
        const float3 filteredGuide = unpack(gWork[2028 + (i)]), filteredInput = unpack(gWork[1352 + (i)]);
        float3 lo = boxMin - clampError, hi = boxMax + clampError;
        float3 boxSize = range * filteringWeight + q * 2.0 * filteringWeight;
        float moireError = 0;
        if (moire)
        {
            moireError = readMoire(clamp(origin + c, 0, size - 1), group);
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
        gWork[2704 + (i)] = pack(energy);
        gWork[3380 + (i)] = asuint(min3(saturate(1.0 - energy / delta)));
    }
    GroupMemoryBarrierWithGroupSync();

    // 5: 3 x 3 medians (columns sorted, then max of the lows, median of the mids, min of the highs, their median)
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 6 - PREFIX_STAGES)) continue;
        float4 lows[3], mids[3], highs[3];
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float4 column[3];
            [unroll] for (int y = -1; y <= 1; ++y)
            {
                const uint ni = cellIndex(c + int2(x, y));
                column[y + 1] = float4(unpack(gWork[2704 + (ni)]), asfloat(gWork[3380 + (ni)]));
            }
            sortLmh(column[0], column[1], column[2]);
            lows[x + 1] = column[0];
            mids[x + 1] = column[1];
            highs[x + 1] = column[2];
        }
        const float4 m = median3(max(lows[0], max(lows[1], lows[2])), median3(mids[0], mids[1], mids[2]), min(highs[0], min(highs[1], highs[2])));
        gWork[0 + (i)] = pack(m.rgb);
        gWork[676 + (i)] = asuint(m.a);
    }
    GroupMemoryBarrierWithGroupSync();

    // 6: this thread's pixel
    const int2 pixel = int2(group) * TILE + int2(local);
    if (any(pixel >= size)) return;
    const int2 cell = int2(local) + BORDER;
    float3 filteredEnergy = 0;
    float clampBlend = 1;
    FOR_3X3(cell, {
        filteredEnergy = max(filteredEnergy, unpack(gWork[0 + (ni)]));
        clampBlend = min(clampBlend, asfloat(gWork[676 + (ni)]));
    })
    const uint ci = cellIndex(cell);
    float3 boxSize = unpack(gWork[4056 + (ci)]) * filteringWeight + q * 2.0 * filteringWeight;
    if (moire)
    {
        const float3 energy = unpack(gWork[2704 + (ci)]);
        const float total = energy.r + energy.g + energy.b;
        boxSize = max(boxSize, (total > 0 ? energy / total : float3(0, 0, 0)) * (filteringWeight * 3.0 * readMoire(pixel, group)));
    }
    const float3 delta = max(abs(unpack(gWork[1352 + (ci)]) - unpack(gWork[2028 + (ci)])), boxSize);
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
    const bool antiAlias = gWork[4732 + (local.y * TILE + local.x)] != 0 && (rejection < 0.25 || disoccluded);
    RWTexture2D<float4> rejectionOut = ResourceDescriptorHeap[P[0].w];
    // (the stores round down: a value is never raised by its 8 bits)
    rejectionOut[pixel] = float4(floor(float3(rejection, disableClamp, 1.0 - increaseValidity) * float3(255, 255, 255) + float3(0, 0, 0.999)) / 255.0,
                                 ((disoccluded ? 0.0 : 1.0) + (resurrected ? 2.0 : 0.0) + ((maskBits & 8u) != 0 ? 4.0 : 0.0)) / 255.0);
    RWTexture2D<float2> aaInput = ResourceDescriptorHeap[P[1].y];
    const float luma = dot(min(input, 65504.0), float3(0.299, 0.587, 0.114));
    aaInput[pixel] = float2(luma / (0.5 + luma), antiAlias ? 1.0 : 0.0);
}

[numthreads(16,16,1)]
void main(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    flicker(group, lane);
    GroupMemoryBarrierWithGroupSync(); // retire flicker reads before RGB scratch reuse
    reject(group, local, lane);
}
