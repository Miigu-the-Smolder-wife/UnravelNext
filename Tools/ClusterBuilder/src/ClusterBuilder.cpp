#include "unx/clusterbuilder/ClusterBuilder.h"

#include "FeatureWidth.h"
#include "unx/clusterbuilder/ClusterHierarchy.h"
#include "unx/core/Jobs.h"
#include "unx/core/Sha256.h"
#include "unx/core/Log.h"
#include "ClusterBuilderSourceHash.generated.h"

#include <meshoptimizer.h>
#include <windows.h>

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
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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

void buildSubmesh(const Settings& settings, bool morphed, const clodConfig& config, const clodMesh& mesh, const std::vector<unsigned int>& remap, std::vector<unsigned char>& locks,
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

    // C4: a mesh with blend shapes or a vertex animation keeps its source clusters only (terminal, one group each): its
    // simplification errors would be measured on the bind pose, which the morphs leave (a coarse cut could miss the
    // curvature a shape adds). Exact at every distance; its cost is the source triangle count.
    if (morphed)
    {
        for (int ci : pending)
        {
            clodBounds bounds = clusters[ci].bounds;
            bounds.error = FLT_MAX;
            emit({ ci }, bounds);
        }
        return;
    }
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

MeshOut buildMesh(const scene::Mesh& m, const Settings& settings, const std::vector<uint32_t>& seamVertices)
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
    // Seams with other instances (findSeams): the shared border stays at the source vertices at every level, so two
    // meshes that meet (terrain tiles, modular pieces) are watertight whatever cut each one draws.
    for (uint32_t v : seamVertices) boundaryLock[remap[v]] = meshopt_SimplifyVertex_Lock;
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
        buildSubmesh(settings, settings.noSimplification || !m.blendShapes.empty() || m.vertexAnimation.framesPerSecond > 0, config, mesh, remap, locks, widths, s, out);
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

namespace
{
// Identity of a mesh's hierarchy: everything buildMesh reads (positions, normals, uv0, indices, submesh ranges) and the
// settings. Materials are applied when the scene's buffers are assembled, so they are not part of it.
std::string meshKey(const scene::Mesh& m, const Settings& settings, const std::vector<uint32_t>& seamVertices)
{
    Sha256 h;
    auto add = [&](const auto& v) {
        const uint64_t n = v.size();
        h.update(&n, sizeof n);
        if (n) h.update(v.data(), n * sizeof(v[0]));
    };
    add(m.positions);
    add(m.normals);
    add(m.uv0);
    add(m.indices);
    const uint32_t morphed = settings.noSimplification || !m.blendShapes.empty() || m.vertexAnimation.framesPerSecond > 0;
    h.update(&morphed, sizeof morphed);
    add(seamVertices);
    for (const scene::Submesh& sm : m.submeshes)
    {
        const uint32_t range[2] = { sm.indexOffset, sm.indexCount };
        h.update(range, sizeof range);
    }
    const uint32_t ints[3] = { settings.clusterTriangles, settings.clusterVertices, settings.clusterMinTriangles };
    const float floats[3] = { settings.maxRelativeWidthError, settings.widthAreaPercentile, settings.sheetOrientationMinWidth };
    h.update(ints, sizeof ints);
    h.update(floats, sizeof floats);
    const std::array<uint8_t, 32> d = h.finish();
    return std::string(reinterpret_cast<const char*>(d.data()), d.size());
}

// Seam vertices of every mesh (sorted mesh vertex indices): open-border vertices on the mesh's bounding box whose world
// position (any static instance) coincides with such a vertex of another instance, within 1e-5 of the coordinate
// magnitude (at least 1e-5 m). Neighbours whose cuts differ would otherwise open a crack of up to both LOD errors along
// the shared border; a mesh that meets nothing keeps its border free to simplify. Placement is part of the scene, so
// a moved static instance changes its mesh's key (the builder runs on commit; instance edits after commit do not
// rebuild hierarchies, INTERFACES 6.3).
std::vector<std::vector<uint32_t>> findSeams(const scene::Scene& scene)
{
    const uint32_t meshCount = (uint32_t)scene.meshes.size();
    std::vector<std::vector<uint32_t>> candidates(meshCount), seams(meshCount);
    std::vector<uint8_t> used(meshCount, 0);
    for (const scene::Instance& in : scene.instances)
        if (in.mesh < meshCount && !(in.flags & (scene::InstanceDynamic | scene::InstanceSkinned))) used[in.mesh] = 1;
    Jobs::instance().parallelFor(meshCount, [&](uint32_t mi) {
        const scene::Mesh& m = scene.meshes[mi];
        if (!used[mi] || m.positions.empty()) return;
        float3 lo = m.positions[0], hi = lo;
        for (const float3& p : m.positions)
        {
            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
        }
        // Welded by exact position; edges counted over every submesh.
        auto bitsOf = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        std::map<std::tuple<uint32_t, uint32_t, uint32_t>, uint32_t> byPosition;
        std::vector<uint32_t> id(m.positions.size());
        for (uint32_t v = 0; v < (uint32_t)m.positions.size(); ++v)
        {
            const float3 p = m.positions[v];
            id[v] = byPosition.emplace(std::make_tuple(bitsOf(p.x), bitsOf(p.y), bitsOf(p.z)), v).first->second;
        }
        std::unordered_map<uint64_t, uint32_t> edges;
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
        {
            const uint32_t a = id[m.indices[t]], b = id[m.indices[t + 1]], c = id[m.indices[t + 2]];
            if (a == b || b == c || a == c) continue;
            for (auto [u, w] : { std::pair{ a, b }, std::pair{ b, c }, std::pair{ c, a } }) ++edges[(uint64_t)std::min(u, w) << 32 | std::max(u, w)];
        }
        auto onBox = [&](const float3& p) { return p.x == lo.x || p.x == hi.x || p.y == lo.y || p.y == hi.y || p.z == lo.z || p.z == hi.z; };
        std::vector<uint32_t> out;
        for (const auto& [e, count] : edges)
            if (count == 1)
                for (uint32_t v : { (uint32_t)(e >> 32), (uint32_t)e })
                    if (onBox(m.positions[v])) out.push_back(v);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        candidates[mi] = std::move(out);
    });
    // World positions of every static instance's candidates on a grid of cells twice the tolerance; matches are searched
    // in the 27 neighbouring cells.
    struct Point
    {
        double x, y, z;
        uint32_t instance, mesh, vertex;
    };
    std::vector<Point> points;
    for (uint32_t ii = 0; ii < (uint32_t)scene.instances.size(); ++ii)
    {
        const scene::Instance& in = scene.instances[ii];
        if (in.mesh >= meshCount || (in.flags & (scene::InstanceDynamic | scene::InstanceSkinned))) continue;
        const float3x4& t = in.transform;
        for (uint32_t v : candidates[in.mesh])
        {
            const float3 p = scene.meshes[in.mesh].positions[v];
            double w[3];
            for (int r = 0; r < 3; ++r) w[r] = (double)t.m[r][0] * p.x + (double)t.m[r][1] * p.y + (double)t.m[r][2] * p.z + t.m[r][3];
            points.push_back({ w[0], w[1], w[2], ii, in.mesh, v });
        }
    }
    double extent = 1.0;
    for (const Point& p : points) extent = std::max({ extent, std::fabs(p.x), std::fabs(p.y), std::fabs(p.z) });
    const double tolerance = std::max(1e-5, extent * 1e-5), cell = 2 * tolerance;
    auto cellOf = [&](double c) { return (int64_t)std::floor(c / cell); };
    auto key = [](int64_t x, int64_t y, int64_t z) { return (uint64_t)(x * 73856093) ^ (uint64_t)(y * 19349663) ^ (uint64_t)(z * 83492791); };
    std::unordered_map<uint64_t, std::vector<uint32_t>> grid;
    for (uint32_t k = 0; k < (uint32_t)points.size(); ++k) grid[key(cellOf(points[k].x), cellOf(points[k].y), cellOf(points[k].z))].push_back(k);
    std::vector<uint8_t> seam(points.size(), 0);
    Jobs::instance().parallelFor((uint32_t)points.size(), [&](uint32_t k) {
        const Point& p = points[k];
        const int64_t cx = cellOf(p.x), cy = cellOf(p.y), cz = cellOf(p.z);
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz)
                {
                    auto it = grid.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == grid.end()) continue;
                    for (uint32_t o : it->second)
                    {
                        const Point& q = points[o];
                        if (q.instance != p.instance && std::fabs(q.x - p.x) <= tolerance && std::fabs(q.y - p.y) <= tolerance && std::fabs(q.z - p.z) <= tolerance)
                        {
                            seam[k] = 1;
                            return;
                        }
                    }
                }
    });
    for (uint32_t k = 0; k < (uint32_t)points.size(); ++k)
        if (seam[k]) seams[points[k].mesh].push_back(points[k].vertex);
    for (std::vector<uint32_t>& list : seams)
    {
        std::sort(list.begin(), list.end());
        list.erase(std::unique(list.begin(), list.end()), list.end());
    }
    return seams;
}

// Hierarchies of the previous build, by mesh identity (D0 editing: a re-commit of an edited level rebuilds only the meshes
// that are new or changed). The cache keeps exactly the meshes of the latest build.
std::mutex g_meshCacheMutex;
std::unordered_map<std::string, std::shared_ptr<const MeshOut>> g_meshCache;
std::string g_diskCache;  // empty: no disk cache
bool g_diskCacheSet = false;

// ---- Disk cache (C1: a reopened project or an editor reload rebuilds nothing that was built before) ---------------------
// File <dir>/clusters/<key hex>.unxcl: magic, format version, the builder's source hash (UNX_CLUSTERBUILDER_SOURCE_HASH:
// SHA-256 of this builder's sources and meshoptimizer's, so any code change invalidates every entry), the mesh key, the
// payload, and the payload's SHA-256 (a torn or corrupted file is rebuilt, never used). Written to a temporary file and
// renamed, so readers never see a partial entry.
constexpr uint32_t kDiskMagic = 0x4C43584Eu;  // "NXCL"
constexpr uint32_t kDiskFormat = 1;

std::string hexOf(const std::string& key)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (unsigned char c : key)
    {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string diskCacheDirectory()
{
    {
        std::lock_guard lock(g_meshCacheMutex);
        if (g_diskCacheSet) return g_diskCache;
    }
    // Default: UNX_COOK_CACHE (the Unity editor sets it to Library/UnravelNextCook before the renderer loads content).
    // GetEnvironmentVariableW sees values the process set after start-up, unlike the CRT's copy.
    wchar_t buffer[1024];
    const DWORD n = GetEnvironmentVariableW(L"UNX_COOK_CACHE", buffer, 1024);
    if (n == 0 || n >= 1024) return {};
    return std::filesystem::path(std::wstring(buffer, n)).string();
}

struct Writer
{
    std::vector<uint8_t> bytes;
    void raw(const void* p, size_t n) { bytes.insert(bytes.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    template <class T> void pod(const T& v) { raw(&v, sizeof v); }
    template <class T> void vec(const std::vector<T>& v)
    {
        pod((uint64_t)v.size());
        if (!v.empty()) raw(v.data(), v.size() * sizeof(T));
    }
};

struct Reader
{
    const uint8_t* p;
    size_t left;
    bool ok = true;
    void raw(void* out, size_t n)
    {
        if (n > left)
        {
            ok = false;
            return;
        }
        std::memcpy(out, p, n);
        p += n;
        left -= n;
    }
    template <class T> void pod(T& v) { raw(&v, sizeof v); }
    template <class T> void vec(std::vector<T>& v)
    {
        uint64_t n = 0;
        pod(n);
        if (!ok || n > left / sizeof(T))
        {
            ok = false;
            return;
        }
        v.resize((size_t)n);
        if (n) raw(v.data(), (size_t)n * sizeof(T));
    }
};

std::vector<uint8_t> serialize(const MeshOut& mo)
{
    Writer w;
    w.pod((uint64_t)mo.clusters.size());
    for (const ClusterOut& c : mo.clusters)
    {
        w.vec(c.indices);
        w.pod(c.refined);
        w.pod(c.lod);
        w.pod(c.submesh);
        w.pod(c.width);
    }
    w.vec(mo.groups);
    w.vec(mo.nodes);
    w.pod(mo.levelCount);
    w.pod(mo.stats);
    return std::move(w.bytes);
}

bool deserialize(const uint8_t* p, size_t n, MeshOut& mo)
{
    Reader r{ p, n };
    uint64_t clusters = 0;
    r.pod(clusters);
    if (!r.ok || clusters > n) return false;
    mo.clusters.resize((size_t)clusters);
    for (ClusterOut& c : mo.clusters)
    {
        r.vec(c.indices);
        r.pod(c.refined);
        r.pod(c.lod);
        r.pod(c.submesh);
        r.pod(c.width);
        if (!r.ok) return false;
    }
    r.vec(mo.groups);
    r.vec(mo.nodes);
    r.pod(mo.levelCount);
    r.pod(mo.stats);
    return r.ok && r.left == 0;
}

std::filesystem::path diskPath(const std::string& dir, const std::string& key)
{
    return std::filesystem::path(dir) / "clusters" / (hexOf(key) + ".unxcl");
}

// Header: magic, format, source hash (32), key (32); then the payload; then the payload's SHA-256 (32).
constexpr size_t kHeaderBytes = 8 + 32 + 32;

std::string sourceHash()
{
    // UNX_CLUSTERBUILDER_SOURCE_HASH is 64 hex digits; the file stores its 32 bytes.
    const char* hex = UNX_CLUSTERBUILDER_SOURCE_HASH;
    std::string out(32, '\0');
    auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    for (int i = 0; i < 32; ++i) out[i] = (char)(v(hex[2 * i]) * 16 + v(hex[2 * i + 1]));
    return out;
}

std::shared_ptr<const MeshOut> loadFromDisk(const std::string& dir, const std::string& key)
{
    std::ifstream f(diskPath(dir, key), std::ios::binary);
    if (!f) return nullptr;
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < kHeaderBytes + 32) return nullptr;
    uint32_t head[2];
    std::memcpy(head, file.data(), 8);
    const std::string source = sourceHash();
    if (head[0] != kDiskMagic || head[1] != kDiskFormat || std::memcmp(file.data() + 8, source.data(), 32) != 0 ||
        std::memcmp(file.data() + 40, key.data(), 32) != 0)
        return nullptr;
    const size_t payload = file.size() - kHeaderBytes - 32;
    Sha256 h;
    h.update(file.data() + kHeaderBytes, payload);
    const std::array<uint8_t, 32> d = h.finish();
    if (std::memcmp(d.data(), file.data() + kHeaderBytes + payload, 32) != 0)
    {
        logf("UnravelNext cluster cache: %s is corrupted (checksum); rebuilding it\n", diskPath(dir, key).string().c_str());
        return nullptr;
    }
    auto mo = std::make_shared<MeshOut>();
    if (!deserialize(file.data() + kHeaderBytes, payload, *mo)) return nullptr;
    return mo;
}

void storeToDisk(const std::string& dir, const std::string& key, const MeshOut& mo)
{
    std::error_code ec;
    const std::filesystem::path path = diskPath(dir, key);
    std::filesystem::create_directories(path.parent_path(), ec);
    const std::vector<uint8_t> payload = serialize(mo);
    Sha256 h;
    h.update(payload.data(), payload.size());
    const std::array<uint8_t, 32> d = h.finish();
    const uint32_t head[2] = { kDiskMagic, kDiskFormat };
    const std::string source = sourceHash();
    const std::filesystem::path tmp = path.string() + ".tmp" + std::to_string(GetCurrentThreadId());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
        {
            logf("UnravelNext cluster cache: cannot write %s\n", tmp.string().c_str());
            return;
        }
        f.write((const char*)head, 8);
        f.write(source.data(), 32);
        f.write(key.data(), 32);
        f.write((const char*)payload.data(), (std::streamsize)payload.size());
        f.write((const char*)d.data(), 32);
        if (!f)
        {
            logf("UnravelNext cluster cache: writing %s failed\n", tmp.string().c_str());
            f.close();
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) std::filesystem::remove(tmp, ec);
}
} // namespace

void clearMeshCache()
{
    std::lock_guard lock(g_meshCacheMutex);
    g_meshCache.clear();
}

void setDiskCache(const std::string& directory)
{
    std::lock_guard lock(g_meshCacheMutex);
    g_diskCache = directory;
    g_diskCacheSet = true;
}

render::ClusterData build(const scene::Scene& scene, const Settings& settings, BuildStats* stats)
{
    const auto t0 = std::chrono::steady_clock::now();
    const uint32_t meshCount = (uint32_t)scene.meshes.size();
    std::vector<std::string> keys(meshCount);
    const std::vector<std::vector<uint32_t>> seams = findSeams(scene);
    Jobs::instance().parallelFor(meshCount, [&](uint32_t i) { keys[i] = meshKey(scene.meshes[i], settings, seams[i]); });
    std::vector<std::shared_ptr<const MeshOut>> built(meshCount);
    {
        std::lock_guard lock(g_meshCacheMutex);
        for (uint32_t i = 0; i < meshCount; ++i)
            if (auto it = g_meshCache.find(keys[i]); it != g_meshCache.end()) built[i] = it->second;
    }
    std::vector<uint32_t> missing;
    for (uint32_t i = 0; i < meshCount; ++i)
        if (!built[i]) missing.push_back(i);
    // Duplicate meshes in one scene build once.
    std::unordered_map<std::string, uint32_t> firstOf;
    std::vector<uint32_t> toBuild;
    for (uint32_t i : missing)
        if (firstOf.emplace(keys[i], i).second) toBuild.push_back(i);
    const std::string disk = diskCacheDirectory();
    std::vector<uint8_t> fromDisk(toBuild.size(), 0);
    Jobs::instance().parallelFor((uint32_t)toBuild.size(), [&](uint32_t k) {
        const uint32_t i = toBuild[k];
        if (!disk.empty())
            if (auto cached = loadFromDisk(disk, keys[i]))
            {
                built[i] = std::move(cached);
                fromDisk[k] = 1;
                return;
            }
        auto mo = std::make_shared<const MeshOut>(buildMesh(scene.meshes[i], settings, seams[i]));
        if (!disk.empty()) storeToDisk(disk, keys[i], *mo);
        built[i] = std::move(mo);
    });
    uint32_t diskMeshes = 0;
    for (uint8_t f : fromDisk) diskMeshes += f;
    for (uint32_t i : missing) built[i] = built[firstOf[keys[i]]];
    {
        std::lock_guard lock(g_meshCacheMutex);
        g_meshCache.clear();
        for (uint32_t i = 0; i < meshCount; ++i) g_meshCache[keys[i]] = built[i];
    }
    const uint32_t reused = meshCount - (uint32_t)toBuild.size();
    std::vector<MeshStats> meshStats(meshCount);
    for (uint32_t i = 0; i < meshCount; ++i) meshStats[i] = built[i]->stats;

    render::ClusterData data;
    data.meshes.resize(scene.meshes.size());
    // Named buffers exist even when empty so V can always bind them.
    appendNamed(data, kClusterNodes, nullptr, 0, sizeof(gpu::ClusterNode));
    appendNamed(data, kMeshClusterRoots, nullptr, 0, sizeof(gpu::MeshClusterRoots));
    appendNamed(data, kClusterLodSpheres, nullptr, 0, sizeof(float4));
    appendNamed(data, kClusterSheets, nullptr, 0, sizeof(float4));
    uint32_t nodeBase = 0;
    for (size_t mi = 0; mi < built.size(); ++mi)
    {
        const MeshOut& mo = *built[mi];
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
        meshStats[mi].lodLevels = range.lodLevelCount;
        meshStats[mi].coarsestTriangles = range.lodLevelCount ? data.lodLevels.back().triangleCount : 0;
    }
    if (stats)
    {
        stats->meshes.clear();
        stats->meshes = meshStats;
        stats->reusedMeshes = reused;
        stats->diskMeshes = diskMeshes;
        stats->wallMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    return data;
}
} // namespace unx::clusterbuilder
