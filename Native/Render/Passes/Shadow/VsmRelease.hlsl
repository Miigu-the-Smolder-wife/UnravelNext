// unx-kernel: cs_6_6 main
// Per page slot, before allocation: clears last frame's dirty flag; marks every resident page stale on a scene-wide
// invalidation (sun direction, scene reload); releases pages that left their level's window (clipmap scroll: the slot
// now maps a different absolute page) or were not requested for cacheFrames frames.
// P[0].x page table UAV (raw), P[0].y requests SRV (raw), P[0].z page metadata UAV, P[0].w free list UAV (raw)
// P[1].x VSM constants SRV, P[1].y constants offset
#include "Passes/Shadow/VsmCommon.hlsli"

[numthreads(256, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    if (slot >= VSM_SUN_SLOTS) return;
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    uint2 e = table.Load2(slot * 8);
    if ((e.x & VSM_FLAG_RESIDENT) == 0) return;
    const VsmConstants c = vsmLoadConstants(P[1].x, P[1].y);
    ByteAddressBuffer requests = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<uint4> meta = ResourceDescriptorHeap[P[0].z];
    const uint k = slot / VSM_SLOTS_PER_LEVEL;
    const int2 page = vsmSlotAbsPage(c, slot % VSM_SLOTS_PER_LEVEL, k);
    const uint phys = e.x & VSM_PHYS_MASK;
    const bool requested = requests.Load(slot * 4) != 0;
    const bool scrolled = e.y != vsmTag(page);
    const bool aged = !requested && (c.frame - meta[phys].y) > c.cacheFrames;
    if (scrolled || aged)
    {
        table.Store2(slot * 8, uint2(0, 0));
        meta[phys] = uint4(0, 0, 0, 0);
        RWByteAddressBuffer freeList = ResourceDescriptorHeap[P[0].w];
        uint at;
        freeList.InterlockedAdd(0, 1, at);
        freeList.Store(4 + at * 4, phys);
        return;
    }
    e.x &= ~VSM_FLAG_DIRTY;
    if (c.sceneInvalidate) e.x |= VSM_FLAG_STALE;
    table.Store(slot * 8, e.x);
}
