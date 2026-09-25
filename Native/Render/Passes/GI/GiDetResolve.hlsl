// unx-kernel: cs_6_6 main
// Deterministic update selection (gi.deterministic), one radix level resolved per tier (group = tier): the digit whose
// cumulative count reaches the entries still to take extends the priority prefix. After the last level the prefix is
// the exact priority v of the quota-th entry: GiSelect takes every entry below v, and the entries equal to v only when
// all of them fit (a hash tie at the boundary leaves those slots unused rather than picking by arrival order).
// Level 0 also initialises the tier's state from GI_H_SELECT. The histogram is cleared for the next level.
// P[0] = { cache UAV, selection state UAV (raw), level 0..3, 0 }
#include "Passes/GI/GiInternal.hlsli"

groupshared uint g_scan[256];

[numthreads(256, 1, 1)]
void main(uint d : SV_GroupThreadID, uint tier : SV_GroupID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
    const uint level = P[0].z;
    const uint base = tier * GI_DET_TIER_BYTES;
    const uint count = state.Load(base + d * 4);
    g_scan[d] = count;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint offset = 1; offset < 256; offset <<= 1)
    {
        const uint add = d >= offset ? g_scan[d - offset] : 0u;
        GroupMemoryBarrierWithGroupSync();
        g_scan[d] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint inclusive = g_scan[d], exclusive = inclusive - count;
    const uint remaining = level == 0 ? b.Load(GI_H_SELECT + tier * 16 + 4) : state.Load(base + GI_DET_REMAINING);
    const uint prefix = level == 0 ? 0u : state.Load(base + GI_DET_PREFIX);
    GroupMemoryBarrierWithGroupSync();
    if (remaining > 0 && exclusive < remaining && remaining <= inclusive)
    {
        const uint left = remaining - exclusive;  // still needed from the entries with this digit
        state.Store(base + GI_DET_PREFIX, prefix | (d << (24 - 8 * level)));
        state.Store(base + GI_DET_REMAINING, left);
        if (level == 3) state.Store(base + GI_DET_EQUAL, count == left ? 1u : 0u);
    }
    if (level == 0 && remaining == 0 && d == 0)
    {
        state.Store(base + GI_DET_PREFIX, 0u);
        state.Store(base + GI_DET_REMAINING, 0u);
        state.Store(base + GI_DET_EQUAL, 0u);
    }
    state.Store(base + d * 4, 0u);
}
