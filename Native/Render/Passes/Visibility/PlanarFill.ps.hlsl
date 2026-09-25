// unx-kernel: ps_6_6 main
// Planar reflection view depth fill (PlanarFill.ms): writes the nearest depth and VIS_NONE where the pixel is not a
// mirror pixel (ViewDesc::planarMask zero); mirror pixels are left to the raster.
//   P[0].x mask SRV (Texture2D<uint>, R8_UINT)
#include "Bindless.hlsli"

uint main(float4 position : SV_Position) : SV_Target0
{
    Texture2D<uint> mask = ResourceDescriptorHeap[P[0].x];
    if (mask[uint2(position.xy)] != 0) discard;
    return 0;  // VIS_NONE (VisBuffer.hlsli)
}
