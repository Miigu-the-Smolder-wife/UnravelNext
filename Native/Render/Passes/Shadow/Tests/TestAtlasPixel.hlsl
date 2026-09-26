// unx-kernel: ps_6_6 main
// S test stand-in for V's tile atlas mode (INTERFACES 5.3 v1.32; TestRaster.h): the view is drawn over its whole
// viewport; a pixel in a tile whose mask bit is set lands in that tile's atlas slot (the same whole-pixel shift V makes)
// and keeps the largest device depth (reversed Z: the nearest surface), as V's hardware depth test would. Positive float
// bits order as uints, so InterlockedMax on a R32_UINT copy of the atlas is that test; TestRaster copies it to the atlas.
// P[4].x atlas copy UAV (RWTexture2D<uint>), P[4].y tile mask SRV (raw), P[4].z atlas slots SRV (raw), P[4].w the view's
// cullMaskOffset (words); P[5].x the view's tiles per row, P[5].y atlas tiles per row, P[5].z tile px
#include "Passes/Visibility/DepthRaster.hlsli"

void main(DepthRasterPixel p)
{
    if (!depthRasterCovered(p)) discard;
    const uint2 px = uint2(p.position.xy);
    const uint tilePx = P[5].z;
    const uint2 tile = px / tilePx;
    const uint bit = tile.y * P[5].x + tile.x;
    ByteAddressBuffer mask = ResourceDescriptorHeap[P[4].y];
    if (((mask.Load((P[4].w + (bit >> 5)) * 4) >> (bit & 31)) & 1u) == 0) return;
    ByteAddressBuffer slots = ResourceDescriptorHeap[P[4].z];
    const uint slot = slots.Load((P[4].w * 32 + bit) * 4);
    const uint2 dst = uint2(slot % P[5].y, slot / P[5].y) * tilePx + px % tilePx;
    RWTexture2D<uint> atlas = ResourceDescriptorHeap[P[4].x];
    InterlockedMax(atlas[dst], asuint(saturate(p.position.z)));
}
