// The frame's card captures as the GPU passes read them (SurfaceCacheCards.cpp writes one record per capture):
//   words 0..3   card page index, card index, flags, 0
//   words 4..7   capture atlas x | y << 16, width | height << 16, card atlas x | y << 16, width | height << 16
//   words 8..11  the page's rectangle in the card's uv (float: min xy, max zw)
#ifndef UNX_CARD_CAPTURE_LIST_HLSLI
#define UNX_CARD_CAPTURE_LIST_HLSLI
#include "Bindless.hlsli"

#define CC_CAPTURE_BYTES 48u
#define CC_FLAG_RESAMPLE 1u   // the card had pages before this frame: its lighting is carried over (r.card.resample)
#define CC_FLAG_REFRESH 2u    // a resident page captured again in place: its lighting stays

struct CcCapture
{
    uint page, card, flags;
    uint2 captureOrigin, captureSize, atlasOrigin, atlasSize;
    float4 cardUvRect;
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
    return c;
}

#endif
