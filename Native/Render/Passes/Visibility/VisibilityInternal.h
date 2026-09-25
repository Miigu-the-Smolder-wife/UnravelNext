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
constexpr uint32_t kViewTileSingle = 4;  // tile-local pairs are single tiles (atlas mode)

// Cull state words.
constexpr uint32_t kStateNodeWrite = 0, kStateNodeEnd = 2, kStateGroupWrite = 3, kStateVisible = 5, kStateDeferInstances = 6, kStateDeferNodes = 7,
                   kStateDeferClusters = 8, kStateListCount = 9, kStateOverflow = 21, kStateStatInstances = 22, kStateStatNodes = 23, kStateStatClusters = 24,
                   kStateStatTriangles = 25, kStateTilePairs = 29, kStateCovPool = 30, kStateCovFragments = 32, kStateCovTiles = 33, kStateCovChunks = 34,
                   kStateStatBandClusters = 35, kStateCovLost = 38, kStateCovHeavy = 39, kStateWords = 40;
constexpr uint32_t kLists = 6;
constexpr uint32_t kListABack = 0, kListANone = 1, kListAAlphaBack = 2, kListAAlphaNone = 3, kListB = 4, kListC = 5;
constexpr uint32_t kAListCount = 4;  // lists drawn by the vis buffer raster

// Indirect argument words.
constexpr uint32_t kArgNodes = 0, kArgGroups = 3, kArgDeferredClusters = 6, kArgDeferredInstances = 9, kArgSeedNodes = 12, kArgMesh = 15, kArgCovMesh = 33,
                   kArgCovClear = 36, kArgCovTiles = 39, kArgWords = 42;

// Band modes of a cull run (CullShared.hlsli BAND_MODE_*): A = every band in the band A lists (raster service, secondary
// views; classification still runs and is reported in Stats::triangles); Coverage = bands B and C in the coverage layer
// list until the band C bricks exist; Full = band C in its own list.
constexpr uint32_t kBandModeA = 0, kBandModeCoverage = 1, kBandModeFull = 2;

// Coverage layer (CoverageTiles.hlsli): tiles, chunks, tile list header.
constexpr uint32_t kCovTilePx = 8, kCovTileWords = 8, kCovChunkRecords = 64, kCovChunkBytes = 1024, kCovExtSlots = 255;
constexpr uint32_t kCovTileCount = 0, kCovTileZNear = 1, kCovTileZFar = 2, kCovTileOpaqueLo = 3, kCovTileOpaqueHi = 4, kCovTileExt = 5;
constexpr uint32_t kCovListArgs = 0, kCovListCount = 3, kCovListFragments = 4, kCovListChunks = 5, kCovListTableSlots = 6, kCovListTilesX = 7,
                   kCovListHeavyArgs = 8, kCovListHeavyCount = 11, kCovListHeavyMin = 12, kCovListHeavyStart = 13, kCovListTiles = 16;
// Record pool: raw views count 32-bit elements, at most 2^27 (512 MB = 524,288 chunks = 33.5 M fragments).
constexpr uint32_t kCovPoolMaxChunks = (1u << 27) / (kCovChunkBytes / 4);
} // namespace unx::visibility::detail
