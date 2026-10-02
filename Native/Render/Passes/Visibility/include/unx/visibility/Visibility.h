#pragma once
// V (visibility) public C++ API for tests, gates and tools. The frame entry points are in Tracks.h; this header only
// exposes what V measured, so gates can check the design quantities (triangles per band, visible clusters) and the
// capacities.
#include "unx/render/Frame.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct ID3D12Resource;

namespace unx::visibility
{
struct Stats
{
    uint64_t frameIndex = UINT64_MAX;  // frame these counts belong to (read back framesInFlight frames later)
    uint32_t instancesVisible = 0;     // instances that reached the hierarchy traversal (both phases)
    uint32_t nodesTested = 0;
    uint32_t clustersTested = 0;
    uint32_t visibleClusters = 0;
    uint32_t triangles[3] = {};        // visible triangles classified band A, B, C
    uint32_t bandClusters[3] = {};     // visible clusters classified band A, B, C (visibleClusters counts list entries)
    uint32_t listEntries[8] = {};      // draw-list entries (A back, A two-sided, A alpha back, A alpha two-sided, B, C,
                                       // translucent back, translucent two-sided);
                                       // tile-local raster runs: (cluster, tile rectangle) pairs
    uint32_t tilePairs = 0;            // tile-local raster runs: pairs drawn (DepthRasterRequest::tileLocal)
    uint32_t deferredInstances = 0, deferredNodes = 0, deferredClusters = 0;  // phase 1 -> phase 2
    uint32_t coverageFragments = 0;    // coverage layer fragments appended (main view)
    uint32_t coverageTiles = 0;        // 8 x 8 tiles with coverage records
    uint32_t coverageBlocks = 0;       // blocks of 1,024 records over the listed tiles
    uint32_t coverageHeavyTiles = 0;   // tiles of more than one block
    uint32_t coveragePoolRecords = 0;  // the frame's record capacity
    uint32_t coverageSpecial = 0;      // special records (hair, streams) listed for their owners' shading (v1.73)
    uint32_t oceanEdges = 0;           // ocean edge pixels listed for W (v1.73)
    uint32_t coverageMeasured = 0;     // fragments of the raster measurement stages (nothing stored)
    uint32_t coverageInvocations = 0;  // coverage pixel kernel invocations (visibility.coverage_debug_stage != 0 only)
    // Where the coverage layer's work went (the band B list of the main view; coverageFragments = the fragments stored).
    uint32_t coverageClustersBehindBandA = 0;   // list entries behind band A (the final HiZ): not drawn (depth buckets)
    uint32_t coverageClustersBehindCover = 0;   // list entries behind the tiles' opaque coverage of nearer buckets: not drawn
    uint32_t coverageTriangles = 0;             // triangles the hardware rasteriser took (band B and translucent lists)
    uint32_t coverageTrianglesBehindBandA = 0;  // triangles the mesh kernel culled behind band A
    uint32_t coverageTrianglesBehindCover = 0;  // triangles the mesh kernel culled behind the tiles' opaque coverage
    uint32_t coverageTrianglesCompute = 0;      // triangles the compute rasteriser took (visibility.coverage_compute_raster)
    uint32_t coverageFragmentsCompute = 0;      // fragments it stored (part of coverageFragments)
    // visibility.coverage_statistics only (per-wave counters in the fragment kernels; 0 otherwise):
    uint32_t coverageEvaluated = 0;             // fragments with area in their pixel, before the tests
    uint32_t coverageCutBandA = 0;              // ... dropped behind band A
    uint32_t coverageCutCover = 0;              // ... dropped behind the opaque coverage of nearer depth buckets
    uint32_t coverageCutAlpha = 0;              // ... cut out whole by the alpha test
    uint32_t coverageCutWeight = 0;             // ... dropped for having no weight (no area step and no subsample)
    uint32_t mixedClusters = 0;        // sheet clusters drawn in both rasters, split per triangle (counted in bandClusters[1])
    uint32_t mixedTriangles = 0;       // their triangles (counted in triangles[1]'s cluster rule: see the mesh kernels)
    uint32_t chunkItems = 0;           // C3: (chunk, view) items that passed chunk culling in phase 1
    uint32_t deferredChunks = 0;       // C3: chunks occluded against the previous HiZ (retested in phase 2)
    uint32_t overflow = 0;             // capacity bits (0 = every list fit); nonzero means geometry was dropped
                                       // (0x100: the coverage record pool ran out; it grows from the next
                                       // completed frame)
    uint32_t overflowSeen = 0;         // 'overflow' of every frame of the run read back so far, OR-ed (gates)
    // visibility.software_raster: clusters and triangles the compute rasteriser drew in this run (the main view's vis
    // buffer; a raster request's atlas pages, and the set tiles it listed for its target).
    uint32_t softwareClusters = 0, softwareTriangles = 0, softwareTiles = 0;
    uint32_t visibleCapacity = 0;      // the run's visible-list capacity now (visibility.visible_clusters_follow_need)
    // visibility.cluster_streaming: clusters this run drew in place of a finer group whose page is not resident, and
    // the groups its cut wanted that are not resident (each requested).
    uint32_t streamStandIns = 0, streamWaiting = 0;
};
// Stats::overflow bits of the pools that grow from the measured need (a frame over one is expected after a cut: the
// coverage records, the special record list, the ocean edge list); every other bit is a capacity or a shader loop bound.
constexpr uint32_t kOverflowGrowingPools = 0x100u | 0x2000u | 0x4000u;

// Cluster streaming (visibility.cluster_streaming; the reference's streaming manager). The cook puts the vertex bits of
// the compressed clusters' groups into pages (clusterbuilder::StreamPages; visibility.cluster_compression); the owner of
// the renderer writes them to a page file, opens a page streamer on it and gives V this source (unx/visibility/
// ClusterPages.h does the three with C's streamer; V's module does not link it). Each frame V reads which pages the
// cuts of an earlier frame wanted (a word per page the cull kernels stamp), requests those and what they depend on for
// cluster_streaming_keep_frames frames, lets the source update, and gives the frame's kernels a page table: a group
// whose page is not resident is not drawn and the clusters simplified from it are, in its place. The pool's budget,
// the upload per frame and the eviction (least recently requested, never a page a frame in flight reads) are the
// source's.
struct ClusterPageSource
{
    uint32_t pageCount = 0;                 // pages of the installed cluster data (StreamPages::pages)
    uint32_t slotBytes = 0, heapSlots = 0;  // the pool: slot s is in heap s / heapSlots at byte (s % heapSlots) x slotBytes
    std::function<void(uint32_t page, float priority)> request;  // this frame's need (higher priority first)
    std::function<void(uint64_t frameIndex)> update;             // once a frame, after the requests
    std::function<uint32_t(uint32_t page)> residentSlot;         // UINT32_MAX: not resident
    std::function<uint32_t()> heapCount;
    std::function<ID3D12Resource*(uint32_t heap)> heapBuffer;    // null while the heap is evicted
};
// Installs the source for the cluster data the scene holds (after GpuScene::setClusters, and again after another one).
void setClusterPageSource(render::TrackState& trackState, ClusterPageSource source);
struct ClusterStreamStats
{
    uint32_t pages = 0;        // 0: no streaming
    uint32_t wanted = 0;       // pages requested this frame (wanted by a cut within the keep frames, and their dependencies)
    uint32_t resident = 0;     // pages the frame's table gives as resident (with their dependencies)
};
ClusterStreamStats clusterStreamStats(render::TrackState& trackState);

// Statistics of the latest frame whose readback has completed (frameIndex = UINT64_MAX before the first): "main" for
// the main view, "view<id>" for an auxiliary full view (A14, ViewResources::viewId), "secondary" for the last planar
// reflection view of the frame, or a depth-raster request's name (DepthRasterRequest::name) for that request's cull run.
Stats latestStats(render::TrackState& state, const std::string& run = "main");
// The latest statistics of every run recorded so far, by name (gates: the raster requests' runs beside the main view's).
std::vector<std::pair<std::string, Stats>> latestStatsOfRuns(render::TrackState& state);
} // namespace unx::visibility
