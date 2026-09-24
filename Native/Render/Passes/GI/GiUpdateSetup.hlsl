// unx-kernel: cs_6_6 main
// Splits this frame's fixed ray budget into whole-hemisphere updates (64 rays each): the stalest requested entries first
// (age histogram scanned from the oldest bucket: every bucket above the threshold is taken, the threshold bucket up to
// its quota), leftover updates go to background entries round-robin over the pool.
// P[0] = { cache UAV, updates per frame (budget / 64), 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    const uint updates = P[0].y;
    uint taken = 0, threshold = 0, quota = 0;
    [loop] for (int bucket = GI_AGE_BUCKETS - 1; bucket >= 0; --bucket)
    {
        const uint n = b.Load(GI_HISTOGRAM + bucket * 4);
        if (taken + n >= updates)
        {
            threshold = bucket;
            quota = updates - taken;
            taken = updates;
            break;
        }
        taken += n;
        threshold = bucket;
        quota = n;
    }
    b.Store3(GI_H_SELECT_THRESHOLD, uint3(threshold, quota, 0));
    b.Store(GI_H_BG_COUNT, updates - min(taken, updates));
    b.Store(GI_H_LIVE_COUNT, h.capacity - h.freeCount);
}
