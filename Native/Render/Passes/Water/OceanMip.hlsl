// unx-kernel: cs_6_6 main
// Ocean field mip level l from level l - 1: the mean of the 2 x 2 children, for the displacement and slope textures
// (each slice = one cascade). The mean of displacement and slopes over a footprint is their low-pass at that footprint
// (the surface a coarser vertex spacing or pixel sees); the tile is periodic and every level halves exactly (512 = 2^9).
// Root constants: P[0] displacement source UAV (mip l - 1), displacement destination UAV (mip l), slopes source,
// slopes destination; P[1] destination size, 0, 0, 0
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint size = P[1].x;
    if (id.x >= size || id.y >= size) return;
    RWTexture2DArray<float4> dSrc = ResourceDescriptorHeap[P[0].x], dDst = ResourceDescriptorHeap[P[0].y];
    RWTexture2DArray<float4> sSrc = ResourceDescriptorHeap[P[0].z], sDst = ResourceDescriptorHeap[P[0].w];
    uint3 c = uint3(2 * id.xy, id.z);
    dDst[id] = ((dSrc[c] + dSrc[c + uint3(1, 0, 0)]) + (dSrc[c + uint3(0, 1, 0)] + dSrc[c + uint3(1, 1, 0)])) * 0.25;
    sDst[id] = ((sSrc[c] + sSrc[c + uint3(1, 0, 0)]) + (sSrc[c + uint3(0, 1, 0)] + sSrc[c + uint3(1, 1, 0)])) * 0.25;
}
