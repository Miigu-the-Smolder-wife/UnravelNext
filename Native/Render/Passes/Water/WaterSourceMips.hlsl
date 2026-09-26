// unx-kernel: cs_6_6 main
// The refraction source's box pyramid (WaterFootprint.hlsli): level k + 1 texel = the mean of its 2 x 2 texels of level
// k (an odd edge repeats the last texel's column or row: the clamp the sampler applies). One dispatch per level.
// P[0] source level SRV (texture), destination level UAV, destination width, destination height
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].z || id.y >= P[0].w) return;
    Texture2D<float4> src = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> dst = ResourceDescriptorHeap[P[0].y];
    uint w, h;
    src.GetDimensions(w, h);
    const uint2 p = id.xy * 2;
    const uint2 q = min(p + 1, uint2(w, h) - 1);
    dst[id.xy] = 0.25 * (src[p] + src[uint2(q.x, p.y)] + src[uint2(p.x, q.y)] + src[q]);
}
