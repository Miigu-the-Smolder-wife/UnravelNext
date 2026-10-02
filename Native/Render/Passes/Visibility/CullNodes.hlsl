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
//   QUEUE=1: the whole traversal in one dispatch of a fixed number of groups (visibility.traversal_worker_groups), the
//            node items a work queue (the reference's persistent cull: NaniteHierarchyTraversal.ush). Each wave of a
//            group is a worker: it claims up to a wave of published items (compare-exchange on VS_NODE_READ, below
//            VS_NODE_COMMIT), tests them, appends the children (nodeReserve: VS_NODE_WRITE, and VS_NODE_PENDING before
//            they are stored) and publishes them in reservation order (compare-exchange on VS_NODE_COMMIT from the
//            wave's first entry to its end, after the entries are in memory), so every item below VS_NODE_COMMIT is
//            stored when a worker reads it. A worker with nothing to claim waits while VS_NODE_PENDING is not 0 (items
//            another worker holds or is about to publish) and leaves when it is 0: no item is left and none can come.
//            Bounds (INTERFACES 3.6): a worker runs at most CAP_NODES + TRAVERSE_IDLE_ROUNDS rounds - a round claims at
//            least one of at most CAP_NODES items, loses its claim to another worker, or finds nothing published (at
//            most TRAVERSE_IDLE_ROUNDS times) - and a publish waits at most TRAVERSE_PUBLISH_SPINS exchanges for the
//            waves that reserved before it (each stores at most a wave of nodes' children first). A worker at its round
//            bound leaves without holding an item: the others go on, and one that pushed children claims them itself,
//            so the queue still empties; a publish at its bound sets OVERFLOW_ITERATION_LIMIT and its items stay
//            pending. Items left when every worker has gone show as VS_NODE_PENDING != 0 (CullPrepare MODE=3:
//            OVERFLOW_NODE_DEPTH).
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
#define TRAVERSE_IDLE_ROUNDS 65536u    // rounds a worker may find nothing published before it leaves
#define TRAVERSE_CLAIM_TRIES 4u        // compare-exchanges of one round's claim
#define TRAVERSE_PUBLISH_SPINS 65536u  // compare-exchanges a publish waits for the waves that reserved before it

#define ROUND_CLAIMED 0u   // the wave holds [start, start + count)
#define ROUND_LOST 1u      // items were published, other workers took them
#define ROUND_IDLE 2u      // nothing published; items are pending
#define ROUND_DONE 3u      // nothing published and nothing pending (or a publish failed: the queue cannot empty)

[numthreads(64, 1, 1)]
void main()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    // The queue's words and items as other waves of the dispatch write them while this one reads: coherent views of the
    // same buffers (with the barrier before a publish).
    globallycoherent RWByteAddressBuffer queue = ResourceDescriptorHeap[STATE_UAV];
    globallycoherent RWStructuredBuffer<uint2> items = ResourceDescriptorHeap[NODE_ITEMS_UAV];
    const uint lanes = WaveActiveCountBits(true), lane = WavePrefixCountBits(true);  // every exit below is uniform over the wave
    const uint groupBegin = state.Load(4 * VS_GROUP_BEGIN);  // (the phase's first group item: fixed while the traversal runs)
    uint idle = 0;
    [loop] for (uint round = 0; round < CAP_NODES + TRAVERSE_IDLE_ROUNDS; ++round)
    {
        uint start = 0, count = 0, outcome = ROUND_LOST;
        if (WaveIsFirstLane())
        {
            [loop] for (uint attempt = 0; attempt < TRAVERSE_CLAIM_TRIES && outcome == ROUND_LOST; ++attempt)
            {
                const uint read = queue.Load(4 * VS_NODE_READ);
                const uint ready = min(queue.Load(4 * VS_NODE_COMMIT), CAP_NODES);
                if (read >= ready)
                {
                    const bool failed = (queue.Load(4 * VS_OVERFLOW) & OVERFLOW_ITERATION_LIMIT) != 0;
                    outcome = queue.Load(4 * VS_NODE_PENDING) == 0 || failed ? ROUND_DONE : ROUND_IDLE;
                }
                else
                {
                    const uint n = min(lanes, ready - read);
                    uint seen;
                    queue.InterlockedCompareExchange(4 * VS_NODE_READ, read, read + n, seen);
                    if (seen == read)
                    {
                        start = read;
                        count = n;
                        outcome = ROUND_CLAIMED;
                    }
                }
            }
        }
        start = WaveReadLaneFirst(start);
        count = WaveReadLaneFirst(count);
        outcome = WaveReadLaneFirst(outcome);
        if (outcome == ROUND_DONE) break;
        if (outcome == ROUND_IDLE && ++idle >= TRAVERSE_IDLE_ROUNDS) break;
        if (outcome != ROUND_CLAIMED) continue;

        NodeResult r = (NodeResult)0;
        if (lane < count) r = testNode(items[start + lane]);
        // The children: reserved and counted as pending in the step that retires this wave's 'count' items, then stored,
        // then published in reservation order.
        uint first, total;
        const uint childBase = nodeReserve(state, r.pushChildren ? r.node.count : 0, CAP_NODES, count, first, total);
        if (r.pushChildren)
        {
            for (uint k = 0; k < r.node.count; ++k)
                if (childBase + k < CAP_NODES) items[childBase + k] = packItem(itemInstance(r.item), r.node.first + k, itemView(r.item));
        }
        if (total > 0)
        {
            DeviceMemoryBarrier();
            if (WaveIsFirstLane())
            {
                bool published = false;
                [loop] for (uint spin = 0; spin < TRAVERSE_PUBLISH_SPINS && !published; ++spin)
                {
                    uint seen;
                    queue.InterlockedCompareExchange(4 * VS_NODE_COMMIT, first, first + total, seen);
                    published = seen == first;
                }
                if (!published) queue.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_ITERATION_LIMIT);
            }
        }
        emitNodes(state, r, count, groupBegin);
    }
}
#endif
