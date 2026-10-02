#pragma once
// V internal: C++ mirror of VisibilityCommon.hlsli / CullShared.hlsli (sizes and word offsets must match).
#include "unx/core/Math.h"

#include <cstdint>

namespace unx::visibility::detail
{
struct CullView  // 384 B
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
    float3 prevPosition;
    float bandAMinPx;
    float bandAHysteresisPx;
    uint32_t cullSceneSrv;  // CullScene of the run (C3: instance chunks, flat list, skinned bounds)
    uint32_t runtimeFirst, runtimeCount;  // C2b runtime instances (GpuScene::staticInstanceCount onwards)
    uint32_t gpuFirst, gpuCapacity;       // GPU-written instances (GpuScene::gpuInstanceRange; live count in g_patchData)
    uint32_t instanceFirst, instanceEnd;  // RasterView's instance batch (instanceEnd 0: every instance)
    float minInstancePx;                  // RasterView::minInstanceTexels (0: every instance)
    uint32_t instanceSet;                 // RasterView::instanceSet (0 every instance, 1 not movable, 2 movable)
    uint32_t occluderSrv, occluderSlotsSrv;  // DepthRasterRequest::tileOccludersSrv, atlasSlotsSrv (kViewTileOccluders)
};

// C3 instance hierarchy (VisibilityCommon.hlsli CullScene, CullChunk).
struct CullScene
{
    uint32_t chunkSrv, chunkInstancesSrv, chunkCount, flatSrv;
    uint32_t flatCount, skinBoundsSrv, skinListSrv, skinCount;
};
static_assert(sizeof(CullScene) == 32);
struct CullChunk
{
    float4 sphere;  // world; radius < 0 until ChunkBounds ran
    uint32_t first, count, windBits, pad1;
};
static_assert(sizeof(CullChunk) == 32);
struct SkinJointSphere  // SkinBounds.hlsl
{
    float4 sphere;
    uint32_t joint, pad[3];
};
static_assert(sizeof(SkinJointSphere) == 32);
constexpr uint32_t kChunkInstances = 256;  // CHUNK_INSTANCES
constexpr float kChunkCell = 64.0f;         // metres (ARCHITECTURE 2.1: 64 m cells)
constexpr uint32_t kSkinJointOrigin = 0xFFFFFFFFu;
static_assert(sizeof(CullView) == 384);

constexpr uint32_t kViewOcclusion = 1;
constexpr uint32_t kViewCullBack = 2;
constexpr uint32_t kViewTileSingle = 4;  // tile-local pairs are single tiles (atlas mode)
constexpr uint32_t kViewTileOccluders = 8;  // tested against the request's tile occluders (RasterView::tileOccluders)

// Cull state words.
constexpr uint32_t kStateNodeWrite = 0, kStateNodeEnd = 2, kStateGroupWrite = 3, kStateVisible = 5, kStateDeferInstances = 6, kStateDeferNodes = 7,
                   kStateDeferClusters = 8, kStateListCount = 9, kStateCovSpecial = 17, kStateOceanEdges = 18, kStateOverflow = 21, kStateStatInstances = 22, kStateStatNodes = 23, kStateStatClusters = 24,
                   kStateStatTriangles = 25, kStateTilePairs = 29, kStateCovPool = 30, kStateCovInvocations = 31, kStateCovFragments = 32, kStateCovTiles = 33, kStateCovMeasured = 34,
                   kStateStatBandClusters = 35, kStateCovBlocks = 38, kStateCovHeavy = 39, kStateStatMixedClusters = 40,
                   kStateStatMixedTriangles = 41, kStateChunkItems = 42, kStateDeferChunks = 43, kStateStatChunks = 44, kStateNodeCommit = 45, kStateNodeRead = 46,
                   kStateNodePending = 47, kStateListPhase1 = 48, kStateWords = 56;
constexpr uint32_t kLists = 8;
constexpr uint32_t kListABack = 0, kListANone = 1, kListAAlphaBack = 2, kListAAlphaNone = 3, kListB = 4, kListC = 5, kListTBack = 6, kListTNone = 7;
constexpr uint32_t kBandLists = 6;  // lists of the cull bands (the depth raster service draws these)
constexpr uint32_t kAListCount = 4;  // lists drawn by the vis buffer raster

// Indirect argument words.
constexpr uint32_t kArgNodes = 0, kArgGroups = 3, kArgDeferredClusters = 6, kArgDeferredInstances = 9, kArgSeedNodes = 12, kArgGpuInstances = 15, kArgCovMesh = 33,
                   kArgCovClear = 36, kArgCovRecords = 39, kArgChunkItems = 42, kArgDeferredChunks = 45, kArgMesh = 48, kArgCovTMesh = 72, kArgWords = 78;

// Band modes of a cull run (CullShared.hlsli BAND_MODE_*): A = every band in the band A lists (raster service, secondary
// views; classification still runs and is reported in Stats::triangles); Coverage = bands B and C in the coverage layer
// list until the band C bricks exist; Full = band C in its own list; CVisible = band C in the band A lists.
constexpr uint32_t kBandModeA = 0, kBandModeCoverage = 1, kBandModeFull = 2, kBandModeCVisible = 3;

// Coverage layer (CoverageTiles.hlsli, CoverageLayer.hlsli): tiles, tile list, blocks, scratch slots.
constexpr uint32_t kCovTilePx = 8, kCovTileWords = 8, kCovTilePixels = 64, kCovBlock = 1024, kCovScratchWords = 256;
constexpr uint32_t kCovTileCount = 0, kCovTileBase = 1, kCovTileListed = 2, kCovTileOpaqueLo = 3, kCovTileOpaqueHi = 4, kCovTileHeavy = 5;
constexpr uint32_t kCovListArgs = 0, kCovListCount = 3, kCovListRecords = 4, kCovListBlocks = 5, kCovListPool = 6, kCovListTilesX = 7,
                   kCovListBlockArgs = 8, kCovListHeavyCount = 11, kCovListHeavyArgs = 12, kCovListInfo = 16;
// Record capacity: a structured view (16 B elements) holds at most 2^27 elements (2 GB = 134 M records).
constexpr uint32_t kCovPoolMaxRecords = 1u << 27;
// Scratch slots of a capacity: a tile of more than one block holds more than kCovBlock records.
inline uint32_t coverageScratchSlots(uint32_t capacity) { return capacity / (kCovBlock + 1) + 1; }
} // namespace unx::visibility::detail
