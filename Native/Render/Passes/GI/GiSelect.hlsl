// unx-kernel: cs_6_6 main
// Selects this frame's updates from the requested entries (GiUpdateSetup's per-tier age threshold and quota) and stamps them.
// P[0] = { cache UAV, deterministic selection state UAV (UNX_NONE: first come in the threshold bucket), 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (i >= min(h.updateCount, h.capacity)) return;
    const uint item = b.Load(h.offUpdate + i * 4);
    const uint tier = item >> 31, entry = item & ~GI_TIER_HIT;
    const uint bucket = giAgeBucket(b, h, entry);
    const uint2 select = b.Load2(GI_H_SELECT + tier * 16);  // threshold, quota
    if (bucket < select.x || (select.y == 0 && bucket == select.x)) return;
    if (bucket == select.x)
    {
        if (P[0].y != UNX_NONE)
        {
            // gi.deterministic: the quota's entries by key priority (GiDetResolve's final prefix), not by arrival.
            RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
            const uint base = tier * GI_DET_TIER_BYTES, v = state.Load(base + GI_DET_PREFIX), p = giDetPriority(b, h, entry);
            if (!(p < v || (p == v && state.Load(base + GI_DET_EQUAL) != 0))) return;
        }
        else
        {
            uint fill;
            b.InterlockedAdd(GI_H_SELECT + tier * 16 + 8, 1u, fill);
            if (fill >= select.y) return;
        }
    }
    uint slot;
    b.InterlockedAdd(GI_H_SELECTED_COUNT, 1u, slot);
    b.Store(h.offSelected + slot * 4, entry);
    b.Store(h.offSh + entry * GI_SH_STRIDE + GI_SH_LAST_UPDATE, h.frame);
}
