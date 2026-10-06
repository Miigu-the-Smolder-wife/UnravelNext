// unx-kernel: cs_6_6 main
// m.tsr.aa (Tsr.hlsli; the reference's TSRSpatialAntiAliasing): the spatial anti-aliaser of the pixels whose history is
// rejected or missing. In the LDR luma: the edge through the pixel (horizontal or vertical, by the neighbourhood's
// variation), the side of the pixel it lies on, then the edge followed both ways - a bilinear tap halfway between the
// pixel's row and the edge's, up to 8 pixels each way, until the luma leaves the band between the two rows on either
// side. Where the edge ends by rising on one side the pixel sits on a stair: its sample moves across the edge by
// 0.5 - (its distance to the stair's end + 0.5) / the stair's length. Also the noise measure the history update
// softens its kernel by.
// P[0] = { AA input SRV (RG8: LDR luma, mask), output UAV (RG8_UINT: encoded offset, noise x 255), width, height }
#include "Passes/Shading/Tsr.hlsli"

#define ITERATIONS 8

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const int2 size = int2(P[0].zw);
    if (any(int2(id) >= size)) return;
    Texture2D<float2> input = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint2> output = ResourceDescriptorHeap[P[0].y];
    const float2 centre = input.Load(int3(id, 0));
    if (!(centre.g > 0.5))
    {
        output[id] = uint2(tsrEncodeAaOffset(float2(0, 0)), 0);
        return;
    }
#define LUMA(x, y) input.Load(int3(clamp(int2(id) + int2(x, y), 0, size - 1), 0)).r
    const float c = centre.r;
    const float n = LUMA(0, -1), s = LUMA(0, 1), e = LUMA(1, 0), w = LUMA(-1, 0);
    const float ne = LUMA(1, -1), nw = LUMA(-1, -1), se = LUMA(1, 1), sw = LUMA(-1, 1);
#undef LUMA
    const float variationHN = abs(0.5 * (ne + nw) - n), variationH = abs(0.5 * (e + w) - c), variationHS = abs(0.5 * (se + sw) - s);
    const float variationVE = abs(0.5 * (ne + se) - e), variationV = abs(0.5 * (n + s) - c), variationVW = abs(0.5 * (nw + sw) - w);
    const float diffN = abs(n - c), diffS = abs(s - c), diffE = abs(e - c), diffW = abs(w - c);
    const bool vertical = (variationHN + variationH + variationHS) > (variationVE + variationV + variationVW);
    const float noise = max(saturate(2.0 * variationH - max(diffE, diffW)), saturate(2.0 * variationV - max(diffN, diffS)));
    const int2 browse = vertical ? int2(0, 1) : int2(1, 0);
    int2 side = 0;
    float edgeLuma;
    if (vertical)
    {
        side.x = diffW > diffE ? -1 : 1;
        edgeLuma = diffW > diffE ? w : e;
    }
    else
    {
        side.y = diffN > diffS ? -1 : 1;
        edgeLuma = diffN > diffS ? n : s;
    }
    const float lumaDelta = abs(edgeLuma - c) * 0.5;
    float offset = 0;
    if (lumaDelta > TSR_AA_MIN_LUMINANCE)
    {
        const float2 texel = 1.0 / float2(size);
        const float2 kernelUv = (float2(id) + 0.5 + float2(side) * 0.5) * texel;
        const float merged = 0.5 * (edgeLuma + c);
        const float lumaMin = merged - 0.25 * abs(edgeLuma - c), lumaMax = merged + 0.25 * abs(edgeLuma - c);
        const float2 uvMin = 0.5 * texel, uvMax = 1.0 - 0.5 * texel;
        bool minP = false, maxP = false, minN = false, maxN = false;
        float lengthP = ITERATIONS, lengthN = ITERATIONS;
        [loop] for (int i = 0; i < ITERATIONS; ++i)
        {
            const float2 step = float2(browse) * texel * (float)(i + 1);
            // Once an edge end is latched its classification cannot change.
            // Preserve the original lengths without fetching past that end.
            if (!minP && !maxP)
            {
                const float sampleP = input.SampleLevel(g_linearClamp, min(kernelUv + step, uvMax), 0).r;
                minP = sampleP < lumaMin;
                maxP = sampleP > lumaMax;
            }
            if (!minN && !maxN)
            {
                const float sampleN = input.SampleLevel(g_linearClamp, max(kernelUv - step, uvMin), 0).r;
                minN = sampleN < lumaMin;
                maxN = sampleN > lumaMax;
            }
            lengthP -= (minP || maxP) ? 1.0 : 0.0;
            lengthN -= (minN || maxN) ? 1.0 : 0.0;
            if ((minP || maxP) && (minN || maxN))
            {
                lengthP -= float(ITERATIONS - 1 - i);
                lengthN -= float(ITERATIONS - 1 - i);
                break;
            }
        }
        const bool brighter = edgeLuma > c;
        const bool incrementP = brighter ? maxP : minP, incrementN = brighter ? maxN : minN;
        const bool decrementP = brighter ? minP : maxP, decrementN = brighter ? minN : maxN;
        const float inverseLength = 1.0 / (1.0 + lengthN + lengthP);
        if (!incrementN && !incrementP && !decrementP && !decrementN) offset = 0;          // no aliasing around
        else if (incrementN && incrementP) offset = 0.5;                                   // a concave detail
        else if (decrementN && decrementP) offset = 0;                                     // a convex detail
        else if (incrementN) offset = 0.5 - (lengthN + 0.5) * inverseLength;               // a stair from - to +
        else if (incrementP) offset = 0.5 - (lengthP + 0.5) * inverseLength;               // a stair from + to -
        offset *= saturate(lumaDelta / TSR_AA_MIN_LUMINANCE);
    }
    output[id] = uint2(tsrEncodeAaOffset(float2(side) * offset), (uint)round(noise * 255.0));
}
