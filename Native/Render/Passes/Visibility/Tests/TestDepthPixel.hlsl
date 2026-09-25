// unx-kernel: ps_6_6 main
// unx-variants: KEY=0,1
// V test: a requester's pixel kernel for the depth raster service (DepthRasterPixel contract, INTERFACES 5.3). Keeps
// the nearest depth per pixel (positive floats order like their bits), records instance | userData << 24 and counts
// the fragments per pixel.
//   P[4].x RWTexture2D<uint> depth bits, P[4].y ids (below), P[4].z RWTexture2D<uint> fragments (UNX_NONE: not counted),
//   P[4].w row length in pixels (KEY=1)
// KEY=0: P[4].y RWTexture2D<uint> instance | userData << 24 of a nearest fragment (the store follows the depth max
//        without being atomic with it: ties and near-simultaneous fragments leave either id).
// KEY=1: P[4].y raw buffer of one uint64 per pixel, the max of depth bits << 32 | id: the nearest fragment, ties to
//        the larger id -- the same result in any fragment order (comparing two rasters' ids).
#include "Passes/Visibility/DepthRaster.hlsli"

void main(DepthRasterPixel p)
{
    const uint2 px = uint2(p.position.xy);
    if (P[4].z != UNX_NONE)
    {
        RWTexture2D<uint> fragments = ResourceDescriptorHeap[P[4].z];
        InterlockedAdd(fragments[px], 1);
    }
    if (!depthRasterCovered(p)) discard;
    RWTexture2D<uint> depth = ResourceDescriptorHeap[P[4].x];
    RWTexture2D<uint> ids = ResourceDescriptorHeap[P[4].y];
    uint previous;
    InterlockedMax(depth[px], asuint(p.position.z), previous);
#if KEY
    RWByteAddressBuffer keys = ResourceDescriptorHeap[P[4].y];
    keys.InterlockedMax64(8 * (px.y * P[4].w + px.x), ((uint64_t)asuint(p.position.z) << 32) | (p.instance | (p.userData << 24)));
#else
    if (asuint(p.position.z) >= previous) ids[px] = p.instance | (p.userData << 24);
#endif
}
