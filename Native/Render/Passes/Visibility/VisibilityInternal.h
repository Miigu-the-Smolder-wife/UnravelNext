#pragma once
// V internal: C++ mirror of VisibilityCommon.hlsli / CullShared.hlsli (sizes and word offsets must match).
#include "unx/core/Math.h"

#include <cstdint>

namespace unx::visibility::detail
{
struct CullView  // 320 B
{
    float4x4 viewProj;
    float4x4 prevViewProj;
    float4 planes[6];
    float4 clipPlane;
    float3 position;
    float lodScale;
    float lodThreshold;
    uint32_t orthographic;
    float nearPlane;
    uint32_t flags;
    float2 viewportSize;
    float2 viewportOffset;
    float4 viewDirection;
    uint32_t cullMaskOffset;
    uint32_t userData;
    uint32_t tilesX;
    uint32_t tilePx;
};
static_assert(sizeof(CullView) == 320);

constexpr uint32_t kViewOcclusion = 1;
constexpr uint32_t kViewCullBack = 2;

// Cull state words.
constexpr uint32_t kStateNodeWrite = 0, kStateNodeEnd = 2, kStateGroupWrite = 3, kStateVisible = 5, kStateDeferInstances = 6, kStateDeferNodes = 7,
                   kStateDeferClusters = 8, kStateListCount = 9, kStateOverflow = 21, kStateStatInstances = 22, kStateStatNodes = 23, kStateStatClusters = 24,
                   kStateStatTriangles = 25, kStateWords = 32;
constexpr uint32_t kLists = 6;
constexpr uint32_t kListABack = 0, kListANone = 1, kListAAlphaBack = 2, kListAAlphaNone = 3, kListB = 4, kListC = 5;

// Indirect argument words.
constexpr uint32_t kArgNodes = 0, kArgGroups = 3, kArgDeferredClusters = 6, kArgDeferredInstances = 9, kArgSeedNodes = 12, kArgMesh = 15, kArgWords = 33;

// Until the coverage layer draws bands B and C, every visible cluster is drawn in band A (classification still runs
// and is reported in Stats::triangles).
constexpr uint32_t kBandMode = 0;
} // namespace unx::visibility::detail
