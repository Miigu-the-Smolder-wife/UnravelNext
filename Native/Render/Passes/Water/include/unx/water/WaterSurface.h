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
#include <vector>

namespace unx::water
{
// Counts of the last recorded frame's water samples (WaterSurface.hlsli WATER_STAT_*), read back when that frame's GPU
// work has completed (the caller waited for it).
struct WaterSurfaceStats
{
    uint64_t frameIndex = UINT64_MAX;  // UINT64_MAX: no frame shaded water yet
    uint32_t shaded = 0, offscreen = 0, exited = 0, occluded = 0, steps = 0, inside = 0, unlit = 0;
    uint32_t rayOverflow = 0, reflectJobs = 0, refractJobs = 0, traced = 0;  // stage 3 (0 without R's service)
    uint32_t planar = 0;                  // calm water: samples whose mirror lobe came from a reflection camera
    uint32_t planarMask[4] = {};          // pixels each candidate plane's mask pass drew (WaterPlanarMask)
    uint32_t planarViews = 0;             // reflection cameras the frame rendered
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
    int planar = -1;              // calm water's reflection cameras: -1 by the cost rule, 0 never (rays), 1 always
};

// Calm water (A14 planar reflection camera, FEATURES_GAME 1.9 stage 3 (i)): a layer-1 stream whose surface rests on a
// plane (W2 basins: the still level) is pushed here by its producer each frame with its index in
// FrameResources::triangleStreams. waterSurface draws the stream's reflection from the mirrored camera where the cost
// rule picks it (measured: camera 1.44 ms + 0.80 ns per mask pixel against 3.0 ns saved per sample it serves, from the
// counts of the last completed frame; break-even ~650 k pixels, 7.9 % of 4K) and the samples whose surface is that
// plane within WaterSurface.hlsli's image-shift bound read
// it in place of a reflection job; the others keep their jobs. corners: the plane region's corners (the camera's
// rectangle is their projection; any corner behind the eye takes the whole view).
struct WaterPlane
{
    uint32_t stream = 0;
    float4 plane{};
    float3 corners[4]{};
};
struct WaterPlanes
{
    uint64_t frame = UINT64_MAX;
    std::vector<WaterPlane> list;
};
void addWaterPlane(render::FramePassContext& fc, const WaterPlane& plane);

// B7: the frame's sea as the surface pass shades it (W's waterGeometry fills the track state "W.oceanFrame" when the
// frame has a sea it draws; a record of another frame is stale). The view grid's surface (FrameResources::waterSurface)
// gives each sea pixel its rest position and depth; the cascades' displacement and slopes its normal; the foam clipmap
// its foam (Passes/Water/OceanShading.hlsli).
struct OceanSurfaceFrame
{
    uint64_t frame = UINT64_MAX;
    render::TextureRef surface, displacement, slopes, foam;  // (foam invalid: none)
    uint32_t foamParams = 0xFFFFFFFFu;
    uint32_t material = 0xFFFFFFFFu;  // scene material of the sea's water; none: the built-in open ocean's
    float lengths[3] = {};            // the cascades' tile sizes (m)
    // the spectrum's tail for the slopes a pixel does not resolve: Phillips' constant alpha of the sea state, its peak
    // wavenumber, the cascades' finest wavenumber (1/m) and the wind at 10 m (m/s)
    float alpha = 0, peakWavenumber = 0, finestWavenumber = 0, windSpeed = 0;
};

// Flow on the basins' surfaces (PoolFrame::flow*; Passes/Water/WaterFlow.hlsli): this frame's rows of the slot table's
// flow block, by stream slot (W's poolGeometry fills the track state "W.poolFlows"; a record of another frame is stale).
struct WaterFlowRow
{
    uint32_t stream = 0;
    float values[20] = {};  // WaterFlow.hlsli's 80 B (word 12: the velocity map's SRV)
};
struct WaterFlows
{
    uint64_t frame = UINT64_MAX;
    std::vector<WaterFlowRow> rows;
};

void waterSurface(render::FramePassContext& fc, render::ViewResources& view);
WaterSurfaceStats latestWaterSurfaceStats(render::TrackState& state);
} // namespace unx::water
