// unx-kernel: cs_6_6 main
// Stages0–2 once per pixel. Each group produces16x16 pixels using a20x20
// original-input region and18x18 stage1 region. Output includes five pixels
// of real overscan; together with this kernel's two-pixel halo it preserves
// the original seven-pixel support. Intermediate stages never clamp edges.
// P0: input colour SRV, guide SRV, packed UINT4 output UAV, image width
// P1.x: image height. Output = {clamped input, clamped guide, original input, alias}.
#include "Passes/Shading/Tsr.hlsli"
#define TILE 16
#define SIDE 20
#define CELLS 400
groupshared uint gA[CELLS], gB[CELLS], gC[CELLS], gD[CELLS];
groupshared uint gAlias[TILE * TILE];
#include "Passes/Shading/TsrRejectCodes.hlsli"
uint indexOf(int2 c) { return uint(c.y * SIDE + c.x); }

[numthreads(TILE, TILE, 1)]
void main(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID, uint lane : SV_GroupIndex)
{
    const int2 size = int2(P[0].w, P[1].x);
    const int2 origin = int2(group) * TILE - 7;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> guide = ResourceDescriptorHeap[P[0].y];
    uint i;
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 pixel = clamp(origin + int2(i % SIDE, i / SIDE), 0, size - 1);
        float3 value = colour.Load(int3(pixel, 0)).rgb;
        value = all(isfinite(value)) ? max(value, 0.0) : float3(0, 0, 0);
        gA[i] = pack(tsrLinearToMeasure(value));
        gB[i] = pack(tsrGuideToMeasure(guide.Load(int3(pixel, 0)).rgb));
    }
    GroupMemoryBarrierWithGroupSync();
    for (i = lane; i < CELLS; i += TILE * TILE)
    {
        const int2 c = int2(i % SIDE, i / SIDE);
        if (any(c < 1) || any(c >= SIDE - 1)) continue;
        uint3 inputLo = uint3(2047, 2047, 1023), inputHi = 0, guideLo = inputLo, guideHi = 0;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const uint ni = indexOf(c + int2(x, y));
            const uint3 a = colourCodes(gA[ni]), b = colourCodes(gB[ni]);
            inputLo = min(inputLo, a); inputHi = max(inputHi, a);
            guideLo = min(guideLo, b); guideHi = max(guideHi, b);
        }
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
    if (any(outputPixel >= uint2(size + 10))) return;
    const int2 c = int2(local) + 2;
    uint3 aMin = uint3(2047, 2047, 1023), aMax = 0, bMin = aMin, bMax = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const uint ni = indexOf(c + int2(x, y));
        const uint3 a = colourCodes(gC[ni]), b = colourCodes(gD[ni]);
        aMin = min(aMin, a); aMax = max(aMax, a);
        bMin = min(bMin, b); bMax = max(bMax, b);
    }
    const uint own = indexOf(c);
    RWTexture2D<uint4> output = ResourceDescriptorHeap[P[0].z];
    output[outputPixel] = uint4(packCodes(clamp(colourCodes(gA[own]), aMin, aMax)), packCodes(clamp(colourCodes(gB[own]), bMin, bMax)),
        gA[own], gAlias[local.y * TILE + local.x]);
}
