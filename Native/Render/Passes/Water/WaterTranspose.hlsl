// unx-kernel: cs_6_6 main
// Lossless uint4 transpose between FFT axes. Both global reads and writes are
// contiguous across a tile; only the shared-memory access is transposed.
// P0={source SRV,destination UAV,width,height}, P1={source pitch,dest pitch,0,0}.
#include "Bindless.hlsli"
groupshared uint4 g_tile[16][17];

[numthreads(16, 16, 1)]
void main(uint2 group : SV_GroupID, uint2 local : SV_GroupThreadID)
{
    ByteAddressBuffer source = ResourceDescriptorHeap[P[0].x];
    const uint2 input = group * 16 + local;
    uint4 value = 0;
    if (all(input < P[0].zw)) value = source.Load4(16 * (input.y * P[1].x + input.x));
    g_tile[local.y][local.x] = value;
    GroupMemoryBarrierWithGroupSync();
    const uint2 output = group.yx * 16 + local;
    if (all(output < P[0].wz))
    {
        RWByteAddressBuffer destination = ResourceDescriptorHeap[P[0].y];
        destination.Store4(16 * (output.y * P[1].y + output.x), g_tile[local.x][local.y]);
    }
}
