// unx-kernel: cs_6_6 main
// GI frame setup (one group): frame stamp, lighting epoch, main camera (cell levels for every view), per-frame counters
// and statistics, the age histogram, this frame's hit list, and the background cursor advanced past last frame's range.
// P[0] = { cache UAV, frame, epoch, flags (bit 0 = gi.deterministic, bit 1 = gi.anchor_visibility) }, P[1] = { camera xyz (float bits), 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(128, 1, 1)]
void main(uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    b.Store(GI_HISTOGRAM + lane * 4, 0u);  // 2 tiers x 64 buckets
    if (lane != 0) return;
    const GiHeader h = giHeader(b);
    b.Store(GI_H_FRAME, P[0].y);
    b.Store(GI_H_EPOCH, P[0].z);
    // GiHeader.flags; bit 2: the pool is under pressure (fewer than 1/8 of the entries free) - GiEvict's age limit, decided
    // here once: its threads read the free count while the same dispatch raised it, so the limit could flip mid-dispatch.
    b.Store(244, P[0].w | (h.freeCount < h.capacity / 8 ? 4u : 0u));
    b.Store3(GI_H_CAMERA, P[1].xyz);
    b.Store(GI_H_UPDATE_COUNT, 0u);
    b.Store(GI_H_SELECTED_COUNT, 0u);
    b.Store(GI_H_BG_CURSOR, (h.backgroundCursor + h.backgroundCount) % h.capacity);
    b.Store(GI_H_BG_COUNT, 0u);
    b.Store(GI_H_HIT_COUNT + 4 * (P[0].y & 1u), 0u);  // this frame's hit list; last frame's is carried by GiCarry
    b.Store4(GI_H_STAT_CREATED, uint4(0, 0, 0, 0));
    b.Store(GI_H_STAT_RESETS, 0u);
    b.Store2(GI_H_STAT_HIT_LOOKUPS, uint2(0, 0));
    b.Store2(GI_H_STAT_G_SAMPLES, uint2(0, 0));
    b.Store4(GI_H_STAT_G_HIST, uint4(0, 0, 0, 0));
    b.Store4(GI_H_STAT_G_HIST + 16, uint4(0, 0, 0, 0));
    b.Store(GI_H_STAT_G_HIST + 32, 0u);
    b.Store(GI_H_STAT_G_ZERO, 0u);
    b.Store4(GI_H_SELECT, uint4(0, 0, 0, 0));
    b.Store4(GI_H_SELECT + 16, uint4(0, 0, 0, 0));
}
