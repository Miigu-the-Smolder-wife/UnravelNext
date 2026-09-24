// unx-kernel: cs_6_6 main
// Age histogram of the requested entries: frames since each one's last update (63 = 63 or more, or never updated).
// P[0] = { cache UAV, 0, 0, 0 }
#include "Passes/GI/GiInternal.hlsli"

uint giAgeBucket(RWByteAddressBuffer b, GiHeader h, uint entry)
{
    const uint last = b.Load(h.offSh + entry * GI_SH_STRIDE + GI_SH_LAST_UPDATE);
    return last == 0 ? GI_AGE_BUCKETS - 1 : min(h.frame - last, GI_AGE_BUCKETS - 1);
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (i >= min(h.updateCount, h.capacity)) return;
    const uint entry = b.Load(h.offUpdate + i * 4);
    b.InterlockedAdd(GI_HISTOGRAM + giAgeBucket(b, h, entry) * 4, 1u);
}
