#pragma once
// Band C aggregate bricks (COVERAGE_REDESIGN 6, request Docs/Design/Requests/20260925_V_aggregate_bricks.md). Owner: V.
//
// Every mesh with thin geometry (the area-weighted 10th percentile of its component widths w at most
// visibility.brick_max_feature_width) gets a stack of voxel levels: level 0 voxel v_0 = 4 w (its projection is one
// pixel where the geometry turns band C, w_px = 0.25), level k voxel v_k = 2^k v_0, up to the level whose one brick
// (edge voxels) covers the mesh. Each level is a dense grid of bricks over the mesh bounds; empty bricks are absent.
// A brick holds, structure of arrays:
//   density: 1 B per voxel, the voxel's optical depth along its densest direction tau = v sqrt(lambda_max(S)) coded
//            logarithmically (0 = empty; q in 1..255 -> tau = 8 x 2^((q - 255) / 24), step 2.9 %)
//   shape:   6 B per voxel, the SGGX matrix S / lambda_max(S) coded as Heitz 2015 (sigma_x, sigma_y, sigma_z,
//            r_xy, r_xz, r_yz), each unorm8 (r as (r + 1) / 2)
//   header:  32 B, BrickHeader
//   occupancy: 64 bits, one per 4^3 cell of the brick (a set bit: some voxel of the cell is not empty)
// so the extinction of a voxel along a unit direction w is sigma(w) = (tau / v) sqrt(w^T S_hat w), S_hat = S /
// lambda_max.
//
// The bake measures every non-empty voxel's exact transmittance along 13 directions (the 26 face, edge and corner
// directions; a chord is the same both ways): parallel rays over the voxel's cross-section, intersected with the
// mesh's triangles, alpha-tested at their uv against the material's base-colour texture. It fits S by least squares on
// tau(w)^2 = l(w)^2 w^T S w (linear in S's six entries; l(w) = the cube's mean chord v / (|w_x| + |w_y| + |w_z|),
// tau = -ln T), projects S to positive semi-definite and records per voxel:
//   r      max over w of |exp(-l sigma(w)) - T(w)|          (the full-voxel residual)
//   rHalf  the same over half chords (entry and exit voxels of a march)
//   rBrick the same with the brick-mean shape instead of the voxel's (what a density-only march uses)
// The mesh's P99 and maximum of each go to BrickMeshStats; a mesh whose r P99 exceeds visibility.brick_residual_max
// fails the build (design gate 1).
#include "unx/core/Config.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <string>
#include <vector>

namespace unx::clusterbuilder
{
namespace gpu
{
struct BrickMesh  // 32 B
{
    float3 boundsMin;
    float voxel0;                  // level 0 voxel edge (m)
    uint32_t firstLevel, levelCount;
    uint32_t pad0, pad1;
};
static_assert(sizeof(BrickMesh) == 32);

struct BrickLevel  // 32 B
{
    uint32_t dims[3];              // bricks per axis
    uint32_t gridOffset;           // first slot in the grid buffer (x fastest, then y, then z)
    float voxel;                   // voxel edge (m)
    float pad[3];
};
static_assert(sizeof(BrickLevel) == 32);

struct BrickHeader  // 32 B
{
    uint32_t meshLevel;            // brick mesh << 8 | level
    uint32_t cell;                 // grid position x | y << 10 | z << 20
    uint32_t shape[3];             // brick-mean S_hat, fp16 x 6 (xx, yy, zz, xy, xz, yz)
    uint32_t albedo;               // mean linear albedo of the voxels' surfaces, RGB8 (R low)
    uint32_t material;             // most frequent scene material of the brick's surfaces
    uint32_t residual;             // unorm8 x 4: r P99, r max, rHalf P99, rBrick P99 (of the brick's voxels)
};
static_assert(sizeof(BrickHeader) == 32);
} // namespace gpu

struct BrickSettings
{
    uint32_t brickEdge = 16;       // visibility.brick_resolution
    float maxFeatureWidth = 0;     // visibility.brick_max_feature_width (m); 0 = no bricks
    uint32_t bakeRays = 64;        // visibility.brick_bake_rays: rays per voxel cross-section and direction
    float residualMax = 0;         // visibility.brick_residual_max: largest allowed r P99 of a mesh
    bool occupancyOnly = false;    // analysis: voxels that geometry overlaps get density 255 and no shape; no rays, no fit
    static BrickSettings fromQuality(const QualityConfig& quality);
};

struct BrickMeshStats
{
    uint32_t mesh = 0;             // scene mesh
    float featureWidth = 0, voxel0 = 0;
    uint32_t levels = 0, bricks = 0, voxels = 0;  // non-empty voxels
    float rP99 = 0, rMax = 0, rHalfP99 = 0, rHalfMax = 0, rBrickP99 = 0, rBrickMax = 0;
    double bakeMs = 0;
    bool cached = false;
};

struct BrickData
{
    std::vector<uint32_t> meshOf;               // per scene mesh: brick mesh index, UINT32_MAX = none
    std::vector<gpu::BrickMesh> meshes;
    std::vector<gpu::BrickLevel> levels;
    std::vector<uint32_t> grid;                 // brick index per slot, UINT32_MAX = empty
    std::vector<gpu::BrickHeader> headers;
    std::vector<uint8_t> density;               // edge^3 per brick
    std::vector<uint8_t> shape;                 // 6 edge^3 per brick, 6 B per voxel
    std::vector<uint64_t> occupancy;            // one per brick
};

// Bakes every eligible mesh (meshes in parallel on the job pool). Results are cached per mesh under 'cacheDirectory'
// (empty = no cache) by the SHA-256 of the mesh, its materials' alpha inputs and the settings. Deterministic.
BrickData bakeBricks(const scene::Scene& scene, const BrickSettings& settings, const std::string& cacheDirectory,
                     std::vector<BrickMeshStats>* stats = nullptr);

// Decoding helpers shared with the tests (the HLSL side mirrors them in Passes/Visibility/Bricks.hlsli).
float brickDecodeDepth(uint8_t q);                // optical depth tau of a voxel along its densest direction
uint8_t brickEncodeDepth(float tau);
} // namespace unx::clusterbuilder
