#pragma once
// The surface cache's direct light as (cell, light) pairs (surface_cache.direct_pairs; Passes/SurfaceCache/
// SurfaceCacheLightPairs.hlsli states the passes; owner A for S2's surface cache). It replaces the r.sc.cells pass
// (SurfaceCacheLight.hlsl SurfaceCacheCellsGen: every light of a cell evaluated and traced inside one ray generation
// thread) by three passes in which no thread traces more than one ray: r.sc.pairs.select (compute, no ray),
// r.sc.pairs.trace (one shadow ray per pair, bands of at most 262,144 threads), r.sc.pairs.store (compute).
#include "unx/render/Frame.h"

#include <functional>

namespace unx::render::refl
{
struct SurfaceCachePairsInputs
{
    BufferRef surfaceCache;
    uint32_t budget = 0;      // cells relit this frame (ReflectionSystem: slots / surface_cache.direct_update_factor)
    uint32_t frame = 0;       // the cache's frame word (P[0].z of its passes)
    uint32_t lightFlags = 0;  // P[0].w of SurfaceCacheLight.hlsl (bits 0, 1, 5, 7, 8, 10, 11 are read here)
    uint32_t skyVariant = 0;  // 0: atmosphere LUTs, 1: constant sky (the SKY variant of the caller's kernels)
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;
    // The caller's shared declarations and root constants of its surface cache passes (sky and sun: words 4..14; the
    // ray scene: words 24..31). Words 0..3 and 16..19 are written here.
    std::function<void(PassBuilder&)> declareShared;
    std::function<void(PassContext&, uint32_t*)> sharedConstants;
};
void recordSurfaceCacheLightPairs(FramePassContext& fc, const SurfaceCachePairsInputs& in);
} // namespace unx::render::refl
