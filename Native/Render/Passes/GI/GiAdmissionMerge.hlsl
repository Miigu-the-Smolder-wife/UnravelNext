// unx-kernel: cs_6_6 main
// Stable parallel merge: one record binary-searches the sibling run. At most
// 32 comparisons per thread/dispatch; no spin on another thread's publication.
// P[0]: cache, request capacity, source parity, run length.
#include "Passes/GI/GiAdmission.hlsli"
[numthreads(256, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    const uint i = giAdmissionIndex(group, lane), n = b.Load(h.offAdmission + 8);
    if (i >= n) return;
    const uint source = giAdmissionArray(h, P[0].y, P[0].z);
    const uint span = P[0].w, pair = (i / (2 * span)) * (2 * span);
    const bool right = i - pair >= span;
    const uint start = right ? pair : min(pair + span, n);
    uint lo = start, hi = min(start + span, n);
    const GiAdmissionRecord r = giAdmissionLoad(b, source + i * 32);
    [loop] for (uint step = 0; step < 32 && lo < hi; ++step)
    {
        const uint mid = lo + (hi - lo) / 2;
        const GiAdmissionRecord s = giAdmissionLoad(b, source + mid * 32);
        // Left run precedes equal records (stable, also makes output ranks unique).
        const bool before = right ? !giAdmissionBefore(r, s) : giAdmissionBefore(s, r);
        if (before) lo = mid + 1; else hi = mid;
    }
    const uint rank = pair + (i - (right ? pair + span : pair)) + lo - start;
    giAdmissionStore(b, giAdmissionArray(h, P[0].y, 1 - P[0].z) + rank * 32, r);
}
