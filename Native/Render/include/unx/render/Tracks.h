#pragma once
// Track entry points (INTERFACES_KO.md 5.2). Core's FrameRenderer calls them in the order of ARCHITECTURE 4.1; each
// track implements its functions in its own folder. The signatures are fixed; the passes inside are the track's.
#include "unx/render/Frame.h"

namespace unx::render::tracks
{
// Entry points a track has not implemented yet call this: it logs once per entry and the entry declares no passes.
void pending(const char* entry);

// ---- V: visibility (core session) - Native/Render/Passes/Visibility, Tools/ClusterBuilder
// Culling (two phase), band A/B/C classification, band A vis buffer + depth, HiZ, coverage layer (bands B/C).
// Writes view.depth, visId, visibleClusters, hiz, coverageFragments, coverageHeads.
void visibility(FramePassContext& fc, ViewResources& view);
// Service behind FrameServices::rasterizeDepth.
void rasterizeDepth(FramePassContext& fc, const DepthRasterRequest& request);

// ---- M: material and shading (M session) - Passes/Material, Passes/Shading
void materialResolve(FramePassContext& fc, ViewResources& view);  // writes view.gbuffer
void shading(FramePassContext& fc, ViewResources& view);          // writes view.color (+ edge/coverage composite)

// ---- S: shadows and sky - Passes/Shadow, Passes/Atmosphere
void atmosphere(FramePassContext& fc);                            // sky/aerial LUTs -> FrameResources
void shadowPages(FramePassContext& fc, const ViewResources& main);  // VSM marking, dirty page raster (via rasterizeDepth)
void froxels(FramePassContext& fc, const ViewResources& main);    // light lists + froxel integration
void shadowVisibility(FramePassContext& fc, ViewResources& view); // writes view.shadowVisibility

// ---- R: rays, GI, reflections - RayTracing, Passes/GI, Passes/Reflection
void accelerationStructures(FramePassContext& fc);                // static/dynamic TLAS, BLAS refits -> FrameResources
void globalIllumination(FramePassContext& fc, ViewResources& main);  // cache update rays, screen probes, near occlusion
void reflections(FramePassContext& fc, ViewResources& main);      // K/G/M rays, planar mirrors (via renderView)
} // namespace unx::render::tracks
