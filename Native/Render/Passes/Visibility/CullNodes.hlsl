// unx-kernel: cs_6_6 main
// unx-variants: PHASE=1,2 QUEUE=0,1
// The cluster hierarchy traversal over the node items. A node is skipped when its worst group error projects to at most
// the threshold (every group below is replaced by a coarser one that is drawn instead); otherwise a leaf passes its group
// to cluster culling and an internal node pushes its children.
// Frustum + clip plane on the node's sphere (encloses all geometry below), raster-service tile mask; HiZ occlusion as
// in CullInstances (PHASE=1: previous frame, occluded nodes deferred; PHASE=2: this frame, occluded nodes dropped).
// Cluster streaming (PAGE_TABLE): a leaf whose group's page is not resident does not pass its group (CullClusters draws
// the clusters simplified from it in its place) and notes the page as wanted; a resident one is noted too (its use).
//   QUEUE=0: one level of the traversal, node items [VS_NODE_BEGIN, VS_NODE_END): a pass per tree level, each after a
//            CullPrepare pass (visibility.traversal_work_queue false).
//   QUEUE=1: persistent workers consume a per-entry-ready queue. A reservation
//            is not a publication: its bit becomes visible only after its data.
//            Pending counts include reserved and locally held work, so an empty
//            published prefix cannot end the traversal. Producers never spin on
//            a global commit cursor. Exhausted bounds report incomplete work.
#include "Passes/Visibility/CullShared.hlsli"

struct NodeResult
{
    bool pushChildren, pushGroup, defer, waiting;
    uint2 item;  // the tested item (packItem: instance, node, view)
    ClusterNode node;
};

NodeResult testNode(uint2 it)
{
    NodeResult r = (NodeResult)0;
    r.item = it;
    StructuredBuffer<ClusterNode> nodes = ResourceDescriptorHeap[NODES_SRV];
    r.node = nodes[itemIndex(it)];
    const GpuInstance inst = loadInstance(itemInstance(it));
    const CullView v = loadView(itemView(it));
    const bool skinned = (inst.flags & (INSTANCE_SKINNED | INSTANCE_VIEW_MODEL)) != 0;  // untested bounds (A12 view models: remapped projection)
    const float4 s = worldSphere(inst, inst.objectToWorld, r.node.lodSphere);
    bool keep = skinned || frustumVisible(v, s);
    // C5: a node reaching a terrain patch's replaced rectangle is traversed down to the source clusters.
    keep = keep && (patchForcesSource(inst, r.node.lodSphere) || projectedError(v, s, r.node.lodError * instanceScale(inst)) > v.lodThreshold);
    const bool twoPhase = (v.flags & CULL_VIEW_TILE_TWO_PHASE) != 0;
    keep = keep && (skinned || (tileVisible(v, itemView(it), s) && (twoPhase || !tilesOcclude(v, TILE_MASK_SRV, s, false))));
    if (keep && !skinned && twoPhase)
    {
        // tile occluders in two phases (VisibilityCommon.hlsli tilesOcclude): as the HiZ's two phases below
#if PHASE == 1
        if (tilesOcclude(v, TILE_MASK_SRV, s, true))
        {
            keep = false;
            r.defer = true;
        }
#else
        keep = !tilesOcclude(v, TILE_MASK_SRV, s, false);
#endif
    }
    if (keep && !skinned && (v.flags & CULL_VIEW_OCCLUSION) != 0)
    {
#if PHASE == 1
        if (hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.prevViewProj, v.viewportSize, worldSphere(inst, inst.prevObjectToWorld, r.node.lodSphere)))
        {
            keep = false;
            r.defer = true;
        }
#else
        keep = !hizOccluded(HIZ_SRV, HIZ_MIPS, HIZ_SIZE, v.viewProj, v.viewportSize, s);
#endif
    }
    r.pushGroup = keep && r.node.leaf != 0;
    r.pushChildren = keep && r.node.leaf == 0;
    if (r.pushGroup && PAGE_TABLE != 0)
    {
        const uint page = clusterStreamPages(r.node.first).x;  // (a group's clusters share a page)
        pageWanted(page);
        r.waiting = !pageResident(page);
        r.pushGroup = !r.waiting;
    }
    return r;
}

// The group, deferral and statistics of a wave's tested nodes ('processed' of them; uniform control flow). QUEUE=1 keeps
// the cluster pass's arguments current with the groups it appends (one group per item past 'groupBegin', the phase's
// first group item; CullPrepare MODE=1 stores the same): the level passes cannot, their dispatch reads the arguments.
void emitNodes(RWByteAddressBuffer state, NodeResult r, uint processed, uint groupBegin)
{
    const uint g = waveAppend(state, VS_GROUP_WRITE, r.pushGroup ? 1 : 0, CAP_GROUPS, OVERFLOW_GROUPS);
    if (r.pushGroup && g < CAP_GROUPS)
    {
        RWStructuredBuffer<uint2> groups = ResourceDescriptorHeap[GROUP_ITEMS_UAV];
        groups[g] = r.item;
    }
#if QUEUE == 1
    const uint pushed = WaveActiveCountBits(r.pushGroup);
    if (pushed > 0 && WaveIsFirstLane())  // (the first lane's 'g' is the wave's first group item)
    {
        RWByteAddressBuffer args = ResourceDescriptorHeap[ARGS_UAV];
        raiseDispatch(args, VA_GROUPS, min(g + pushed, CAP_GROUPS) - min(groupBegin, CAP_GROUPS));
    }
#endif
    const uint d = waveAppend(state, VS_DEFER_NODES, r.defer ? 1 : 0, CAP_DEFERRED, OVERFLOW_DEFER_NODES);
    if (r.defer && d < CAP_DEFERRED)
    {
        RWStructuredBuffer<uint2> deferred = ResourceDescriptorHeap[DEFER_NODES_UAV];
        deferred[d] = r.item;
    }
    if (WaveIsFirstLane() && processed > 0) state.InterlockedAdd(4 * VS_STAT_NODES, processed);
    const uint waiting = WaveActiveCountBits(r.waiting);
    if (WaveIsFirstLane() && waiting > 0) state.InterlockedAdd(4 * VS_STREAM_WAITING, waiting);
}

#if QUEUE == 0
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    const uint begin = state.Load(4 * VS_NODE_BEGIN), end = state.Load(4 * VS_NODE_END);
    const uint item = begin + i;
    NodeResult r = (NodeResult)0;
    if (item < end)
    {
        RWStructuredBuffer<uint2> items = ResourceDescriptorHeap[NODE_ITEMS_UAV];
        r = testNode(items[item]);
    }
    const uint childBase = waveAppend(state, VS_NODE_WRITE, r.pushChildren ? r.node.count : 0, CAP_NODES, OVERFLOW_NODES);
    if (r.pushChildren)
    {
        RWStructuredBuffer<uint2> items = ResourceDescriptorHeap[NODE_ITEMS_UAV];
        for (uint k = 0; k < r.node.count; ++k)
            if (childBase + k < CAP_NODES) items[childBase + k] = packItem(itemInstance(r.item), r.node.first + k, itemView(r.item));
    }
    emitNodes(state, r, WaveActiveCountBits(item < end), 0);
}
#else
// A worker claims a range of reserved entries, then consumes the ready lanes in
// that range. Producers never wait for unrelated earlier reservations. Each
// lane's ready bit is published after its payload's device-memory barrier.
#define TRAVERSE_IDLE_ROUNDS 65536u
#define TRAVERSE_CLAIM_TRIES 4u
#define ROUND_CLAIMED 0u
#define ROUND_LOST 1u
#define ROUND_IDLE 2u
#define ROUND_DONE 3u

[numthreads(64, 1, 1)]
void main()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    globallycoherent RWByteAddressBuffer queue = ResourceDescriptorHeap[STATE_UAV];
    globallycoherent RWStructuredBuffer<uint2> items = ResourceDescriptorHeap[NODE_ITEMS_UAV];
    globallycoherent RWByteAddressBuffer readyBits = ResourceDescriptorHeap[NODE_READY_UAV];
    const uint lanes = WaveActiveCountBits(true), lane = WavePrefixCountBits(true);
    const uint groupBegin = state.Load(4 * VS_GROUP_BEGIN);
    uint item = 0, idle = 0;
    bool pending = false;
    [loop] for (uint round = 0; round < CAP_NODES + TRAVERSE_IDLE_ROUNDS; ++round)
    {
        if (!WaveActiveAnyTrue(pending))
        {
            uint start = 0, count = 0, outcome = ROUND_LOST;
            if (WaveIsFirstLane())
            {
                [loop] for (uint attempt = 0; attempt < TRAVERSE_CLAIM_TRIES && outcome == ROUND_LOST; ++attempt)
                {
                    const uint read = queue.Load(4 * VS_NODE_READ);
                    const uint reserved = min(queue.Load(4 * VS_NODE_WRITE), CAP_NODES);
                    if (read >= reserved)
                    {
                        const bool failed = (queue.Load(4 * VS_OVERFLOW) & OVERFLOW_ITERATION_LIMIT) != 0;
                        outcome = queue.Load(4 * VS_NODE_PENDING) == 0 || failed ? ROUND_DONE : ROUND_IDLE;
                    }
                    else
                    {
                        const uint n = min(lanes, reserved - read);
                        uint seen;
                        queue.InterlockedCompareExchange(4 * VS_NODE_READ, read, read + n, seen);
                        if (seen == read) { start = read; count = n; outcome = ROUND_CLAIMED; }
                    }
                }
            }
            outcome = WaveReadLaneFirst(outcome);
            if (outcome == ROUND_DONE) break;
            if (outcome != ROUND_CLAIMED)
            {
                if (outcome == ROUND_IDLE && ++idle >= TRAVERSE_IDLE_ROUNDS)
                {
                    if (WaveIsFirstLane()) queue.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);
                    break;
                }
                continue;
            }
            item = WaveReadLaneFirst(start) + lane;
            pending = lane < WaveReadLaneFirst(count);
        }
        bool ready = false;
        if (pending) ready = (readyBits.Load((item >> 5) * 4) & (1u << (item & 31u))) != 0;
        const uint processed = WaveActiveCountBits(ready);
        if (processed == 0)
        {
            if (++idle >= TRAVERSE_IDLE_ROUNDS)
            {
                if (WaveIsFirstLane()) queue.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT | OVERFLOW_NODE_PUBLICATION);
                break;
            }
            continue;
        }
        idle = 0;
        // Acquire payloads only after observing their publication bits.
        DeviceMemoryBarrier();
        NodeResult result = (NodeResult)0;
        if (ready) result = testNode(items[item]);
        pending = pending && !ready;
        uint first, total;
        const uint children = result.pushChildren ? result.node.count : 0u;
        const uint childBase = nodeReserve(state, children, CAP_NODES, processed, first, total);
        if (result.pushChildren)
        {
            for (uint child = 0; child < children; ++child)
                if (childBase + child < CAP_NODES)
                    items[childBase + child] = packItem(itemInstance(result.item), result.node.first + child, itemView(result.item));
        }
        nodePublishRange(childBase, children, false);
        emitNodes(state, result, processed, groupBegin);
    }
}
#endif
