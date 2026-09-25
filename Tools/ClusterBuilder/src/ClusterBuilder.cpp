#include "unx/clusterbuilder/ClusterBuilder.h"

#include "FeatureWidth.h"
#include "unx/clusterbuilder/ClusterHierarchy.h"
#include "unx/core/Jobs.h"
#include "unx/core/Log.h"

#include <meshoptimizer.h>

// meshoptimizer's cluster LOD scheme (demo/clusterlod.h, MIT, Arseny Kapoulkine): its helpers (clusterize, partition,
// lockBoundary, boundsCompute, mergeGroups) are used as is; the build loop below is ours because it adds the
// thin-feature error limit and drops the sloppy fallback (which merges disconnected leaves and blades into blobs).
#pragma warning(push, 0)
#pragma warning(disable : 4505)  // unused static helpers of the header
#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>
#pragma warning(pop)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <unordered_map>

namespace unx::clusterbuilder
{
namespace
{
constexpr uint32_t kNodeWidth = 8;       // children per hierarchy node (GPU traversal fan-out)
constexpr uint32_t kAttributeCount = 5;  // normal xyz, uv

struct ClusterOut
{
    std::vector<uint32_t> indices;  // mesh vertex indices
    int refined = -1;               // group this cluster was simplified from (-1 = source geometry)
    clodBounds lod{};               // sphere + error the cluster's own error is measured on
    uint32_t submesh = 0;
    float width = 0;                // signed minimum feature width
};

struct GroupOut
{
    clodBounds simplified{};  // parent sphere + error of this group's clusters (FLT_MAX: terminal)
    int depth = 0;
    uint32_t firstCluster = 0, clusterCount = 0;
};

struct MeshOut
{
    std::vector<ClusterOut> clusters;  // grouped: every group's clusters are contiguous
    std::vector<GroupOut> groups;
    std::vector<clodNode> nodes;
    uint32_t levelCount = 0;
    MeshStats stats;
};

std::vector<unsigned int> simplifyLimited(const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& indices, const std::vector<unsigned char>& locks,
                                          size_t targetCount, float maxError, float* error)
{
    if (targetCount > indices.size()) return indices;
    std::vector<unsigned int> lod(indices.size());
    const unsigned int options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute | (config.simplify_permissive ? meshopt_SimplifyPermissive : 0u) |
                                 (config.simplify_regularize ? meshopt_SimplifyRegularize : 0u);
    lod.resize(meshopt_simplifyWithAttributes(lod.data(), indices.data(), indices.size(), mesh.vertex_positions, mesh.vertex_count, mesh.vertex_positions_stride,
                                              mesh.vertex_attributes, mesh.vertex_attributes_stride, mesh.attribute_weights, mesh.attribute_count, locks.data(), targetCount,
                                              maxError, options, error));
    return lod;
}

// meshoptimizer rejects a collapse that turns a triangle by more than ~75 degrees relative to its state just before
// that collapse, so a chain of collapses can still end with a triangle facing away from the surface it replaces,
// typically a sliver between locked border vertices. Returns the triangles (offsets into lod) that face away from the
// source surface at any of their corners (area-weighted vertex normals of the group's input triangles).
std::vector<size_t> foldedTriangles(const clodMesh& mesh, const std::vector<unsigned int>& remap, const std::vector<unsigned int>& source,
                                    const std::vector<unsigned int>& lod)
{
    const size_t stride = mesh.vertex_positions_stride / sizeof(float);
    auto pos = [&](unsigned int v) { const float* p = mesh.vertex_positions + v * stride; return float3{ p[0], p[1], p[2] }; };
    std::unordered_map<unsigned int, float3> normals;  // by welded position
    normals.reserve(source.size());
    for (size_t t = 0; t < source.size(); t += 3)
    {
        const float3 n = cross(pos(source[t + 1]) - pos(source[t]), pos(source[t + 2]) - pos(source[t]));
        for (int k = 0; k < 3; ++k)
        {
            float3& acc = normals[remap[source[t + k]]];
            acc = acc + n;
        }
    }
    std::vector<size_t> folded;
    for (size_t t = 0; t < lod.size(); t += 3)
    {
        const float3 n = cross(pos(lod[t + 1]) - pos(lod[t]), pos(lod[t + 2]) - pos(lod[t]));
        if (dot(n, n) <= 0) continue;
        // Every corner must agree: a sliver folded between border vertices on a slope can still point roughly along
        // the sum of the corner normals.
        if (dot(n, normals[remap[lod[t]]]) <= 0 || dot(n, normals[remap[lod[t + 1]]]) <= 0 || dot(n, normals[remap[lod[t + 2]]]) <= 0) folded.push_back(t);
    }
    return folded;
}

void buildSubmesh(const Settings& settings, const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& remap, std::vector<unsigned char>& locks,
                  const detail::MeshWidthContext& widths, uint32_t submesh, MeshOut& out)
{
    using namespace clod;
    std::vector<Cluster> clusters = clusterize(config, mesh, mesh.indices, mesh.index_count);
    std::vector<detail::MeshWidthContext::Width> clusterWidth;
    for (Cluster& c : clusters)
    {
        c.bounds = boundsCompute(mesh, c.indices, 0.f);
        clusterWidth.push_back(widths.clusterWidth(c.indices.data(), c.indices.size()));
    }
    out.stats.sourceClusters += (uint32_t)clusters.size();
    std::vector<int> pending(clusters.size());
    for (size_t i = 0; i < clusters.size(); ++i) pending[i] = (int)i;
    int depth = 0;
    // Positions of terminal clusters stay locked at every later level: those clusters are drawn at every coarser error,
    // so their neighbours' simplifications must keep the shared border (lockBoundary only locks borders between the
    // groups of the current level).
    std::vector<unsigned char> frozen(mesh.vertex_count, 0);  // by welded position

    auto emit = [&](const std::vector<int>& group, const clodBounds& simplified) {
        GroupOut g;
        g.simplified = simplified;
        g.depth = depth;
        g.firstCluster = (uint32_t)out.clusters.size();
        g.clusterCount = (uint32_t)group.size();
        for (int ci : group)
        {
            ClusterOut c;
            c.indices.assign(clusters[ci].indices.begin(), clusters[ci].indices.end());
            c.refined = clusters[ci].refined;
            c.lod = clusters[ci].bounds;
            c.submesh = submesh;
            c.width = clusterWidth[ci].narrowest;
            if (simplified.error == FLT_MAX)
                for (unsigned int v : c.indices) frozen[remap[v]] = 1;
            out.clusters.push_back(std::move(c));
        }
        out.groups.push_back(g);
        if (simplified.error == FLT_MAX) ++out.stats.terminalGroups;
        return (int)out.groups.size() - 1;
    };

    while (pending.size() > 1)
    {
        std::vector<std::vector<int>> groups = partition(config, mesh, clusters, pending, remap);
        pending.clear();
        lockBoundary(locks, groups, clusters, remap, mesh.vertex_lock);
        for (size_t v = 0; v < mesh.vertex_count; ++v)
            if (frozen[remap[v]]) locks[v] |= meshopt_SimplifyVertex_Lock;
        for (const std::vector<int>& group : groups)
        {
            std::vector<unsigned int> merged;
            float groupWidth = FLT_MAX;
            for (int ci : group)
            {
                merged.insert(merged.end(), clusters[ci].indices.begin(), clusters[ci].indices.end());
                groupWidth = std::min(groupWidth, clusterWidth[ci].guard);
            }
            const size_t target = size_t((merged.size() / 3) * config.simplify_ratio) * 3;
            clodBounds bounds = mergeGroups(clusters, group);  // encloses every member's sphere: monotone
            float error = 0.f;
            const float maxError = groupWidth >= FLT_MAX ? FLT_MAX : settings.maxRelativeWidthError * groupWidth;
            std::vector<unsigned int> simplified;
            bool stuck = false;
            // A fold-over is removed by locking the input vertices around its corners (the collapses that produced
            // it) and simplifying again at the same error; the rest of the group still simplifies.
            for (int attempt = 0;; ++attempt)
            {
                simplified = simplifyLimited(config, mesh, merged, locks, target, maxError, &error);
                stuck = simplified.size() > merged.size() * config.simplify_threshold;
                if (stuck) break;
                const std::vector<size_t> folded = foldedTriangles(mesh, remap, merged, simplified);
                if (folded.empty()) break;
                ++out.stats.foldRetries;
                if (attempt == 7)
                {
                    stuck = true;  // still folding after 8 attempts: keep the group's geometry
                    ++out.stats.foldTerminalGroups;
                    break;
                }
                std::unordered_map<unsigned int, bool> corner;
                for (size_t t : folded)
                    for (int k = 0; k < 3; ++k) corner[remap[simplified[t + k]]] = true;
                std::unordered_map<unsigned int, bool> ring;
                for (size_t t = 0; t < merged.size(); t += 3)
                    if (corner.count(remap[merged[t]]) || corner.count(remap[merged[t + 1]]) || corner.count(remap[merged[t + 2]]))
                        for (int k = 0; k < 3; ++k) ring[remap[merged[t + k]]] = true;
                for (unsigned int v : merged)
                    if (ring.count(remap[v])) locks[v] |= meshopt_SimplifyVertex_Lock;
            }
            if (stuck)
            {
                bounds.error = FLT_MAX;  // terminal: stuck, the thin-feature error limit, or folds stop it
                emit(group, bounds);
                continue;
            }
            bounds.error = std::max(bounds.error * config.simplify_error_merge_previous, error) + error * config.simplify_error_merge_additive;
            const int refined = emit(group, bounds);
            for (int ci : group) clusters[ci].indices = std::vector<unsigned int>();
            std::vector<Cluster> split = clusterize(config, mesh, simplified.data(), simplified.size());
            for (Cluster& c : split)
            {
                c.refined = refined;
                c.bounds = bounds;
                clusterWidth.push_back(widths.clusterWidth(c.indices.data(), c.indices.size()));
                clusters.push_back(std::move(c));
                pending.push_back((int)clusters.size() - 1);
            }
        }
        ++depth;
    }
    if (!pending.empty())
    {
        clodBounds bounds = clusters[pending[0]].bounds;
        bounds.error = FLT_MAX;
        emit(pending, bounds);
    }
}

// Sign-agnostic orientation of sheet triangles (a two-sided sheet's winding does not matter): the principal direction
// of sum(area n n^T) and cos of the largest angle between it and a triangle's normal line (0: 90 degrees or more).
struct SheetOrientation
{
    float3 axis{ 0, 0, 1 };
    float cosSpread = 0;
};

SheetOrientation sheetOrientation(const std::vector<float3>& positions, const uint32_t* indices, size_t indexCount)
{
    double s[3][3] = {};
    bool any = false;
    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
        float3 n = cross(positions[indices[i + 1]] - positions[indices[i]], positions[indices[i + 2]] - positions[indices[i]]);
        const float area2 = length(n);
        if (!(area2 > 0)) continue;
        n = n * (1.0f / area2);
        const double w = area2, v[3] = { n.x, n.y, n.z };
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) s[r][c] += w * v[r] * v[c];
        any = true;
    }
    SheetOrientation out;
    if (!any) return out;
    // Power iteration from the column of the largest diagonal (never orthogonal to the dominant eigenvector unless the
    // matrix is isotropic, where every axis is as good).
    int start = 0;
    for (int k = 1; k < 3; ++k)
        if (s[k][k] > s[start][start]) start = k;
    double v[3] = { s[0][start], s[1][start], s[2][start] };
    for (int it = 0; it < 64; ++it)
    {
        double u[3];
        for (int r = 0; r < 3; ++r) u[r] = s[r][0] * v[0] + s[r][1] * v[1] + s[r][2] * v[2];
        const double len = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        if (!(len > 0)) break;
        for (int r = 0; r < 3; ++r) v[r] = u[r] / len;
    }
    out.axis = normalize(float3{ (float)v[0], (float)v[1], (float)v[2] });
    float minDot = 1;
    for (size_t i = 0; i + 2 < indexCount; i += 3)
    {
        const float3 n = cross(positions[indices[i + 1]] - positions[indices[i]], positions[indices[i + 2]] - positions[indices[i]]);
        const float area2 = length(n);
        if (area2 > 0) minDot = std::min(minDot, std::fabs(dot(n, out.axis)) / area2);
    }
    out.cosSpread = std::max(minDot, 0.0f);
    return out;
}

// Orientation class of a planar component (spread within 5 degrees): one of 144 cells of an octahedral map of the
// axis hemisphere; 0 for curved or solid components. Components share no vertices, so building each class as its own
// DAG needs no locks, and a cluster never mixes sheets of different orientations (crossed cards): its sheet spread stays
// small, so only the sheets seen edge-on go to band B.
int32_t orientationClass(const SheetOrientation& o)
{
    constexpr float kPlanarCos = 0.9961947f;  // cos 5 degrees
    constexpr int32_t kCells = 12;             // cells of about 8 degrees
    if (o.cosSpread < kPlanarCos) return 0;
    float3 a = o.axis;
    if (a.z < 0 || (a.z == 0 && (a.y < 0 || (a.y == 0 && a.x < 0)))) a = a * -1.0f;
    const float l1 = std::fabs(a.x) + std::fabs(a.y) + a.z;
    const float px = a.x / l1, py = a.y / l1;  // |px| + |py| <= 1
    const float u = std::clamp((px + py + 1) * 0.5f, 0.0f, 0.999999f), w = std::clamp((py - px + 1) * 0.5f, 0.0f, 0.999999f);
    return 1 + (int32_t)(u * kCells) + kCells * (int32_t)(w * kCells);
}

MeshOut buildMesh(const scene::Mesh& m, const Settings& settings)
{
    const auto t0 = std::chrono::steady_clock::now();
    MeshOut out;
    const size_t vertexCount = m.positions.size();
    out.stats.sourceTriangles = (uint32_t)(m.indices.size() / 3);
    if (m.submeshes.size() > 0xFFFF) fail("cluster builder: mesh '%s' has %zu submeshes (limit 65536)", m.name.c_str(), m.submeshes.size());
    if (vertexCount == 0 || m.indices.empty()) return out;

    detail::MeshWidthContext widths(m.positions, m.indices);
    const std::vector<uint32_t>& remap = widths.weld();

    // Attributes for attribute-aware simplification: normal (weighted) and uv (protected at seams only).
    std::vector<float> attributes(vertexCount * kAttributeCount, 0.0f);
    for (size_t v = 0; v < vertexCount; ++v)
    {
        if (v < m.normals.size())
        {
            attributes[v * kAttributeCount + 0] = m.normals[v].x;
            attributes[v * kAttributeCount + 1] = m.normals[v].y;
            attributes[v * kAttributeCount + 2] = m.normals[v].z;
        }
        if (v < m.uv0.size())
        {
            attributes[v * kAttributeCount + 3] = m.uv0[v].x;
            attributes[v * kAttributeCount + 4] = m.uv0[v].y;
        }
    }
    const float weights[kAttributeCount] = { 0.5f, 0.5f, 0.5f, 0.0f, 0.0f };

    // Submesh boundaries stay fixed (clusters never span submeshes, so each submesh's DAG must keep the shared edge).
    std::vector<uint32_t> owner(vertexCount, UINT32_MAX);
    std::vector<unsigned char> boundaryLock(vertexCount, 0);
    for (uint32_t s = 0; s < (uint32_t)m.submeshes.size(); ++s)
    {
        const scene::Submesh& sm = m.submeshes[s];
        for (uint32_t i = sm.indexOffset; i < sm.indexOffset + sm.indexCount; ++i)
        {
            const uint32_t w = remap[m.indices[i]];
            if (owner[w] == UINT32_MAX) owner[w] = s;
            else if (owner[w] != s) boundaryLock[w] = meshopt_SimplifyVertex_Lock;
        }
    }
    for (size_t v = 0; v < vertexCount; ++v) boundaryLock[v] = boundaryLock[remap[v]];

    clodConfig config = clodDefaultConfig(settings.clusterTriangles);
    config.max_vertices = settings.clusterVertices;
    // Disconnected geometry (a leaf, a blade per component): the flex builder ends a meshlet at min_triangles when the
    // best next triangle is not connected, so min_triangles sets the fill of foliage clusters (v1.36).
    config.min_triangles = settings.clusterMinTriangles;
#if MESHOPTIMIZER_VERSION < 1000
    config.min_triangles &= ~3;
#endif
    config.simplify_fallback_sloppy = false;   // never merge disconnected geometry into blobs
    config.simplify_error_edge_limit = 0.0f;   // error is geometric only
    config.optimize_bounds = false;            // cluster.bounds = the LOD sphere; tight culling bounds computed below

    // Each submesh is built as one DAG per width class: connected components whose widths differ by 4x or more never
    // share a group, so a group's error limit (thin-feature guard) suits every component in it. Components share no
    // vertices, so splitting them needs no locks.
    auto widthClass = [&](uint32_t vertex) {
        const float w = widths.componentWidth(vertex);
        return w >= FLT_MAX ? INT32_MAX : (int32_t)std::floor(std::log(std::max(w, 1e-9f)) / std::log(4.0f));
    };
    // Orientation class per connected component (orientationClass): planar components are split by orientation, so a
    // cluster's sheet spread stays small and only the sheets seen edge-on leave band A. Components narrower than
    // sheetOrientationMinWidth (grass blades: band B beyond a few metres whatever their orientation) are clustered
    // across orientations: one class per blade would leave a blade per cluster (v1.36).
    std::unordered_map<uint32_t, std::vector<uint32_t>> componentTriangles;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
    {
        std::vector<uint32_t>& t = componentTriangles[widths.component(m.indices[i])];
        t.insert(t.end(), { m.indices[i], m.indices[i + 1], m.indices[i + 2] });
    }
    std::unordered_map<uint32_t, int32_t> componentOrientation;
    for (const auto& [component, tris] : componentTriangles)
        componentOrientation[component] =
            widths.componentWidth(tris[0]) < settings.sheetOrientationMinWidth ? 0 : orientationClass(sheetOrientation(m.positions, tris.data(), tris.size()));
    componentTriangles.clear();
    for (uint32_t s = 0; s < (uint32_t)m.submeshes.size(); ++s)
    {
        const scene::Submesh& sm = m.submeshes[s];
        // Degenerate triangles (repeated welded vertex or zero area) cover nothing and are dropped.
        std::map<std::pair<int32_t, int32_t>, std::vector<unsigned int>> byClass;  // (width class, orientation class)
        for (uint32_t i = sm.indexOffset; i + 2 < sm.indexOffset + sm.indexCount; i += 3)
        {
            const uint32_t a = m.indices[i], b = m.indices[i + 1], c = m.indices[i + 2];
            if (remap[a] == remap[b] || remap[b] == remap[c] || remap[a] == remap[c]) continue;
            if (length(cross(m.positions[b] - m.positions[a], m.positions[c] - m.positions[a])) <= 0) continue;
            std::vector<unsigned int>& target = byClass[{ widthClass(a), componentOrientation[widths.component(a)] }];
            target.insert(target.end(), { a, b, c });
        }
        for (const auto& [cls, indices] : byClass)
        {
        clodMesh mesh{};
        mesh.indices = indices.data();
        mesh.index_count = indices.size();
        mesh.vertex_count = vertexCount;
        mesh.vertex_positions = &m.positions[0].x;
        mesh.vertex_positions_stride = sizeof(float3);
        mesh.vertex_attributes = attributes.data();
        mesh.vertex_attributes_stride = kAttributeCount * sizeof(float);
        mesh.vertex_lock = boundaryLock.data();
        mesh.attribute_weights = weights;
        mesh.attribute_count = kAttributeCount;
        mesh.attribute_protect_mask = (1u << kAttributeCount) - 1;  // normal and uv seams

        // Protect attribute discontinuities (same position, different attributes), as clodBuild does.
        std::vector<unsigned char> locks(vertexCount, 0);
        for (size_t v = 0; v < vertexCount; ++v)
        {
            const uint32_t r = remap[v];
            if (r == v) continue;
            for (uint32_t k = 0; k < kAttributeCount; ++k)
                if (attributes[v * kAttributeCount + k] != attributes[r * kAttributeCount + k])
                {
                    locks[v] |= meshopt_SimplifyVertex_Protect;
                    break;
                }
        }
        buildSubmesh(settings, config, mesh, remap, locks, widths, s, out);
        }
    }

    // DAG invariants the cut relies on (exactly one of a group and its simplification is drawn at any error):
    // a cluster's own error is its source group's error, and never exceeds the error of the group it is in.
    for (const GroupOut& g : out.groups)
        for (uint32_t k = 0; k < g.clusterCount; ++k)
        {
            const ClusterOut& c = out.clusters[g.firstCluster + k];
            const float own = c.refined < 0 ? 0.0f : out.groups[c.refined].simplified.error;
            if (c.refined >= 0 && c.lod.error != own) fail("cluster builder: '%s' cluster error %g != source group error %g", m.name.c_str(), c.lod.error, own);
            if (!(own <= g.simplified.error)) fail("cluster builder: '%s' non-monotone errors: cluster %g in group %g (depth %d)", m.name.c_str(), own, g.simplified.error, g.depth);
        }

    // Hierarchy: one tree per DAG depth over all groups of the mesh (roots are nodes 0 .. levelCount-1).
    int maxDepth = -1;
    for (const GroupOut& g : out.groups) maxDepth = std::max(maxDepth, g.depth);
    out.levelCount = (uint32_t)(maxDepth + 1);
    std::vector<clodGroup> groups(out.groups.size());
    for (size_t i = 0; i < groups.size(); ++i) groups[i] = { out.groups[i].depth, out.groups[i].simplified };
    if (!groups.empty())
    {
        out.nodes.resize(clodBuildHierarchyBound(groups.size(), kNodeWidth, out.levelCount));
        out.nodes.resize(clodBuildHierarchy(out.nodes.data(), groups.data(), groups.size(), kNodeWidth, out.levelCount));
    }
    out.stats.clusters = (uint32_t)out.clusters.size();
    out.stats.groups = (uint32_t)out.groups.size();
    out.stats.depth = out.levelCount;
    out.stats.nodes = (uint32_t)out.nodes.size();
    out.stats.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

void appendNamed(render::ClusterData& data, const char* name, const void* bytes, size_t size, uint32_t stride)
{
    for (render::ClusterData::Named& n : data.named)
        if (n.name == name)
        {
            const uint8_t* p = static_cast<const uint8_t*>(bytes);
            n.bytes.insert(n.bytes.end(), p, p + size);
            return;
        }
    render::ClusterData::Named n;
    n.name = name;
    n.stride = stride;
    const uint8_t* p = static_cast<const uint8_t*>(bytes);
    n.bytes.assign(p, p + size);
    data.named.push_back(std::move(n));
}
} // namespace

Settings Settings::fromQuality(const QualityConfig& q)
{
    Settings s;
    s.clusterTriangles = (uint32_t)q.integer("visibility.cluster_triangles");
    s.clusterVertices = (uint32_t)q.integer("visibility.cluster_vertices");
    s.maxRelativeWidthError = (float)q.number("visibility.lod_max_relative_width_error");
    s.clusterMinTriangles = (uint32_t)q.integer("visibility.cluster_min_triangles");
    s.sheetOrientationMinWidth = (float)q.number("visibility.sheet_orientation_min_width");
    if (s.clusterTriangles < 4 || s.clusterTriangles > 128) fail("visibility.cluster_triangles must be 4..128 (vis id triangle field is 7 bits)");
    if (s.clusterVertices < 3 || s.clusterVertices > 128) fail("visibility.cluster_vertices must be 3..128 (V's mesh shaders output at most 128 vertices)");
    if (s.clusterMinTriangles < 1 || s.clusterMinTriangles > s.clusterTriangles) fail("visibility.cluster_min_triangles must be 1..cluster_triangles");
    if (!(s.sheetOrientationMinWidth >= 0)) fail("visibility.sheet_orientation_min_width must be >= 0");
    return s;
}

render::ClusterData build(const scene::Scene& scene, const Settings& settings, BuildStats* stats)
{
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<MeshOut> meshes(scene.meshes.size());
    Jobs::instance().parallelFor((uint32_t)scene.meshes.size(), [&](uint32_t i) { meshes[i] = buildMesh(scene.meshes[i], settings); });

    render::ClusterData data;
    data.meshes.resize(scene.meshes.size());
    // Named buffers exist even when empty so V can always bind them.
    appendNamed(data, kClusterNodes, nullptr, 0, sizeof(gpu::ClusterNode));
    appendNamed(data, kMeshClusterRoots, nullptr, 0, sizeof(gpu::MeshClusterRoots));
    appendNamed(data, kClusterLodSpheres, nullptr, 0, sizeof(float4));
    appendNamed(data, kClusterSheets, nullptr, 0, sizeof(float4));
    uint32_t nodeBase = 0;
    for (size_t mi = 0; mi < meshes.size(); ++mi)
    {
        MeshOut& mo = meshes[mi];
        const scene::Mesh& m = scene.meshes[mi];
        const uint32_t clusterBase = (uint32_t)data.clusters.size();
        render::ClusterData::MeshRange& range = data.meshes[mi];
        range.clusterOffset = clusterBase;
        range.clusterCount = (uint32_t)mo.clusters.size();

        // Clusters (group order), their LOD spheres and index pools.
        for (size_t gi = 0; gi < mo.groups.size(); ++gi)
        {
            const GroupOut& g = mo.groups[gi];
            for (uint32_t k = 0; k < g.clusterCount; ++k)
            {
                const ClusterOut& c = mo.clusters[g.firstCluster + k];
                const size_t indexCount = c.indices.size();
                const meshopt_Bounds b = meshopt_computeClusterBounds(c.indices.data(), indexCount, &m.positions[0].x, m.positions.size(), sizeof(float3));
                unsigned int localVertices[256];
                unsigned char localTriangles[3 * 256];
                const size_t vertexCount = clodLocalIndices(localVertices, localTriangles, c.indices.data(), indexCount);
                const size_t triangleCount = indexCount / 3;
                if (vertexCount > 255 || triangleCount > 128) fail("cluster builder: cluster with %zu vertices / %zu triangles", vertexCount, triangleCount);
                render::gpu::Cluster rc{};
                rc.boundsSphere = { b.center[0], b.center[1], b.center[2], b.radius };
                rc.normalCone = { b.cone_axis[0], b.cone_axis[1], b.cone_axis[2], b.cone_cutoff };
                rc.vertexOffset = (uint32_t)data.clusterVertexIndices.size();
                rc.triangleOffset = (uint32_t)data.clusterTriangles.size();
                rc.counts = (uint32_t)vertexCount | (uint32_t)triangleCount << 8 | c.submesh << 16;
                rc.material = m.submeshes[c.submesh].material;
                rc.lodError = c.refined < 0 ? 0.0f : c.lod.error;
                rc.parentLodError = g.simplified.error;
                rc.minFeatureWidth = c.width;
                rc.brick = render::gpu::kNone;
                data.clusterVertexIndices.insert(data.clusterVertexIndices.end(), localVertices, localVertices + vertexCount);
                for (size_t t = 0; t < triangleCount; ++t)
                    data.clusterTriangles.push_back((uint32_t)localTriangles[3 * t] | (uint32_t)localTriangles[3 * t + 1] << 8 | (uint32_t)localTriangles[3 * t + 2] << 16);
                data.clusters.push_back(rc);
                const float4 sphere{ c.lod.center[0], c.lod.center[1], c.lod.center[2], c.lod.radius };
                appendNamed(data, kClusterLodSpheres, &sphere, sizeof sphere, sizeof(float4));
                const SheetOrientation sheet = sheetOrientation(m.positions, c.indices.data(), indexCount);
                const float3 centre{ b.center[0], b.center[1], b.center[2] };
                float slab = 0;
                for (size_t v = 0; v < vertexCount; ++v) slab = std::max(slab, std::fabs(dot(sheet.axis, m.positions[localVertices[v]] - centre)));
                const float3 scaled = sheet.cosSpread > 0 ? sheet.axis * sheet.cosSpread : float3{ 0, 0, 0 };
                const float4 sheetData{ scaled.x, scaled.y, scaled.z, slab };
                appendNamed(data, kClusterSheets, &sheetData, sizeof sheetData, sizeof(float4));
            }
        }

        // Hierarchy nodes.
        std::vector<gpu::ClusterNode> nodes(mo.nodes.size());
        for (size_t n = 0; n < mo.nodes.size(); ++n)
        {
            const clodNode& cn = mo.nodes[n];
            gpu::ClusterNode& gn = nodes[n];
            gn.lodSphere = { cn.bounds.center[0], cn.bounds.center[1], cn.bounds.center[2], cn.bounds.radius };
            gn.lodError = cn.bounds.error;
            if (cn.group >= 0)
            {
                gn.first = clusterBase + mo.groups[cn.group].firstCluster;
                gn.count = mo.groups[cn.group].clusterCount;
                gn.leaf = 1;
            }
            else
            {
                gn.first = nodeBase + cn.child_offset;
                gn.count = cn.child_count;
                gn.leaf = 0;
            }
        }
        appendNamed(data, kClusterNodes, nodes.data(), nodes.size() * sizeof(gpu::ClusterNode), sizeof(gpu::ClusterNode));
        const gpu::MeshClusterRoots roots{ nodeBase, mo.levelCount };
        appendNamed(data, kMeshClusterRoots, &roots, sizeof roots, sizeof roots);
        nodeBase += (uint32_t)nodes.size();

        // Uniform-error cuts (ray tracing geometry, diagnostics): 0 and each depth's largest finite group error.
        std::vector<float> thresholds{ 0.0f };
        for (uint32_t d = 0; d < mo.levelCount; ++d)
        {
            float t = -1;
            for (const GroupOut& g : mo.groups)
                if (g.depth == (int)d && g.simplified.error != FLT_MAX) t = std::max(t, g.simplified.error);
            if (t >= 0) thresholds.push_back(t);
        }
        std::sort(thresholds.begin(), thresholds.end());
        thresholds.erase(std::unique(thresholds.begin(), thresholds.end()), thresholds.end());
        range.lodLevelOffset = (uint32_t)data.lodLevels.size();
        uint32_t previousTriangles = UINT32_MAX;
        for (float t : thresholds)
        {
            render::gpu::LodLevel level{};
            level.clusterOffset = (uint32_t)data.lodLevelClusters.size();
            level.error = t;
            std::vector<uint32_t> cut;
            for (uint32_t c = 0; c < range.clusterCount; ++c)
            {
                const render::gpu::Cluster& rc = data.clusters[clusterBase + c];
                if (rc.lodError <= t && t < rc.parentLodError)
                {
                    cut.push_back(clusterBase + c);
                    level.triangleCount += (rc.counts >> 8) & 0xFFu;
                }
            }
            if (level.triangleCount >= previousTriangles) continue;  // keep strictly coarser cuts only
            previousTriangles = level.triangleCount;
            level.clusterCount = (uint32_t)cut.size();
            data.lodLevelClusters.insert(data.lodLevelClusters.end(), cut.begin(), cut.end());
            data.lodLevels.push_back(level);
        }
        range.lodLevelCount = (uint32_t)data.lodLevels.size() - range.lodLevelOffset;
        mo.stats.lodLevels = range.lodLevelCount;
        mo.stats.coarsestTriangles = range.lodLevelCount ? data.lodLevels.back().triangleCount : 0;
    }
    if (stats)
    {
        stats->meshes.clear();
        for (const MeshOut& mo : meshes) stats->meshes.push_back(mo.stats);
        stats->wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    return data;
}
} // namespace unx::clusterbuilder
