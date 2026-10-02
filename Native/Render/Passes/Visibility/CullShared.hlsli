// V internal: root-constant layout and helpers shared by the cull kernels. Owner: V.
//   P[0] views SRV, state UAV (raw), args UAV (raw), phase (1 | 2)
//   P[1] node items UAV (uint2), group items UAV (uint2), visible UAV (uint2: packItem of instance, cluster, view), lists
//        UAV (raw)
//   P[2] deferred instances UAV (uint2: instance, view), deferred nodes UAV (uint2), deferred clusters UAV (uint2), HiZ SRV
//   P[3] HiZ mips (bits 0-4) | chunk work UAV << 5 (uint2 (chunk, view): CullChunks items and deferred chunks), HiZ width,
//        HiZ height, instance mask
//   P[4] cluster nodes SRV, mesh roots SRV, cluster LOD spheres SRV, tile mask SRV (UNX_NONE = none)
//   P[5] capacity: node items, group items, visible (= each list), deferred items
//   P[6] view count, instance count, band mode (BAND_MODE_*, bits 0-7) | TILE_STORED_PAIRS << 8 | NODE_WORK_QUEUE << 9,
//        tile pairs UAV (uint3; UNX_NONE = not tile-local: list entries are visible indices; else they index the pairs,
//        VisibilityCommon.hlsli)
//   P[7] band A minimum width px | band C maximum width px << 16 (f16 each), cluster sheets SRV (float4), coarse tile mask
//        SRV (TileMaskCoarse.hlsl), its words per view
//   P[8] raster requests with the software rasteriser (visibility.software_raster; DepthRasterSw.hlsl): the tiles' software
//        pages SRV (raw: a word per mask bit, SW_PAGE_*; UNX_NONE = none in this run), the largest cluster rectangle in
//        pixels the software rasteriser takes (float) - read by CullClusters only; cluster streaming
//        (visibility.cluster_streaming): the frame's page table's SRV + 1 (StructuredBuffer<uint2>, ClusterStream.hlsli;
//        0: none - every page counts as resident), the page feedback's UAV + 1 (raw: a word per page, the frame index + 1
//        of the last frame a cut wanted the page) - read by CullNodes and CullClusters.
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
#define HIZ_MIPS (P[3].x & 31u)
#define CHUNK_WORK_UAV (P[3].x >> 5)
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
#define BAND_MODE (P[6].z & 0xFFu)
#define TILE_STORED_PAIRS ((P[6].z >> 8) & 1u)  // visibility.raster_amplification false: the stored pair list (A/B)
#define NODE_WORK_QUEUE ((P[6].z >> 9) & 1u)    // visibility.traversal_work_queue: the traversal is CullNodes QUEUE=1
#define TILE_PAIRS_UAV P[6].w
#define BAND_A_MIN_PX f16tof32(P[7].x)
#define BAND_C_MAX_PX f16tof32(P[7].x >> 16)
#define SHEETS_SRV P[7].y
#define TILE_COARSE_SRV P[7].z
#define TILE_COARSE_WORDS P[7].w
#define SW_PAGES_SRV P[8].x
#define SW_REQUEST_LIMIT_PX asfloat(P[8].y)
#define PAGE_TABLE P[8].z
#define PAGE_FEEDBACK P[8].w

// Band modes of a cull run: which list a cluster of each band is drawn from.
#define BAND_MODE_A 0u         // every band in the band A lists (raster service, secondary views)
#define BAND_MODE_COVERAGE 1u  // bands B and C in the coverage layer list (LIST_B) until the band C bricks exist
#define BAND_MODE_FULL 2u      // band B in LIST_B, band C in LIST_C
#define BAND_MODE_C_VISIBLE 3u // band B in LIST_B, band C in the band A lists: the visibility buffer's one sample a pixel
                               // (a sliver under a quarter pixel wide covers a pixel centre as often as its coverage: the
                               // temporal upscale averages it - what the reference does with every sub-pixel triangle)

CullView loadView(uint view)
{
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[VIEWS_SRV];
    return views[view];
}

TileMasks tileMasks()
{
    TileMasks m;
    m.fine = TILE_MASK_SRV;
    m.coarse = TILE_COARSE_SRV;
    m.coarseWords = TILE_COARSE_WORDS;
    return m;
}

// A raster-service view whose tile mask has no tile set (its coarse words all 0): every cluster of it fails the tile
// test (CullClusters), so nothing of it is drawn; the instance and chunk passes drop it at once (static frames with the
// shadow page caches leave every local-light view empty). False without a mask. Uniform over a group (one view).
bool cullViewTilesEmpty(CullView v, uint view)
{
    if (TILE_MASK_SRV == UNX_NONE || TILE_COARSE_SRV == UNX_NONE || v.cullMaskOffset == UNX_NONE) return false;
    ByteAddressBuffer coarse = ResourceDescriptorHeap[TILE_COARSE_SRV];
    uint any = 0;
    [loop] for (uint w = 0; w < TILE_COARSE_WORDS; ++w) any |= coarse.Load(4 * (view * TILE_COARSE_WORDS + w));
    return any == 0;
}

// Raises the dispatch arguments at 'word' to 'groups' groups (65535 per row, as CullPrepare's storeDispatch). The kernel
// that appends a pass's items keeps that pass's arguments current with it, so no pass stands between the two
// (visibility.cull_pass_merge); CullReset leaves such arguments at (0, 0, 1). Both words only grow with the item count.
void raiseDispatch(RWByteAddressBuffer args, uint word, uint groups)
{
    args.InterlockedMax(4 * word, min(groups, 65535u));
    args.InterlockedMax(4 * (word + 1), (groups + 65534u) / 65535u);
}

// Cluster streaming: whether a page is resident this frame (UNX_NONE - always resident - and every page without a table).
bool pageResident(uint page)
{
    if (page == UNX_NONE || PAGE_TABLE == 0) return true;
    StructuredBuffer<uint2> table = ResourceDescriptorHeap[PAGE_TABLE - 1];
    return table[page].x != UNX_NONE;
}

// ... and the note that a cut wanted it this frame (resident or not: the page's use, and its request).
void pageWanted(uint page)
{
    if (page == UNX_NONE || PAGE_FEEDBACK == 0) return;
    RWByteAddressBuffer feedback = ResourceDescriptorHeap[PAGE_FEEDBACK - 1];
    feedback.Store(4 * page, g_frameIndex + 1);
}

// Raster-service tile mask test of a bounding sphere (true without a mask).
bool tileVisible(CullView v, uint view, float4 s) { return TILE_MASK_SRV == UNX_NONE || tileMaskCovered(v, view, tileMasks(), s); }

#endif
