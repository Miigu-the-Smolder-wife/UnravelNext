#pragma once
// V (visibility) public C++ API for tests, gates and tools. The frame entry points are in Tracks.h; this header only
// exposes what V measured, so gates can check the design quantities (triangles per band, visible clusters) and the
// capacities.
#include "unx/render/Frame.h"

#include <cstdint>
#include <string>

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
    uint32_t listEntries[6] = {};      // draw-list entries (A back, A two-sided, A alpha back, A alpha two-sided, B, C);
                                       // tile-local raster runs: (cluster, tile rectangle) pairs
    uint32_t tilePairs = 0;            // tile-local raster runs: pairs drawn (DepthRasterRequest::tileLocal)
    uint32_t deferredInstances = 0, deferredNodes = 0, deferredClusters = 0;  // phase 1 -> phase 2
    uint32_t coverageFragments = 0;    // coverage layer fragments appended (main view)
    uint32_t coverageTiles = 0;        // 8 x 8 tiles with coverage records
    uint32_t coverageBlocks = 0;       // blocks of 1,024 records over the listed tiles
    uint32_t coverageHeavyTiles = 0;   // tiles of more than one block
    uint32_t coveragePoolRecords = 0;  // the frame's record capacity
    uint32_t coverageMeasured = 0;     // fragments of the raster measurement stages (nothing stored)
    uint32_t coverageInvocations = 0;  // coverage pixel kernel invocations (visibility.coverage_debug_stage != 0 only)
    uint32_t mixedClusters = 0;        // sheet clusters drawn in both rasters, split per triangle (counted in bandClusters[1])
    uint32_t mixedTriangles = 0;       // their triangles (counted in triangles[1]'s cluster rule: see the mesh kernels)
    uint32_t chunkItems = 0;           // C3: (chunk, view) items that passed chunk culling in phase 1
    uint32_t deferredChunks = 0;       // C3: chunks occluded against the previous HiZ (retested in phase 2)
    uint32_t overflow = 0;             // capacity bits (0 = every list fit); nonzero means geometry was dropped
                                       // (0x100: the coverage record pool ran out; it grows from the next
                                       // completed frame)
};

// Statistics of the latest frame whose readback has completed (frameIndex = UINT64_MAX before the first): "main" for
// the main view, "secondary" for the last secondary view (planar reflection) of the frame, or a depth-raster
// request's name (DepthRasterRequest::name) for that request's cull run.
Stats latestStats(render::TrackState& state, const std::string& run = "main");
} // namespace unx::visibility
