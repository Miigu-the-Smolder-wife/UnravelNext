// unx-kernel: ps_6_6 main
// The software rasteriser's page depth into the atlas (visibility.software_raster; DepthSwMerge.ms draws the page's
// quad over its slot): a texel whose word holds a surface writes its depth (SV_Depth) under the depth test GREATER
// (reversed Z) - where the mesh raster's surface is nearer or equal it stays; a texel without one is discarded.
//   P[0] page slots SRV, page capacity, tile px, atlas tiles per row
//   P[1] atlas size, page depth SRV (raw: page x tilePx^2 words of depth bits), 0, 0
#include "Bindless.hlsli"

float main(float4 position : SV_Position, nointerpolation uint page : PAGE, nointerpolation uint2 origin : ORIGIN) : SV_Depth
{
    ByteAddressBuffer depth = ResourceDescriptorHeap[P[1].y];
    const uint tilePx = P[0].z;
    const uint2 local = min((uint2)position.xy - origin, tilePx - 1);
    const uint word = depth.Load(4 * (page * tilePx * tilePx + local.y * tilePx + local.x));
    if (word == 0) discard;
    return asfloat(word);
}
