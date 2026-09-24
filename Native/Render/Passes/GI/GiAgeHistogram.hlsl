// unx-kernel: cs_6_6 main
// Age histograms of the requested entries, one per tier (GiInternal.hlsli GI_HISTOGRAM). Each group counts into
// groupshared bins first and adds its non-zero bins once (the global bins are 128 addresses: direct atomics from every
// thread serialise on them).
// P[0] = { cache UAV, 0, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

groupshared uint g_bins[2 * GI_AGE_BUCKETS];

[numthreads(128, 1, 1)]
void main(uint i : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    g_bins[lane] = 0;
    GroupMemoryBarrierWithGroupSync();
    if (i < min(h.updateCount, h.capacity))
    {
        const uint item = b.Load(h.offUpdate + i * 4);
        const uint tier = item >> 31;
        InterlockedAdd(g_bins[tier * GI_AGE_BUCKETS + giAgeBucket(b, h, item & ~GI_TIER_HIT)], 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (g_bins[lane] != 0) b.InterlockedAdd(GI_HISTOGRAM + lane * 4, g_bins[lane]);
}
