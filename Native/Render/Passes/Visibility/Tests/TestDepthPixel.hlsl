// unx-kernel: ps_6_6 main
// V test: a requester's pixel kernel for the depth raster service (DepthRasterPixel contract, INTERFACES 5.3). Keeps
// the nearest depth per pixel (positive floats order like their bits), records instance | userData << 24 and counts
// the fragments per pixel.
//   P[4].x RWTexture2D<uint> depth bits, P[4].y RWTexture2D<uint> instance | userData << 24,
//   P[4].z RWTexture2D<uint> fragments (UNX_NONE: not counted)
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
    if (asuint(p.position.z) >= previous) ids[px] = p.instance | (p.userData << 24);
}
