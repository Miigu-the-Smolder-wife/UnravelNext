#pragma once
// Mesh cards (Docs/Status/MESH_CARDS_INTERFACE_KO.md 2): per mesh, a few axis-aligned boxes ("cards"), each an
// orthographic capture of the surfaces facing one of the six axis directions. The surface cache rasterises a mesh's
// instances through its cards into atlases, independent of the view. The generation rules and numbers are Unreal's
// (MeshCardRepresentationUtilities.cpp, UE 5.8), converted from cm to m.
#include "unx/core/Math.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <vector>

namespace unx::scene
{
constexpr uint32_t kMeshCardDirections = 6;   // -X, +X, -Y, +Y, -Z, +Z: the side the captured surfaces face
constexpr uint32_t kMaxMeshCards = 12;        // per mesh (MaxLumenMeshCards)

struct MeshCard
{
    float3 origin;        // box centre, mesh space
    float3 extent;        // half sizes along the card's axes (x, y in the card plane, z = half depth range)
    uint32_t direction = 0;
};

struct MeshCardStats  // what the generator did (diagnostics)
{
    uint32_t passes = 0;          // voxel grids tried (halved while the mesh gives more than 10,000 surfels)
    uint64_t columnTests = 0;     // ray-triangle tests of the column rays
    uint64_t hemisphereRays = 0;  // surfel visibility rays
    float bvhMs = 0, columnMs = 0, visibilityMs = 0, clusterMs = 0;
};

struct MeshCards
{
    float3 boundsMin, boundsMax;   // the mesh bounds the cards were built in (mesh bounds grown by 1 cm)
    bool mostlyTwoSided = false;   // a quarter or more of the triangles are two-sided (foliage): outer cards only
    std::vector<MeshCard> cards;   // sorted by direction, then by importance
    uint32_t surfels = 0;          // surface elements the clustering saw (diagnostics)
    MeshCardStats stats;
};

// A card's axes in mesh space: z = the direction's normal; x, y span the card plane (the generator's cluster basis).
void meshCardAxes(uint32_t direction, float3& x, float3& y, float3& z);

// Cards of one mesh at its bind pose. 'triangleTwoSided': one flag per triangle (indices / 3) or empty = none.
// 'metresPerUnit': the uniform scale the mesh is shown at (its instances' scale): the generator's lengths (10 cm voxels,
// 1 cm bounds growth, ...) are world metres, so a mesh authored in other units and scaled by its instances gets the
// same cards as the same shape authored in metres. Unreal builds in asset units and assumes they are world units.
// Deterministic: the same mesh and scale give the same cards.
MeshCards buildMeshCards(const Mesh& mesh, const std::vector<uint8_t>& triangleTwoSided, uint32_t maxCards = kMaxMeshCards, float metresPerUnit = 1.0f);
// The flags from the mesh's own materials (Material::twoSided of each submesh).
MeshCards buildMeshCards(const Mesh& mesh, const std::vector<Material>& materials, uint32_t maxCards = kMaxMeshCards, float metresPerUnit = 1.0f);
} // namespace unx::scene
