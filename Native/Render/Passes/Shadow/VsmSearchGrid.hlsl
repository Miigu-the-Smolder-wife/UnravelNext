// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Search bound grid for the visibility pass: per page slot of every level, the highest caster the blocker search of a
// receiver in that page can meet (the 3 x 3 neighbouring pages, each from its finest resident level at or above), so the
// visibility pass reads one value per pixel instead of nine page-table chains (same value as the per-pixel walk).
// MODE 0: fill[slot] = page maximum of the slot's page, or of its finest resident ancestor (VSM_EMPTY if none).
// MODE 1: bound[slot] = max of fill over the 3 x 3 pages around the slot's page (neighbours outside the level's window
//         take their finest in-window ancestor's fill).
// P[0].x page table SRV (raw), P[0].y page metadata SRV (VsmPageMeta), P[0].z fill (MODE 0: UAV, MODE 1: SRV),
// P[0].w bound UAV (MODE 1), P[1].x VSM constants CBV, P[1].y unused
#include "Passes/Shadow/VsmCommon.hlsli"

uint entryOf(ConstantBuffer<VsmConstants> c, ByteAddressBuffer table, int2 page, uint k)
{
    if (!vsmInWindow(c, page, k)) return 0;
    const uint2 e = table.Load2(vsmSlot(page, k) * 8);
    return ((e.x & VSM_FLAG_RESIDENT) != 0 && e.y == vsmTag(page)) ? e.x : 0;
}

[numthreads(256, 1, 1)]
void main(uint slot : SV_DispatchThreadID)
{
    if (slot >= VSM_SUN_SLOTS) return;
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[1].x];
    const uint k = slot / VSM_SLOTS_PER_LEVEL;
    const int2 page = vsmSlotAbsPage(c, slot % VSM_SLOTS_PER_LEVEL, k);
#if MODE == 0
    ByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<VsmPageMeta> meta = ResourceDescriptorHeap[P[0].y];
    uint m = VSM_EMPTY;
    [loop] for (uint j = k; j < VSM_LEVELS; ++j)
    {
        const uint e = entryOf(c, table, page >> (int)(j - k), j);
        if (e != 0)
        {
            m = meta[e & VSM_PHYS_MASK].maxHeight;
            break;
        }
    }
    RWByteAddressBuffer fill = ResourceDescriptorHeap[P[0].z];
    fill.Store(slot * 4, m);
#else
    ByteAddressBuffer fill = ResourceDescriptorHeap[P[0].z];
    uint m = VSM_EMPTY;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            const int2 q = page + int2(dx, dy);
            [loop] for (uint j = k; j < VSM_LEVELS; ++j)
            {
                const int2 a = q >> (int)(j - k);
                if (vsmInWindow(c, a, j))
                {
                    m = max(m, fill.Load(vsmSlot(a, j) * 4));
                    break;
                }
            }
        }
    RWByteAddressBuffer bound = ResourceDescriptorHeap[P[0].w];
    bound.Store(slot * 4, m);
#endif
}
