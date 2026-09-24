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
#include <vector>

namespace unx::clusterbuilder
{
struct Settings
{
    uint32_t clusterTriangles = 0;     // visibility.cluster_triangles
    uint32_t clusterVertices = 0;      // visibility.cluster_vertices
    float maxRelativeWidthError = 0;   // visibility.lod_max_relative_width_error
    float widthAreaPercentile = 0;     // visibility.feature_width_area_percentile

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
};

// Builds the hierarchy of every mesh (meshes in parallel on the job pool). Deterministic: the same scene and
// settings give byte-identical output.
render::ClusterData build(const scene::Scene& scene, const Settings& settings, BuildStats* stats = nullptr);

// Named V buffers inside ClusterData (ClusterHierarchy.h).
constexpr const char* kClusterNodes = "clusterNodes";
constexpr const char* kMeshClusterRoots = "meshClusterRoots";
constexpr const char* kClusterLodSpheres = "clusterLodSpheres";
} // namespace unx::clusterbuilder
