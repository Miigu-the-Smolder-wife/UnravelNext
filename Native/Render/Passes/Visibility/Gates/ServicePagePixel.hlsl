// unx-kernel: ps_6_6 main
// V gate: the per-fragment work of a VSM page raster through the depth raster service (S's VsmPagePixel without its
// page table): fragments of tiles that are not requested write nothing; the others keep the nearest depth of a
// 128^2 page (InterlockedMax).
//   P[4].x tile mask SRV (raw; 512 words = 128 x 128 tiles per view, view = userData), P[4].y page UAV (raw, 128^2)
#include "Passes/Visibility/DepthRaster.hlsli"

void main(DepthRasterPixel p)
{
    if (!depthRasterCovered(p)) discard;
    const uint2 px = uint2(p.position.xy);
    const uint tile = (px.y >> 7) * 128 + (px.x >> 7);
    ByteAddressBuffer mask = ResourceDescriptorHeap[P[4].x];
    if ((mask.Load(4 * (p.userData * 512 + (tile >> 5))) & (1u << (tile & 31))) == 0) return;
    RWByteAddressBuffer page = ResourceDescriptorHeap[P[4].y];
    page.InterlockedMax(4 * ((px.y & 127) * 128 + (px.x & 127)), asuint(p.position.z));
}
