// unx-kernel: cs_6_6 main
// Per requested page slot: allocate a physical page if not resident (from the free list; exhaustion is counted and the
// page stays absent, lookups then fall back to coarser levels), turn requested stale or new pages dirty (rendered this
// frame: dirty list for clear, raster and page max), and consume the request.
// P[0].x page table UAV (raw), P[0].y requests UAV (raw), P[0].z page metadata UAV, P[0].w free list UAV (raw)
// P[1].x dirty list UAV (raw: count, pad, then (slot, phys) pairs), P[1].y stats UAV (raw), P[1].z VSM constants CBV,
// P[1].w local lights SRV (StructuredBuffer<VsmLocalLight>; local slots take their light's generation as tag)
// Stats words: 0 requested, 1 allocated, 2 dirty, 3 pool exhausted, 4 free pages after allocation (VsmFinalize),
// 5 requested by pixels (the rest by propagation)
#include "Scene.hlsli"
#include "Passes/Shadow/VsmLocal.hlsli"

[numthreads(256, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    if (slot >= VSM_TOTAL_SLOTS) return;
    RWByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
    const uint req = requests.Load(slot * 4);
    if (req == 0) return;
    requests.Store(slot * 4, 0);
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<VsmPageMeta> meta = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[1].y];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[1].z];
    stats.InterlockedAdd(0, 1);
    if (req & VSM_REQ_PIXEL) stats.InterlockedAdd(20, 1);
    uint tag;
    if (slot < VSM_SUN_SLOTS)
    {
        const uint k = slot / VSM_SLOTS_PER_LEVEL;
        tag = vsmTag(vsmSlotAbsPage(c, slot % VSM_SLOTS_PER_LEVEL, k));
    }
    else
    {
        StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[1].w];
        tag = lights[(slot - VSM_SUN_SLOTS) / VSM_LOCAL_LIGHT_SLOTS].generation;
    }
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
        e = uint2(newPhys | VSM_FLAG_RESIDENT | VSM_FLAG_STALE, tag);
        stats.InterlockedAdd(4, 1);
    }
    const uint phys = e.x & VSM_PHYS_MASK;
    VsmPageMeta m = meta[phys];
    m.owner = slot | 0x80000000u;
    m.lastRequested = c.frame;
    if (e.x & VSM_FLAG_STALE)
    {
        e.x = (e.x & ~VSM_FLAG_STALE) | VSM_FLAG_DIRTY;
        m.renderTime = asuint(c.time);
        m.maxHeight = VSM_EMPTY;  // rebuilt after the raster (VsmPageMax)
        m.windScale = 0;          // rebuilt by the raster (VsmPagePixel)
        m.windCaster = 0;
        m.windSpeed = asuint(c.windSpeed);  // the wind this render sees (the rule bounds the change from it)
        m.windDirection = octEncode(c.windSpeed > 0 ? c.windDirection : float3(1, 0, 0));
        m.layer = 0;
        RWByteAddressBuffer dirty = ResourceDescriptorHeap[P[1].x];
        uint at;
        dirty.InterlockedAdd(0, 1, at);
        dirty.Store2(8 + at * 8, uint2(slot, phys));
        stats.InterlockedAdd(8, 1);
    }
    meta[phys] = m;
    table.Store2(slot * 8, e);
}
