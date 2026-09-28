// unx-kernel: cs_6_6 main
// P[0]: cache, request capacity, hierarchy level, unused. Scan groups of 256,
// write exclusive sums in place and totals to the next level. <= 4 levels.
#include "Passes/GI/GiAdmission.hlsli"
groupshared uint gs_prefix[256];
[numthreads(256, 1, 1)]
void main(uint group : SV_GroupID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    uint count = (b.Load(h.offAdmission + 8) + 255) / 256;
    uint offset = giAdmissionScan(h, P[0].y);
    [unroll] for (uint level = 0; level < 4; ++level)
    {
        if (level >= P[0].z) break;
        offset += count * 4; count = (count + 255) / 256;
    }
    const uint index = group * 256 + lane;
    const uint value = index < count ? b.Load(offset + index * 4) : 0;
    gs_prefix[lane] = value;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 1; step < 256; step *= 2)
    {
        const uint add = lane >= step ? gs_prefix[lane - step] : 0;
        GroupMemoryBarrierWithGroupSync(); gs_prefix[lane] += add; GroupMemoryBarrierWithGroupSync();
    }
    if (index < count) b.Store(offset + index * 4, gs_prefix[lane] - value);
    if (lane == 255 && group * 256 < count) b.Store(offset + count * 4 + group * 4, gs_prefix[lane]);
}
