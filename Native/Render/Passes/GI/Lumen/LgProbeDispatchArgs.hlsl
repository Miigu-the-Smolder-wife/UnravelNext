// unx-kernel: cs_6_6 main
// One compute group per live screen probe. Atlas capacity is storage, not execution domain.
// P[0] = { adaptive SRV, dispatch-args UAV, uniform probe count, max adaptive probe count }.
#include "Passes/Common/Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    const uint live = P[0].z + min(adaptive.Load(0), P[0].w);
    args.Store3(0, uint3(live, 1, 1));
}
