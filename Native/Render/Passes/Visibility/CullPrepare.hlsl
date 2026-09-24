// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2,3
// Single-thread indirect-argument setup between cull passes (CullShared.hlsli root layout).
//   MODE=0: next traversal level: node items [previous end, written) -> VA_NODES (64 per group).
//   MODE=1: cluster pass: group items [previous end, written) -> VA_GROUPS (one group per item, 65535 per row).
//   MODE=2: phase 2 inputs: deferred instances, deferred nodes (seed), deferred clusters (64 per group).
//   MODE=3: DispatchMesh arguments of every list for phase CULL_PHASE (phase 1 snapshots its counts; phase 2 draws
//           the entries appended after them) and the traversal completeness check.
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
#else
    for (uint k = 0; k < VS_LISTS; ++k)
    {
        const uint count = min(state.Load(4 * (VS_LIST_COUNT + k)), CAP_VISIBLE);
        uint base = 0;
        if (CULL_PHASE == 1) state.Store(4 * (VS_LIST_PHASE1 + k), count);
        else base = state.Load(4 * (VS_LIST_PHASE1 + k));
        storeDispatch(args, VA_MESH + 3 * k, count - base);
    }
    if (min(state.Load(4 * VS_NODE_WRITE), CAP_NODES) != state.Load(4 * VS_NODE_END)) state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_NODE_DEPTH);
#endif
}
