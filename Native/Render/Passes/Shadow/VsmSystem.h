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
    uint32_t cacheFrames, instanceCount, windTexels, pad0;
    VsmLevelCpu level[12];
};
static_assert(sizeof(VsmConstantsCpu) == 64 + 16 * 2 + 12 * 16);

constexpr uint32_t kLevels = 12, kPage = 128, kTable = 128, kVirtual = 16384;
constexpr uint32_t kSlots = kLevels * kTable * kTable;

struct VsmStats
{
    uint64_t frame = 0;          // frame the counters belong to
    uint32_t requested = 0, allocated = 0, dirty = 0, exhausted = 0, freePages = 0;
};

// shadowPages: requests, dirty rules, allocation and the dirty-page raster for this frame.
void recordPages(FramePassContext& fc, const ViewResources& main);
// shadowVisibility: 4 B per pixel (INTERFACES 7.3) for any view whose depth and G-buffer are in 'view'.
void recordVisibility(FramePassContext& fc, ViewResources& view);

// Counters of the most recent frame whose GPU work has completed (read back without stalling).
const VsmStats& stats(TrackState& state);
// The light basis and level windows of the last recorded frame (tests, tools).
const VsmConstantsCpu& lastConstants(TrackState& state);
} // namespace unx::render::shadow
