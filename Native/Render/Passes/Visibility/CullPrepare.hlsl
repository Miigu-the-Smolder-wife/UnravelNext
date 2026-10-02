// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3,4,5
// Single-thread indirect-argument setup between cull passes (CullShared.hlsli root layout).
//   MODE=0: next traversal level: node items [previous end, written) -> VA_NODES (64 per group). Not with the node work
//           queue (CullNodes QUEUE=1: one dispatch, no arguments).
//   MODE=1: cluster pass: group items [previous end, written) -> VA_GROUPS (one group per item, 65535 per row). Not
//           with visibility.cull_pass_merge and the node work queue: CullNodes QUEUE=1 raises VA_GROUPS itself.
//   MODE=2: phase 2 inputs: deferred instances, deferred nodes (seed), deferred clusters (64 per group).
//   MODE=4: chunk instance pass: one group per visible chunk item (CullChunks PHASE=1) -> VA_CHUNK_ITEMS. Not with
//           visibility.cull_pass_merge: CullChunks PHASE=1 raises VA_CHUNK_ITEMS itself.
//   MODE=5: phase 2 chunk pass: deferred chunks -> VA_DEFERRED_CHUNKS (64 per group); runs before MODE=2, whose deferred
//           instance count then includes the members of the chunks that pass. Not with visibility.cull_pass_merge:
//           MODE=3 of phase 1 stores it.
//   MODE=3: DispatchMesh arguments of every list for phase CULL_PHASE (phase 1 snapshots its counts; phase 2 draws
//           the entries appended after them; the translucent lists: every entry so far) and the traversal completeness
//           check. Phase 1 also leaves what phase 2 starts from: VS_GROUP_BEGIN = phase 1's group items, VA_GROUPS
//           empty again, VA_DEFERRED_CHUNKS over the chunks phase 1 deferred (all final by now); and VA_PROXIES, the
//           proxy kernel's groups over the visible chunk items (DepthProxy.ms.hlsl: 4 groups of 64 members an item).
#include "Passes/Visibility/CullShared.hlsli"

void storeDispatch(RWByteAddressBuffer args, uint word, uint groups)
{
    args.Store3(4 * word, uint3(min(groups, 65535u), (groups + 65534u) / 65535u, 1));
}

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer state = ResourceDescriptorHeap[STATE_UAV];
    RWByteAddressBuffer args = ResourceDescriptorHeap[ARGS_UAV];
#if MODE == 0
    const uint begin = state.Load(4 * VS_NODE_END), end = min(state.Load(4 * VS_NODE_WRITE), CAP_NODES);
    state.Store(4 * VS_NODE_BEGIN, begin);
    state.Store(4 * VS_NODE_END, end);
    args.Store3(4 * VA_NODES, uint3((end - begin + 63) / 64, 1, 1));
#elif MODE == 1
    const uint begin = state.Load(4 * VS_GROUP_END), end = min(state.Load(4 * VS_GROUP_WRITE), CAP_GROUPS);
    state.Store(4 * VS_GROUP_BEGIN, begin);
    state.Store(4 * VS_GROUP_END, end);
    storeDispatch(args, VA_GROUPS, end - begin);
#elif MODE == 2
    args.Store3(4 * VA_DEFERRED_INSTANCES, uint3((min(state.Load(4 * VS_DEFER_INSTANCES), CAP_DEFERRED) + 63) / 64, 1, 1));
    args.Store3(4 * VA_SEED_NODES, uint3((min(state.Load(4 * VS_DEFER_NODES), CAP_DEFERRED) + 63) / 64, 1, 1));
    args.Store3(4 * VA_DEFERRED_CLUSTERS, uint3((min(state.Load(4 * VS_DEFER_CLUSTERS), CAP_DEFERRED) + 63) / 64, 1, 1));
#elif MODE == 4
    storeDispatch(args, VA_CHUNK_ITEMS, min(state.Load(4 * VS_CHUNK_ITEMS), CAP_DEFERRED));
#elif MODE == 5
    args.Store3(4 * VA_DEFERRED_CHUNKS, uint3((min(state.Load(4 * VS_DEFER_CHUNKS), CAP_DEFERRED) + 63) / 64, 1, 1));
#else
    for (uint k = 0; k < VS_LISTS; ++k)
    {
        const uint count = min(state.Load(4 * (VS_LIST_COUNT + k)), CAP_VISIBLE);
        uint base = 0;
        if (CULL_PHASE == 1) state.Store(4 * (VS_LIST_PHASE1 + k), count);
        else base = state.Load(4 * (VS_LIST_PHASE1 + k));
        storeDispatch(args, VA_MESH + 3 * k, k >= LIST_T_BACK ? count : count - base);  // translucent lists: drawn once, after phase 2
    }
    if (CULL_PHASE == 1)
    {
        state.Store(4 * VS_GROUP_BEGIN, min(state.Load(4 * VS_GROUP_WRITE), CAP_GROUPS));
        args.Store3(4 * VA_GROUPS, uint3(0, 0, 1));
        args.Store3(4 * VA_DEFERRED_CHUNKS, uint3((min(state.Load(4 * VS_DEFER_CHUNKS), CAP_DEFERRED) + 63) / 64, 1, 1));
        const uint proxyGroups = min(state.Load(4 * VS_CHUNK_ITEMS), CAP_DEFERRED) * (CHUNK_INSTANCES / 64u);
        args.Store3(4 * VA_PROXIES, uint3(min(proxyGroups, PROXY_DISPATCH_ROW), (proxyGroups + PROXY_DISPATCH_ROW - 1) / PROXY_DISPATCH_ROW, 1));
    }
    // (work queue: items still pending after its workers left; level passes: items appended by the last level)
    const bool complete = NODE_WORK_QUEUE != 0 ? state.Load(4 * VS_NODE_PENDING) == 0 : min(state.Load(4 * VS_NODE_WRITE), CAP_NODES) == state.Load(4 * VS_NODE_END);
    if (!complete) state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_NODE_DEPTH);
#endif
}
