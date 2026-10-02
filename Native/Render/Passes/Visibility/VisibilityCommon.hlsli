// V internal: cull views, cull state layout, culling math (frustum, LOD, HiZ occlusion, bands). Owner: V.
// C++ mirror: Passes/Visibility/VisibilityInternal.h (sizes and word offsets checked there).
#ifndef UNX_VISIBILITY_COMMON_HLSLI
#define UNX_VISIBILITY_COMMON_HLSLI
#include "Bindless.hlsli"
#include "Deformation.hlsli"
#include "Scene.hlsli"
#include "Passes/Visibility/ClusterHierarchy.hlsli"
#include "Passes/ViewModel/ViewModel.hlsli"

// One view of a cull run (main view: one; depth raster service: one per RasterView). 384 B.
struct CullView
{
    row_major float4x4 viewProj;
    row_major float4x4 prevViewProj;  // phase 1 occlusion against the previous frame's HiZ
    float4 planes[6];                  // world frustum planes, normalised (keep dot(n, p) + w >= 0)
    float4 clipPlane;                  // world clip plane (keep >= 0); all zero = none
    float3 position;                   // camera position (perspective)
    float lodScale;                    // perspective: pixels per metre at distance 1; orthographic: pixels per metre
    float lodThreshold;                // pixels
    uint orthographic;
    float nearPlane;
    uint flags;                        // CULL_VIEW_*
    float2 viewportSize;               // pixels
    float2 viewportOffset;
    float4 viewDirection;              // orthographic: world direction the view looks along (xyz)
    uint cullMaskOffset;               // tile mask words of this view (UNX_NONE = no mask)
    uint userData;
    uint tilesX;                       // ceil(viewport width / tilePx)
    uint tilePx;
    float3 prevPosition;               // the previous frame's camera position (band hysteresis; = position after a cut)
    float bandAMinPx;                  // visibility.band_a_min_width_px
    float bandAHysteresisPx;           // visibility.band_a_hysteresis_px: band B stays B below this if it was B last frame
    uint cullSceneSrv;                 // CullScene (instance chunks, flat list, skinned bounds) of the run
    uint runtimeFirst, runtimeCount;   // C2b: runtime instances [first, first + count) follow the flat list
    uint gpuFirst, gpuCapacity;        // GPU-written instances (A3 mesh particles) after them; live count gpuInstanceCount()
    uint instanceFirst, instanceEnd;   // RasterView's instance batch: only these scene instances (instanceEnd 0: every instance)
    float minInstancePx;               // RasterView::minInstanceTexels: instances whose bounds project to a smaller radius are
                                       // not drawn into the view (0: every instance)
    uint instanceSet;                  // RasterView::instanceSet: 0 every instance, 1 the ones that are not movable, 2 the movable
                                       // ones (instanceInSet)
    uint occluderSrv, occluderSlotsSrv; // CULL_VIEW_TILE_OCCLUDERS: the request's tile occluders and atlas slots (raw SRVs)
};

// The view's instance set (RasterView::instanceSet): movable = INSTANCE_MOVABLE_FLAGS, a bone palette, morph or terrain
// patch, or an instance past the scene's uploaded ones (run-time and GPU-written: v.runtimeFirst on). Judged per
// instance from its record of this frame; chunks are not skipped by set (a member can become movable - a terrain patch
// set at run time - without its chunk being rebuilt), so a view of the movable set tests the members of every chunk it
// sees.
bool instanceInSet(CullView v, GpuInstance inst, uint instance)
{
    if (v.instanceSet == 0) return true;
    const bool movable = (inst.flags & INSTANCE_MOVABLE_FLAGS) != 0 || inst.bonePalette != UNX_NONE || inst.morph != UNX_NONE || inst.patch != UNX_NONE ||
                         instance >= v.runtimeFirst;
    return (v.instanceSet == 2) == movable;
}

// Live count of the GPU-written instances (GpuScene::gpuInstanceRange; GpuSceneLayout.h kGpuInstanceCountElement).
uint gpuInstanceCount(CullView v)
{
    if (v.gpuCapacity == 0) return 0;
    StructuredBuffer<uint4> slots = ResourceDescriptorHeap[g_patchData];
    return min(slots[64 * 35].x, v.gpuCapacity);
}

// Instance hierarchy and skinned bounds of a cull run (C3; VisibilityTrack refreshScene / skinBoundsPass). Static
// instances (not dynamic, not skinned) are grouped by 64 m cell and cut into chunks of at most CHUNK_INSTANCES; a chunk's
// sphere bounds its members' world spheres (worldSphere, wind included; ChunkBounds.hlsl at each scene revision; static
// instances never move, the contract R's static TLAS and S's page caching rely on too). Every other instance is in the
// flat list. Skinned instances have a world sphere per frame (SkinBounds.hlsl) over the palette-transformed spheres of
// the joints that influence their vertices.
struct CullScene
{
    uint chunkSrv, chunkInstancesSrv, chunkCount, flatSrv;
    uint flatCount, skinBoundsSrv, skinListSrv, skinCount;
};

struct CullChunk
{
    float4 sphere;  // world; radius < 0 until ChunkBounds ran
    uint first, count, pad0, pad1;  // pad0: the members' largest wind inflation per (m/s)^2 (float bits, ChunkBounds.hlsl)
};

#define CHUNK_INSTANCES 256u

CullScene loadCullScene(uint srv)
{
    StructuredBuffer<CullScene> b = ResourceDescriptorHeap[srv];
    return b[0];
}

// Slot of 'instance' in the run's sorted skinned-instance list, UNX_NONE when absent (binary search, <= 32 steps).
uint skinSlot(CullScene cs, uint instance)
{
    if (cs.skinCount == 0) return UNX_NONE;
    StructuredBuffer<uint> list = ResourceDescriptorHeap[cs.skinListSrv];
    uint lo = 0, hi = cs.skinCount;
    [loop] for (uint it = 0; it < 32 && lo < hi; ++it)
    {
        const uint mid = (lo + hi) >> 1;
        if (list[mid] < instance) lo = mid + 1;
        else hi = mid;
    }
    return lo < cs.skinCount && list[lo] == instance ? lo : UNX_NONE;
}

#define CULL_VIEW_OCCLUSION 1u   // HiZ occlusion (two-phase) for this view
#define CULL_VIEW_CULL_BACK 2u   // back faces of one-sided materials are culled (cone test allowed)
#define CULL_VIEW_TILE_SINGLE 4u // tile-local pairs are single tiles (DepthRasterRequest atlas mode: one slot per tile)
#define CULL_VIEW_TILE_OCCLUDERS 8u  // tested against the request's tile occluders (tilesOcclude)

// Cull state words (RWByteAddressBuffer, 4 B each).
#define VS_NODE_WRITE 0u
#define VS_NODE_BEGIN 1u
#define VS_NODE_END 2u
#define VS_GROUP_WRITE 3u
#define VS_GROUP_BEGIN 4u     // first group item of the phase's cluster pass (0; phase 2: phase 1's end)
#define VS_VISIBLE 5u
#define VS_DEFER_INSTANCES 6u
#define VS_DEFER_NODES 7u
#define VS_DEFER_CLUSTERS 8u
#define VS_LIST_COUNT 9u      // + list (VS_LISTS lists, words 9 .. 16): entries appended so far (both phases)
#define VS_COV_SPECIAL 17u    // special coverage records appended by the scatter (may exceed the capacity: the need)
#define VS_OCEAN_EDGES 18u    // ocean edge pixels listed for W (OceanEdges.hlsl; may exceed the capacity: the need)
#define VS_COV_CUT_BAND_A 19u // coverage fragments behind band A (visibility.coverage_statistics; CoverageFragment.hlsli)
#define VS_COV_CUT_COVER 20u  // ... behind the opaque coverage of the depth buckets drawn before theirs
#define VS_OVERFLOW 21u       // bits: capacity exceeded (OVERFLOW_*)
#define VS_STAT_INSTANCES 22u // instances that reached the node pass
#define VS_STAT_NODES 23u     // node items processed
#define VS_STAT_CLUSTERS 24u  // clusters tested
#define VS_STAT_TRIANGLES 25u // + band (3): triangles of visible clusters per band A, B, C
#define VS_GROUP_END 28u      // CullPrepare MODE=1 only: the group items its arguments covered
#define VS_TILE_PAIRS 29u     // tile-local raster: (cluster, tile rectangle) pairs appended
#define VS_COV_POOL 30u       // coverage record capacity of the frame (records; CoverageBuild MODE 2, for the statistics)
#define VS_COV_INVOCATIONS 31u // coverage pixel kernel invocations (measurement stages only)
#define VS_COV_FRAGMENTS 32u  // coverage layer (CoverageLayer.hlsli): fragments appended by the raster (may exceed the
                              // capacity: the need, for the pool sizing)
#define VS_COV_TILES 33u      // tiles with records (tile list entries)
#define VS_COV_MEASURED 34u   // fragments of the raster measurement stages (nothing stored)
#define VS_STAT_BAND_CLUSTERS 35u  // + band (3): visible clusters per band A, B, C (mixed sheet clusters count as B)
#define VS_COV_BLOCKS 38u     // blocks of COV_BLOCK records over the listed tiles
#define VS_COV_HEAVY 39u      // listed tiles of more than one block
#define VS_STAT_MIXED_CLUSTERS 40u  // sheet clusters drawn in both rasters, split per triangle by the mesh kernels
#define VS_STAT_MIXED_TRIANGLES 41u // their triangles
#define VS_CHUNK_ITEMS 42u    // (chunk, view) items of the chunks that passed CullChunks in this phase
#define VS_DEFER_CHUNKS 43u   // chunks occluded against the previous HiZ in phase 1 (tested again in phase 2)
#define VS_STAT_CHUNKS 44u    // chunk items expanded to their instances (both phases)
#define VS_NODE_COMMIT 45u    // node work queue (CullNodes QUEUE=1): node items [0, min(this, capacity)) are stored
#define VS_NODE_READ 46u      // node work queue: node items claimed by the traversal's workers
#define VS_NODE_PENDING 47u   // node work queue: items kept in the queue whose processing has not finished (0: the
                              // traversal is complete)
#define VS_COV_CUT_WEIGHT 64u // coverage fragments of no weight: area under half a record step and no subsample (statistics)
#define VS_COV_CUT_ALPHA 65u  // ... cut out whole by the alpha test (statistics)
#define VS_COV_CLUSTERS_HIZ 66u   // band B list entries behind band A (the final HiZ; CoverageBins.hlsl): not drawn
#define VS_LIST_PHASE1 48u    // + list (words 48 .. 55): entries of phase 1 (snapshot)
#define VS_COV_CLUSTERS_TILE 56u  // band B list entries behind the tiles' opaque coverage (depth buckets): not drawn
#define VS_COV_TRIANGLES 57u      // coverage triangles the rasteriser took (CoverageRaster.ms: the band B list and the
                                  // translucent lists; not culled by the mesh kernel)
#define VS_COV_TRIANGLES_HIZ 58u  // ... culled by the mesh kernel: behind band A over the triangle's rectangle
#define VS_COV_TRIANGLES_TILE 59u // ... culled by the mesh kernel: behind the tiles' opaque coverage
#define VS_COV_TRIANGLES_SW 60u   // ... rasterised by the compute kernel (CoverageRasterSw.hlsl)
#define VS_COV_FRAGMENTS_SW 61u   // fragments the compute kernel appended (counted in VS_COV_FRAGMENTS too)
#define VS_COV_EVALUATED 62u      // coverage fragments with area in their pixel, before the tests (statistics)
                                  // word 63: unused
#define VS_SW_CLUSTERS 67u        // clusters the software rasteriser drew (visibility.software_raster: RasterBins.hlsl's
                                  // classification and VisRasterSw.hlsl; CullClusters' and DepthRasterSw.hlsl)
#define VS_SW_TRIANGLES 68u       // their triangles that reached the pixel loop (not culled, some pixel under the rectangle)
#define VS_SW_TILES 69u           // raster service: set tiles of the request's views (each wants a software page; may
                                  // exceed the page capacity: the need)
                                  // words 70 .. 71: unused
#define VS_WORDS 72u  // (words 64 .. 66: the coverage statistics above; kStateWords, and the readback slot holds them: kReadbackBytes)

#define VS_LISTS 8u
#define LIST_A_BACK 0u        // band A, opaque, back faces culled
#define LIST_A_NONE 1u        // band A, opaque, two-sided (or cull none requested)
#define LIST_A_ALPHA_BACK 2u  // band A, alpha tested
#define LIST_A_ALPHA_NONE 3u
#define LIST_B 4u             // coverage layer
#define LIST_C 5u             // aggregate bricks
#define LIST_T_BACK 6u        // translucent layer (A6, v1.67): band A width glass and water clusters of the main view with
#define LIST_T_NONE 7u        // the coverage layer on, back faces culled / two-sided; drawn after both phases (all entries)
#define LIST_SW LIST_T_BACK   // raster requests (band mode A: no translucent lists) with visibility.software_raster: the
                              // clusters the software rasteriser draws (CullClusters softwareCluster, DepthRasterSw.hlsl)
#define VS_BAND_LISTS 6u      // lists of the cull bands (the depth raster service draws these)
#define VS_A_LISTS 4u         // lists drawn by the vis buffer raster: 0 .. VS_A_LISTS - 1

// Indirect argument words (3 per dispatch).
#define VA_NODES 0u
#define VA_GROUPS 3u
#define VA_DEFERRED_CLUSTERS 6u
#define VA_DEFERRED_INSTANCES 9u
#define VA_SEED_NODES 12u
#define VA_GPU_INSTANCES 15u  // phase 1 over the live GPU-written instances (CullReset: ceil(live / 64) x views)
                              // words 18 .. 32: unused
#define VA_COV_MESH 33u       // coverage raster: every band B list entry (both phases)
#define VA_COV_CLEAR 36u      // tile clear over last frame's coverage tiles (one group per tile)
#define VA_COV_RECORDS 39u    // count and scatter over the stored stream entries (one group per COV_BLOCK)
#define VA_CHUNK_ITEMS 42u    // instance pass over visible chunk items (one group per item)
#define VA_DEFERRED_CHUNKS 45u // phase 2 chunk pass over the deferred chunks (64 per group)
#define VA_MESH 48u           // + 3 * list (words 48 .. 71); the translucent lists: all entries (set in phase 1 and 2)
#define VA_COV_T_MESH 72u     // + 3 * (list - LIST_T_BACK): coverage raster over the translucent lists (A6 records)
#define VA_WORDS 78u

// Overflow bits (VS_OVERFLOW): a capacity was exceeded; the run's statistics report them (Stats::overflow).
#define OVERFLOW_NODES 1u
#define OVERFLOW_GROUPS 2u
#define OVERFLOW_VISIBLE 4u
#define OVERFLOW_DEFER_INSTANCES 8u
#define OVERFLOW_DEFER_NODES 16u
#define OVERFLOW_DEFER_CLUSTERS 32u
#define OVERFLOW_NODE_DEPTH 64u        // node items left unprocessed after the last traversal iteration (work queue: after
                                       // its workers left)
#define OVERFLOW_TILE_PAIRS 128u
#define OVERFLOW_COVERAGE 256u         // the coverage record pool ran out (its fragments are lost; the pool grows)
#define OVERFLOW_COVERAGE_DEPTH 512u   // unused since v1.41 (was: a coverage tile past its extension tree)
#define OVERFLOW_ITERATION_LIMIT 1024u // a data-dependent shader loop reached its hard bound (INTERFACES 3.6)
#define OVERFLOW_CHUNK_ITEMS 2048u     // visible chunk items past the deferred-item capacity (their instances are lost)
#define OVERFLOW_DEFER_CHUNKS 4096u    // deferred chunks past the capacity
#define OVERFLOW_COVERAGE_SPECIAL 8192u // special record list past its capacity (entries lost; the capacity grows)
#define OVERFLOW_OCEAN_EDGES 16384u    // ocean edge pixel list past its capacity (pixels lost; the capacity grows)

// Wave-aggregated append of 'n' entries per lane to a counter word; returns this lane's first index. Must be called
// from uniform control flow (every active lane of the wave). Entries at or beyond 'capacity' set 'overflowBit'.
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

// Node queue append (the hierarchy traversal's items; CullNodes.hlsl). Wave-aggregated like waveAppend: 'n' entries per
// lane, returns this lane's first index; 'first' and 'total' are the wave's (uniform). The entries kept (below the
// capacity) are counted in VS_NODE_PENDING before they are stored, less 'retired' (uniform over the wave: the items the
// wave finished with these appends, 0 outside the traversal), so the count never reads 0 while entries are still to come.
// After storing them the caller publishes them: nodePublish in the passes that run before the traversal, the
// traversal's own ordered publish while its workers read the queue. Uniform control flow.
uint nodeReserve(RWByteAddressBuffer state, uint n, uint capacity, uint retired, out uint first, out uint total)
{
    total = WaveActiveSum(n);
    const uint prefix = WavePrefixSum(n);
    uint base = 0;
    if (WaveIsFirstLane())
    {
        if (total > 0)
        {
            state.InterlockedAdd(4 * VS_NODE_WRITE, total, base);
            if (base + total > capacity) state.InterlockedOr(4 * VS_OVERFLOW, OVERFLOW_NODES);
        }
        const uint kept = min(base + total, capacity) - min(base, capacity);
        if (kept != retired) state.InterlockedAdd(4 * VS_NODE_PENDING, kept - retired);  // (two's complement: may subtract)
    }
    first = WaveReadLaneFirst(base);
    return first + prefix;
}

// Publishes a wave's node items [first, first + total) from a pass that runs before the traversal (nothing reads the
// queue meanwhile): the queue's end is the largest published end, which is VS_NODE_WRITE once the pass is done.
void nodePublish(RWByteAddressBuffer state, uint first, uint total)
{
    if (total > 0 && WaveIsFirstLane()) state.InterlockedMax(4 * VS_NODE_COMMIT, first + total);
}

uint packItem(uint index, uint view) { return index | (view << 24); }
uint itemIndex(uint packed) { return packed & 0xFFFFFFu; }
uint itemView(uint packed) { return packed >> 24; }

float instanceScale(GpuInstance inst) { return length(inst.objectToWorld[0].xyz); }

// Object-space sphere -> world sphere, inflated by the wind displacement bound.
float4 worldSphere(GpuInstance inst, float4 rows[3], float4 objectSphere)
{
    const float scale = instanceScale(inst);
    const float wind = windOffsetBound(inst, objectSphere.xyz, objectSphere.w);
    // C4: blend shapes / vertex animation move vertices by at most morphRadius (object space) from the bind pose.
    return float4(transformPoint(rows, objectSphere.xyz), (objectSphere.w + wind + inst.morphRadius) * scale);
}

bool frustumVisible(CullView v, float4 s)
{
    [unroll] for (uint i = 0; i < 6; ++i)
        if (dot(v.planes[i].xyz, s.xyz) + v.planes[i].w < -s.w) return false;
    if (any(v.clipPlane != 0) && dot(v.clipPlane.xyz, s.xyz) + v.clipPlane.w < -s.w) return false;
    return true;
}

// C5 terrain deformation patches (GpuScene::setPatchRegion, GpuSceneLayout.h kPatchSlotElements): a terrain tile
// instance whose blocks are replaced by patch meshes. (1) Every simplified cluster (own error > 0) whose own LOD sphere
// reaches the replaced rectangle is not drawn, and every hierarchy node whose sphere reaches it is traversed: the cut
// there is the source clusters. The leaf node of a group and the clusters simplified from that group test the same
// sphere, so the forced cut stays consistent (no overlap, no gap) and it is monotone up the hierarchy (parents enclose
// children). (2) Source triangles whose object-space centroid is in a replaced block are dropped (a patch block's
// boundary is on source cell edges, so every source triangle is wholly in or out).
bool patchForcesSource(GpuInstance inst, float4 objectSphere)
{
    if (inst.patch == UNX_NONE) return false;
    StructuredBuffer<uint4> slots = ResourceDescriptorHeap[g_patchData];
    const float4 rect = asfloat(slots[inst.patch * 35 + 1]);
    const float2 d = max(max(rect.xy - objectSphere.xz, objectSphere.xz - rect.zw), 0.0);
    return dot(d, d) <= objectSphere.w * objectSphere.w;
}

bool patchReplaced(GpuInstance inst, float3 objectCentroid)
{
    if (inst.patch == UNX_NONE) return false;
    StructuredBuffer<uint4> slots = ResourceDescriptorHeap[g_patchData];
    const uint base = inst.patch * 35;
    const float4 grid = asfloat(slots[base]);
    const uint side = slots[base + 2].x;
    const float2 b = floor((objectCentroid.xz - grid.xy) / grid.zw);
    if (any(b < 0) || any(b >= (float)side)) return false;
    const uint k = (uint)b.y * side + (uint)b.x;
    const uint4 words = slots[base + 3 + k / 128];
    const uint w = (k / 32) % 4;
    const uint word = w == 0 ? words.x : w == 1 ? words.y : w == 2 ? words.z : words.w;
    return ((word >> (k % 32)) & 1u) != 0;
}

// A source triangle of a patched instance: its centroid from the mesh's object-space positions.
bool patchDropsTriangle(GpuInstance inst, GpuMesh mesh, GpuCluster cl, uint3 tri)
{
    if (inst.patch == UNX_NONE || cl.lodError > 0) return false;
    StructuredBuffer<uint> clusterVertices = ResourceDescriptorHeap[g_clusterVertexIndices];
    const float3 a = loadVertex(mesh, clusterVertices[cl.vertexOffset + tri.x]).position;
    const float3 b = loadVertex(mesh, clusterVertices[cl.vertexOffset + tri.y]).position;
    const float3 c = loadVertex(mesh, clusterVertices[cl.vertexOffset + tri.z]).position;
    return patchReplaced(inst, (a + b + c) / 3.0);
}

// Screen-space error in pixels of an object-space error measured on a world sphere.
float projectedError(CullView v, float4 s, float worldError)
{
    if (worldError >= 3.0e38) return 3.0e38;
    if (v.orthographic) return worldError * v.lodScale;
    const float d = max(length(s.xyz - v.position) - s.w, v.nearPlane);
    return worldError * v.lodScale / d;
}

// Distance-free projected size in pixels of a world length at the sphere (for bands).
// Projected silhouette width of a sheet triangle seen from 'eye' (COVERAGE_REDESIGN 14.9 correction 3, request 16: the
// band definition per element): the triangle's smallest altitude x pixels per metre / the centroid's distance x |cos| of
// the angle between the view ray to the centroid and the triangle's normal.
float sheetTriangleWidthPx(CullView v, float3 a, float3 b, float3 c, float3 eye)
{
    const float3 n = cross(b - a, c - a);
    const float twiceArea = length(n);
    const float longest = sqrt(max(dot(b - a, b - a), max(dot(c - b, c - b), dot(a - c, a - c))));
    if (!(twiceArea > 0) || !(longest > 0)) return 0;
    const float altitude = twiceArea / longest;
    if (v.orthographic) return altitude * v.lodScale * abs(dot(n, v.viewDirection.xyz)) / twiceArea;
    const float3 ray = (a + b + c) * (1.0 / 3.0) - eye;
    const float d = max(length(ray), v.nearPlane);
    return altitude * v.lodScale / d * abs(dot(n, ray)) / (twiceArea * d);
}

// Band B for a triangle of a mixed sheet cluster: narrower than band A's minimum, or (hysteresis (a)) narrower than the
// hysteresis width and band B from the previous camera. Both mesh kernels call this with the same deformed vertices, so
// every triangle is drawn by exactly one of them.
bool sheetTriangleBandB(CullView v, float3 a, float3 b, float3 c)
{
    const float now = sheetTriangleWidthPx(v, a, b, c, v.position);
    if (now < v.bandAMinPx) return true;
    if (now >= v.bandAHysteresisPx) return false;
    return sheetTriangleWidthPx(v, a, b, c, v.prevPosition) < v.bandAMinPx;
}

// List entries carry the visible index; a mixed sheet cluster (drawn in a band A list and in the coverage list, split
// per triangle) has LIST_ENTRY_MIXED set.
#define LIST_ENTRY_MIXED 0x80000000u
// A list entry the software rasteriser draws (visibility.software_raster; set by RasterBins.hlsl in the sorted lists of a
// full view): the mesh kernel leaves it out. (A raster request's software clusters have a list of their own, LIST_SW.)
#define LIST_ENTRY_SOFTWARE 0x40000000u
#define LIST_ENTRY_FLAGS (LIST_ENTRY_MIXED | LIST_ENTRY_SOFTWARE)
// A raster request's software pages (DepthRasterSw.hlsl): a word per bit of the request's tile mask.
#define SW_PAGE_NONE 0xFFFFFFFFu  // the tile is not set
#define SW_PAGE_FULL 0xFFFFFFFEu  // the tile is set and has no software page (past the capacity): its clusters stay hardware

float projectedLength(CullView v, float4 s, float worldLength)
{
    if (v.orthographic) return worldLength * v.lodScale;
    const float d = max(length(s.xyz - v.position) - s.w, v.nearPlane);
    return worldLength * v.lodScale / d;
}

// An instance too small for the view (RasterView::minInstanceTexels): its bounding sphere's radius projects to under the
// view's minimum, taken at the sphere's nearest point (the largest it can appear).
bool instanceBelowView(CullView v, float4 bounds) { return v.minInstancePx > 0 && projectedLength(v, bounds, bounds.w) < v.minInstancePx; }

// Screen rectangle (pixels, inclusive) and nearest device depth of a world sphere under viewProj; false when the
// sphere's box reaches the near plane (then it can never be occluded).
bool projectSphere(row_major float4x4 viewProj, float2 viewportSize, float4 s, out float4 rect, out float nearestDepth)
{
    float2 lo = 1e30, hi = -1e30;
    nearestDepth = 0;
    rect = 0;
    [unroll] for (uint k = 0; k < 8; ++k)
    {
        const float3 corner = s.xyz + s.w * float3((k & 1) ? 1 : -1, (k & 2) ? 1 : -1, (k & 4) ? 1 : -1);
        const float4 clip = mul(viewProj, float4(corner, 1));
        if (clip.w <= 1e-6 || clip.z > clip.w) return false;  // behind the camera or in front of the near plane
        const float3 ndc = clip.xyz / clip.w;
        lo = min(lo, ndc.xy);
        hi = max(hi, ndc.xy);
        nearestDepth = max(nearestDepth, ndc.z);  // reversed Z: nearer = larger
    }
    // NDC -> pixels (y down).
    rect = float4((lo.x * 0.5 + 0.5) * viewportSize.x, (0.5 - hi.y * 0.5) * viewportSize.y, (hi.x * 0.5 + 0.5) * viewportSize.x, (0.5 - lo.y * 0.5) * viewportSize.y);
    return true;
}

// HiZ: texel (i, j) of mip m holds the farthest (minimum reversed-Z) depth of pixels [i 2^(m+1), (i+1) 2^(m+1)) x ...
bool hizOccluded(uint hizSrv, uint hizMips, uint2 hizSize, row_major float4x4 viewProj, float2 viewportSize, float4 s)
{
    float4 rect;
    float nearest;
    if (!projectSphere(viewProj, viewportSize, s, rect, nearest)) return false;
    rect = clamp(rect, 0, float4(viewportSize, viewportSize) - 1);
    if (rect.z < rect.x || rect.w < rect.y) return false;
    // Level where the rectangle spans at most 2 x 2 texels: texels are 2^(m+1) pixels.
    // A segment no longer than the texel size touches at most two texels: 2^(m+1) >= extent.
    const float extent = max(rect.z - rect.x, rect.w - rect.y);
    const uint mip = min((uint)max(0.0, ceil(log2(max(extent, 1.0))) - 1.0), hizMips - 1);
    const uint shift = mip + 1;
    const uint2 a = uint2(rect.xy) >> shift, b = uint2(rect.zw) >> shift;
    const uint2 size = (hizSize + (1u << mip) - 1) >> mip;  // mip sizes round up (HiZ.hlsl)
    Texture2D<float> hiz = ResourceDescriptorHeap[hizSrv];
    float farthest = 1;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const uint2 p = min(uint2((k & 1) ? b.x : a.x, (k & 2) ? b.y : a.y), size - 1);
        farthest = min(farthest, hiz.Load(int3(p, mip)));
    }
    return nearest < farthest;
}

// Tile range [a, b] (inclusive tiles) of a sphere in a raster-service view: the whole viewport when the sphere reaches
// the eye plane; false when its rectangle misses the viewport.
bool tileRange(CullView v, float4 s, out uint2 a, out uint2 b)
{
    const uint tilesY = ((uint)v.viewportSize.y + v.tilePx - 1) / v.tilePx;
    a = 0;
    b = uint2(v.tilesX, tilesY) - 1;
    float4 rect;
    float nearest;
    if (!projectSphere(v.viewProj, v.viewportSize, s, rect, nearest)) return true;
    rect = clamp(rect, 0, float4(v.viewportSize, v.viewportSize) - 1);
    if (rect.z < rect.x || rect.w < rect.y) return false;
    a = uint2(rect.xy) / v.tilePx;
    b = min(uint2(rect.zw) / v.tilePx, b);
    return true;
}

// Set bits of word w of a bit mask starting at word 'offset', restricted to bit positions [lo, hi]; 0 for words
// outside the range.
uint tileMaskBits(ByteAddressBuffer mask, uint offset, uint w, uint lo, uint hi)
{
    if (w * 32 > hi || w * 32 + 31 < lo) return 0;
    const uint first = max(lo, w * 32) - w * 32, last = min(hi, w * 32 + 31) - w * 32;
    return mask.Load(4 * (offset + w)) & (0xFFFFFFFFu >> (31 - last)) & (0xFFFFFFFFu << first);
}

// Tile rectangle of a tile-local pair: x0 | y0 << 16, x1 | y1 << 16 (inclusive tiles).
uint2 packTileRect(uint2 a, uint2 b) { return uint2(a.x | (a.y << 16), b.x | (b.y << 16)); }

// Tile mask of one run (DepthRasterRequest::cullMask) and its coarse summary (TileMaskCoarse.hlsl: bit per 8 x 8
// tiles, coarseWords words per view).
struct TileMasks
{
    uint fine, coarse, coarseWords;
};

#define TILE_VISIT_COUNT 0u
#define TILE_VISIT_WRITE 1u

// Visits the set tiles of the range [a, b] of view 'view': coarse cells with a set bit, then the row segments of each
// such cell. Pairs (tile-local raster, DepthRasterRequest::tileLocal) are the runs of consecutive set tiles of a row
// segment, or one pair for the whole range when every tile of it is set; with CULL_VIEW_TILE_SINGLE (atlas mode) every
// set tile is a pair of its own.
//   TILE_VISIT_COUNT: returns the pair count (0 = no set tile under the range) and sets 'whole'.
//   TILE_VISIT_WRITE: writes the pairs counted before ('whole' as counted): pair j = uint3(visibleIndex, tile rectangle)
//                     at pairBase + j, and list slot + j points at it (entries at or beyond capacity are dropped: the
//                     appends already flagged the overflow).
uint tileVisit(uint mode, CullView v, uint view, TileMasks masks, uint2 a, uint2 b, inout bool whole, uint visibleIndex, uint pairBase, uint slot, uint list,
               uint capacity, uint pairsUav, uint listsUav)
{
    if (mode == TILE_VISIT_COUNT)
    {
        whole = true;
        if (v.cullMaskOffset == UNX_NONE) return 1;  // no mask: the whole range
    }
    if (mode == TILE_VISIT_WRITE && whole)
    {
        if (pairBase < capacity && slot < capacity)
        {
            RWStructuredBuffer<uint3> pairs = ResourceDescriptorHeap[pairsUav];
            RWByteAddressBuffer lists = ResourceDescriptorHeap[listsUav];
            pairs[pairBase] = uint3(visibleIndex, packTileRect(a, b));
            lists.Store(4 * (list * capacity + slot), pairBase);
        }
        return 1;
    }
    ByteAddressBuffer mask = ResourceDescriptorHeap[masks.fine];
    ByteAddressBuffer coarse = ResourceDescriptorHeap[masks.coarse];
    const bool single = (v.flags & CULL_VIEW_TILE_SINGLE) != 0;
    const uint coarseX = (v.tilesX + 7) / 8;
    const uint2 ca = a >> 3, cb = b >> 3;
    uint set = 0, pairs = 0;
    for (uint cy = ca.y; cy <= cb.y; ++cy)
    {
        const uint cl = cy * coarseX + ca.x, ch = cy * coarseX + cb.x;
        for (uint cw = cl >> 5; cw <= (ch >> 5); ++cw)
        {
            uint cells = tileMaskBits(coarse, view * masks.coarseWords, cw, cl, ch);
            while (cells != 0)
            {
                const uint cx = cw * 32 + firstbitlow(cells) - cy * coarseX;
                cells &= cells - 1;
                const uint2 lo = max(a, uint2(cx, cy) * 8), hi = min(b, uint2(cx, cy) * 8 + 7);
                for (uint y = lo.y; y <= hi.y; ++y)
                {
                    const uint l = y * v.tilesX + lo.x, h = y * v.tilesX + hi.x;
                    uint previous = 0, start = 0;
                    for (uint w = l >> 5; w <= (h >> 5); ++w)
                    {
                        const uint m = tileMaskBits(mask, v.cullMaskOffset, w, l, h);
                        const uint starts = m & ~((m << 1) | (previous >> 31));  // set bits whose predecessor is clear
                        if (mode == TILE_VISIT_COUNT)
                        {
                            set += countbits(m);
                            pairs += countbits(single ? m : starts);
                        }
                        else
                        {
                            const uint next = tileMaskBits(mask, v.cullMaskOffset, w + 1, l, h);
                            const uint ends = single ? m : m & ~((m >> 1) | (next << 31));  // set bits whose successor is clear
                            uint events = starts | ends;
                            while (events != 0)
                            {
                                const uint bit = firstbitlow(events);
                                events &= events - 1;
                                const uint x = w * 32 + bit - y * v.tilesX;
                                if (single || (starts & (1u << bit)) != 0) start = x;
                                if (ends & (1u << bit))
                                {
                                    if (pairBase + pairs < capacity && slot + pairs < capacity)
                                    {
                                        RWStructuredBuffer<uint3> pairBuffer = ResourceDescriptorHeap[pairsUav];
                                        RWByteAddressBuffer lists = ResourceDescriptorHeap[listsUav];
                                        pairBuffer[pairBase + pairs] = uint3(visibleIndex, packTileRect(uint2(start, y), uint2(x, y)));
                                        lists.Store(4 * (list * capacity + slot + pairs), pairBase + pairs);
                                    }
                                    ++pairs;
                                }
                            }
                        }
                        previous = m;
                    }
                }
            }
        }
    }
    if (mode == TILE_VISIT_COUNT)
    {
        whole = !single && set == (b.x - a.x + 1) * (b.y - a.y + 1);
        if (whole) pairs = 1;
    }
    return pairs;
}

// True when a tile of the range [a, b] of view 'view' is set. Stops at the first one: a set coarse cell that lies
// wholly inside the range answers at once (its bit is the OR of its 8 x 8 tiles); a cell cut by the range edge is
// checked row by row. Instance and node tests need only this, not tileVisit's pair count over every set tile.
bool tileAnySet(CullView v, uint view, TileMasks masks, uint2 a, uint2 b)
{
    ByteAddressBuffer mask = ResourceDescriptorHeap[masks.fine];
    ByteAddressBuffer coarse = ResourceDescriptorHeap[masks.coarse];
    const uint coarseX = (v.tilesX + 7) / 8;
    const uint2 ca = a >> 3, cb = b >> 3;
    for (uint cy = ca.y; cy <= cb.y; ++cy)
    {
        const uint cl = cy * coarseX + ca.x, ch = cy * coarseX + cb.x;
        for (uint cw = cl >> 5; cw <= (ch >> 5); ++cw)
        {
            uint cells = tileMaskBits(coarse, view * masks.coarseWords, cw, cl, ch);
            while (cells != 0)
            {
                const uint cx = cw * 32 + firstbitlow(cells) - cy * coarseX;
                cells &= cells - 1;
                const uint2 cellLo = uint2(cx, cy) * 8, cellHi = cellLo + 7;
                const uint2 lo = max(a, cellLo), hi = min(b, cellHi);
                if (all(lo == cellLo) && all(hi == cellHi)) return true;
                for (uint y = lo.y; y <= hi.y; ++y)
                {
                    const uint l = y * v.tilesX + lo.x, h = y * v.tilesX + hi.x;
                    for (uint w = l >> 5; w <= (h >> 5); ++w)
                        if (tileMaskBits(mask, v.cullMaskOffset, w, l, h) != 0) return true;
                }
            }
        }
    }
    return false;
}

// Tile occluders of a raster-service view (DepthRasterRequest::tileOccluders, CULL_VIEW_TILE_OCCLUDERS): true when the
// sphere is hidden in every set tile under it - in each, its nearest device depth is farther than the farthest depth
// stored over the blocks its rectangle touches in the tile (341 floats per atlas slot: blocks of tilePx / 16 pixels, then
// of 2, 4, 8 and 16 times that; read on the block level where the rectangle spans at most 2 x 2 blocks; 0 = a pixel with
// nothing stored: not hidden). False (not hidden) without occluders, when the sphere reaches the view's near plane, or
// when it spans more than 2 x 2 tiles. Reversed Z: nearer = larger.
bool tilesOcclude(CullView v, uint maskSrv, float4 s)
{
    if ((v.flags & CULL_VIEW_TILE_OCCLUDERS) == 0 || v.cullMaskOffset == UNX_NONE || maskSrv == UNX_NONE) return false;
    float4 rect;
    float nearest;
    if (!projectSphere(v.viewProj, v.viewportSize, s, rect, nearest)) return false;
    rect = clamp(rect, 0, float4(v.viewportSize, v.viewportSize) - 1);
    if (rect.z < rect.x || rect.w < rect.y) return false;
    const uint2 p0 = uint2(rect.xy), p1 = uint2(rect.zw);
    const uint2 a = p0 / v.tilePx, b = p1 / v.tilePx;
    if (b.x - a.x > 1 || b.y - a.y > 1) return false;
    ByteAddressBuffer mask = ResourceDescriptorHeap[maskSrv];
    ByteAddressBuffer slots = ResourceDescriptorHeap[v.occluderSlotsSrv];
    ByteAddressBuffer occluders = ResourceDescriptorHeap[v.occluderSrv];
    const uint blockPx = max(v.tilePx / 16, 1u);
    bool any = false;
    [unroll] for (uint t = 0; t < 4; ++t)
    {
        const uint2 tile = a + uint2(t & 1, t >> 1);
        if (tile.x > b.x || tile.y > b.y) continue;
        const uint i = tile.y * v.tilesX + tile.x;
        if (((mask.Load(4 * (v.cullMaskOffset + (i >> 5))) >> (i & 31u)) & 1u) == 0) continue;  // not drawn: nothing to hide from
        any = true;
        const uint slot = slots.Load(4 * (v.cullMaskOffset * 32 + i));
        // the rectangle inside the tile, in blocks; the level where it spans at most 2 x 2 of them
        const uint2 origin = tile * v.tilePx;
        const uint2 lo = (max(p0, origin) - origin) / blockPx, hi = (min(p1, origin + v.tilePx - 1) - origin) / blockPx;
        uint m = 0;
        [unroll] for (uint q = 0; q < 4; ++q)
            if ((hi.x >> m) - (lo.x >> m) > 1 || (hi.y >> m) - (lo.y >> m) > 1) ++m;
        const uint perTile = 16u >> m;
        const uint offset = m == 0 ? 0u : m == 1 ? 256u : m == 2 ? 320u : m == 3 ? 336u : 340u;
        const uint2 b0 = min(lo >> m, perTile - 1), b1 = min(hi >> m, perTile - 1);
        float farthest = 1;
        [unroll] for (uint c = 0; c < 4; ++c)
        {
            const uint2 blk = uint2((c & 1) ? b1.x : b0.x, (c & 2) ? b1.y : b0.y);
            farthest = min(farthest, asfloat(occluders.Load(4 * (slot * 341 + offset + blk.y * perTile + blk.x))));
        }
        if (!(nearest < farthest)) return false;  // it can show in this tile
    }
    return any;
}

// Tile mask of a raster-service view: true when the sphere's viewport rectangle covers a set tile (or no mask).
bool tileMaskCovered(CullView v, uint view, TileMasks masks, float4 s)
{
    if (v.cullMaskOffset == UNX_NONE) return true;
    uint2 a, b;
    if (!tileRange(v, s, a, b)) return false;
    return tileAnySet(v, view, masks, a, b);
}

#endif
