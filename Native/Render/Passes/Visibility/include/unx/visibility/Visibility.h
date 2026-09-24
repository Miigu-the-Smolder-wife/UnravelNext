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
    uint32_t listEntries[6] = {};      // draw-list entries (A back, A two-sided, A alpha back, A alpha two-sided, B, C)
    uint32_t deferredInstances = 0, deferredNodes = 0, deferredClusters = 0;  // phase 1 -> phase 2
    uint32_t overflow = 0;             // capacity bits (0 = every list fit); nonzero means geometry was dropped
};

// Statistics of the latest frame whose readback has completed (frameIndex = UINT64_MAX before the first): "main" for
// the main view, or a depth-raster request's name (DepthRasterRequest::name) for that request's cull run.
Stats latestStats(render::TrackState& state, const std::string& run = "main");
} // namespace unx::visibility
