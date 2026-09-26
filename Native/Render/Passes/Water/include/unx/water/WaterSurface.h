#pragma once
// Water surface shading, stage 1 (FEATURES_GAME 1.9; kernels WaterSurface.hlsli, WaterInterior.hlsl): W's part of M's
// shading, called from tracks::water after band A is shaded. The view's water layer (waterVis / waterDepth, V v1.63)
// pixels that the layer covers whole get the water's radiance in the shaded colour (exposed linear float); edge pixels
// are M's composite of W's records (record pass, with render A's ViewResources::coverageRecordRadiance).
#include "unx/render/Frame.h"
#include "unx/render/FrameResources.h"

#include <cstdint>

namespace unx::water
{
// Counts of the last recorded frame's water samples (WaterSurface.hlsli WATER_STAT_*), read back when that frame's GPU
// work has completed (the caller waited for it).
struct WaterSurfaceStats
{
    uint64_t frameIndex = UINT64_MAX;  // UINT64_MAX: no frame shaded water yet
    uint32_t shaded = 0, offscreen = 0, exited = 0, occluded = 0, steps = 0, inside = 0, unlit = 0;
    uint32_t fallbacks() const { return offscreen + exited + occluded + steps; }
};
// Tests: with `status` on, the next recorded frame also writes an R8_UINT image of each interior pixel's
// WATER_STAT_* + 1 (0 = not an interior water pixel), left in `image` for a readback in the same graph.
struct WaterSurfaceDebug
{
    bool status = false;
    render::TextureRef image;
    render::TextureRef march;  // RGBA32F: the march's hit or exit screen position, step, and a depth (WaterSurface.hlsli)
};

void waterSurface(render::FramePassContext& fc, render::ViewResources& view);
WaterSurfaceStats latestWaterSurfaceStats(render::TrackState& state);
} // namespace unx::water
