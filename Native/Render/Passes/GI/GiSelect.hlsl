// unx-kernel: cs_6_6 main
// Selects this frame's updates from the requested entries (GiUpdateSetup's age threshold and quota) and stamps them.
// P[0] = { cache UAV, 0, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (i >= min(h.updateCount, h.capacity)) return;
    const uint entry = b.Load(h.offUpdate + i * 4);
    const uint shAddress = h.offSh + entry * GI_SH_STRIDE;
    const uint last = b.Load(shAddress + GI_SH_LAST_UPDATE);
    const uint bucket = last == 0 ? GI_AGE_BUCKETS - 1 : min(h.frame - last, GI_AGE_BUCKETS - 1);
    const uint threshold = b.Load(GI_H_SELECT_THRESHOLD);
    if (bucket < threshold) return;
    if (bucket == threshold)
    {
        uint fill;
        b.InterlockedAdd(GI_H_SELECT_FILL, 1u, fill);
        if (fill >= b.Load(GI_H_SELECT_QUOTA)) return;
    }
    uint slot;
    b.InterlockedAdd(GI_H_SELECTED_COUNT, 1u, slot);
    b.Store(h.offSelected + slot * 4, entry);
    b.Store(shAddress + GI_SH_LAST_UPDATE, h.frame);
}
