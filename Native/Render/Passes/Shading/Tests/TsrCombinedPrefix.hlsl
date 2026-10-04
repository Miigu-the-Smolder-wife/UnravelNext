// unx-kernel: cs_6_6 main
// Test-only shared RGB/luma stages 0-2. P0={colour,guide,rgbOut,width}, P1={height,flickerHistory,lumaOut,0}.
// RGB overscan 5; luma overscan 9 supports combined tail moire halo. No intermediate edge clamp.
#include "Passes/Shading/Tsr.hlsli"
#define TILE 16
#define SIDE 20
#define CELLS 400
groupshared uint gA[CELLS], gB[CELLS], gC[CELLS], gD[CELLS];
groupshared uint gLH[CELLS], gAB[CELLS], gGB[CELLS];
groupshared uint gAlias[TILE * TILE];
#include "Passes/Shading/TsrRejectCodes.hlsli"
uint pack2(float a, float b) { return (uint)(saturate(a) * 65535.0 + 0.5) | ((uint)(saturate(b) * 65535.0 + 0.5) << 16); }
float2 unpack2(uint v) { return float2(v & 0xFFFFu, v >> 16) * (1.0 / 65535.0); }
uint2 codes2(uint v) { return uint2(v & 0xFFFFu, v >> 16); }
uint packCodes2(uint2 v) { return v.x | (v.y << 16); }
uint indexOf(int2 c) { return uint(c.y * SIDE + c.x); }

[numthreads(TILE, TILE, 1)]
void main(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const int2 size = int2(P[0].w, P[1].x);
    const int2 origin = int2(group) * TILE - 11;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> guide = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> history = ResourceDescriptorHeap[P[1].y];
    uint i;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 pixel = clamp(origin + int2(i % SIDE, i / SIDE), 0, size - 1);
        float3 value = colour.Load(int3(pixel, 0)).rgb;
        value = all(isfinite(value)) ? max(value, 0.0) : float3(0, 0, 0);
        const float3 measured = tsrLinearToMeasure(value);
        gA[i] = pack(measured);
        const float4 h = history.Load(int3(pixel, 0));
        gLH[i] = pack2(dot(measured, float3(1, 1, 1) / 3.0), h.r * h.r);
        gGB[i] = pack2(h.g, 0);
        gB[i] = pack(tsrGuideToMeasure(guide.Load(int3(pixel, 0)).rgb));
    }
    GroupMemoryBarrierWithGroupSync();
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (any(c < 1) || any(c >= SIDE - 1)) continue;
        uint2 lumaLo = 0xFFFFu, lumaHi = 0;
        uint3 inputLo = uint3(2047, 2047, 1023), inputHi = 0, guideLo = inputLo, guideHi = 0;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const uint ni = indexOf(c + int2(x, y));
            const uint2 luma = codes2(gLH[ni]);
            lumaLo = min(lumaLo, luma); lumaHi = max(lumaHi, luma);
            const uint3 a = colourCodes(gA[ni]), b = colourCodes(gB[ni]);
            inputLo = min(inputLo, a); inputHi = max(inputHi, a);
            guideLo = min(guideLo, b); guideHi = max(guideHi, b);
        }
        gAB[i] = packCodes2(clamp(codes2(gLH[i]).yx, lumaLo, lumaHi));
        gC[i] = packCodes(clamp(colourCodes(gB[i]), inputLo, inputHi));
        gD[i] = packCodes(clamp(colourCodes(gA[i]), guideLo, guideHi));
        if (all(c >= 2) && all(c < TILE + 2))
        {
            const float3 lo = unpack(packCodes(inputLo)), hi = unpack(packCodes(inputHi));
            gAlias[(c.y - 2) * TILE + c.x - 2] = dot(hi - lo, float3(0.299, 0.587, 0.114)) > TSR_AA_MIN_LUMINANCE ? 1u : 0u;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 outputPixel = group * TILE + local;
    if (any(outputPixel >= uint2(size + 18))) return;
    const int2 c = int2(local) + 2;
    uint2 lumaLo = 0xFFFFu, lumaHi = 0;
    uint3 aMin = uint3(2047, 2047, 1023), aMax = 0, bMin = aMin, bMax = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const uint ni = indexOf(c + int2(x, y));
        const uint2 luma = codes2(gAB[ni]);
        lumaLo = min(lumaLo, luma); lumaHi = max(lumaHi, luma);
        const uint3 a = colourCodes(gC[ni]), b = colourCodes(gD[ni]);
        aMin = min(aMin, a); aMax = max(aMax, a);
        bMin = min(bMin, b); bMax = max(bMax, b);
    }
    const uint own = indexOf(c);
    RWTexture2D<uint4> lumaOut = ResourceDescriptorHeap[P[1].z];
    lumaOut[outputPixel] = uint4(gLH[own], packCodes2(clamp(codes2(gLH[own]), lumaLo, lumaHi)), gGB[own], 0);
    if (any(outputPixel < 4) || any(outputPixel >= uint2(size + 14))) return;
    RWTexture2D<uint4> output = ResourceDescriptorHeap[P[0].z];
    output[outputPixel - 4] = uint4(packCodes(clamp(colourCodes(gA[own]), aMin, aMax)), packCodes(clamp(colourCodes(gB[own]), bMin, bMax)),
        gA[own], gAlias[local.y * TILE + local.x]);
}
