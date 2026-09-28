// unx-kernel: cs_6_6 main
// P[0]: cache, request capacity, first allocation. Growth preserves pending
// records in array A; its start is fixed, independent of workspace capacity.
#include "Passes/GI/GiInternal.hlsli"
[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    const GiHeader h = giHeader(b);
    if (P[0].z != 0)
    {
        b.Store4(h.offAdmission, 0u);
        b.Store4(h.offAdmission + 16, 0u);
    }
    b.Store(h.offAdmission + 4, P[0].y);
}
