#pragma once
// C5 terrain deformation D (FEATURES_GAME 9, PHYSICS 9.7): footprints, wheel ruts and craters as real geometry. The
// producer of D is physics (a height offset window around the player, texels aligned to the terrain grid); the renderer
// replaces every block of a cooked terrain tile whose closure touches non-zero D by a mesh at D's resolution (runtime
// geometry, GpuScene::addRuntimeMesh), and V drops the tile's source triangles inside those blocks (the tile's cut is
// forced to source clusters there). Exact by construction:
//   - every patch vertex is on the source surface (barycentric in the source triangle with the tile's diagonal) plus D
//     at that texel; vertices on the source grid take the source vertex bit for bit (plus D);
//   - sides whose neighbour block is not replaced carry only the source vertices, so the patch meets the tile's source
//     edges exactly (no T-junction); D is 0 there because the neighbour's closure shares that line;
//   - sides between two replaced blocks carry every texel vertex on both sides (identical positions);
//   - with D = 0 the patch is the source surface and shades as it does (interpolated source normals, uv, tangents).
// Cost [예상]: per replaced block (4 x 4 cells of 0.5 m at 5 cm texels: 41^2 vertices, ~3,200 triangles) about 0.1 ms of
// CPU to build on the calling thread; raster of the patches at D resolution (5 cm quads project >= 4 px inside 25 m at
// 4K), and the forced source clusters of the tile inside the window.
#include "unx/core/Math.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <vector>

namespace unx::scene
{
// A cooked terrain tile (Unravel Editor/Cook/UnravelNextTerrainCook): (cells + 1)^2 vertices row-major, cell (i, j) has
// vertices a = j (cells + 1) + i, b = a + 1, c = a + cells + 1, d = c + 1 and triangles (a c d), (a d b) in either
// winding (the diagonal a-d), hole cells have no triangles; the grid is axis aligned in object space (either sign).
struct TerrainGrid
{
    const Mesh* mesh = nullptr;
    uint32_t cells = 0;
    float originX = 0, originZ = 0;  // object xz of vertex 0
    float stepX = 0, stepZ = 0;      // object-space step per cell along i and j (signed)
    std::vector<uint8_t> solid;      // per cell (j cells + i): has triangles
    float facing = 1;                // sign of the source triangles' normal y (winding after the host's axis mapping)
};
TerrainGrid terrainGrid(const Mesh& mesh);  // fails when the mesh is not such a tile

// D in the tile's object space (the host maps world to object; tiles are placed by translation). Texel (tx, tz) is at
// (originX + tx spacing, originZ + tz spacing); |step| / spacing must be an integer and the tile's vertex 0 on a texel.
struct TerrainDeformation
{
    float originX = 0, originZ = 0;
    float spacing = 0.05f;
    uint32_t size = 0;           // texels per side
    const float* height = nullptr;  // size^2 (not owned), rows along z; metres added to y (negative = pressed in)
};

constexpr uint32_t kPatchBlockCells = 4;  // a replaced block is 4 x 4 source cells (2 m at 0.5 m)

// Texels per source cell (|step| / spacing), checked; and whether block (bi, bj) of the grid (may lie outside the tile:
// neighbours across a tile edge) has non-zero D in its closure.
uint32_t texelsPerCell(const TerrainGrid& grid, const TerrainDeformation& d);
bool patchBlockActive(const TerrainGrid& grid, const TerrainDeformation& d, int32_t bi, int32_t bj);
// Blocks of the tile (bj * blocksPerSide + bi) that are replaced.
std::vector<uint32_t> activePatchBlocks(const TerrainGrid& grid, const TerrainDeformation& d);
uint32_t patchBlocksPerSide(const TerrainGrid& grid);
// Hash of everything buildTerrainPatch reads for block (bi, bj) besides the tile: D over the block's closure and a
// one-texel ring (normals), and which of the four neighbours are replaced (stitched sides). Equal hashes: same patch.
uint64_t patchBlockHash(const TerrainGrid& grid, const TerrainDeformation& d, uint32_t bi, uint32_t bj);
// The replacement mesh of block (bi, bj) of the tile: one submesh with the tile's first material.
Mesh buildTerrainPatch(const TerrainGrid& grid, const TerrainDeformation& d, uint32_t bi, uint32_t bj);
} // namespace unx::scene
