// unx-kernel: cs_6_6 main
// Deterministic background updates (gi.deterministic): the candidates the tier-2 radix select took (priority below the
// final prefix, or equal to it when all equal ones fit) join the selected list; the index-range background is emptied.
// P[0] = { cache UAV, selection state UAV (raw), 0 (append) | 1 (one thread: empty the index-range background), 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
    const GiHeader h = giHeader(b);
    if (P[0].z == 1)
    {
        if (i == 0) b.Store(GI_H_BG_COUNT, 0u);
        return;
    }
    if (i >= h.capacity || !giDetBackgroundCandidate(b, h, i)) return;
    const uint base = 2 * GI_DET_TIER_BYTES, v = state.Load(base + GI_DET_PREFIX), p = giDetPriority(b, h, i);
    if (!(p < v || (p == v && state.Load(base + GI_DET_EQUAL) != 0))) return;
    uint slot;
    b.InterlockedAdd(GI_H_SELECTED_COUNT, 1u, slot);
    b.Store(h.offSelected + slot * 4, i);
    b.Store(h.offSh + i * GI_SH_STRIDE + GI_SH_LAST_UPDATE, h.frame);
}
