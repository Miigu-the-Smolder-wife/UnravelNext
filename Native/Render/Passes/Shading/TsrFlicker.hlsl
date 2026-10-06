// unx-kernel: cs_6_6 main
// m.tsr.flicker (Tsr.hlsli; the reference's flickering temporal analysis, TSRShadingAnalysis.ush ComputeMoireError): a
// pattern finer than the pixel grid (tile joints, gratings, distant detail) changes its samples with every jitter
// offset; the shading rejection would take that for a change of shading and drop the history each frame - the image
// flickers at the jitter's rate. Here each pixel's luma is followed over time: the rejection's own measure on the luma
// alone, the history update it would cause, and the gradient that leaves between the updated history and a history that
// just ghosts (5 % a frame). A gradient whose sign flips against the last frame's is a flicker; the smaller of the two
// gradients' sizes is its amplitude. Their running sum and count (20 frames, x 0.95 a frame; only on what stands
// still) give the moire error: the luma band inside which m.tsr.reject lets the history ghost.
// Thin geometry (P[2].y: TsrThin.hlsl's relaxation weight w; the reference's r.TSR.ThinGeometryDetection.AntiFlickering):
// where m.tsr.reject relaxes its clamp box by w, this measure does the same on the luma - the box between the filtered
// input's range and the filtered history's by w, the history clamped into it, and where w > 0 that result's 3 x 3 range
// as the box - so the gradient followed here is the one the relaxed rejection leaves. And thin geometry still flickers
// under the relaxation with gradients too small for the history's 8 bits: where w > 0 a gradient under the encoding
// error counts (as long as either frame's is above 7e-5), and the error fades in at the count a flicker of three times
// the period settles at (a slower flicker is taken for one).
// One group per 16 x 16 internal pixels over a 34 x 34 region in group memory (the 3 x 3 chain reaches 7 pixels, the
// 5 x 5 dilation of the flicker 2 more), values as 16-bit pairs.
// P[0] = { colour SRV (internal, exposed linear), reprojected flickering history SRV (RGBA8: luma in the guide space,
//          gradient x 127 / 255 + 127 / 255, total variation, count / 20), decimate mask SRV (RG8), info SRV (RGBA16F,
//          TsrDilate.hlsl: a = offset + is-moving / 2) }
// P[1] = { moire error UAV (R16F), flickering history UAV (RGBA8, next frame's), width, height }
// P[2] = { flags (1: reset), thin geometry relaxation SRV (R8; UNX_NONE: none), 0, 0 }
// Frame constants b1 = the main view (the frame's time step).
#include "Passes/Shading/Tsr.hlsli"
#include "Passes/Common/Frame.hlsli"

#include "Passes/Shading/TsrTiles.h"
#ifndef TSR_TILE
#define TSR_TILE UNX_TSR_FLICKER_TILE
#endif
#define TILE TSR_TILE
#define BORDER 9
#define THIN_GRADIENT 0.00007  // (the reference's ValidGeometryGradientThreshold)
#define SIDE (TILE + 2 * BORDER)
#define CELLS (SIDE * SIDE)
#define MIN_BLEND 0.05
#define ENCODING_ERROR (1.0 / 127.0)
#define MAX_COUNT 20.0
#ifndef TSR_WAVE_RANGES
#define TSR_WAVE_RANGES 0
#endif
#ifndef TSR_PACKED_RANGES
// Keep stage 4's float clamp box: reassociating that decode changes rare
// history quantization thresholds. Stages 3b, 5 and 6 retain exact code values.
#define TSR_PACKED_RANGES 13
#endif

// Stage domains shrink with the filter halo. Reuse the original input storage
// for ranges/energy, then refill it for the final update; no quantization changes.
// At TILE=16 the four buffers occupy 15,456 bytes instead of 27,744 bytes.
groupshared uint gLH[CELLS];
groupshared uint gAB[(SIDE - 2) * (SIDE - 2)];
groupshared uint gCD[(SIDE - 4) * (SIDE - 4)];
groupshared uint gF[(SIDE - 6) * (SIDE - 6)];
uint compactCell(uint i, uint margin)
{
    return (i / SIDE - margin) * (SIDE - 2 * margin) + (i % SIDE - margin);
}
#define AB(i) gAB[compactCell(i, 1)]
#define CD(i) gCD[compactCell(i, 2)]
#define FILTERED(i) gF[compactCell(i, 3)]
// Neighbourhood loops already hold two-dimensional coordinates. Address the
// compact tiles directly instead of flattening to SIDE and dividing by SIDE
// again for every tap. Margins ensure the coordinates stay in the same row.
#define AB_AT(c) gAB[((c).y - 1) * (SIDE - 2) + (c).x - 1]
#define CD_AT(c) gCD[((c).y - 2) * (SIDE - 4) + (c).x - 2]
#define FILTERED_AT(c) gF[((c).y - 3) * (SIDE - 6) + (c).x - 3]

uint pack2(float a, float b) { return (uint)(saturate(a) * 65535.0 + 0.5) | ((uint)(saturate(b) * 65535.0 + 0.5) << 16); }
float2 unpack2(uint v) { return float2(v & 0xFFFFu, v >> 16) * (1.0 / 65535.0); }
uint2 codes2(uint v) { return uint2(v & 0xFFFFu, v >> 16); }
uint packCodes2(uint2 v) { return v.x | (v.y << 16); }
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
void sortCodes3(inout uint2 a, inout uint2 b, inout uint2 c)
{
    const uint2 lo=min(a,min(b,c)), hi=max(a,max(b,c)), mid=a+b+c-lo-hi;
    a=lo;b=mid;c=hi;
}
uint2 medianCodes3(uint2 a,uint2 b,uint2 c){return max(min(a,b),min(max(a,b),c));}

[numthreads(TILE, TILE, 1)]
void main(uint2 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint2 local = uint2(lane % TILE, lane / TILE);
#if TSR_WAVE_RANGES
    // Keep the device's texture-friendly 2D quad layout. Resolve neighbours once,
    // before any lane leaves the halo loops. Check logical thread identities so
    // linear, quad and unsupported wave layouts all retain exact scalar results.
    const uint wl = WaveGetLaneIndex(), wc = WaveGetLaneCount();
    const uint linearLeft = max(wl, 1u) - 1, linearRight = min(wl + 1, wc - 1);
    const uint quadLeft = (wl & 1u) ? wl - 1 : (wl >= 3 ? wl - 3 : wl);
    const uint quadRight = min((wl & 1u) ? wl + 3 : wl + 1, wc - 1);
    const bool linearOrder = WaveReadLaneAt(lane, linearLeft) + 1 == lane && WaveReadLaneAt(lane, linearRight) == lane + 1;
    const bool quad = WaveReadLaneAt(lane, quadLeft) + 1 == lane && WaveReadLaneAt(lane, quadRight) == lane + 1;
    const bool shareRange = lane > 0 && lane + 1 < TILE * TILE && (linearOrder || quad);
    const uint leftLane = linearOrder ? linearLeft : quadLeft, rightLane = linearOrder ? linearRight : quadRight;
#endif
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
    }
    GroupMemoryBarrierWithGroupSync();

    // 1, 2: mutual annihilation of input and history (each clamped into the other's 3 x 3 range, twice)
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        uint2 lo = 0xFFFFu, hi = 0;
#if TSR_WAVE_RANGES
        // Three vertical LDS reads per lane; adjacent columns exchange their
        // exact integer extrema in the wave. All lanes participate before the
        // halo predicate. Wave edges retain the scalar path, including partial
        // waves and the non-power-of-two tile pitch.
        const uint2 a = codes2(gLH[cellIndex(int2(c.x, max(c.y - 1, 0)))]);
        const uint2 b = codes2(gLH[i]);
        const uint2 d = codes2(gLH[cellIndex(int2(c.x, min(c.y + 1, SIDE - 1)))]);
        const uint2 vl = min(a, min(b, d)), vh = max(a, max(b, d));
        const uint2 ll = WaveReadLaneAt(vl, leftLane), rl = WaveReadLaneAt(vl, rightLane);
        const uint2 lh = WaveReadLaneAt(vh, leftLane), rh = WaveReadLaneAt(vh, rightLane);
        if (!inMargin(c, 1)) continue;
        if (shareRange) { lo = min(vl, min(ll, rl)); hi = max(vh, max(lh, rh)); }
        else
#else
        if (!inMargin(c, 1)) continue;
#endif
        {
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const uint2 v = codes2(gLH[cellIndex(c + int2(k % 3, k / 3) - 1)]);
            lo = min(lo, v);
            hi = max(hi, v);
        }
        }
        const uint2 own = codes2(gLH[i]);
        AB(i) = packCodes2(clamp(own.yx, lo, hi));  // history into the input's range, input into the history's
    }
    GroupMemoryBarrierWithGroupSync();
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        uint2 lo = 0xFFFFu, hi = 0;
#if TSR_WAVE_RANGES
        const int2 q = clamp(c, 1, SIDE - 2);
        const uint2 a = codes2(AB_AT(int2(q.x, max(q.y - 1, 1))));
        const uint2 b = codes2(AB_AT(q));
        const uint2 d = codes2(AB_AT(int2(q.x, min(q.y + 1, SIDE - 2))));
        const uint2 vl = min(a, min(b, d)), vh = max(a, max(b, d));
        const uint2 ll = WaveReadLaneAt(vl, leftLane), rl = WaveReadLaneAt(vl, rightLane);
        const uint2 lh = WaveReadLaneAt(vh, leftLane), rh = WaveReadLaneAt(vh, rightLane);
        if (!inMargin(c, 2)) continue;
        if (shareRange) { lo = min(vl, min(ll, rl)); hi = max(vh, max(lh, rh)); }
        else
#else
        if (!inMargin(c, 2)) continue;
#endif
        {
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const uint2 v = codes2(AB_AT(c + int2(k % 3, k / 3) - 1));
            lo = min(lo, v);
            hi = max(hi, v);
        }
        }
        const uint2 own = codes2(gLH[i]);
        CD(i) = packCodes2(clamp(own, lo, hi));  // the clamped input, the clamped history
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
            const float2 v = unpack2(CD_AT(c + o));
            blurred += v * ((o.x == 0 ? 0.5 : 0.25) * (o.y == 0 ? 0.5 : 0.25));
            lo = min(lo, v.x);
            hi = max(hi, v.x);
        }
        FILTERED(i) = pack2(blurred.x, blurred.y);
        gLH[i] = pack2(0, hi - lo);  // original inputs are no longer read until stage 6
    }
    GroupMemoryBarrierWithGroupSync();

    // 3b: the filtered history clamped into the box relaxed by the thin geometry's weight (gAB is free until stage 6)
    const bool thin = P[2].y != UNX_NONE;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 4)) continue;
        #if (TSR_PACKED_RANGES & 1)
        uint2 lowCode=0xFFFFu, highCode=0;
        #else
        float2 lo = 1, hi = 0;
        #endif
        [unroll] for (int k = 0; k < 9; ++k)
        {
            #if (TSR_PACKED_RANGES & 1)
            const uint2 v=codes2(FILTERED_AT(c+int2(k%3,k/3)-1));
            lowCode=min(lowCode,v);highCode=max(highCode,v);
            #else
            const float2 v = unpack2(FILTERED_AT(c + int2(k % 3, k / 3) - 1));
            lo = min(lo, v);
            hi = max(hi, v);
            #endif
        }
        #if (TSR_PACKED_RANGES & 1)
        const float2 lo=float2(lowCode)*(1.0/65535.0), hi=float2(highCode)*(1.0/65535.0);
        #endif
        float weight = 0;
        if (thin)
        {
            Texture2D<float> relaxationTexture = ResourceDescriptorHeap[P[2].y];
            weight = relaxationTexture.Load(int3(clamp(origin + c, 0, size - 1), 0));
            weight = weight > 1.0 / 127.0 ? weight : 0.0;
        }
        AB(i) = pack2(clamp(unpack2(FILTERED(i)).y, lerp(lo.x, lo.y, weight), lerp(hi.x, hi.y, weight)), unpack2(gLH[i]).y);
    }
    GroupMemoryBarrierWithGroupSync();

    // 4: the clamp box, the energy it removes from the filtered history, the raw rejection
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 5)) continue;
        bool relaxed = false;
        if (thin)
        {
            Texture2D<float> relaxationTexture = ResourceDescriptorHeap[P[2].y];
            relaxed = relaxationTexture.Load(int3(clamp(origin + c, 0, size - 1), 0)) > 1.0 / 127.0;
        }
        #if (TSR_PACKED_RANGES & 2)
        uint lowCode=0xFFFFu, highCode=0;
        #else
        float lo = 1, hi = 0;
        #endif
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const uint ni = cellIndex(c + int2(k % 3, k / 3) - 1);
            #if (TSR_PACKED_RANGES & 2)
            const uint v=(relaxed?AB(ni):FILTERED(ni))&0xFFFFu;
            lowCode=min(lowCode,v);highCode=max(highCode,v);
            #else
            const float v = relaxed ? unpack2(AB(ni)).x : unpack2(FILTERED(ni)).x;
            lo = min(lo, v);
            hi = max(hi, v);
            #endif
        }
        #if (TSR_PACKED_RANGES & 2)
        const float lo=float(lowCode)*(1.0/65535.0), hi=float(highCode)*(1.0/65535.0);
        #endif
        const float range = unpack2(AB(i)).y;
        const float clampError = max(q, range * (filteringWeight * 0.25)) + q;
        const float2 filtered = unpack2(FILTERED(i));
        const float clamped = clamp(filtered.y, lo - clampError, hi + clampError);
        const float delta = max(abs(filtered.x - filtered.y), range * filteringWeight + q * 2.0 * filteringWeight);
        const float energy = abs(clamped - filtered.y);
        gLH[i] = pack2(energy, saturate(1.0 - energy / delta));
    }
    GroupMemoryBarrierWithGroupSync();

    // 5: 3 x 3 medians
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 6)) continue;
        #if (TSR_PACKED_RANGES & 4)
        uint2 lows[3], mids[3], highs[3];
        #else
        float2 lows[3], mids[3], highs[3];
        #endif
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            #if (TSR_PACKED_RANGES & 4)
            uint2 a=codes2(gLH[cellIndex(c+int2(x,-1))]),b=codes2(gLH[cellIndex(c+int2(x,0))]),d=codes2(gLH[cellIndex(c+int2(x,1))]);
            sortCodes3(a,b,d);
            #else
            float2 a = unpack2(gLH[cellIndex(c + int2(x, -1))]), b = unpack2(gLH[cellIndex(c + int2(x, 0))]), d = unpack2(gLH[cellIndex(c + int2(x, 1))]);
            sort3(a, b, d);
            #endif
            lows[x + 1] = a;
            mids[x + 1] = b;
            highs[x + 1] = d;
        }
        #if (TSR_PACKED_RANGES & 4)
        const uint2 m=medianCodes3(max(lows[0],max(lows[1],lows[2])),medianCodes3(mids[0],mids[1],mids[2]),min(highs[0],min(highs[1],highs[2])));
        CD(i)=packCodes2(m);
        #else
        const float2 m = median3(max(lows[0], max(lows[1], lows[2])), median3(mids[0], mids[1], mids[2]), min(highs[0], min(highs[1], highs[2])));
        // Stage 3 consumed the clamps; its barrier precedes this reuse.
        CD(i) = pack2(m.x, m.y);
        #endif
    }
    GroupMemoryBarrierWithGroupSync();

    // The median pass has finished reading energy. Refill the original inputs
    // cooperatively so the plus-shaped range has the same border/rounding as 0.
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 6)) continue;
        const int2 p = clamp(origin + c, 0, size - 1);
        float3 value = colour.Load(int3(p, 0)).rgb;
        value = all(isfinite(value)) ? max(value, 0.0) : float3(0, 0, 0);
        const float4 history = historyTexture.Load(int3(p, 0));
        gLH[i] = pack2(dot(tsrLinearToMeasure(value), float3(1, 1, 1) / 3.0), history.r * history.r);
    }
    GroupMemoryBarrierWithGroupSync();

    // 6: the rejection per cell, the history update it would cause, the gradient and whether it flickers
    const bool cut = (P[2].x & 1u) != 0;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 7)) continue;
        bool relaxed = false;
        if (thin)
        {
            Texture2D<float> relaxationTexture = ResourceDescriptorHeap[P[2].y];
            relaxed = relaxationTexture.Load(int3(clamp(origin + c, 0, size - 1), 0)) > 1.0 / 127.0;
        }
        #if (TSR_PACKED_RANGES & 8)
        uint energyCode=0, blendCode=0xFFFFu;
        #else
        float filteredEnergy = 0, clampBlend = 1;
        #endif
        [unroll] for (int k = 0; k < 9; ++k)
        {
            #if (TSR_PACKED_RANGES & 8)
            const uint2 v=codes2(CD_AT(c+int2(k%3,k/3)-1));
            energyCode=max(energyCode,v.x);blendCode=min(blendCode,v.y);
            #else
            const float2 v = unpack2(CD_AT(c + int2(k % 3, k / 3) - 1));
            filteredEnergy = max(filteredEnergy, v.x);
            clampBlend = min(clampBlend, v.y);
            #endif
        }
        #if (TSR_PACKED_RANGES & 8)
        const float filteredEnergy=float(energyCode)*(1.0/65535.0), clampBlend=float(blendCode)*(1.0/65535.0);
        #endif
        const float2 filtered = unpack2(FILTERED(i));
        const float oldGradient = historyTexture.Load(int3(clamp(origin + c, 0, size - 1), 0)).g;
        const float2 gradientRange = float2(unpack2(pack2(oldGradient, 0)).x, unpack2(AB(i)).y);
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
        bool withinError = abs(gradient) < ENCODING_ERROR || abs(previousGradient) < ENCODING_ERROR;
        // (thin geometry: its residual flicker is that small)
        if (relaxed && (abs(gradient) > THIN_GRADIENT || abs(previousGradient) > THIN_GRADIENT)) withinError = false;
        const float flicker = sameSign || withinError || cut ? 0.0 : 1.0;
        AB(i) = pack2(min(abs(previousGradient), abs(gradient)) * flicker, flicker);
        // Stage 5 consumed raw energy/rejection. Keep medians in gCD intact
        // while adjacent cells still read them in this stage.
        FILTERED(i) = pack2(updated, gradient * 0.5 + 0.5);
    }
    GroupMemoryBarrierWithGroupSync();

    // 7: this thread's pixel
    const int2 pixel = int2(group) * TILE + int2(local);
    if (any(pixel >= size)) return;
    const int2 cell = int2(local) + BORDER;
    float2 contribution = 0;  // the gradient variation and the flicker, dilated over 5 x 5
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x) contribution = max(contribution, unpack2(AB_AT(cell + int2(x, y))));
    const uint ci = cellIndex(cell);
    const float2 state = unpack2(FILTERED(ci));
    const float updated = state.x, gradient = state.y * 2.0 - 1.0;
    const float flicker = unpack2(AB(ci)).y;
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
    // (the flicker's period in frames: 2, longer below 60 Hz; the count fades the error in past half of 1 - 0.95^period;
    // a thin region's small gradients - the stage 6 rule again, at the pixel - at three times the period's count)
    float period = 2.0 / max(g_deltaTime * 60.0, 1.0);
    if (thin)
    {
        Texture2D<float> relaxationTexture = ResourceDescriptorHeap[P[2].y];
        const bool small = abs(gradient) < ENCODING_ERROR || abs(previousGradient) < ENCODING_ERROR;
        const bool present = abs(gradient) > THIN_GRADIENT || abs(previousGradient) > THIN_GRADIENT;
        if (relaxationTexture.Load(int3(pixel, 0)) > 1.0 / 127.0 && small && present) period *= 3.0;
    }
    const float fadeIn = saturate(count * (1.0 - pow(1.0 - MIN_BLEND, period)) - 0.5);
    const float error = count > 0 ? (abs(variation / count) + count * ENCODING_ERROR) * fadeIn : 0.0;
    RWTexture2D<float> errorOut = ResourceDescriptorHeap[P[1].x];
    errorOut[pixel] = saturate(error * still);
    RWTexture2D<float4> historyOut = ResourceDescriptorHeap[P[1].y];
    historyOut[pixel] = float4(sqrt(saturate(updated)), saturate(clamp(newGradient, -1.0, 1.0) * (127.0 / 255.0) + 127.0 / 255.0), saturate(variation),
                               saturate(quantizedCount / MAX_COUNT));
}
