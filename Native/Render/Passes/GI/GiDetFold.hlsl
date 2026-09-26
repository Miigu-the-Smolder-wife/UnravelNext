// unx-kernel: cs_6_6 main
// Deterministic anchors (gi.deterministic): folds the anchor candidates collected per hash-table slot into the entries.
// A thread whose key was just inserted by another thread (the table slot holds the key, its entry index is not published
// yet: GI_ENTRY_PENDING) cannot reach the entry, but it knows the slot; giFindOrCreate puts its candidate there, so the
// entry's anchor (the minimum over every candidate) does not depend on which threads arrived before the publication.
// Runs over the table while it still holds this frame's (or last frame's) keys: before GiDetAnchors decodes the anchors,
// and before the table is cleared at the start of the next frame (candidates of the later ray passes).
// P[0] = { cache UAV, 0, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (slot >= h.tableSlots || (h.flags & 1u) == 0) return;
    const uint2 candidate = b.Load2(h.offSlotAnchor + slot * 8);
    if (candidate.x == 0xFFFFFFFFu && candidate.y == 0xFFFFFFFFu) return;
    const uint4 s = b.Load4(h.offTable + slot * 16);
    const uint64_t key = (uint64_t)s.x | ((uint64_t)s.y << 32);
    if (key == 0 || s.z == GI_ENTRY_PENDING) return;
    const uint2 meta = b.Load2(h.offMeta + s.z * 16);
    if (meta.x != s.x || meta.y != s.y) return;  // the entry was freed or reused since
    if (b.Load(h.offSh + s.z * GI_SH_STRIDE + GI_SH_UPDATES) != 0) return;  // anchored for good once updated
    uint64_t previous;
    b.InterlockedMin64(h.offAnchorMin + s.z * 8, (uint64_t)candidate.x | ((uint64_t)candidate.y << 32), previous);
    b.Store2(h.offSlotAnchor + slot * 8, uint2(0xFFFFFFFFu, 0xFFFFFFFFu));
}
