// The frame's card captures as the GPU passes read them (SurfaceCacheCards.cpp writes one record per capture):
//   words 0..3   card page index, card index, flags, 0
//   words 4..7   capture atlas x | y << 16, width | height << 16, card atlas x | y << 16, width | height << 16
//   words 8..11  the page's rectangle in the card's uv (float: min xy, max zw)
//   words 12..15 the card's half sizes along its axes (float xyz), its direction | flags << 8 (bit 0: its image in the
//                capture mirrors the side it is seen from - the card's axes are a left-handed set there)
//   words 16..19 the card's centre in mesh card space (float xyz: the mesh's axes, scaled metres), 0
// (words 12..19: what the clusters' capture kernel needs of a card whose record is not uploaded yet,
// CardCaptureCluster.ps.hlsl)
#ifndef UNX_CARD_CAPTURE_LIST_HLSLI
#define UNX_CARD_CAPTURE_LIST_HLSLI
#include "Bindless.hlsli"

#define CC_CAPTURE_BYTES 80u
#define CC_FLAG_RESAMPLE 1u   // the card had pages before this frame: its lighting is carried over (r.card.resample)
#define CC_FLAG_REFRESH 2u    // a resident page captured again in place: its lighting stays

struct CcCapture
{
    uint page, card, flags;
    uint2 captureOrigin, captureSize, atlasOrigin, atlasSize;
    float4 cardUvRect;
    float3 cardExtent;
    uint direction;      // the card's direction (CardCapture.hlsli ccAxes)
    bool mirrored;
    float3 cardOrigin;
};

CcCapture ccLoadCapture(uint srv, uint index)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    const uint4 a = b.Load4(index * CC_CAPTURE_BYTES), r = b.Load4(index * CC_CAPTURE_BYTES + 16);
    CcCapture c;
    c.page = a.x, c.card = a.y, c.flags = a.z;
    c.captureOrigin = uint2(r.x & 0xFFFFu, r.x >> 16);
    c.captureSize = uint2(r.y & 0xFFFFu, r.y >> 16);
    c.atlasOrigin = uint2(r.z & 0xFFFFu, r.z >> 16);
    c.atlasSize = uint2(r.w & 0xFFFFu, r.w >> 16);
    c.cardUvRect = asfloat(b.Load4(index * CC_CAPTURE_BYTES + 32));
    const uint4 e = b.Load4(index * CC_CAPTURE_BYTES + 48);
    c.cardExtent = asfloat(e.xyz);
    c.direction = e.w & 7u;
    c.mirrored = ((e.w >> 8) & 1u) != 0;
    c.cardOrigin = asfloat(b.Load3(index * CC_CAPTURE_BYTES + 64));
    return c;
}

#endif
