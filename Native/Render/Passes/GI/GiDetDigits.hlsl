// unx-kernel: cs_6_6 main
// Deterministic update selection (gi.deterministic), one radix level: the requested entries in a tier's threshold age
// bucket are ranked by a priority hash of their key and the frame (giDetPriority), not by the order their atomic fills
// arrive in. Level L counts the entries whose priority matches the prefix resolved so far by their next 8 bits.
// Tiers 0 and 1: the request tiers' threshold buckets; 3 and 4: their young ranges' (gi.update_tiers).
// Mode 1 (P[0].w): the background updates instead (tier 2): every live entry not updated this frame, quota =
// GI_H_BG_COUNT (the index-range background of the default mode depends on allocation order).
// P[0] = { cache UAV, selection state UAV (raw, GI_DET_* layout), level 0..3, mode 0 (update list) | 1 (background) }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
    const GiHeader h = giHeader(b);
    uint tier, entry;
    if (P[0].w == 0)
    {
        if (i >= min(h.updateCount, h.capacity)) return;
        const uint item = b.Load(h.offUpdate + i * 4);
        tier = item >> 31;
        entry = item & ~GI_TIER_HIT;
        // gi.update_tiers: the young range (GI_H_SELECT_T0) resolves in state slot 3 + tier
        const uint bucket = giPriorityBucket(b, h, entry);
        const bool young = (b.Load(GI_P1_FLAGS) & 1u) != 0 && bucket >= GI_T0_BUCKET;
        const uint2 select = b.Load2(young ? GI_H_SELECT_T0 + tier * 16 : GI_H_SELECT + tier * 16);  // threshold bucket, quota
        if (select.y == 0 || bucket != select.x) return;
        if (young) tier += 3;
    }
    else
    {
        tier = 2;
        entry = i;
        if (i >= h.capacity || !giDetBackgroundCandidate(b, h, entry)) return;
    }
    const uint level = P[0].z;
    const uint p = giDetPriority(b, h, entry);
    const uint base = tier * GI_DET_TIER_BYTES;
    if (level > 0)
    {
        const uint mask = 0xFFFFFFFFu << (32 - 8 * level);
        if ((p & mask) != (state.Load(base + GI_DET_PREFIX) & mask)) return;
    }
    state.InterlockedAdd(base + ((p >> (24 - 8 * level)) & 255u) * 4, 1u);
}
