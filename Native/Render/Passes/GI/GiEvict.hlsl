// unx-kernel: cs_6_6 main
// Frees entries unused for more than the header's maxAge frames (their pool slots return to the free list).
// P[0] = { cache UAV, 0, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint entry : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (entry >= h.capacity) return;
    const uint4 meta = b.Load4(h.offMeta + entry * 16);
    if (meta.y == 0) return;  // free (keys always have bit 63 set)
    if (h.frame - meta.z <= h.maxAge) return;
    b.Store2(h.offMeta + entry * 16, uint2(0, 0));
    uint slot;
    b.InterlockedAdd(GI_H_FREE_COUNT, 1u, slot);
    b.Store(h.offFree + slot * 4, entry);
    b.InterlockedAdd(GI_H_STAT_EVICTED, 1u);
}
