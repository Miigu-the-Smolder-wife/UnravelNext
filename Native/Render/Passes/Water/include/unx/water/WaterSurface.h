#pragma once
// Water surface shading, stage 1 (FEATURES_GAME 1.9; kernels WaterSurface.hlsli, WaterInterior.hlsl): W's part of M's
// shading, called from tracks::water after band A is shaded. The view's water layer (waterVis / waterDepth, V v1.63)
// pixels that the layer covers whole get the water's radiance in the shaded colour (exposed linear float); edge pixels
// are M's composite of W's records (record pass, with render A's ViewResources::coverageRecordRadiance).
// Stage 3 (R-W1 / R-W2): with FrameServices::traceRefractions (R's ray service) the interior pixels are shaded in bands of
// rows, each band writing its reflection and refraction jobs, R tracing them, and W's apply pass putting the traced
// radiance in place of stage 1's stand-ins (WaterSurface.hlsli WaterRayTerms). The band holds every job its rows can
// write (2 per pixel), so no sample is left out; the job list's memory is bounded by the band. Edge records (the record pass) use the same
// lists in rounds over coverageSpecial's capacity.
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
    uint32_t rayOverflow = 0, reflectJobs = 0, refractJobs = 0, traced = 0;  // stage 3 (0 without R's service)
    uint32_t fallbacks() const { return offscreen + exited + occluded + steps; }
};
// Tests: with `status` on, the next recorded frame also writes an R8_UINT image of each interior pixel's
// WATER_STAT_* + 1 (0 = not an interior water pixel), left in `image` for a readback in the same graph.
struct WaterSurfaceDebug
{
    bool status = false;
    render::TextureRef image;
    render::TextureRef march;  // RGBA32F: the march's hit or exit screen position, step, and a depth (WaterSurface.hlsli)
    bool copyRadiance = false;    // tests: the next frame copies band A radiance as W left it (before M's edge composite)
    render::TextureRef radiance;  // that copy (RGBA16F), for a readback in the same graph
    uint32_t rayJobCapacity = 0;  // stage 3: jobs per band (0 = the default 2^20); tests set it small to run many bands
    uint32_t rayBands = 0;        // stage 3: bands of the last recorded frame
    uint32_t rayRounds = 0;       // stage 3: record rounds of the last recorded frame (1 without rays)
};

void waterSurface(render::FramePassContext& fc, render::ViewResources& view);
WaterSurfaceStats latestWaterSurfaceStats(render::TrackState& state);
} // namespace unx::water
