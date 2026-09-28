// unx-kernel: cs_6_6 main
// P[0]: cache, request capacity, sorted parity, unused. Prefix of unique live/new
// keys in each 256-record block, plus block totals for the hierarchical scan.
#include "Passes/GI/GiAdmission.hlsli"
groupshared uint gs_prefix[256];
[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    const uint i = giAdmissionIndex(group, lane), n = b.Load(h.offAdmission + 8);
    const uint source = giAdmissionArray(h, P[0].y, P[0].z);
    uint unique = 0;
    if (i < n)
    {
        const GiAdmissionRecord r = giAdmissionLoad(b, source + i * 32);
        if (r.meta.y == 0) unique = 1;
        else if (r.meta.y == 1 && r.keyAnchor.y != 0)
        {
            if (i == 0) unique = 1;
            else
            {
                const GiAdmissionRecord prev = giAdmissionLoad(b, source + (i - 1) * 32);
                unique = prev.meta.y != 1 || any(prev.keyAnchor.xy != r.keyAnchor.xy) ? 1u : 0u;
            }
        }
    }
    gs_prefix[lane] = unique;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint step = 1; step < 256; step *= 2)
    {
        const uint add = lane >= step ? gs_prefix[lane - step] : 0;
        GroupMemoryBarrierWithGroupSync(); gs_prefix[lane] += add; GroupMemoryBarrierWithGroupSync();
    }
    if (i < n) b.Store2(source + i * 32 + 24, uint2(gs_prefix[lane] - unique, unique));
    if (lane == 255)
        b.Store(giAdmissionScan(h, P[0].y) + (group.y * 65535u + group.x) * 4, gs_prefix[lane]);
}
