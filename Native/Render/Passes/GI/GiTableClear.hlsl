// unx-kernel: cs_6_6 main
// Clears the hash table before the per-frame rehash of the live entries, and the radiance map owners (tableSlots >=
// capacity, checked by GiSystem).
// P[0] = { cache UAV, 0, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (slot >= h.tableSlots) return;
    if ((h.flags & 1u) == 0) b.Store4(h.offTable + slot * 16, uint4(0, 0, GI_ENTRY_PENDING, 0));
    // deterministic anchors (GiDetFold): written only in gi.deterministic (GiInternal giFindOrCreate), 4 MB a frame otherwise
    if ((h.flags & 1u) != 0) b.Store2(h.offSlotAnchor + slot * 8, uint2(0xFFFFFFFFu, 0xFFFFFFFFu));
    if (slot < h.capacity) b.Store(b.Load(GI_H_MAP_OWNER) + slot * 4, 0xFFFFFFFFu);
}
