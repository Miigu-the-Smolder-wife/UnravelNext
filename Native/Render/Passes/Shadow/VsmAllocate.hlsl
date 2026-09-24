// unx-kernel: cs_6_6 main
// Per requested page slot: allocate a physical page if not resident (from the free list; exhaustion is counted and the
// page stays absent, lookups then fall back to coarser levels), turn requested stale or new pages dirty (rendered this
// frame: dirty list for clear, raster and page max), and consume the request.
// P[0].x page table UAV (raw), P[0].y requests UAV (raw), P[0].z page metadata UAV, P[0].w free list UAV (raw)
// P[1].x dirty list UAV (raw: count, pad, then (slot, phys) pairs), P[1].y stats UAV (raw), P[1].z VSM constants SRV,
// P[1].w constants offset
// Stats words: 0 requested, 1 allocated, 2 dirty, 3 pool exhausted, 4 free pages after allocation (VsmFinalize)
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(256, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    if (slot >= VSM_SUN_SLOTS) return;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
    const uint req = requests.Load(slot * 4);
    if (req == 0) return;
    requests.Store(slot * 4, 0);
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<uint4> meta = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[1].y];
    const VsmConstants c = vsmLoadConstants(P[1].z, P[1].w);
    stats.InterlockedAdd(0, 1);
    const uint k = slot / VSM_SLOTS_PER_LEVEL;
    const int2 page = vsmSlotAbsPage(c, slot % VSM_SLOTS_PER_LEVEL, k);
    uint2 e = table.Load2(slot * 8);
    if ((e.x & VSM_FLAG_RESIDENT) == 0)
    {
        RWByteAddressBuffer freeList = ResourceDescriptorHeap[P[0].w];
        int before;
        freeList.InterlockedAdd(0, -1, before);
        if (before <= 0)
        {
            freeList.InterlockedAdd(0, 1);
            stats.InterlockedAdd(12, 1);
            return;
        }
        const uint newPhys = freeList.Load(4 + (before - 1) * 4);
        e = uint2(newPhys | VSM_FLAG_RESIDENT | VSM_FLAG_STALE, vsmTag(page));
        stats.InterlockedAdd(4, 1);
    }
    const uint phys = e.x & VSM_PHYS_MASK;
    uint4 m = meta[phys];
    m.x = slot | 0x80000000u;  // owner
    m.y = c.frame;             // last requested
    if (e.x & VSM_FLAG_STALE)
    {
        e.x = (e.x & ~VSM_FLAG_STALE) | VSM_FLAG_DIRTY;
        m.z = asuint(c.time);  // render time (wind rule)
        m.w = VSM_EMPTY;       // page max, rebuilt after the raster
        RWByteAddressBuffer dirty = ResourceDescriptorHeap[P[1].x];
        uint at;
        dirty.InterlockedAdd(0, 1, at);
        dirty.Store2(8 + at * 8, uint2(slot, phys));
        stats.InterlockedAdd(8, 1);
    }
    meta[phys] = m;
    table.Store2(slot * 8, e);
}
