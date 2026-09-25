// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Render-graph test kernel for castable view formats (TextureDesc::srvFormat/uavFormat).
//   MODE=0: writes packed RGB9E5 bits through an R32_UINT UAV: texel (x, y) = castTestBits(x, y). P[0].x UAV, P[0].y size
//   MODE=1: reads the same texture through an R9G9B9E5_SHAREDEXP SRV (the hardware decodes) into a float4 buffer.
//           P[0].x SRV, P[0].y size, P[0].z output buffer UAV (raw, float4 per texel)
#include "Bindless.hlsli"

uint castTestBits(uint x, uint y)
{
    return ((x * 37) & 511) | (((y * 59) & 511) << 9) | ((((x + y) * 13) & 511) << 18) | ((10 + ((x + y) & 7)) << 27);
}

[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= P[0].y)) return;
#if MODE == 0
    RWTexture2D<uint> t = ResourceDescriptorHeap[P[0].x];
    t[p] = castTestBits(p.x, p.y);
#else
    Texture2D<float4> t = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].z];
    o.Store4(16 * (p.y * P[0].y + p.x), asuint(t.Load(int3(p, 0))));
#endif
}
