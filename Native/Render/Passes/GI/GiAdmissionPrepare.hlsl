// unx-kernel: cs_6_6 main
// P[0]: cache, indirect args UAV, request capacity, unused. Existing requests
// occupy array A after capacity records. Every pool slot supplies one record.
#include "Passes/GI/GiAdmission.hlsli"
[numthreads(256, 1, 1)]
void main(uint entry : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (entry >= h.capacity) return;
    const uint4 m = b.Load4(h.offMeta + entry * 16);
    GiAdmissionRecord r;
    r.keyAnchor = uint4(m.xy, 0, 0);
    r.meta = uint4(entry, m.y == 0 ? 2u : 0u, 0, 0);
    giAdmissionStore(b, giAdmissionArray(h, P[0].z, 0) + entry * 32, r);
    if (entry == 0)
    {
        const uint n = h.capacity + min(b.Load(h.offAdmission), P[0].z);
        b.Store2(h.offAdmission + 8, uint2(n, h.capacity - h.freeCount));
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
        const uint groups = (n + 255) / 256;
        args.Store4(0, uint4(min(groups, 65535u), (groups + 65534) / 65535, 1, 0));
    }
}
