// unx-kernel: cs_6_6 main
// Selects this frame's updates from the requested entries (GiUpdateSetup's per-tier threshold and quota; with
// gi.update_tiers the young range has its own, GI_H_SELECT_T0) and stamps them.
// P[0] = { cache UAV, deterministic selection state UAV (UNX_NONE: first come in the threshold bucket), 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    bool taken = false;
    uint range = 0;  // 0 = the lower buckets, 1 = the young range (T0), 2 = T1 (statistics only)
    uint entry = 0;
    if (i < min(h.updateCount, h.capacity))
    {
        const uint item = b.Load(h.offUpdate + i * 4);
        const uint tier = item >> 31;
        entry = item & ~GI_TIER_HIT;
        const uint bucket = giPriorityBucket(b, h, entry);
        const bool tiers = (b.Load(GI_P1_FLAGS) & 1u) != 0;
        const bool young = tiers && bucket >= GI_T0_BUCKET;
        range = young ? 1u : (tiers && bucket >= GI_T1_BUCKET ? 2u : 0u);
        const uint record = young ? GI_H_SELECT_T0 + tier * 16 : GI_H_SELECT + tier * 16;
        const uint2 select = b.Load2(record);  // threshold, quota
        taken = !(bucket < select.x || (select.y == 0 && bucket == select.x));
        if (taken && bucket == select.x)
        {
            if (P[0].y != UNX_NONE)
            {
                // gi.deterministic: the quota's entries by key priority (GiDetResolve's final prefix), not by arrival; the
                // young ranges resolve in state slots 3 + tier (GiDetDigits).
                RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
                const uint base = (young ? 3 + tier : tier) * GI_DET_TIER_BYTES, v = state.Load(base + GI_DET_PREFIX), p = giDetPriority(b, h, entry);
                taken = p < v || (p == v && state.Load(base + GI_DET_EQUAL) != 0);
            }
            else
            {
                uint fill;
                b.InterlockedAdd(record + 8, 1u, fill);
                taken = fill < select.y;
            }
        }
    }
    const uint t0 = WaveActiveCountBits(taken && range == 1), t1 = WaveActiveCountBits(taken && range == 2);  // P1 statistics
    if (WaveIsFirstLane())
    {
        if (t0) b.InterlockedAdd(GI_P1_STAT_T0, t0);
        if (t1) b.InterlockedAdd(GI_P1_STAT_T1, t1);
    }
    if (!taken) return;
    uint slot;
    b.InterlockedAdd(GI_H_SELECTED_COUNT, 1u, slot);
    b.Store(h.offSelected + slot * 4, entry);
    b.Store(h.offSh + entry * GI_SH_STRIDE + GI_SH_LAST_UPDATE, h.frame);
}
