// unx-kernel: cs_6_6 main
// m.tsr.resurrect (Tsr.hlsli; the reference's history resurrection in TSRRejectShading): what a pixel showed a few
// frames ago is often what it shows again - a door closes, a character walks past a wall, the camera turns back. The
// history's ring keeps such a frame (Upscale.cpp); m.tsr.decimate reprojected its guide by the cameras alone. Here that
// guide is measured against this frame's input exactly as m.tsr.reject measures the previous frame's (the same chain of
// 3 x 3 operators over a 28 x 28 region in group memory, without the flickering and thin geometry terms), and each
// pixel is marked where the kept frame is the closer one: the sum over 3 x 3 and the channels of |input - previous
// guide| - |input - kept guide| above 0.05 a sample, for more than 4 of the pixel's 3 x 3. m.tsr.reject takes the kept
// frame for a marked pixel where its rejection is better than the previous frame's by 0.1.
// A tile with no marked pixel stops after the mark (most tiles: the two guides agree wherever the history holds).
// P[0] = { colour SRV (internal, exposed linear), reprojected guide SRV (R10G10B10A2, the previous frame's),
//          resurrected guide SRV (R10G10B10A2, the kept frame's), decimate mask SRV (RG8) }
// P[1] = { measure UAV (RGBA8: r = the kept frame's rejection, g = its history clamp disable, b = 1: the kept frame is
//          the closer one, a = 0), width, height, 0 }
#include "Passes/Shading/Tsr.hlsli"

#define TILE 16
#define BORDER 6
#define SIDE (TILE + 2 * BORDER)
#define CELLS (SIDE * SIDE)

groupshared uint gA[CELLS];
groupshared uint gB[CELLS];
groupshared uint gC[CELLS];
groupshared uint gD[CELLS];
groupshared uint gE[CELLS];
groupshared uint gF[CELLS];
groupshared uint gG[CELLS];
groupshared float gMatch[CELLS];  // |input - previous guide| - |input - kept guide|, the channels' sum
groupshared uint gCloser[CELLS];  // its 3 x 3 sum is above the threshold
groupshared uint gAnyCloser;

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
    const int2 size = int2(P[1].yz);
    const int2 origin = int2(group) * TILE - BORDER;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> previousGuide = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> keptGuide = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> measure = ResourceDescriptorHeap[P[1].x];
    const int2 pixel = int2(group) * TILE + int2(local);
    const int2 cell = int2(local) + BORDER;
    uint i;

    // 0: the region's input and kept guide in the measurement space; which of the two guides the input is nearer
    if (lane == 0) gAnyCloser = 0;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 p = clamp(origin + int2(i % SIDE, i / SIDE), 0, size - 1);
        float3 c = colour.Load(int3(p, 0)).rgb;
        c = all(isfinite(c)) ? max(c, 0.0) : float3(0, 0, 0);
        const float3 input = tsrLinearToMeasure(c);
        const float3 kept = tsrGuideToMeasure(keptGuide.Load(int3(p, 0)).rgb);
        const float3 previous = tsrGuideToMeasure(previousGuide.Load(int3(p, 0)).rgb);
        gA[i] = pack(input);
        gB[i] = pack(kept);
        gMatch[i] = dot(abs(input - previous) - abs(input - kept), float3(1, 1, 1));
    }
    GroupMemoryBarrierWithGroupSync();

    // 1: each clamped to the other's 3 x 3 range; the match's 3 x 3 sum
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 1)) continue;
        float3 inputMin = 1, inputMax = 0, guideMin = 1, guideMax = 0;
        float match = 0;
        FOR_3X3(c, {
            const float3 a = unpack(gA[ni]);
            const float3 b = unpack(gB[ni]);
            inputMin = min(inputMin, a);
            inputMax = max(inputMax, a);
            guideMin = min(guideMin, b);
            guideMax = max(guideMax, b);
            match += gMatch[ni];
        })
        gC[i] = pack(clamp(unpack(gB[i]), inputMin, inputMax));
        gD[i] = pack(clamp(unpack(gA[i]), guideMin, guideMax));
        gCloser[i] = match > 0.05 * 3.0 * 9.0 ? 1u : 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    // this thread's pixel: the kept frame is the closer one for most of its 3 x 3 and lies on the kept frame's screen
    bool closer = false;
    if (all(pixel < size))
    {
        uint count = 0;
        FOR_3X3(cell, { count += gCloser[ni]; })
        Texture2D<float2> decimateMask = ResourceDescriptorHeap[P[0].w];
        const bool offKept = ((uint)round(decimateMask.Load(int3(pixel, 0)).r * 255.0) & 16u) != 0;
        closer = count > 4u && !offKept;
        if (closer) InterlockedOr(gAnyCloser, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gAnyCloser == 0)
    {
        if (all(pixel < size)) measure[pixel] = float4(0, 0, 0, 0);
        return;
    }

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

    // 4: the clamp box, what it removes from the filtered guide, the raw rejection
    const float q = TSR_QUANTIZATION_ERROR, filteringWeight = 0.25;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 4)) continue;
        float3 blurredVariation = 0, boxMin = 1, boxMax = 0;
        FOR_3X3(c, {
            blurredVariation += unpack(gB[ni]) * nw;
            const float3 f = unpack(gC[ni]);
            boxMin = min(boxMin, f);
            boxMax = max(boxMax, f);
        })
        const float3 range = unpack(gG[i]);
        const float3 clampError = max(max(float3(q, q, q), blurredVariation), range * (filteringWeight * 0.25)) + q;
        const float3 filteredGuide = unpack(gD[i]), filteredInput = unpack(gC[i]);
        const float3 boxSize = range * filteringWeight + q * 2.0 * filteringWeight;
        const float3 energy = abs(clamp(filteredGuide, boxMin - clampError, boxMax + clampError) - filteredGuide);
        const float3 delta = max(abs(filteredInput - filteredGuide), boxSize);
        gE[i] = pack(energy);
        gF[i] = asuint(min3(saturate(1.0 - energy / delta)));
    }
    GroupMemoryBarrierWithGroupSync();

    // 5: 3 x 3 medians (columns sorted, then max of the lows, median of the mids, min of the highs, their median)
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (!inMargin(c, 5)) continue;
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
    if (any(pixel >= size)) return;
    float3 filteredEnergy = 0;
    float clampBlend = 1;
    FOR_3X3(cell, {
        filteredEnergy = max(filteredEnergy, unpack(gA[ni]));
        clampBlend = min(clampBlend, asfloat(gB[ni]));
    })
    const uint ci = cellIndex(cell);
    const float3 boxSize = unpack(gG[ci]) * filteringWeight + q * 2.0 * filteringWeight;
    const float3 delta = max(abs(unpack(gC[ci]) - unpack(gD[ci])), boxSize);
    const float rejection = min3(saturate(1.0 - filteredEnergy / delta));
    // (the stores round down, as m.tsr.reject's)
    measure[pixel] = float4(floor(float2(rejection, clampBlend) * 255.0) / 255.0, closer ? 1.0 : 0.0, 0);
}
