#pragma once
// Track entry points (INTERFACES_KO.md 5.2). Core's FrameRenderer calls them in the order of ARCHITECTURE 4.1; each
// track implements its functions in its own folder. The signatures are fixed; the passes inside are the track's.
#include "unx/render/Frame.h"
#include "unx/render/TrackPending.h"

namespace unx::render::tracks
{
// ---- FX: GPU simulation (FX session) - Native/Render/Passes/FX
// C0 of ARCHITECTURE 4.1, first in the frame after prepareScene: the GPU simulation slices (particles first; cloth,
// hair guides, water, Matter later), serial in the frame (ARCHITECTURE 2.14).
void simulation(FramePassContext& fc);
// The particle layer of a view (v1.41, FX request 20260926_FX_particle_render_pass 8a): after reflections and before
// shadow visibility and shading (VSM pages, froxel light lists, air and the GI cache are ready; M composites the layer
// before tonemapping). Writes view.particleLayer, particleDepthRange, particleEdges (invalid = none).
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
// M (A4): the main view's EV100 of this frame under automatic exposure (FrameContext::autoExposure), before any frame
// constants; framesInFlight: the lag after which the host waited for a frame's histogram.
float autoExposureEv100(TrackState& state, Device& device, const QualityConfig& quality, const FrameContext& frame, uint32_t framesInFlight);
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

// ---- E: engine 2 (reassigned 2026-09-26) - Passes/Volume, Passes/Debug, Passes/Decal, Passes/Hair
// Smoke and fire media (request 20260925_FX_particle_render_rules 3b): called by S's froxels() of the main view between
// its light lists (froxelLights: FroxelGrid header + lists) and the integration; returns the view's volumeSlices texture
// (invalid: no particle media this frame, the integration unchanged).
TextureRef volumeMedia(FramePassContext& fc, ViewResources& main, BufferRef froxelLights);
// Heat haze of a view (FEATURES_GAME 0.A-8): after particles(); writes view.distortionOffset and distortionDepth.
void distortion(FramePassContext& fc, ViewResources& view);
// E (A15, FEATURES_GAME 7.1): debug drawing. debugBegin runs first in the frame (before any frame constants): it opens
// this frame's debug primitive buffer when debug drawing is on (quality debug.draw / debug.hud / debug.view, or CPU
// primitives queued in debug::drawList) and returns its UAV for FrameConstants::debugDraw (0xFFFFFFFF = off).
uint32_t debugBegin(FramePassContext& fc);
// E (A7, FEATURES_GAME 5): the view's projected decals, after visibility (reads view.depth) and before the material
// resolve: writes view.decalFrames and view.decalTiles (invalid when no decal is live).
void decals(FramePassContext& fc, ViewResources& view);
// E (A7, WORLD_VFX 10.4): uploads what changed in the surface state field (surface::surfaceField) and publishes
// FrameResources::surfaceConstants / surfaceTable / surfacePool (invalid while the field has never had a brick).
void surfaceState(FramePassContext& fc);
// Last in the frame: the buffer visualization (debug.view, replaces the view's colour), then the debug primitives
// (CPU and GPU appends) and the HUD drawn over view.color.
void debugOverlay(FramePassContext& fc, ViewResources& view);
} // namespace unx::render::tracks
