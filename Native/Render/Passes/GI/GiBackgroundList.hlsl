// unx-kernel: cs_6_6 main
// Background updates into the selected list before the trace (the default mode; gi.deterministic has GiDetBackground):
// mode 0, one thread per background index i < BG_COUNT: the entry at (cursor + i) mod capacity joins the selected list
// when it is live and not selected this frame, stamped as selected; mode 1, one thread: the cursor moves past the range
// and BG_COUNT becomes 0 (GiBegin's advance, done here), so GiTrace and GiIntegrate see only selected slots.
// GiTrace and GiIntegrate used to test the index range each on its own (giUpdateSlot's background branch): an index
// free when the trace looked (no samples written) could be taken by a new entry during the trace (giFindOrCreate pops
// the free list), and the integration then folded that slot's stale samples - the transient ray-sample buffer of an
// earlier use, possibly NaN - into the new entry (and the 64 threads of one slot could disagree). Now both read the list.
// P[0] = { cache UAV, mode, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (P[0].y == 1)
    {
        if (i == 0)
        {
            b.Store(GI_H_BG_CURSOR, (h.backgroundCursor + h.backgroundCount) % h.capacity);
            b.Store(GI_H_BG_COUNT, 0u);
        }
        return;
    }
    if (i >= h.backgroundCount) return;
    const uint entry = (h.backgroundCursor + i) % h.capacity;
    if (b.Load(h.offMeta + entry * 16 + 4) == 0) return;                               // free
    if (b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_LAST_UPDATE) == h.frame) return;  // selected this frame
    uint slot;
    b.InterlockedAdd(GI_H_SELECTED_COUNT, 1u, slot);
    b.Store(h.offSelected + slot * 4, entry);
    b.Store(h.offSh + entry * GI_SH_STRIDE + GI_SH_LAST_UPDATE, h.frame);
}
