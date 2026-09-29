// Optional FP32 component history (never allocated/read by SPLIT0).
// Per entry: 16 B header (independent update count), then 64 texels, 81
// irradiance-map values and 9 SH coefficients, each {direct RGB, bounce RGB}.
// Public cache products remain unchanged: each stores the sum of the means.
#ifndef UNX_GI_SPLIT_HISTORY_HLSLI
#define UNX_GI_SPLIT_HISTORY_HLSLI
#define GI_SPLIT_STRIDE 3712u
uint giSplitBase(RWByteAddressBuffer b, uint entry) { return b.Load(772) + entry * GI_SPLIT_STRIDE; }
float3 giSplitBlend(RWByteAddressBuffer b, uint base, uint index, float3 direct, float3 bounce,
                   float directAlpha, float bounceAlpha, bool reset)
{
    const uint address = base + 16 + index * 24;
    const float3 heldDirect = reset ? float3(0, 0, 0) : asfloat(b.Load3(address));
    const float3 heldBounce = reset ? float3(0, 0, 0) : asfloat(b.Load3(address + 12));
    const bool goodDirect = all(isfinite(direct)), goodBounce = all(isfinite(bounce));
    const float3 d = goodDirect ? lerp(heldDirect, direct, directAlpha) : heldDirect;
    const float3 r = goodBounce ? lerp(heldBounce, bounce, bounceAlpha) : heldBounce;
    b.Store3(address, asuint(d)); b.Store3(address + 12, asuint(r));
    return d + r;
}
#endif
