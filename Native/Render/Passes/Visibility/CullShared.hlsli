// V internal: root-constant layout and helpers shared by the cull kernels. Owner: V.
//   P[0] views SRV, state UAV (raw), args UAV (raw), phase (1 | 2)
//   P[1] node items UAV (uint2), group items UAV (uint2), visible UAV (uint2: instance, cluster | view << 24), lists UAV (raw)
//   P[2] deferred instances UAV (uint), deferred nodes UAV (uint2), deferred clusters UAV (uint2), HiZ SRV
//   P[3] HiZ mips, HiZ width, HiZ height, instance mask
//   P[4] cluster nodes SRV, mesh roots SRV, cluster LOD spheres SRV, tile mask SRV (UNX_NONE = none)
//   P[5] capacity: node items, group items, visible (= each list), deferred items
//   P[6] view count, instance count, band mode (0: everything band A, 1: classify), unused
//   P[7] asfloat: band A minimum width px, band C maximum width px, unused, unused
#ifndef UNX_CULL_SHARED_HLSLI
#define UNX_CULL_SHARED_HLSLI
#include "Passes/Visibility/VisibilityCommon.hlsli"

#define VIEWS_SRV P[0].x
#define STATE_UAV P[0].y
#define ARGS_UAV P[0].z
#define CULL_PHASE P[0].w
#define NODE_ITEMS_UAV P[1].x
#define GROUP_ITEMS_UAV P[1].y
#define VISIBLE_UAV P[1].z
#define LISTS_UAV P[1].w
#define DEFER_INSTANCES_UAV P[2].x
#define DEFER_NODES_UAV P[2].y
#define DEFER_CLUSTERS_UAV P[2].z
#define HIZ_SRV P[2].w
#define HIZ_MIPS P[3].x
#define HIZ_SIZE P[3].yz
#define INSTANCE_MASK P[3].w
#define NODES_SRV P[4].x
#define ROOTS_SRV P[4].y
#define LOD_SPHERES_SRV P[4].z
#define TILE_MASK_SRV P[4].w
#define CAP_NODES P[5].x
#define CAP_GROUPS P[5].y
#define CAP_VISIBLE P[5].z
#define CAP_DEFERRED P[5].w
#define VIEW_COUNT P[6].x
#define INSTANCE_COUNT P[6].y
#define BAND_MODE P[6].z
#define BAND_A_MIN_PX asfloat(P[7].x)
#define BAND_C_MAX_PX asfloat(P[7].y)

#define OVERFLOW_NODES 1u
#define OVERFLOW_GROUPS 2u
#define OVERFLOW_VISIBLE 4u
#define OVERFLOW_DEFER_INSTANCES 8u
#define OVERFLOW_DEFER_NODES 16u
#define OVERFLOW_DEFER_CLUSTERS 32u
#define OVERFLOW_NODE_DEPTH 64u  // node items left unprocessed after the last traversal iteration

CullView loadView(uint view)
{
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[VIEWS_SRV];
    return views[view];
}

// Wave-aggregated append of 'n' entries per lane to a counter word; returns this lane's first index. Must be called
// from uniform control flow (every lane of the wave). Entries at or beyond 'capacity' set 'overflowBit'.
uint waveAppend(RWByteAddressBuffer state, uint word, uint n, uint capacity, uint overflowBit)
{
    const uint total = WaveActiveSum(n);
    const uint prefix = WavePrefixSum(n);
    uint base = 0;
    if (total > 0 && WaveIsFirstLane()) state.InterlockedAdd(4 * word, total, base);
    base = WaveReadLaneFirst(base);
    if (total > 0 && base + total > capacity && WaveIsFirstLane()) state.InterlockedOr(4 * VS_OVERFLOW, overflowBit);
    return base + prefix;
}

#endif
