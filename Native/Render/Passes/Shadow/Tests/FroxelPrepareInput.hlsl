// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[1].x || id.y >= P[1].y) return;
    RWTexture2D<float2> readers = ResourceDescriptorHeap[P[0].x];
    readers[id.xy] = float2((id.x * 3 + id.y * 5) % 17, P[1].w == 0 ? 0 : P[1].w == 1 ? 1 : ((id.x + id.y) % 5 == 0));
    RWTexture3D<float4> media = ResourceDescriptorHeap[P[0].y];
    for (uint z = 0; z < P[1].z; ++z)
    {
        const bool active = (id.x + id.y * 11 + z * 3) % 13 == 0;
        media[uint3(id.xy, z)] = active ? float4(0.07, 0.01, 0.2, 0) : 0;
        media[uint3(id.xy, z + P[1].z)] = active ? float4(1, 2, 3, 0) : 0;
    }
    if (id.y == 0 && id.x == 0)
    {
        RWTexture2D<float4> params = ResourceDescriptorHeap[P[0].z];
        for (uint x = 0; x < 11; ++x) params[uint2(x, 0)] = 0;
    }
}
