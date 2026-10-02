#pragma once
// Cluster LOD builder (ARCHITECTURE 2.1, 7.2; INTERFACES_KO.md 6.5). Owner: V.
//
// For every scene mesh: 64-triangle clusters (visibility.cluster_triangles) of every submesh, a DAG of simplified
// groups with monotone errors and enclosing spheres (meshoptimizer's clusterlod scheme), a per-depth node forest for
// GPU traversal, uniform-error cuts for ray tracing (gpu::LodLevel), and the minimum feature width of every cluster
// (bands A/B/C). Output is ClusterData for GpuScene::setClusters.
//
// Thin geometry is protected from LOD thinning: a group may be simplified only with an error below
// visibility.lod_max_relative_width_error x its narrowest feature. A group that limit stops is terminal and stays at
// its geometry, unless visibility.lod_thin_preserve_area is on and the caller takes the builder's vertices
// (LodVertices): then whole disconnected pieces of the group (leaves, blades, needles) are dropped evenly through it
// and the remaining ones are enlarged until the group has its surface area again, so a far tree has few clusters and
// still covers the same share of the screen.
#include "unx/core/Config.h"
#include "unx/render/GpuScene.h"
#include "unx/scene/SceneData.h"

#include <cstdint>
#include <string>
#include <vector>

namespace unx::clusterbuilder
{
struct StreamPages;

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
    // Thin geometry has LOD and keeps its area (see above). The enlarged pieces need vertices of their own, so it
    // takes effect only in a build that returns them (build's lodVertices); other builds keep such groups terminal.
    bool thinPreserveArea = false;          // visibility.lod_thin_preserve_area

    // Compressed cluster vertices (ClusterStream.h): the clusters of rigid meshes get a bit stream of their own vertices and
    // the GPU's cluster vertex pool their handles. Not part of a mesh's cached hierarchy: the stream is made from it
    // when the scene's data is put together, so cache entries serve both settings.
    bool compression = false;               // visibility.cluster_compression
    bool streaming = false;                 // visibility.cluster_streaming: with compression and build's 'pages', the
                                            // groups' vertex bits go into streaming pages (ClusterStream.h)
    float positionStep = 0;                 // visibility.cluster_position_step (m): the position grid is at most this
    uint32_t normalBits = 0, tangentBits = 0, uvBits = 0;  // visibility.cluster_normal_bits, _tangent_bits, _uv_bits

    // Source clusters only, no LOD DAG (C2b runtime meshes: shallow hierarchies for GpuScene::addRuntimeMesh; meshes with
    // blend shapes or a vertex animation get this regardless). Not a quality key: exact at every distance.
    bool noSimplification = false;

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
    uint32_t thinnedGroups = 0;       // groups past the thin-feature limit: pieces dropped, the rest enlarged (thinPreserveArea)
    uint32_t lodVertices = 0;         // vertices made for their enlarged pieces (LodVertices)
    double buildMs = 0;
};

struct BuildStats
{
    std::vector<MeshStats> meshes;
    double wallMs = 0;
    uint32_t reusedMeshes = 0;  // meshes whose hierarchy came from the previous build (same content and settings)
    uint32_t diskMeshes = 0;    // meshes not in memory whose hierarchy was read from the disk cache (setDiskCache)
};

// Vertices the builder makes (Settings::thinPreserveArea). Clusters index their mesh's vertices, and an enlarged piece
// of thin geometry is at none of them, so the builder gives it new vertices after the mesh's own: cluster vertex index
// (the mesh's vertex count) + i is meshes[m].positions[i], with every other stream (normal, tangent, uv, skin) of the
// mesh vertex meshes[m].source[i]. The scene's meshes must hold them before anything reads vertices through cluster
// indices (GpuScene::upload, R's cut geometry): build, then appendTo, then upload.
struct LodVertices
{
    struct Mesh
    {
        std::vector<float3> positions;  // object space
        std::vector<uint32_t> source;   // mesh vertex the other streams are copied from
    };
    std::vector<Mesh> meshes;  // one per scene mesh; empty lists for a mesh without any
    // Appends every mesh's vertices to it (no triangle of Mesh::indices uses them). Once per build: the appended scene is
    // another scene to the builder (its meshes' keys differ).
    void appendTo(scene::Scene& scene) const;
};

// Builds the hierarchy of every mesh (meshes in parallel on the job pool). Deterministic: the same scene and
// settings give byte-identical output. A mesh whose content (positions, normals, uv0, indices, submesh ranges) and
// settings match one of the previous build's reuses its hierarchy (SHA-256 identity), so re-building an edited scene
// costs only the new and changed meshes; identical meshes in one scene are built once.
// lodVertices: where the builder's own vertices go. Without it no cluster indexes a vertex the mesh does not have, and
// Settings::thinPreserveArea has no effect (the output is that of the setting off).
// pages (ClusterStream.h StreamPages): with Settings::compression and Settings::streaming, where the streamed groups'
// vertex bits go - the cook writes them to a page file (streaming::PageFileWriter) for the renderer's page source.
render::ClusterData build(const scene::Scene& scene, const Settings& settings, BuildStats* stats = nullptr, LodVertices* lodVertices = nullptr,
                          StreamPages* pages = nullptr);
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
