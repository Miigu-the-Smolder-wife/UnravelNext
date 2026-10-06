// unx-kernel: cs_6_6 main
// Original production scan, retained as an independent bit-exact oracle.
#include "Bindless.hlsli"
groupshared float4 gs_sum[1024];

[numthreads(1024, 1, 1)]
void main(uint3 gid : SV_GroupID, uint t : SV_GroupIndex)
{
    RWTexture2D<float4> map = ResourceDescriptorHeap[P[0].y];
    const uint width = P[1].x, row = gid.x;
    float4 carry = 0;
    for (uint c0 = 0; c0 < width; c0 += 1024)
    {
        const uint x = c0 + t;
        gs_sum[t] = x < width ? map[uint2(x, row)] : 0;
        GroupMemoryBarrierWithGroupSync();
        for (uint s = 1; s < 1024; s <<= 1)  // Hillis-Steele: a fixed combination order (deterministic)
        {
            const float4 o = t >= s ? gs_sum[t - s] : 0;
            GroupMemoryBarrierWithGroupSync();
            gs_sum[t] += o;
            GroupMemoryBarrierWithGroupSync();
        }
        if (x < width) map[uint2(x, row)] = carry + gs_sum[t];
        carry += gs_sum[1023];
        GroupMemoryBarrierWithGroupSync();
    }
}
