// unx-kernel: cs_6_6 main
// unx-variants: PHASE=1,2
// One level of the cluster hierarchy traversal: node items [VS_NODE_BEGIN, VS_NODE_END). A node is skipped when its
// worst group error projects to at most the threshold (every group below is replaced by a coarser one that is drawn
// instead); otherwise a leaf passes its group to cluster culling and an internal node pushes its children.
// Frustum + clip plane on the node's sphere (encloses all geometry below); HiZ occlusion as in CullInstances
// (PHASE=1: previous frame, occluded nodes deferred; PHASE=2: this frame, occluded nodes dropped).
#include "Passes/Visibility/CullShared.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    const uint begin = state.Load(4 * VS_NODE_BEGIN), end = state.Load(4 * VS_NODE_END);
    const uint item = begin + i;
    bool pushChildren = false, pushGroup = false, defer = false;
    uint instance = 0, packed = 0;
    ClusterNode node = (ClusterNode)0;
    if (item < end)
    {
        RWStructuredBuffer<uint2> items = ResourceDescriptorHeap[NODE_ITEMS_UAV];
        const uint2 it = items[item];
        instance = it.x;
        packed = it.y;
        StructuredBuffer<ClusterNode> nodes = ResourceDescriptorHeap[NODES_SRV];
        node = nodes[itemIndex(packed)];
        const GpuInstance inst = loadInstance(instance);
        const CullView v = loadView(itemView(packed));
        const bool skinned = (inst.flags & INSTANCE_SKINNED) != 0;
        const float4 s = worldSphere(inst, inst.objectToWorld, node.lodSphere);
        bool keep = skinned || frustumVisible(v, s);
        keep = keep && projectedError(v, s, node.lodError * instanceScale(inst)) > v.lodThreshold;
        if (keep && !skinned && (v.flags & CULL_VIEW_OCCLUSION) != 0)
        {
#if PHASE == 1
            if (hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.prevViewProj, v.viewportSize, worldSphere(inst, inst.prevObjectToWorld, node.lodSphere)))
            {
                keep = false;
                defer = true;
            }
#else
            keep = !hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.viewProj, v.viewportSize, s);
#endif
        }
        pushGroup = keep && node.leaf != 0;
        pushChildren = keep && node.leaf == 0;
    }
    const uint childBase = waveAppend(state, VS_NODE_WRITE, pushChildren ? node.count : 0, CAP_NODES, OVERFLOW_NODES);
    if (pushChildren)
    {
        RWStructuredBuffer<uint2> items = ResourceDescriptorHeap[NODE_ITEMS_UAV];
        for (uint k = 0; k < node.count; ++k)
            if (childBase + k < CAP_NODES) items[childBase + k] = uint2(instance, packItem(node.first + k, itemView(packed)));
    }
    const uint g = waveAppend(state, VS_GROUP_WRITE, pushGroup ? 1 : 0, CAP_GROUPS, OVERFLOW_GROUPS);
    if (pushGroup && g < CAP_GROUPS)
    {
        RWStructuredBuffer<uint2> groups = ResourceDescriptorHeap[GROUP_ITEMS_UAV];
        groups[g] = uint2(instance, packed);
    }
    const uint d = waveAppend(state, VS_DEFER_NODES, defer ? 1 : 0, CAP_DEFERRED, OVERFLOW_DEFER_NODES);
    if (defer && d < CAP_DEFERRED)
    {
        RWStructuredBuffer<uint2> deferred = ResourceDescriptorHeap[DEFER_NODES_UAV];
        deferred[d] = uint2(instance, packed);
    }
    const uint processed = WaveActiveCountBits(item < end);
    if (WaveIsFirstLane() && processed > 0) state.InterlockedAdd(4 * VS_STAT_NODES, processed);
}
