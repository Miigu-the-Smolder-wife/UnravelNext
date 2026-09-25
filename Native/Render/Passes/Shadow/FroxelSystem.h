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
FroxelGridCpu froxelGridFor(const QualityConfig& quality, uint32_t width, uint32_t height);

struct FroxelStats
{
    uint64_t frame = 0;  // frame the counters belong to
    uint32_t indexCount = 0;                     // light entries listed over all froxels
    uint32_t overflowLists = 0;                  // froxels reached by more than lights_max lights
    uint32_t droppedLights = 0;                  // light entries lost to truncation
    uint32_t maxCount = 0;                       // most lights reaching one froxel
    uint32_t candidateOverflow = 0;              // tiles whose frustum held more than 1024 lights
};

// froxels(fc, main): fills FrameResources::froxelLights and ::froxels. Needs shadowPages of the same frame.
void recordFroxels(FramePassContext& fc, const ViewResources& main);
// Counters of the most recent frame whose GPU work has completed (read back without stalling).
const FroxelStats& froxelStats(TrackState& state);
} // namespace unx::render::shadow
