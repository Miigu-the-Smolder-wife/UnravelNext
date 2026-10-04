// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3
// Lossless global early stages of TsrReject. A real seven-pixel overscan is
// propagated between stages; only original input loads clamp to the image.
// P0: colour SRV, guide SRV, output UINT4 UAV, image width
// P1: image height, stage0 UINT4 SRV, preceding-stage UINT4 SRV, unused
#include "Passes/Shading/Tsr.hlsli"
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
uint3 codes(uint v) { return uint3(v & 2047u, (v >> 11) & 2047u, v >> 22); }
uint packedCodes(uint3 v) { return v.x | (v.y << 11) | (v.z << 22); }

[numthreads(8, 8, 1)]
void main(uint2 at : SV_DispatchThreadID)
{
    const uint2 imageSize = uint2(P[0].w, P[1].x), paddedSize = imageSize + 14;
    if (any(at < MODE) || any(at >= paddedSize - MODE)) return;
    RWTexture2D<uint4> output = ResourceDescriptorHeap[P[0].z];
#if MODE == 0
    const int2 pixel = clamp(int2(at) - 7, 0, int2(imageSize) - 1);
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> guide = ResourceDescriptorHeap[P[0].y];
    float3 c = colour.Load(int3(pixel, 0)).rgb;
    c = all(isfinite(c)) ? max(c, 0.0) : float3(0, 0, 0);
    output[at] = uint4(pack(tsrLinearToMeasure(c)), pack(tsrGuideToMeasure(guide.Load(int3(pixel, 0)).rgb)), 0, 0);
#elif MODE == 1
    Texture2D<uint4> input = ResourceDescriptorHeap[P[1].z];
    uint3 inputLo = uint3(2047, 2047, 1023), inputHi = 0, guideLo = inputLo, guideHi = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const uint4 v = input.Load(int3(int2(at) + int2(x, y), 0));
        const uint3 a = codes(v.x), b = codes(v.y);
        inputLo = min(inputLo, a); inputHi = max(inputHi, a);
        guideLo = min(guideLo, b); guideHi = max(guideHi, b);
    }
    const uint4 own = input.Load(int3(at, 0));
    const float3 inputMin = unpack(packedCodes(inputLo)), inputMax = unpack(packedCodes(inputHi));
    const uint alias = dot(inputMax - inputMin, float3(0.299, 0.587, 0.114)) > TSR_AA_MIN_LUMINANCE ? 1u : 0u;
    output[at] = uint4(packedCodes(clamp(codes(own.y), inputLo, inputHi)), packedCodes(clamp(codes(own.x), guideLo, guideHi)), alias, 0);
#elif MODE == 2
    Texture2D<uint4> input = ResourceDescriptorHeap[P[1].z];
    Texture2D<uint4> original = ResourceDescriptorHeap[P[1].y];
    uint3 aMin = uint3(2047, 2047, 1023), aMax = 0, bMin = aMin, bMax = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const uint4 v = input.Load(int3(int2(at) + int2(x, y), 0));
        aMin = min(aMin, codes(v.x)); aMax = max(aMax, codes(v.x));
        bMin = min(bMin, codes(v.y)); bMax = max(bMax, codes(v.y));
    }
    const uint4 own = original.Load(int3(at, 0));
    output[at] = uint4(packedCodes(clamp(codes(own.x), aMin, aMax)), packedCodes(clamp(codes(own.y), bMin, bMax)), own.x, input.Load(int3(at, 0)).z);
#else
    Texture2D<uint4> input = ResourceDescriptorHeap[P[1].z];
    float3 filteredInput = 0, filteredGuide = 0, sumC = 0, sumD = 0, cMin = 1, cMax = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
    {
        const uint4 v = input.Load(int3(int2(at) + int2(x, y), 0));
        const float nw = (x == 0 ? 0.5 : 0.25) * (y == 0 ? 0.5 : 0.25);
        const float3 cc = unpack(v.x), gg = unpack(v.y), dd = abs(unpack(v.z) - cc);
        filteredInput += cc * nw; filteredGuide += gg * nw; sumC += cc; sumD += dd;
        cMin = min(cMin, cc); cMax = max(cMax, cc);
    }
    const uint4 own = input.Load(int3(at, 0));
    const float3 centre = unpack(own.x);
    const float3 variationC = abs(1.125 * centre - 0.125 * sumC);
    const float3 variationD = abs(1.125 * abs(unpack(own.z) - centre) - 0.125 * sumD);
    output[at] = uint4(pack(filteredInput), pack(filteredGuide), pack(min(variationC, variationD)), pack(cMax - cMin));
#endif
}
