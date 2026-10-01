#pragma once
// S track, virtual shadow maps (ARCHITECTURE 2.3, 2.4, 2.11). One path (S request 20260926_S_vsm_one_path): the page
// atlas and tables live in the renderer's track state (key "s.vsm"); each frame requests pages from the main view's
// depth and air, assigns every requested page a slot of the depth atlas (deterministic scan), draws them all through V's
// depth raster service (tile atlas, hardware depth) and evaluates shadow visibility per pixel (shadowVisibility).
#include "unx/render/Frame.h"

#include <cstdint>

namespace unx::render::shadow
{
// Mirror of VsmConstants (VsmCommon.hlsli).
struct VsmLevelCpu
{
    float3 lightX;
    float cameraU;
    float3 lightY;
    float cameraV;
    float3 lightZ;
    uint32_t basis;
    int32_t origin[2];
    float hMin, hMax;
};
static_assert(sizeof(VsmLevelCpu) == 64);
struct VsmConstantsCpu
{
    float3 lightX;
    float hMin;
    float3 lightY;
    float hMax;
    float3 lightZ;
    float tanSunRadius;
    uint32_t poolPagesX, poolPagesY, frame, sceneInvalidate;
    float time, lodBias, receiverBiasTexels, maxReceiverSlope;
    uint32_t cacheFrames, instanceCount, windTexels, windChanged;
    float cameraUV[2];
    uint32_t searchTaps, filterTaps;
    float3 windDirection;
    float windSpeed;
    uint32_t useStats;  // 1 + UAV index of the read bits (shadow.vsm.use_stats), 0 = off
    uint32_t atlasSrv;  // SRV of the page atlas (every lookup reads it here)
    uint32_t fragmentCheck;  // shadow.vsm.fragment_check (verification only)
    uint32_t usePad;
    VsmLevelCpu level[20];
};
constexpr uint32_t kLevels = 20, kPage = 128, kTable = 128, kVirtual = 16384;
// Local-light shadows (VsmLocal.hlsli): 128 shadow slots x 6 cube faces x 7 mips (128 .. 8192 texels).
constexpr uint32_t kLocalLights = 128, kLocalMips = 7, kLocalFaceSlots = 5461, kLocalLightSlots = 6 * kLocalFaceSlots;
constexpr uint32_t kLocalViewsPerLight = 6 * kLocalMips;
// Mirror of VsmLocalLight (48 B).
struct VsmLocalLightCpu
{
    float3 position;
    float nearM;
    float farM, radius;
    uint32_t lightIndex, generation;
    uint32_t activeIndex;  // VsmLocal.hlsli: raster-active index (classification pages), 0xFFFFFFFF when not active
    float pad[2];
    uint32_t active;
};
static_assert(sizeof(VsmLocalLightCpu) == 48);
static_assert(sizeof(VsmConstantsCpu) == 64 + 16 * 5 + kLevels * 64 && sizeof(VsmConstantsCpu) <= 2048);
constexpr uint32_t kSlots = kLevels * kTable * kTable;  // the sun's
constexpr uint32_t kTotalSlots = kSlots + kLocalLights * kLocalLightSlots;

struct VsmStats
{
    uint64_t frame = 0;          // frame the counters belong to
    uint32_t requested = 0, allocated = 0, dirty = 0, exhausted = 0, freePages = 0, pixelRequested = 0;
    uint32_t cachedPages = 0;    // sun pages kept from an earlier frame (shadow.vsm.cache; requested = cached + new, dirty = drawn)
    // Visibility passes (all views of the frame): pixels per path (VsmSample.hlsli VSM_PATH_*).
    uint32_t pathNoCaster = 0, pathRegionLit = 0, pathRegionUmbra = 0, pathSearchLit = 0, pathFiltered = 0, pathDiskLit = 0, pathDiskUmbra = 0;
    // Local lights of the latest recorded frame (CPU): shadow slots in use, raster-active, casting lights without a slot.
    uint32_t localAssigned = 0, localActive = 0, localWithoutSlot = 0;
    // Moving sun (CPU, latest recorded frame): levels whose basis refreshed this frame, largest basis age (rad).
    uint32_t levelsRefreshed = 0, pagesRefreshed = 0;  // pages: the refreshed levels' requested pages (last stats)
    float largestBasisAge = 0;
    uint32_t levelPages[20] = {};  // requested sun pages per level (GPU, completed frame)
    uint32_t sampledSubtiles = 0;  // 32^2 sub-tiles of requested sun pages that pixels sample (shadow.vsm.subtile_stats)
    // shadow.vsm.use_stats (measurement only): sun pages by request kind and whether a lookup read them (vsmEntry of
    // the frame before: a static camera requests the same pages): requested by pixels / read, by the air marks only /
    // read, by propagation only / read, not requested but read (cached pages).
    uint32_t usePixel = 0, usePixelRead = 0, useAir = 0, useAirRead = 0, usePropagated = 0, usePropagatedRead = 0, useCachedRead = 0;
    // The same key, visibility pass (all views): surface pixels, those facing away from the sun (geometric N.L <= 0), and
    // those of them the page structures could not settle (penumbra pass).
    uint32_t surfacePixels = 0, backfacePixels = 0, backfaceMixed = 0;
    // Overflow list of the main view (INTERFACES 7.3, v1.20): words the frame's tiles needed, tiles over the capacity
    // (fallback) and their overflow pixels (overage: 0 in steady state), shadow-casting lights past the third over all
    // pixels (N_ovf), and the capacity in words the frame ran with (CPU).
    uint32_t overflowWords = 0, overflowOverTiles = 0, overflowOverPixels = 0, overflowLights = 0, overflowCapacity = 0;
    // Air shadow walk of the froxel integration (atmosphere.froxels.walk_stats = 1, measurement only; main and planar
    // views): slices walked, slices with a mixed page (descended), 32-texel and 8-texel block loads, texel loads.
    uint32_t airSlices = 0, airSlicesMixed = 0, airBlocks32 = 0, airBlocks8 = 0, airTexels = 0;
    // The local lights' air walk (same switch; VsmLocalAirWalk.hlsli): shadowed list entries walked, cells visited (page,
    // block and texel loads), the most cells of one entry, lit runs.
    uint32_t localAirEntries = 0, localAirCells = 0, localAirMaxCells = 0, localAirRuns = 0;
    uint32_t localAirWaveCells = 0, localAirWaveEntries = 0;  // over waves: the lane maxima of cells and of entries (a wave runs its slowest lane)
    // S error bits (INTERFACES 3.6; VsmCommon.hlsli VSM_ERR_*): a shader loop reached its hard cap. errorBits: this frame's
    // (stats word 15); errorBitsSeen: every harvested frame's since the state was created. Gates fail on any bit.
    uint32_t errorBits = 0, errorBitsSeen = 0;
    // Fragment visibility of the coverage layer (ShadowFragments, all views): pixels with records, of them pair pixels
    // (mixed, per-record SMRT); with shadow.vsm.fragment_check: records checked against a settled pixel and those whose
    // settled value differs from the per-record SMRT by more than 1/255, and the largest difference (x 255).
    uint32_t fragmentPixels = 0, fragmentPairs = 0, fragmentChecked = 0, fragmentMismatch = 0, fragmentMaxDiff = 0;
    uint32_t fragmentFirstPixel = 0, fragmentFirstValues = 0;  // the check's first mismatch (pixel y << 16 | x, + 1; values)
};

// shadowPages: requests, page assignment and the raster of every requested page for this frame.
void recordPages(FramePassContext& fc, const ViewResources& main);
// shadowVisibility: 4 B per pixel (INTERFACES 7.3) for any view whose depth and G-buffer are in 'view'.
void recordVisibility(FramePassContext& fc, ViewResources& view);

// This frame's page structures for the other S passes (froxel integration, VsmAir.hlsli); false when shadowPages was
// not recorded this frame.
struct VsmFrameRefs
{
    TextureRef atlas;  // the page atlas (D32; SrvCompute / SrvGraphics for readers)
    BufferRef table, blocks, bound, stats;  // stats: raw VSM counters (walk statistics, words 20..24)
    BufferRef use;  // read bits (shadow.vsm.use_stats; invalid when off): readers declare it as UAV
    BufferRef clsBlocks;  // L3 (14.3-1): the classification pages' 8-texel block maxima (VsmCls.hlsli; invalid = off)
    uint32_t clsActive = 0;  // raster-active lights with classification pages this frame
    uint32_t constantsCbv = UINT32_MAX;  // ConstantBuffer<VsmConstants> of this frame
};
bool frameRefs(FramePassContext& fc, VsmFrameRefs& out);

// Diagnostics: the visibility pass writes each pixel's VSM_PATH_* (0xFF = sky) instead of the visibility.
void setDebugPaths(TrackState& state, bool enabled);

// Counters of the most recent frame whose GPU work has completed (read back without stalling).
const VsmStats& stats(TrackState& state);
// The light basis and level windows of the last recorded frame (tests, tools).
const VsmConstantsCpu& lastConstants(TrackState& state);
} // namespace unx::render::shadow
