#pragma once
// S track, froxels (ARCHITECTURE 2.3 "볼륨 산란", 2.4; INTERFACES 5.6, 7.4): per-froxel light lists of the main view
// and the froxel integration of the air (sun in-scattering removed where the VSM shadows the air, local lights'
// in-scattering). Lives with the VSM because the integration reads this frame's pages (VsmAir.hlsli); the kernels and
// the public lookups are in Passes/Atmosphere (FroxelCommon.hlsli, Froxel.hlsli).
#include "unx/render/Frame.h"

#include <cstdint>

namespace unx::render::shadow
{
struct FroxelGridCpu
{
    uint32_t gridX = 0, gridY = 0, slices = 0, tilePx = 0;
    float nearM = 0, farM = 0;
    float shadowTexelsPerTile = 1;  // VSM texels per tile width for the air's shadows (VsmAir.hlsli vsmAirLevel)
};
// Grid of a view of width x height pixels from Config/quality/atmosphere.toml ([atmosphere.froxels]).
// mainHeight: the frame's main view height (0: this view is it) - the tile size follows it (Frame.h froxelTilePx).
FroxelGridCpu froxelGridFor(const QualityConfig& quality, uint32_t width, uint32_t height, uint32_t mainHeight = 0);

struct FroxelStats
{
    uint64_t frame = 0;  // frame the counters belong to
    uint32_t indexCount = 0;                     // light entries stored over all froxels
    uint32_t overflowLists = 0;                  // froxels whose list was cut by the buffer's capacity (gates: 0)
    uint32_t droppedLights = 0;                  // light entries lost to that cut (gates: 0)
    uint32_t maxCount = 0;                       // most lights reaching one froxel
    uint32_t candidateOverflow = 0;              // tiles whose frustum held more than 1024 lights
    uint32_t needed = 0;                         // entries the frame's lists took (the exact allocation)
    uint32_t capacity = 0;                       // entries the buffer held that frame (needed > capacity: scene lights only)
    uint32_t sceneBound = 0;                     // the scene lights' upper bound that sized it (froxelListBound)
    uint32_t capacityNow = 0;                    // capacity of the main view's buffer as last recorded (froxelListCapacity)
};
// Upper bound of the entries the scene's lights can take in a view's lists this frame (RENDERER_REDESIGN_V2 14.1): per
// light and slice, the tiles whose froxel can pass FroxelLists.hlsl's reach test (a sphere of the light's radius against
// the froxel's bounding sphere), counted from the lateral extent of the slice's froxels. Every GPU-listed (scene light,
// froxel) pair is inside it, so a lists buffer of this capacity never cuts a scene light's entry. FX particle lights
// (ranges computed on the GPU) are not in it: they get an allowance grown from the measured need.
uint64_t froxelListBound(const FroxelGridCpu& grid, const ViewDesc& view, const std::vector<gpu::Light>& lights);
// Size of a view's froxel light list buffer (header, two words per froxel, capacity entries of 16 bits).
uint64_t froxelListBytes(const FroxelGridCpu& grid, uint64_t capacity);
// Entry capacity of the main view's lists buffer as last recorded (tests read the buffer back whole: froxelListBytes).
uint32_t froxelListCapacity(TrackState& state);

// shadowPages: the froxel light lists of the main view (FrameResources::froxelLights) with each entry's shadow-slot bit
// (slotOfLightSrv: StructuredBuffer<uint> scene light -> shadow slot, or 0xFFFFFFFF: none). The local-light page marks
// and the visibility slots read them.
// shadowSlots: S assigned local shadow slots this frame - the visibility slots 1-3 read the lists' head order.
void recordFroxelLists(FramePassContext& fc, const ViewResources& main, uint32_t slotOfLightSrv, bool shadowSlots);
// atmosphere.fog: the fog volume's grid for a view size and the medium (on = false: off). Shared by the volume's passes
// (recordFroxels) and the page requests (VsmSystem.cpp s.vsm.markfog).
FogView fogViewFor(const QualityConfig& q, const FrameContext& frame, uint32_t width, uint32_t height);
// The main view's fog parameters for its frame constants (tracks::fogParams): makes the fog's persistent textures and
// this frame's parameter record; SRV + 1, or 0 with the fog off. The record says "no volume" until recordFroxels has
// recorded the volume's passes.
uint32_t fogPrepare(FramePassContext& fc, const ViewDesc& view);
// A planar reflection view's (tracks::fogParamsSecondary; atmosphere.fog.secondary_views): 0 when the fog is off or the
// frame's four volumes are taken. recordPlanarFroxels records the volume of the view whose frame constants are at key.
uint32_t fogPrepareSecondary(FramePassContext& fc, const ViewDesc& view, uint64_t key);
// froxels(fc, main): the air volume (FrameResources::froxels, ::aerialPerspective); records the lists itself when
// shadowPages did not.
void recordFroxels(FramePassContext& fc, const ViewResources& main);
// Planar reflection views (INTERFACES 7.4, v1.22): the view's own lists and air volume (from the mirror plane on) into
// view.froxelLights / view.airVolume. Called by shadowVisibility before the view's slots.
void recordPlanarFroxels(FramePassContext& fc, ViewResources& view);
// Counters of the most recent frame whose GPU work has completed (read back without stalling).
const FroxelStats& froxelStats(TrackState& state);
// Gates: keep the integration live when no consumer reads the volume yet (the graph culls unread passes).
void setKeepFroxels(TrackState& state, bool keep);
// Tests: integrate every slice of every tile (no reader bound: nodes beyond the farthest surface and the sky
// correction's range are then filled too, for node-by-node comparisons with the reference).
void setFroxelFullDepth(TrackState& state, bool full);
} // namespace unx::render::shadow
