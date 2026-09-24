// unx-kernel: cs_6_6 main
// Inserts every live entry into the cleared table (no tombstones: eviction happened before the clear).
// P[0] = { cache UAV, 0, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint entry : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (entry >= h.capacity) return;
    const uint2 k = b.Load2(h.offMeta + entry * 16);
    if (k.y == 0) return;
    const uint64_t key = (uint64_t)k.x | ((uint64_t)k.y << 32);
    uint slot = giHash(key) & (h.tableSlots - 1);
    [loop] for (uint i = 0; i < GI_PROBE_LIMIT; ++i)
    {
        const uint address = h.offTable + slot * 16;
        uint64_t previous;
        b.InterlockedCompareExchange64(address, 0ull, key, previous);
        if (previous == 0)
        {
            b.Store(address + 8, entry);
            return;
        }
        slot = (slot + 1) & (h.tableSlots - 1);
    }
    // Probe sequence full: the entry is unreachable this frame (counted); it ages out if it stays unreachable.
    b.InterlockedAdd(GI_H_STAT_TABLE_FULL, 1u);
}
