// unx-kernel: cs_6_6 main
// Phase 2: appends the node items deferred by phase 1 (occluded by the previous frame's HiZ) to the node queue, so the
// phase-2 traversal tests them against this frame's HiZ.
#include "Passes/Visibility/CullShared.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    const uint count = min(state.Load(4 * VS_DEFER_NODES), CAP_DEFERRED);
    const bool valid = i < count;
    uint first, total;
    const uint base = nodeReserve(state, valid ? 1 : 0, CAP_NODES, 0, first, total);
    if (valid && base < CAP_NODES)
    {
        RWStructuredBuffer<uint2> deferred = ResourceDescriptorHeap[DEFER_NODES_UAV];
        RWStructuredBuffer<uint2> items = ResourceDescriptorHeap[NODE_ITEMS_UAV];
        items[base] = deferred[i];
    }
    nodePublish(state, first, total);
    nodePublishRange(base, valid ? 1u : 0u, true);
}
