#pragma once
// S track, virtual shadow maps (ARCHITECTURE 2.3, 2.4, 2.11). Persistent page pool and tables live in the renderer's
// track state (key "s.vsm"); each frame requests pages from the main view's depth, applies the dirty rules, renders
// dirty pages through V's depth raster service and evaluates shadow visibility per pixel (shadowVisibility).
#include "unx/render/Frame.h"

#include <cstdint>

namespace unx::render::shadow
{
// Mirror of VsmConstants (VsmCommon.hlsli).
struct VsmLevelCpu
{
    int32_t origin[2];
    float texel;
    float pad;
};
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
    float pad[3];
    uint32_t active;
};
static_assert(sizeof(VsmLocalLightCpu) == 48);
static_assert(sizeof(VsmConstantsCpu) == 64 + 16 * 3 + kLevels * 16 && sizeof(VsmConstantsCpu) <= 1024);
constexpr uint32_t kSlots = kLevels * kTable * kTable;  // the sun's
constexpr uint32_t kTotalSlots = kSlots + kLocalLights * kLocalLightSlots;

struct VsmStats
{
    uint64_t frame = 0;          // frame the counters belong to
    uint32_t requested = 0, allocated = 0, dirty = 0, exhausted = 0, freePages = 0, pixelRequested = 0;
    // Visibility passes (all views of the frame): pixels per path (VsmSample.hlsli VSM_PATH_*).
    uint32_t pathNoCaster = 0, pathRegionLit = 0, pathRegionUmbra = 0, pathSearchLit = 0, pathFiltered = 0, pathDiskLit = 0, pathDiskUmbra = 0;
    // Local lights of the latest recorded frame (CPU): shadow slots in use, raster-active, casting lights without a slot.
    uint32_t localAssigned = 0, localActive = 0, localWithoutSlot = 0;
    // Overflow list of the main view (INTERFACES 7.3, v1.20): words the frame's tiles needed, tiles over the capacity
    // (fallback) and their overflow pixels (overage: 0 in steady state), shadow-casting lights past the third over all
    // pixels (N_ovf), and the capacity in words the frame ran with (CPU).
    uint32_t overflowWords = 0, overflowOverTiles = 0, overflowOverPixels = 0, overflowLights = 0, overflowCapacity = 0;
};

// shadowPages: requests, dirty rules, allocation and the dirty-page raster for this frame.
void recordPages(FramePassContext& fc, const ViewResources& main);
// shadowVisibility: 4 B per pixel (INTERFACES 7.3) for any view whose depth and G-buffer are in 'view'.
void recordVisibility(FramePassContext& fc, ViewResources& view);

// This frame's page structures for the other S passes (froxel integration, VsmAir.hlsli); false when shadowPages was
// not recorded this frame.
struct VsmFrameRefs
{
    BufferRef pool, table, blocks, bound;
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
