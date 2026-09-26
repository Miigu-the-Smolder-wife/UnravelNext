#pragma once
// Cluster LOD builder (ARCHITECTURE 2.1, 7.2; INTERFACES_KO.md 6.5). Owner: V.
//
// For every scene mesh: 64-triangle clusters (visibility.cluster_triangles) of every submesh, a DAG of simplified
// groups with monotone errors and enclosing spheres (meshoptimizer's clusterlod scheme), a per-depth node forest for
// GPU traversal, uniform-error cuts for ray tracing (gpu::LodLevel), and the minimum feature width of every cluster
// (bands A/B/C). Output is ClusterData for GpuScene::setClusters.
//
// Thin geometry is protected from LOD thinning: a group may be simplified only with an error below
// visibility.lod_max_relative_width_error x its narrowest feature; otherwise the group is terminal and stays at its
// geometry (far thin geometry is represented by band C bricks, not by collapsed triangles).
#include "unx/core/Config.h"
#include "unx/render/GpuScene.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <string>
#include <vector>

namespace unx::clusterbuilder
{
struct Settings
{
    uint32_t clusterTriangles = 0;     // visibility.cluster_triangles
    uint32_t clusterVertices = 0;      // visibility.cluster_vertices
    float maxRelativeWidthError = 0;   // visibility.lod_max_relative_width_error
    float widthAreaPercentile = 0;     // visibility.feature_width_area_percentile
    // Fill of disconnected geometry (leaves, blades, cards, wires; v1.36): a meshlet may end at a disconnected
    // neighbour only once it has clusterMinTriangles (meshoptimizer's flex builder), and planar components narrower
    // than sheetOrientationMinWidth are clustered across orientations instead of one DAG per orientation class.
    uint32_t clusterMinTriangles = 0;       // visibility.cluster_min_triangles
    float sheetOrientationMinWidth = 0;     // visibility.sheet_orientation_min_width (m)

    static Settings fromQuality(const QualityConfig& quality);
};

struct MeshStats
{
    uint32_t sourceTriangles = 0;
    uint32_t clusters = 0;            // all DAG clusters
    uint32_t sourceClusters = 0;      // clusters of the source geometry
    uint32_t groups = 0;
    uint32_t terminalGroups = 0;      // simplification stopped (stuck, thin-feature error limit, or folds)
    uint32_t foldRetries = 0;         // simplifications redone with half the error because a triangle folded over
    uint32_t foldTerminalGroups = 0;  // groups left unsimplified because no fold-free result was found
    uint32_t depth = 0;               // DAG depth (levels of groups)
    uint32_t nodes = 0;
    uint32_t lodLevels = 0;
    uint32_t coarsestTriangles = 0;   // triangles of the coarsest uniform cut
    double buildMs = 0;
};

struct BuildStats
{
    std::vector<MeshStats> meshes;
    double wallMs = 0;
    uint32_t reusedMeshes = 0;  // meshes whose hierarchy came from the previous build (same content and settings)
    uint32_t diskMeshes = 0;    // meshes not in memory whose hierarchy was read from the disk cache (setDiskCache)
};

// Builds the hierarchy of every mesh (meshes in parallel on the job pool). Deterministic: the same scene and
// settings give byte-identical output. A mesh whose content (positions, normals, uv0, indices, submesh ranges) and
// settings match one of the previous build's reuses its hierarchy (SHA-256 identity), so re-building an edited scene
// costs only the new and changed meshes; identical meshes in one scene are built once.
render::ClusterData build(const scene::Scene& scene, const Settings& settings, BuildStats* stats = nullptr);
// Forgets the previous build's hierarchies (tests that compare two cold builds).
void clearMeshCache();
// Disk cache of hierarchies (C1, incremental cooking): a mesh whose key (content, settings, builder source hash) has an
// entry in <directory>/clusters is read instead of built, and new builds are written there. Empty: no disk cache. Until
// this is called, the directory is the environment variable UNX_COOK_CACHE (unset: none). Output is byte-identical
// either way.
void setDiskCache(const std::string& directory);

// Named V buffers inside ClusterData (ClusterHierarchy.h).
constexpr const char* kClusterNodes = "clusterNodes";
constexpr const char* kMeshClusterRoots = "meshClusterRoots";
constexpr const char* kClusterLodSpheres = "clusterLodSpheres";
// float4 per cluster: sheet orientation for band classification (ARCHITECTURE 2.1), independent of winding:
// xyz = axis x cos(spread), where axis is the principal direction of sum(area n n^T) over its triangles and spread the
// largest angle between the axis and a triangle normal up to sign (xyz = 0: spread >= 90 degrees); w = half-thickness
// of the slab around its bounds centre along the axis (max |dot(axis, p - centre)|). Object space.
constexpr const char* kClusterSheets = "clusterSheets";
} // namespace unx::clusterbuilder
