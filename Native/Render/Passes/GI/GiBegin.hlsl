// unx-kernel: cs_6_6 main
// GI frame setup (one group): frame stamp, lighting epoch, main camera (cell levels for every view), per-frame counters
// and statistics, the age histogram, this frame's hit list, and the background cursor advanced past last frame's range.
// P[0] = { cache UAV, frame, epoch, 0 }, P[1] = { camera xyz (float bits), 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    b.Store(GI_HISTOGRAM + lane * 4, 0u);
    if (lane != 0) return;
    const GiHeader h = giHeader(b);
    b.Store(GI_H_FRAME, P[0].y);
    b.Store(GI_H_EPOCH, P[0].z);
    b.Store3(GI_H_CAMERA, P[1].xyz);
    b.Store(GI_H_UPDATE_COUNT, 0u);
    b.Store(GI_H_SELECTED_COUNT, 0u);
    b.Store(GI_H_BG_CURSOR, (h.backgroundCursor + h.backgroundCount) % h.capacity);
    b.Store(GI_H_BG_COUNT, 0u);
    b.Store(GI_H_HIT_COUNT + 4 * (P[0].y & 1u), 0u);  // this frame's hit list; last frame's is carried by GiCarry
    b.Store4(GI_H_STAT_CREATED, uint4(0, 0, 0, 0));
    b.Store4(GI_H_STAT_RESETS, uint4(0, 0, 0, 0));
}
