// unx-kernel: cs_6_6 main
// m.tsr.flicker (Tsr.hlsli; the reference's flickering temporal analysis, TSRShadingAnalysis.ush ComputeMoireError): a
// pattern finer than the pixel grid (tile joints, gratings, distant detail) changes its samples with every jitter
// offset; the shading rejection would take that for a change of shading and drop the history each frame - the image
// flickers at the jitter's rate. Here each pixel's luma is followed over time: the rejection's own measure on the luma
// alone, the history update it would cause, and the gradient that leaves between the updated history and a history that
// just ghosts (5 % a frame). A gradient whose sign flips against the last frame's is a flicker; the smaller of the two
// gradients' sizes is its amplitude. Their running sum and count (20 frames, x 0.95 a frame; only on what stands
// still) give the moire error: the luma band inside which m.tsr.reject lets the history ghost.
// One group per 16 x 16 internal pixels over a 32 x 32 region in group memory (the 3 x 3 chain reaches 6 pixels, the
// 5 x 5 dilation of the flicker 2 more), values as 16-bit pairs.
// P[0] = { colour SRV (internal, exposed linear), reprojected flickering history SRV (RGBA8: luma in the guide space,
//          gradient x 127 / 255 + 127 / 255, total variation, count / 20), decimate mask SRV (RG8), info SRV (RGBA16F,
//          TsrDilate.hlsl: a = offset + is-moving / 2) }
// P[1] = { moire error UAV (R16F), flickering history UAV (RGBA8, next frame's), width, height }, P[2].x = flags (1: reset)
// Frame constants b1 = the main view (the frame's time step).
#include "Passes/Shading/Tsr.hlsli"
#include "Passes/Common/Frame.hlsli"

#define TILE 16
#define BORDER 8
#define SIDE (TILE + 2 * BORDER)
#define CELLS (SIDE * SIDE)
#define MIN_BLEND 0.05
#define ENCODING_ERROR (1.0 / 127.0)
#define MAX_COUNT 20.0

groupshared uint gLH[CELLS];   // input luma, history luma
groupshared uint gGB[CELLS];   // previous gradient (snorm), the clamped input's 3 x 3 range
groupshared uint gAB[CELLS];   // stage 1 clamps; later: gradient variation, is-flicker
groupshared uint gCD[CELLS];   // stage 2 clamps; later: the updated history, the current gradient (snorm)
groupshared uint gF[CELLS];    // filtered input, filtered history
groupshared uint gE[CELLS];    // raw clamped energy, raw rejection
groupshared uint gM[CELLS];    // their medians

uint pack2(float a, float b) { return (uint)(saturate(a) * 65535.0 + 0.5) | ((uint)(saturate(b) * 65535.0 + 0.5) << 16); }
float2 unpack2(uint v) { return float2(v & 0xFFFFu, v >> 16) * (1.0 / 65535.0); }
uint cellIndex(int2 c) { return (uint)(c.y * SIDE + c.x); }
bool inMargin(int2 c, int margin) { return all(c >= margin) && all(c < SIDE - margin); }

void sort3(inout float2 a, inout float2 b, inout float2 c)
{
    const float2 lo = min(a, min(b, c)), hi = max(a, max(b, c));
    const float2 mid = a + b + c - lo - hi;
    a = lo;
    b = mid;
    c = hi;
}
float2 median3(float2 a, float2 b, float2 c) { return max(min(a, b), min(max(a, b), c)); }

[numthreads(TILE, TILE, 1)]
void main(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const int2 size = int2(P[1].zw);
    const int2 origin = int2(group) * TILE - BORDER;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> historyTexture = ResourceDescriptorHeap[P[0].y];
    const float q = TSR_QUANTIZATION_ERROR, filteringWeight = 0.25;
    uint i;

    // 0: the region's luma (measurement space, the channels' mean), the history's luma and gradient
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 p = clamp(origin + int2(i % SIDE, i / SIDE), 0, size - 1);
        float3 c = colour.Load(int3(p, 0)).rgb;
        c = all(isfinite(c)) ? max(c, 0.0) : float3(0, 0, 0);
        const float4 h = historyTexture.Load(int3(p, 0));
        gLH[i] = pack2(dot(tsrLinearToMeasure(c), float3(1, 1, 1) / 3.0), h.r * h.r);
        gGB[i] = pack2(h.g, 0);  // (the gradient's code as stored: g x 127 / 255 + 127 / 255)
    }
    GroupMemoryBarrierWithGroupSync();

    // 1, 2: mutual annihilation of input and history (each clamped into the other's 3 x 3 range, twice)
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 1)) continue;
        float2 lo = 1, hi = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const float2 v = unpack2(gLH[cellIndex(c + int2(k % 3, k / 3) - 1)]);
            lo = min(lo, v);
            hi = max(hi, v);
        }
        const float2 own = unpack2(gLH[i]);
        gAB[i] = pack2(clamp(own.y, lo.x, hi.x), clamp(own.x, lo.y, hi.y));  // history into the input's range, input into the history's
    }
    GroupMemoryBarrierWithGroupSync();
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 2)) continue;
        float2 lo = 1, hi = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const float2 v = unpack2(gAB[cellIndex(c + int2(k % 3, k / 3) - 1)]);
            lo = min(lo, v);
            hi = max(hi, v);
        }
        const float2 own = unpack2(gLH[i]);
        gCD[i] = pack2(clamp(own.x, lo.x, hi.x), clamp(own.y, lo.y, hi.y));  // the clamped input, the clamped history
    }
    GroupMemoryBarrierWithGroupSync();

    // 3: the filtered signals and the clamped input's range
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 3)) continue;
        float2 blurred = 0;
        float lo = 1, hi = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const int2 o = int2(k % 3, k / 3) - 1;
            const float2 v = unpack2(gCD[cellIndex(c + o)]);
            blurred += v * ((o.x == 0 ? 0.5 : 0.25) * (o.y == 0 ? 0.5 : 0.25));
            lo = min(lo, v.x);
            hi = max(hi, v.x);
        }
        gF[i] = pack2(blurred.x, blurred.y);
        gGB[i] = pack2(unpack2(gGB[i]).x, hi - lo);
    }
    GroupMemoryBarrierWithGroupSync();

    // 4: the clamp box, the energy it removes from the filtered history, the raw rejection
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 4)) continue;
        float lo = 1, hi = 0;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const float v = unpack2(gF[cellIndex(c + int2(k % 3, k / 3) - 1)]).x;
            lo = min(lo, v);
            hi = max(hi, v);
        }
        const float range = unpack2(gGB[i]).y;
        const float clampError = max(q, range * (filteringWeight * 0.25)) + q;
        const float2 filtered = unpack2(gF[i]);
        const float clamped = clamp(filtered.y, lo - clampError, hi + clampError);
        const float delta = max(abs(filtered.x - filtered.y), range * filteringWeight + q * 2.0 * filteringWeight);
        const float energy = abs(clamped - filtered.y);
        gE[i] = pack2(energy, saturate(1.0 - energy / delta));
    }
    GroupMemoryBarrierWithGroupSync();

    // 5: 3 x 3 medians
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 5)) continue;
        float2 lows[3], mids[3], highs[3];
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 a = unpack2(gE[cellIndex(c + int2(x, -1))]), b = unpack2(gE[cellIndex(c + int2(x, 0))]), d = unpack2(gE[cellIndex(c + int2(x, 1))]);
            sort3(a, b, d);
            lows[x + 1] = a;
            mids[x + 1] = b;
            highs[x + 1] = d;
        }
        const float2 m = median3(max(lows[0], max(lows[1], lows[2])), median3(mids[0], mids[1], mids[2]), min(highs[0], min(highs[1], highs[2])));
        gM[i] = pack2(m.x, m.y);
    }
    GroupMemoryBarrierWithGroupSync();

    // 6: the rejection per cell, the history update it would cause, the gradient and whether it flickers
    const bool cut = (P[2].x & 1u) != 0;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 6)) continue;
        float filteredEnergy = 0, clampBlend = 1;
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const float2 v = unpack2(gM[cellIndex(c + int2(k % 3, k / 3) - 1)]);
            filteredEnergy = max(filteredEnergy, v.x);
            clampBlend = min(clampBlend, v.y);
        }
        const float2 filtered = unpack2(gF[i]);
        const float2 gradientRange = unpack2(gGB[i]);
        const float delta = max(abs(filtered.x - filtered.y), gradientRange.y * filteringWeight + q * 2.0 * filteringWeight);
        const float rejection = saturate(1.0 - filteredEnergy / delta);
        const float2 own = unpack2(gLH[i]);
        const float input = own.x, previous = own.y;
        // (the plus-shaped range of the input)
        const float e = unpack2(gLH[cellIndex(c + int2(1, 0))]).x, w = unpack2(gLH[cellIndex(c + int2(-1, 0))]).x;
        const float s = unpack2(gLH[cellIndex(c + int2(0, 1))]).x, n = unpack2(gLH[cellIndex(c + int2(0, -1))]).x;
        const float clampedPrevious = clamp(previous, min(input, min(min(e, w), min(s, n))), max(input, max(max(e, w), max(s, n))));
        const float finalPrevious = lerp(clampedPrevious, previous, clampBlend);
        const float updated = lerp(finalPrevious, input, max(1.0 - rejection, MIN_BLEND));
        const float ghosting = lerp(previous, input, MIN_BLEND);
        const float gradient = updated - ghosting;
        const float previousGradient = gradientRange.x * (255.0 / 127.0) - 1.0;
        const bool sameSign = gradient * previousGradient > 0;
        const bool withinError = abs(gradient) < ENCODING_ERROR || abs(previousGradient) < ENCODING_ERROR;
        const float flicker = sameSign || withinError || cut ? 0.0 : 1.0;
        gAB[i] = pack2(min(abs(previousGradient), abs(gradient)) * flicker, flicker);
        gCD[i] = pack2(updated, gradient * 0.5 + 0.5);
    }
    GroupMemoryBarrierWithGroupSync();

    // 7: this thread's pixel
    const int2 pixel = int2(group) * TILE + int2(local);
    if (any(pixel >= size)) return;
    const int2 cell = int2(local) + BORDER;
    float2 contribution = 0;  // the gradient variation and the flicker, dilated over 5 x 5
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x) contribution = max(contribution, unpack2(gAB[cellIndex(cell + int2(x, y))]));
    const uint ci = cellIndex(cell);
    const float2 state = unpack2(gCD[ci]);
    const float updated = state.x, gradient = state.y * 2.0 - 1.0;
    const float flicker = unpack2(gAB[ci]).y;
    const float4 history = historyTexture.Load(int3(pixel, 0));
    Texture2D<float2> decimateMask = ResourceDescriptorHeap[P[0].z];
    Texture2D<float4> info = ResourceDescriptorHeap[P[0].w];
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
    // (the flicker's period in frames: 2, longer below 60 Hz; the count fades the error in past half of 1 - 0.95^period)
    const float period = 2.0 / max(g_deltaTime * 60.0, 1.0);
    const float fadeIn = saturate(count * (1.0 - pow(1.0 - MIN_BLEND, period)) - 0.5);
    const float error = count > 0 ? (abs(variation / count) + count * ENCODING_ERROR) * fadeIn : 0.0;
    RWTexture2D<float> errorOut = ResourceDescriptorHeap[P[1].x];
    errorOut[pixel] = saturate(error * still);
    RWTexture2D<float4> historyOut = ResourceDescriptorHeap[P[1].y];
    historyOut[pixel] = float4(sqrt(saturate(updated)), saturate(clamp(newGradient, -1.0, 1.0) * (127.0 / 255.0) + 127.0 / 255.0), saturate(variation),
                               saturate(quantizedCount / MAX_COUNT));
}
