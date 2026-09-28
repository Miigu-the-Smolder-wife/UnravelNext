// unx-kernel: cs_6_6 main
// Validate the published table through the production lookup, including keys
// whose old 32-slot linear probe sequence would be exhausted.
#include "Passes/GI/GiInternal.hlsli"
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<uint2> keys = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<uint> result = ResourceDescriptorHeap[P[0].z];
    if (i < P[0].w) result[i] = giFind(b, giHeader(b), (uint64_t)keys[i].x | ((uint64_t)keys[i].y << 32));
}
