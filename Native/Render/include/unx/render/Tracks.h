#pragma once
// Track entry points (INTERFACES_KO.md 5.2). Core's FrameRenderer calls them in the order of ARCHITECTURE 4.1; each
// track implements its functions in its own folder. The signatures are fixed; the passes inside are the track's.
#include "unx/render/Frame.h"

namespace unx::render::tracks
{
// Entry points a track has not implemented yet call this: it logs once per entry and the entry declares no passes.
void pending(const char* entry);

// ---- FX: GPU simulation (FX session) - Native/Render/Passes/FX
// C0 of ARCHITECTURE 4.1, first in the frame after prepareScene: the GPU simulation slices (particles first; cloth,
// hair guides, water, Matter later), serial in the frame (ARCHITECTURE 2.14).
void simulation(FramePassContext& fc);
// The particle layer of a view (v1.41, FX request 20260926_FX_particle_render_pass 8a): after reflections and before
// shadow visibility and shading (VSM pages, froxel light lists, air and the GI cache are ready; M composites the layer
// before tonemapping). Writes view.particleLayer, particleDepthRange, particleEdges, distortionLayer (invalid = none).
void particles(FramePassContext& fc, ViewResources& view);

// ---- V: visibility (core session) - Native/Render/Passes/Visibility, Tools/ClusterBuilder
// Culling (two phase), band A/B/C classification, band A vis buffer + depth, HiZ, coverage layer (bands B/C).
// Writes view.depth, visId, visibleClusters, hiz and the coverage layer (coverageTiles, coverageRecords, coverageTileList,
// coverageTilePixels, coverageDepthRange).
void visibility(FramePassContext& fc, ViewResources& view);
// Service behind FrameServices::rasterizeDepth.
void rasterizeDepth(FramePassContext& fc, const DepthRasterRequest& request);

// ---- M: material and shading (M session) - Passes/Material, Passes/Shading
// Called first in every frame, before any frame constants of the frame exist: uploads scene textures when the scene
// changed and publishes them in the material records (GpuScene::setMaterialTextures, INTERFACES_KO.md 6.3).
void prepareScene(FramePassContext& fc);
void materialResolve(FramePassContext& fc, ViewResources& view);  // writes view.gbuffer
void shading(FramePassContext& fc, ViewResources& view);          // writes view.color (+ edge/coverage composite)

// ---- S: shadows and sky - Passes/Shadow, Passes/Atmosphere
void atmosphere(FramePassContext& fc);                            // sky/aerial LUTs -> FrameResources
void shadowPages(FramePassContext& fc, const ViewResources& main);  // VSM marking, dirty page raster (via rasterizeDepth)
void froxels(FramePassContext& fc, const ViewResources& main);    // light lists + froxel integration
void shadowVisibility(FramePassContext& fc, ViewResources& view); // writes view.shadowVisibility

// ---- Banded lighting group (v1.31, INTERFACES_KO.md 4): FrameRenderer records S's shadow visibility and M's edge
// detection and shading of a view as one banded group (RenderGraph::addBandedGroup, passBandCount bands):
// shadowVisibilityPasses(...) then shadingPasses(...), band by band, then shadingComposite (whole-view work on the group's
// results, e.g. M's edge composite). The *Passes functions create their resources and add their ordinary passes (which
// run before the group: clears, per-band tile lists) themselves, and return the banded passes: setup declares uses only
// (it runs once per band), execute touches PassContext::band's rows (or band.lagged(rows) when it reads rows below).
// They replace shadowVisibility and shading when both tracks have them (FrameRenderer switches in one commit).
std::vector<RenderGraph::BandedPass> shadowVisibilityPasses(FramePassContext& fc, ViewResources& view);  // S
std::vector<RenderGraph::BandedPass> shadingPasses(FramePassContext& fc, ViewResources& view);           // M
void shadingComposite(FramePassContext& fc, ViewResources& view);                                          // M

// ---- R: rays, GI, reflections - RayTracing, Passes/GI, Passes/Reflection
void accelerationStructures(FramePassContext& fc);                // static/dynamic TLAS, BLAS refits -> FrameResources
void globalIllumination(FramePassContext& fc, ViewResources& main);  // cache update rays, screen probes, near occlusion
void reflections(FramePassContext& fc, ViewResources& main);      // K/G/M rays, planar mirrors (via renderView)
} // namespace unx::render::tracks
