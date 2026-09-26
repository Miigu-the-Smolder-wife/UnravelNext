// Cluster builder self-checks on procedural meshes (CPU only, seconds):
//   limits, crack-free uniform cuts (also across a submesh seam), monotone errors and enclosing spheres, hierarchy
//   coverage, minimum feature widths of known shapes, thin geometry kept out of LOD thinning, determinism.
//   unx_test_clusterbuilder [filter]
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/clusterbuilder/ClusterHierarchy.h"
#include "unx/clusterbuilder/Bricks.h"
#include "unx/core/Config.h"
#include "unx/core/Log.h"
#if UNX_HAS_SCENEGEN
#include "unx/scenegen/SceneGen.h"
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

#include <windows.h>

using namespace unx;
using namespace unx::clusterbuilder;

namespace
{
struct TestCase
{
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}
struct Register
{
    Register(const char* name, std::function<void()> fn) { registry().push_back({ name, std::move(fn) }); }
};
#define UNX_TEST(name) \
    static void name(); \
    static Register reg_##name(#name, name); \
    static void name()
#define CHECK(cond) \
    do { if (!(cond)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #cond); } while (0)

Settings settings()
{
    static const QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    return Settings::fromQuality(q);
}

scene::Scene oneMesh(scene::Mesh m)
{
    scene::Scene s;
    if (m.submeshes.empty()) m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    s.meshes.push_back(std::move(m));
    s.materials.resize(2);
    return s;
}

// Heightfield n x n quads over [0, size]^2, +y facing; optionally split into two submeshes at x = size / 2.
// 'detail' scales the frequencies: 1 = steep relief (70 degree slopes, ridges ~1.5 m thick), 0.3 = rolling terrain.
scene::Mesh heightfield(uint32_t n, float size, float amplitude, bool twoSubmeshes, float detail = 1.0f)
{
    scene::Mesh m;
    const float d = size / n;
    for (uint32_t i = 0; i <= n; ++i)
        for (uint32_t j = 0; j <= n; ++j)
        {
            const float x = j * d, z = i * d;
            m.positions.push_back({ x, amplitude * std::sin(x * 0.7f * detail) * std::cos(z * 0.9f * detail) + 0.3f * amplitude * std::sin((x * 3.1f + z * 2.3f) * detail), z });
            m.normals.push_back({ 0, 1, 0 });
            m.uv0.push_back({ x / size, z / size });
        }
    std::vector<uint32_t> left, right;
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < n; ++j)
        {
            const uint32_t v00 = i * (n + 1) + j, v01 = v00 + 1, v10 = v00 + n + 1, v11 = v10 + 1;
            std::vector<uint32_t>& dst = (twoSubmeshes && j >= n / 2) ? right : left;
            dst.insert(dst.end(), { v00, v10, v01, v01, v10, v11 });
        }
    m.indices = left;
    m.submeshes.push_back({ 0, (uint32_t)left.size(), 0 });
    if (twoSubmeshes)
    {
        m.submeshes.push_back({ (uint32_t)left.size(), (uint32_t)right.size(), 1 });
        m.indices.insert(m.indices.end(), right.begin(), right.end());
    }
    return m;
}

// Open strip (grass blade) of the given width and length in the xy plane, facing +z.
void addStrip(scene::Mesh& m, float3 origin, float width, float length, uint32_t segments)
{
    const uint32_t base = (uint32_t)m.positions.size();
    for (uint32_t k = 0; k <= segments; ++k)
    {
        const float y = length * k / segments;
        m.positions.push_back(origin + float3{ 0, y, 0 });
        m.positions.push_back(origin + float3{ width, y, 0 });
        m.normals.push_back({ 0, 0, 1 });
        m.normals.push_back({ 0, 0, 1 });
        m.uv0.push_back({ 0, (float)k / segments });
        m.uv0.push_back({ 1, (float)k / segments });
    }
    for (uint32_t k = 0; k < segments; ++k)
    {
        const uint32_t a = base + 2 * k, b = a + 1, c = a + 2, d = a + 3;
        m.indices.insert(m.indices.end(), { a, b, c, c, b, d });
    }
}

// Open-ended tube along +y, outward-facing.
scene::Mesh tube(float radius, float length, uint32_t sides, uint32_t rings)
{
    scene::Mesh m;
    const float pi = 3.14159265f;
    for (uint32_t r = 0; r <= rings; ++r)
        for (uint32_t s = 0; s < sides; ++s)
        {
            const float a = 2 * pi * s / sides;
            m.positions.push_back({ radius * std::cos(a), length * r / rings, radius * std::sin(a) });
            m.normals.push_back({ std::cos(a), 0, std::sin(a) });
            m.uv0.push_back({ (float)s / sides, (float)r / rings });
        }
    for (uint32_t r = 0; r < rings; ++r)
        for (uint32_t s = 0; s < sides; ++s)
        {
            const uint32_t a = r * sides + s, b = r * sides + (s + 1) % sides, c = a + sides, d = b + sides;
            m.indices.insert(m.indices.end(), { a, c, b, b, c, d });  // outward (counter-clockwise seen from outside)
        }
    return m;
}

float3 fromRow(const float4& v) { return { v.x, v.y, v.z }; }

struct Built
{
    scene::Scene scene;
    render::ClusterData data;
    BuildStats stats;
};

Built buildOne(scene::Mesh m)
{
    Built b;
    b.scene = oneMesh(std::move(m));
    b.data = build(b.scene, settings(), &b.stats);
    return b;
}

template <typename T>
std::vector<T> named(const render::ClusterData& d, const char* name)
{
    for (const auto& n : d.named)
        if (n.name == name)
        {
            CHECK(n.stride == sizeof(T));
            std::vector<T> out(n.bytes.size() / sizeof(T));
            if (!out.empty()) std::memcpy(out.data(), n.bytes.data(), n.bytes.size());
            return out;
        }
    fail("named buffer %s missing", name);
}

// Triangles (mesh vertex indices) of a set of clusters.
std::vector<uint32_t> trianglesOf(const render::ClusterData& d, const std::vector<uint32_t>& clusters)
{
    std::vector<uint32_t> out;
    for (uint32_t ci : clusters)
    {
        const render::gpu::Cluster& c = d.clusters[ci];
        const uint32_t tris = (c.counts >> 8) & 0xFFu;
        for (uint32_t t = 0; t < tris; ++t)
        {
            const uint32_t p = d.clusterTriangles[c.triangleOffset + t];
            for (int k = 0; k < 3; ++k) out.push_back(d.clusterVertexIndices[c.vertexOffset + ((p >> (8 * k)) & 0xFFu)]);
        }
    }
    return out;
}

// Cut of a mesh at object-space error t: clusters with own error <= t < parent error.
std::vector<uint32_t> cutAt(const render::ClusterData& d, uint32_t mesh, float t)
{
    std::vector<uint32_t> out;
    const auto& r = d.meshes[mesh];
    for (uint32_t c = r.clusterOffset; c < r.clusterOffset + r.clusterCount; ++c)
        if (d.clusters[c].lodError <= t && t < d.clusters[c].parentLodError) out.push_back(c);
    return out;
}

// Crack test: every edge (welded by exact position) used by exactly one triangle of the cut lies on the heightfield's
// outer border; no edge is used by more than two triangles.
void checkWatertight(const scene::Mesh& m, const std::vector<uint32_t>& tris, float size)
{
    std::map<std::pair<std::tuple<float, float, float>, std::tuple<float, float, float>>, int> edges;
    auto key = [&](uint32_t v) { const float3 p = m.positions[v]; return std::make_tuple(p.x, p.y, p.z); };
    for (size_t t = 0; t < tris.size(); t += 3)
        for (int e = 0; e < 3; ++e)
        {
            auto a = key(tris[t + e]), b = key(tris[t + (e + 1) % 3]);
            if (b < a) std::swap(a, b);
            ++edges[{ a, b }];
        }
    auto onBorder = [&](const std::tuple<float, float, float>& p) {
        const float x = std::get<0>(p), z = std::get<2>(p);
        return x == 0 || z == 0 || std::fabs(x - size) < 1e-4f || std::fabs(z - size) < 1e-4f;
    };
    for (const auto& [e, count] : edges)
    {
        if (count > 2 || (count == 1 && !(onBorder(e.first) && onBorder(e.second))))
            fail("crack/overlap: edge (%g %g %g)-(%g %g %g) used %d times", std::get<0>(e.first), std::get<1>(e.first), std::get<2>(e.first), std::get<0>(e.second),
                 std::get<1>(e.second), std::get<2>(e.second), count);
    }
}

// Area projected on the xz plane (a heightfield's coverage seen from above; simplification may flatten bumps, which
// changes the surface area but not this).
double projectedAreaY(const scene::Mesh& m, const std::vector<uint32_t>& tris)
{
    double a = 0;
    for (size_t t = 0; t < tris.size(); t += 3)
        a += 0.5 * cross(m.positions[tris[t + 1]] - m.positions[tris[t]], m.positions[tris[t + 2]] - m.positions[tris[t]]).y;
    return a;
}

double areaOf(const scene::Mesh& m, const std::vector<uint32_t>& tris)
{
    double a = 0;
    for (size_t t = 0; t < tris.size(); t += 3)
        a += 0.5 * length(cross(m.positions[tris[t + 1]] - m.positions[tris[t]], m.positions[tris[t + 2]] - m.positions[tris[t]]));
    return a;
}

void checkStructure(const Built& b)
{
    const render::ClusterData& d = b.data;
    const auto nodes = named<gpu::ClusterNode>(d, kClusterNodes);
    const auto roots = named<gpu::MeshClusterRoots>(d, kMeshClusterRoots);
    const auto spheres = named<float4>(d, kClusterLodSpheres);
    CHECK(spheres.size() == d.clusters.size());
    CHECK(roots.size() == d.meshes.size());
    const Settings st = settings();
    for (size_t mi = 0; mi < d.meshes.size(); ++mi)
    {
        const auto& r = d.meshes[mi];
        const scene::Mesh& m = b.scene.meshes[mi];
        std::vector<int> covered(r.clusterCount, 0);
        // Walk the forest from the roots: children enclose nothing larger than the parent, errors are monotone,
        // leaves cover every cluster of the mesh exactly once.
        std::vector<uint32_t> stack;
        for (uint32_t k = 0; k < roots[mi].rootCount; ++k) stack.push_back(roots[mi].nodeOffset + k);
        while (!stack.empty())
        {
            const gpu::ClusterNode& n = nodes[stack.back()];
            stack.pop_back();
            if (n.leaf)
            {
                for (uint32_t c = n.first; c < n.first + n.count; ++c)
                {
                    CHECK(c >= r.clusterOffset && c < r.clusterOffset + r.clusterCount);
                    ++covered[c - r.clusterOffset];
                    CHECK(d.clusters[c].parentLodError == n.lodError);
                }
                continue;
            }
            for (uint32_t k = 0; k < n.count; ++k)
            {
                const gpu::ClusterNode& child = nodes[n.first + k];
                CHECK(child.lodError <= n.lodError);
                const float gap = length(fromRow(child.lodSphere) - fromRow(n.lodSphere)) + child.lodSphere.w - n.lodSphere.w;
                CHECK(gap <= 1e-4f * std::max(1.0f, n.lodSphere.w));
                stack.push_back(n.first + k);
            }
        }
        for (int c : covered) CHECK(c == 1);
        for (uint32_t c = r.clusterOffset; c < r.clusterOffset + r.clusterCount; ++c)
        {
            const render::gpu::Cluster& cl = d.clusters[c];
            const uint32_t verts = cl.counts & 0xFFu, tris = (cl.counts >> 8) & 0xFFu;
            CHECK(verts >= 3 && verts <= st.clusterVertices);
            CHECK(tris >= 1 && tris <= st.clusterTriangles);
            CHECK((cl.counts >> 16) < m.submeshes.size());
            CHECK(cl.lodError <= cl.parentLodError);
            for (uint32_t v = 0; v < verts; ++v) CHECK(d.clusterVertexIndices[cl.vertexOffset + v] < m.positions.size());
            // The own LOD sphere encloses the cluster's vertices (it is the sphere of the group it came from).
            const float4 s = spheres[c];
            for (uint32_t v = 0; v < verts; ++v)
                CHECK(length(m.positions[d.clusterVertexIndices[cl.vertexOffset + v]] - fromRow(s)) <= s.w * (1 + 1e-4f) + 1e-6f);
            // The tight culling sphere encloses them too.
            for (uint32_t v = 0; v < verts; ++v)
                CHECK(length(m.positions[d.clusterVertexIndices[cl.vertexOffset + v]] - fromRow(cl.boundsSphere)) <= cl.boundsSphere.w * (1 + 1e-4f) + 1e-6f);
        }
        // LodLevels are cuts with strictly decreasing triangle counts; level 0 is the source geometry.
        for (uint32_t l = 0; l < r.lodLevelCount; ++l)
        {
            const render::gpu::LodLevel& lv = d.lodLevels[r.lodLevelOffset + l];
            if (l > 0) CHECK(lv.triangleCount < d.lodLevels[r.lodLevelOffset + l - 1].triangleCount);
            if (l == 0) CHECK(lv.error == 0);
        }
    }
}
} // namespace

UNX_TEST(heightfield_cuts_are_watertight)
{
    // Rolling terrain (must simplify deeply) and steep relief (70 degree slopes: folds and the thin-ridge guard stop it
    // early, but every cut must still be watertight and fold-free).
    for (float relief : { 0.3f, 1.0f })
    {
    const float size = 64.0f;
    const Built b = buildOne(heightfield(192, size, 3.0f, false, relief));
    checkStructure(b);
    const auto& st = b.stats.meshes[0];
    logf("    relief %.1f: %u source tris, %u clusters (%u source), %u groups, depth %u, %u nodes, %u LOD cuts, coarsest %u tris, %u fold retries (%u terminal), %.0f ms\n",
         relief, st.sourceTriangles, st.clusters, st.sourceClusters, st.groups, st.depth, st.nodes, st.lodLevels, st.coarsestTriangles, st.foldRetries, st.foldTerminalGroups,
         st.buildMs);
    if (relief < 0.5f) CHECK(st.depth >= 8 && st.coarsestTriangles * 64 < st.sourceTriangles);
    const scene::Mesh& m = b.scene.meshes[0];
    const double sourceArea = projectedAreaY(m, m.indices);
    // Every stored LOD cut and cuts at arbitrary errors are crack-free and keep the projected area (no fold-over).
    std::vector<float> ts;
    for (uint32_t l = 0; l < b.data.meshes[0].lodLevelCount; ++l) ts.push_back(b.data.lodLevels[b.data.meshes[0].lodLevelOffset + l].error);
    std::mt19937 rng(7);
    for (int k = 0; k < 16; ++k) ts.push_back(std::uniform_real_distribution<float>(0.0f, ts.back() * 1.5f)(rng));
    for (float t : ts)
    {
        const auto tris = trianglesOf(b.data, cutAt(b.data, 0, t));
        checkWatertight(m, tris, size);
        // Fold-free: the source is a height field, so no triangle of any cut faces down. Vertical slivers (three
        // vertices on one grid line seen from above) cover nothing from above; their projected area is float noise
        // (coordinates up to 64 m: ~1e-6 m^2), so a fold must cover more than 5e-6 m^2.
        for (size_t k = 0; k < tris.size(); k += 3)
        {
            const float3 a = m.positions[tris[k]], bb = m.positions[tris[k + 1]], c = m.positions[tris[k + 2]];
            const float3 n = cross(bb - a, c - a);
            if (0.5f * n.y < -5e-6f)
                fail("fold at t=%g: (%g %g %g) (%g %g %g) (%g %g %g) n (%g %g %g)", t, a.x, a.y, a.z, bb.x, bb.y, bb.z, c.x, c.y, c.z, n.x, n.y, n.z);
        }
        const double a = projectedAreaY(m, tris);
        CHECK(std::fabs(a - sourceArea) < 1e-4 * sourceArea);
    }
    }
}

UNX_TEST(submesh_seam_stays_closed)
{
    const float size = 32.0f;
    const Built b = buildOne(heightfield(128, size, 2.0f, true));
    checkStructure(b);
    const scene::Mesh& m = b.scene.meshes[0];
    const auto& r = b.data.meshes[0];
    CHECK(r.lodLevelCount >= 3);
    for (uint32_t l = 0; l < r.lodLevelCount; ++l)
    {
        const auto tris = trianglesOf(b.data, cutAt(b.data, 0, b.data.lodLevels[r.lodLevelOffset + l].error));
        checkWatertight(m, tris, size);
    }
    for (uint32_t c = r.clusterOffset; c < r.clusterOffset + r.clusterCount; ++c)
        CHECK(b.data.clusters[c].material == (b.data.clusters[c].counts >> 16));  // submesh s uses material s here
}

UNX_TEST(tile_seams_stay_closed_at_every_cut)
{
    // Two terrain tiles side by side (separate meshes, instances at x = 0 and x = size) and a third far away. The shared
    // border x = size must keep its source vertices in every cluster of both hierarchies, so any pair of cuts is
    // watertight: every triangle edge on the seam spans exactly one source step. The lone tile's border meets nothing
    // and still simplifies (its coarsest cut is as small as without seams).
    const uint32_t n = 128;
    const float size = 64.0f, d = size / n;
    auto tile = [&](float x0) {
        scene::Mesh m;
        for (uint32_t i = 0; i <= n; ++i)
            for (uint32_t j = 0; j <= n; ++j)
            {
                const float x = j * d, z = i * d, wx = x0 + x;
                m.positions.push_back({ x, 3.0f * std::sin(wx * 0.21f) * std::cos(z * 0.27f) + 0.9f * std::sin(wx * 0.93f + z * 0.69f), z });
                m.normals.push_back({ 0, 1, 0 });
                m.uv0.push_back({ x / size, z / size });
            }
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = 0; j < n; ++j)
            {
                const uint32_t v00 = i * (n + 1) + j, v01 = v00 + 1, v10 = v00 + n + 1, v11 = v10 + 1;
                m.indices.insert(m.indices.end(), { v00, v10, v01, v01, v10, v11 });
            }
        m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
        return m;
    };
    scene::Scene sc;
    sc.materials.resize(1);
    sc.meshes = { tile(0), tile(size), tile(0) };
    for (uint32_t k = 0; k < 3; ++k)
    {
        scene::Instance in;
        in.mesh = k;
        in.transform.m[0][3] = k == 1 ? size : k == 2 ? 1000.0f : 0.0f;
        sc.instances.push_back(in);
    }
    BuildStats bs;
    const render::ClusterData data = build(sc, settings(), &bs);
    for (uint32_t k = 0; k < 3; ++k)
        logf("    tile %u: %u source tris, depth %u, coarsest %u tris\n", k, bs.meshes[k].sourceTriangles, bs.meshes[k].depth, bs.meshes[k].coarsestTriangles);
    // Seam in each tile's own coordinates: x = size for tile 0, x = 0 for tile 1. In every cut (the stored LOD levels and
    // errors between them), the cut's border edges (used by one triangle) on the seam each span one source step: the
    // boundary along the seam is the source chain. (A vertical sliver with all corners on the seam is interior to the
    // cut; its long edge is shared with the neighbouring triangle.)
    uint64_t seamEdges = 0;
    for (uint32_t k = 0; k < 2; ++k)
    {
        const float seamX = k == 0 ? size : 0.0f;
        const scene::Mesh& m = sc.meshes[k];
        const auto& r = data.meshes[k];
        std::vector<float> ts;
        for (uint32_t l = 0; l < r.lodLevelCount; ++l) ts.push_back(data.lodLevels[r.lodLevelOffset + l].error);
        std::mt19937 rng(11);
        for (int j = 0; j < 16; ++j) ts.push_back(std::uniform_real_distribution<float>(0.0f, ts.back() * 1.5f)(rng));
        for (float t : ts)
        {
            const auto tris = trianglesOf(data, cutAt(data, k, t));
            std::map<std::pair<std::pair<float, float>, std::pair<float, float>>, int> edges;  // (z, y) endpoints on the seam
            for (size_t i = 0; i < tris.size(); i += 3)
                for (int e = 0; e < 3; ++e)
                {
                    const float3 p = m.positions[tris[i + e]], q = m.positions[tris[i + (e + 1) % 3]];
                    if (p.x != seamX || q.x != seamX) continue;
                    auto u = std::make_pair(p.z, p.y), w = std::make_pair(q.z, q.y);
                    if (w < u) std::swap(u, w);
                    ++edges[{ u, w }];
                }
            float covered = 0;
            for (const auto& [e, count] : edges)
            {
                if (count != 1) continue;
                ++seamEdges;
                covered += e.second.first - e.first.first;
                if (std::fabs((e.second.first - e.first.first) - d) > 1e-4f)
                    fail("tile %u, cut %g: seam border edge z %g..%g spans %g source steps", k, t, e.first.first, e.second.first, (e.second.first - e.first.first) / d);
            }
            CHECK(std::fabs(covered - size) < 1e-3f);  // the whole seam is border, one step at a time
        }
    }
    logf("    %llu seam edges checked over all clusters of both tiles\n", (unsigned long long)seamEdges);
    CHECK(seamEdges >= 2 * n);
    // The lone tile is not held by seams: as coarse as tile 0 built alone.
    const Built alone = buildOne(tile(0));
    CHECK(bs.meshes[2].coarsestTriangles == alone.stats.meshes[0].coarsestTriangles);
    CHECK(bs.meshes[0].coarsestTriangles > bs.meshes[2].coarsestTriangles);
}

UNX_TEST(feature_width_of_known_shapes)
{
    // Grass blade 4 mm x 30 cm: flat sheet, width 4 mm.
    {
        scene::Mesh m;
        addStrip(m, { 0, 0, 0 }, 0.004f, 0.3f, 64);
        const Built b = buildOne(std::move(m));
        checkStructure(b);
        for (uint32_t c = 0; c < b.data.meshes[0].clusterCount; ++c)
            if (b.data.clusters[c].lodError == 0)
            {
                const float w = b.data.clusters[c].minFeatureWidth;
                logf("    blade source cluster width %.5f m (flat if < 0)\n", w);
                CHECK(w < 0 && std::fabs(-w - 0.004f) < 0.0002f);
            }
    }
    // Tube radius 1 cm, 16 sides: solid, width = distance between opposite faces = 2 r cos(pi/16).
    {
        const Built b = buildOne(tube(0.01f, 1.0f, 16, 64));
        checkStructure(b);
        const float expect = 2 * 0.01f * std::cos(3.14159265f / 16);
        for (uint32_t c = 0; c < b.data.meshes[0].clusterCount; ++c)
            if (b.data.clusters[c].lodError == 0)
            {
                const float w = b.data.clusters[c].minFeatureWidth;
                CHECK(w > 0 && std::fabs(w - expect) < 0.02f * expect);
            }
        logf("    tube source cluster width %.5f m (expected %.5f, solid)\n", b.data.clusters[0].minFeatureWidth, expect);
    }
    // Terrain: open sheet much wider than its clusters' spacing.
    {
        const Built b = buildOne(heightfield(64, 16.0f, 0.5f, false));
        float narrowest = 1e30f;
        for (uint32_t c = 0; c < b.data.meshes[0].clusterCount; ++c) narrowest = std::min(narrowest, std::fabs(b.data.clusters[c].minFeatureWidth));
        logf("    terrain narrowest cluster width %.3f m (quad 0.25 m)\n", narrowest);
        CHECK(narrowest > 0.5f);
    }
}

UNX_TEST(thin_geometry_is_not_thinned)
{
    // 400 blades of 4 mm and 300 leaves of 5 cm, disconnected: LOD must not merge or collapse them (no sloppy
    // fallback, thin-feature error limit), so every cut keeps each blade's and leaf's area.
    scene::Mesh m;
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> u(0.0f, 4.0f);
    for (int i = 0; i < 400; ++i) addStrip(m, { u(rng), 0, u(rng) }, 0.004f, 0.3f, 8);
    for (int i = 0; i < 300; ++i) addStrip(m, { u(rng), 0.5f + 0.1f * u(rng), u(rng) }, 0.05f, 0.05f, 1);
    const Built b = buildOne(std::move(m));
    checkStructure(b);
    const scene::Mesh& mesh = b.scene.meshes[0];
    const double sourceArea = areaOf(mesh, mesh.indices);
    const auto& r = b.data.meshes[0];
    const auto& st = b.stats.meshes[0];
    logf("    thin set: %u source tris, %u clusters, %u groups (%u terminal), %u LOD cuts, coarsest %u tris\n", st.sourceTriangles, st.clusters, st.groups,
         st.terminalGroups, st.lodLevels, st.coarsestTriangles);
    for (uint32_t l = 0; l < r.lodLevelCount; ++l)
    {
        const auto tris = trianglesOf(b.data, cutAt(b.data, 0, b.data.lodLevels[r.lodLevelOffset + l].error));
        const double a = areaOf(mesh, tris);
        CHECK(std::fabs(a - sourceArea) < 0.02 * sourceArea);
    }
    const auto spheres = named<float4>(b.data, kClusterLodSpheres);
    (void)spheres;
}

UNX_TEST(wide_surfaces_simplify_fully)
{
    // Rolling terrain has no thin features: the thin-feature guard must not limit it (coarsest cut within 10 % of the
    // unguarded one). Steep relief has ridges ~1.5 m thick that the guard keeps from being flattened away (logged only).
    for (float detail : { 0.3f, 1.0f })
    {
        uint32_t coarsest[2] = {};
        for (int limited = 0; limited < 2; ++limited)
        {
            Settings st = settings();
            if (!limited) st.maxRelativeWidthError = 1e30f;
            scene::Scene sc = oneMesh(heightfield(192, 64.0f, 3.0f, false, detail));
            BuildStats bs;
            build(sc, st, &bs);
            const auto& m = bs.meshes[0];
            coarsest[limited] = m.coarsestTriangles;
            logf("    relief %.1f, guard %s: %u groups, %u terminal, depth %u, coarsest %u of %u tris\n", detail, limited ? "on " : "off", m.groups, m.terminalGroups,
                 m.depth, m.coarsestTriangles, m.sourceTriangles);
        }
        if (detail < 0.5f) CHECK(coarsest[1] * 10 <= coarsest[0] * 11 && coarsest[1] * 64 < 73728);
    }
}

bool sameClusterData(const render::ClusterData& a, const render::ClusterData& b)
{
    bool same = a.clusters.size() == b.clusters.size() && a.named.size() == b.named.size() && a.clusterVertexIndices == b.clusterVertexIndices &&
                a.clusterTriangles == b.clusterTriangles && a.lodLevelClusters == b.lodLevelClusters && a.meshes.size() == b.meshes.size() &&
                a.lodLevels.size() == b.lodLevels.size();
    if (same) same = std::memcmp(a.clusters.data(), b.clusters.data(), a.clusters.size() * sizeof(render::gpu::Cluster)) == 0;
    if (same) same = std::memcmp(a.meshes.data(), b.meshes.data(), a.meshes.size() * sizeof(a.meshes[0])) == 0;
    if (same) same = std::memcmp(a.lodLevels.data(), b.lodLevels.data(), a.lodLevels.size() * sizeof(a.lodLevels[0])) == 0;
    for (size_t i = 0; same && i < a.named.size(); ++i) same = a.named[i].name == b.named[i].name && a.named[i].bytes == b.named[i].bytes;
    return same;
}

scene::Scene threeMeshScene()
{
    scene::Scene sc = oneMesh(heightfield(64, 20.0f, 1.0f, true));
    sc.meshes.push_back(heightfield(48, 10.0f, 0.5f, false));
    sc.meshes.push_back(sc.meshes[0]);  // a duplicate of mesh 0
    for (uint32_t m = 1; m < 3; ++m)
    {
        scene::Instance in;
        in.mesh = m;
        in.transform.m[0][3] = 100.0f * m;  // apart: no seams between them (findSeams), so mesh 2 still duplicates mesh 0
        sc.instances.push_back(in);
    }
    return sc;
}

UNX_TEST(rebuild_reuses_unchanged_meshes)
{
    // D0 editing: re-building a scene reuses the hierarchies of meshes whose content and settings did not change, builds
    // identical meshes once, and gives byte-identical output to a cold build; a changed mesh is rebuilt.
    setDiskCache("");
    scene::Scene sc = threeMeshScene();
    clearMeshCache();
    BuildStats cold, warm, edited;
    const render::ClusterData a = build(sc, settings(), &cold);
    const render::ClusterData b = build(sc, settings(), &warm);
    logf("    cold build: %u of 3 meshes reused, %.1f ms; warm: %u reused, %.1f ms\n", cold.reusedMeshes, cold.wallMs, warm.reusedMeshes, warm.wallMs);
    CHECK(cold.reusedMeshes == 1 && warm.reusedMeshes == 3 && sameClusterData(a, b));
    clearMeshCache();
    const render::ClusterData c = build(sc, settings());
    CHECK(sameClusterData(a, c));
    sc.meshes[1].positions[0].y += 0.25f;
    const render::ClusterData d = build(sc, settings(), &edited);
    CHECK(edited.reusedMeshes == 2 && !sameClusterData(a, d));
    clearMeshCache();
    CHECK(sameClusterData(d, build(sc, settings())));
}

UNX_TEST(disk_cache_round_trip)
{
    // C1 incremental cooking: a fresh process (memory cache cleared) reads every unchanged mesh's hierarchy from the disk
    // cache and gives output byte-identical to a cold build; a settings change misses; a corrupted entry is detected by
    // its checksum and rebuilt (never used); a truncated entry is rebuilt.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("unx_cluster_cache_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    setDiskCache(dir.string());
    const scene::Scene sc = threeMeshScene();
    clearMeshCache();
    BuildStats cold, fromDisk, otherSettings, repaired;
    const render::ClusterData a = build(sc, settings(), &cold);
    uint32_t files = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir / "clusters"))
        if (e.path().extension() == ".unxcl") ++files;
    CHECK(cold.diskMeshes == 0 && files == 2);  // mesh 2 duplicates mesh 0
    clearMeshCache();
    const render::ClusterData b = build(sc, settings(), &fromDisk);
    logf("    cold %.1f ms, from disk %.1f ms (%u of 3 meshes read)\n", cold.wallMs, fromDisk.wallMs, fromDisk.diskMeshes);
    CHECK(fromDisk.diskMeshes == 2 && sameClusterData(a, b));
    for (size_t m = 0; m < fromDisk.meshes.size(); ++m)
        CHECK(fromDisk.meshes[m].clusters == cold.meshes[m].clusters && fromDisk.meshes[m].lodLevels == cold.meshes[m].lodLevels);
    clearMeshCache();
    Settings other = settings();
    other.maxRelativeWidthError *= 0.5f;
    build(sc, other, &otherSettings);
    CHECK(otherSettings.diskMeshes == 0);
    // Corrupt one byte in the middle of every entry of the original settings, and truncate one.
    std::vector<std::filesystem::path> entries;
    for (const auto& e : std::filesystem::directory_iterator(dir / "clusters")) entries.push_back(e.path());
    std::sort(entries.begin(), entries.end());
    bool truncated = false;
    for (const auto& path : entries)
    {
        std::vector<char> bytes;
        {
            std::ifstream f(path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        if (!truncated) { bytes.resize(bytes.size() / 2); truncated = true; }
        else bytes[bytes.size() / 2] ^= 0x5A;
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), (std::streamsize)bytes.size());
    }
    clearMeshCache();
    const render::ClusterData c = build(sc, settings(), &repaired);
    CHECK(repaired.diskMeshes == 0 && sameClusterData(a, c));
    clearMeshCache();
    BuildStats again;
    const render::ClusterData d = build(sc, settings(), &again);  // the rebuilt entries were written back
    CHECK(again.diskMeshes == 2 && sameClusterData(a, d));
    setDiskCache("");
    clearMeshCache();
    std::filesystem::remove_all(dir, ec);
}

UNX_TEST(build_is_deterministic)
{
    setDiskCache("");
    clearMeshCache();  // two cold builds (a warm one would reuse the first's hierarchy)
    const Built a = buildOne(heightfield(96, 20.0f, 1.0f, true));
    clearMeshCache();
    const Built b = buildOne(heightfield(96, 20.0f, 1.0f, true));
    CHECK(a.data.clusters.size() == b.data.clusters.size());
    CHECK(std::memcmp(a.data.clusters.data(), b.data.clusters.data(), a.data.clusters.size() * sizeof(render::gpu::Cluster)) == 0);
    CHECK(a.data.clusterVertexIndices == b.data.clusterVertexIndices);
    CHECK(a.data.clusterTriangles == b.data.clusterTriangles);
    CHECK(a.data.lodLevelClusters == b.data.lodLevelClusters);
    CHECK(a.data.named.size() == b.data.named.size());
    for (size_t i = 0; i < a.data.named.size(); ++i) CHECK(a.data.named[i].bytes == b.data.named[i].bytes);
}

UNX_TEST(cluster_fill_of_scene_meshes)
{
    // Source clusters of C's generated scenes (with track C): triangles per source cluster (the meshlet limit is
    // visibility.cluster_triangles) and bounding radius, per mesh and weighted by instances -- the load V's culling and
    // raster carry per triangle. Low fill multiplies the per-cluster costs (culling, meshlet launches, pairs).
#if !UNX_HAS_SCENEGEN
    logf("    (no scene generator in this build: skipped)\n");
#else
    const Settings st = settings();
    for (scenegen::SceneId id : { scenegen::SceneId::ForestThin, scenegen::SceneId::ForestCard, scenegen::SceneId::Waterside, scenegen::SceneId::CityBlock })
    {
        scenegen::Request req;
        req.id = id;
        const scene::Scene s = scenegen::generate(req);
        BuildStats bs;
        const render::ClusterData cd = build(s, st, &bs);
        std::vector<uint64_t> instancesOf(s.meshes.size(), 0);
        for (const auto& inst : s.instances) ++instancesOf[inst.mesh];
        uint64_t weightedClusters = 0, weightedTriangles = 0;
        std::vector<std::pair<uint32_t, uint64_t>> fillWeighted;  // (triangles of a source cluster, instances)
        logf("    %s: per mesh (instances): source clusters, triangles per source cluster median / P10, bounding radius median (m)\n", scenegen::sceneName(id));
        for (uint32_t m = 0; m < cd.meshes.size(); ++m)
        {
            const auto& range = cd.meshes[m];
            std::vector<uint32_t> tris;
            std::vector<float> radius;
            for (uint32_t c = range.clusterOffset; c < range.clusterOffset + range.clusterCount; ++c)
            {
                const render::gpu::Cluster& cl = cd.clusters[c];
                if (cl.lodError != 0) continue;
                tris.push_back((cl.counts >> 8) & 0xFFu);
                radius.push_back(cl.boundsSphere.w);
            }
            if (tris.empty()) continue;
            std::vector<uint32_t> sorted = tris;
            std::sort(sorted.begin(), sorted.end());
            std::sort(radius.begin(), radius.end());
            uint64_t sum = 0;
            for (uint32_t t : tris)
            {
                sum += t;
                fillWeighted.push_back({ t, instancesOf[m] });
            }
            weightedClusters += tris.size() * instancesOf[m];
            weightedTriangles += sum * instancesOf[m];
            if (instancesOf[m] * tris.size() >= 1000 || tris.size() >= 64)
                logf("      %-24s (%7llu): %6zu, %3u / %3u, %.3g\n", s.meshes[m].name.c_str(), (unsigned long long)instancesOf[m], tris.size(), sorted[sorted.size() / 2],
                     sorted[sorted.size() / 10], radius[radius.size() / 2]);
        }
        std::sort(fillWeighted.begin(), fillWeighted.end());
        uint64_t total = 0, acc = 0;
        for (const auto& [t, w] : fillWeighted) total += w;
        uint32_t median = 0;
        for (const auto& [t, w] : fillWeighted)
            if ((acc += w) * 2 >= total)
            {
                median = t;
                break;
            }
        logf("    %s: instance-weighted source clusters %llu, triangles %llu, %.1f triangles per cluster (median %u of %u)\n", scenegen::sceneName(id),
             (unsigned long long)weightedClusters, (unsigned long long)weightedTriangles, weightedClusters ? (double)weightedTriangles / weightedClusters : 0.0, median,
             st.clusterTriangles);
    }
#endif
}

namespace
{
// A clump of n flat blades (width 4 mm, height 0.3 m, 3 segments, random yaw and lean) in a 0.3 m disc: separate
// components, the thin geometry band C bricks are for. Material 0: opaque; the optional alpha texture is applied to
// material 1 when 'alphaValue' >= 0.
scene::Scene bladeClump(uint32_t blades, uint32_t seed, int alphaValue = -1)
{
    scene::Scene s;
    scene::Mesh m;
    m.name = "clump";
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    for (uint32_t b = 0; b < blades; ++b)
    {
        const float r = 0.15f * std::sqrt(uni(rng)), phi = 6.2831853f * uni(rng), yaw = 6.2831853f * uni(rng), lean = 0.4f * uni(rng);
        const float3 base{ r * std::cos(phi), 0, r * std::sin(phi) };
        const float3 side{ std::cos(yaw) * 0.002f, 0, std::sin(yaw) * 0.002f };
        const float3 up = normalize(float3{ std::cos(yaw + 1.5708f) * lean, 1, std::sin(yaw + 1.5708f) * lean });
        const uint32_t first = (uint32_t)m.positions.size();
        for (uint32_t k = 0; k <= 3; ++k)
        {
            const float3 c = base + up * (0.1f * k);
            for (int sgn : { -1, 1 })
            {
                m.positions.push_back(c + side * (float)sgn);
                m.normals.push_back(normalize(cross(side, up)));
                m.uv0.push_back({ sgn < 0 ? 0.0f : 1.0f, k / 3.0f });
            }
        }
        for (uint32_t k = 0; k < 3; ++k)
        {
            const uint32_t i = first + 2 * k;
            m.indices.insert(m.indices.end(), { i, i + 1, i + 3, i, i + 3, i + 2 });
        }
    }
    const uint32_t material = alphaValue >= 0 ? 1u : 0u;
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), material });
    s.meshes.push_back(std::move(m));
    s.materials.resize(2);
    if (alphaValue >= 0)
    {
        scene::Texture t;
        t.width = t.height = 4;
        t.texels.assign(4 * 4 * 4, 255);
        for (size_t i = 3; i < t.texels.size(); i += 4) t.texels[i] = (uint8_t)alphaValue;
        s.textures.push_back(t);
        s.materials[1].baseColorTexture = 0;
        s.materials[1].alphaCutoff = 0.5f;
        s.materials[1].twoSided = true;
    }
    return s;
}

// Transmittance of a ray through the brick model of level 'level': the product over the voxels it crosses of
// exp(-sigma(w) x length) (the march S's receiver function and the camera march implement).
float marchTransmittance(const BrickData& d, uint32_t meshIndex, uint32_t level, float3 o, float3 w, float tMax, bool trilinear = false)
{
    const gpu::BrickMesh& bm = d.meshes[meshIndex];
    const gpu::BrickLevel& lv = d.levels[bm.firstLevel + level];
    const float voxel = lv.voxel;
    const uint32_t edge = 16;
    double tau = 0;
    const int n = 4096;  // fine sampling of the segment (the test's reference march, not the GPU's DDA)
    const float dt = tMax / n;
    auto sigmaAt = [&](int vx, int vy, int vz) -> double {  // extinction of one voxel along w (0 outside)
        if (vx < 0 || vy < 0 || vz < 0) return 0;
        const uint32_t bx = vx / edge, by = vy / edge, bz = vz / edge;
        if (bx >= lv.dims[0] || by >= lv.dims[1] || bz >= lv.dims[2]) return 0;
        const uint32_t brick = d.grid[lv.gridOffset + bx + lv.dims[0] * (by + lv.dims[1] * bz)];
        if (brick == UINT32_MAX) return 0;
        const uint32_t local = (vx % edge) + edge * ((vy % edge) + edge * (vz % edge));
        const uint8_t q = d.density[(size_t)brick * edge * edge * edge + local];
        if (q == 0) return 0;
        const uint8_t* c = &d.shape[((size_t)brick * edge * edge * edge + local) * 6];
        const double sx = c[0] / 255.0, sy = c[1] / 255.0, sz = c[2] / 255.0;
        const double rxy = c[3] / 255.0 * 2 - 1, rxz = c[4] / 255.0 * 2 - 1, ryz = c[5] / 255.0 * 2 - 1;
        const double quad = sx * sx * w.x * w.x + sy * sy * w.y * w.y + sz * sz * w.z * w.z +
                            2 * (rxy * sx * sy * w.x * w.y + rxz * sx * sz * w.x * w.z + ryz * sy * sz * w.y * w.z);
        return brickDecodeDepth(q) / voxel * std::sqrt(std::max(quad, 0.0));
    };
    for (int i = 0; i < n; ++i)
    {
        const float3 p = o + w * ((i + 0.5f) * dt) - bm.boundsMin;
        if (!trilinear)
        {
            tau += sigmaAt((int)std::floor(p.x / voxel), (int)std::floor(p.y / voxel), (int)std::floor(p.z / voxel)) * dt;
            continue;
        }
        const float fx = p.x / voxel - 0.5f, fy = p.y / voxel - 0.5f, fz = p.z / voxel - 0.5f;
        const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy), z0 = (int)std::floor(fz);
        const float ax = fx - x0, ay = fy - y0, az = fz - z0;
        double sigma = 0;
        for (int c = 0; c < 8; ++c)
        {
            const float wgt = ((c & 1) ? ax : 1 - ax) * ((c & 2) ? ay : 1 - ay) * ((c & 4) ? az : 1 - az);
            if (wgt > 0) sigma += wgt * sigmaAt(x0 + (c & 1), y0 + ((c >> 1) & 1), z0 + ((c >> 2) & 1));
        }
        tau += sigma * dt;
    }
    return (float)std::exp(-tau);
}

// Exact: the first triangle the segment o + t w, t in (0, tMax), hits (UINT32_MAX: none).
uint32_t firstHit(const scene::Mesh& m, float3 o, float3 w, float tMax)
{
    float best = tMax;
    uint32_t hit = UINT32_MAX;
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
    {
        const float3 a = m.positions[m.indices[i]], b = m.positions[m.indices[i + 1]], c = m.positions[m.indices[i + 2]];
        const float3 e1 = b - a, e2 = c - a, p = cross(w, e2);
        const float det = dot(e1, p);
        if (std::fabs(det) < 1e-20f) continue;
        const float inv = 1 / det;
        const float3 s0 = o - a;
        const float u = dot(s0, p) * inv;
        if (u < 0 || u > 1) continue;
        const float3 q = cross(s0, e1);
        const float v = dot(w, q) * inv;
        if (v < 0 || u + v > 1) continue;
        const float t = dot(e2, q) * inv;
        if (t > 0 && t < best)
        {
            best = t;
            hit = (uint32_t)(i / 3);
        }
    }
    return hit;
}

bool clearPath(const scene::Mesh& m, float3 o, float3 w, float tMax) { return firstHit(m, o, w, tMax) == UINT32_MAX; }
} // namespace

UNX_TEST(brick_depth_encoding)
{
    // Log coding of a voxel's optical depth: monotone, round trip within half a step (2^(1/48) - 1 = 1.45 %).
    for (int q = 1; q < 255; ++q) CHECK(brickDecodeDepth((uint8_t)q) < brickDecodeDepth((uint8_t)(q + 1)));
    for (float tau = 0.006f; tau < 8; tau *= 1.07f)
    {
        const float back = brickDecodeDepth(brickEncodeDepth(tau));
        CHECK(std::fabs(back / tau - 1) <= 0.0146f);
    }
    CHECK(brickEncodeDepth(0) == 0 && brickDecodeDepth(0) == 0 && brickEncodeDepth(100) == 255);
}

// A11 cut faces: a flat 32 x 32 grid, the left half a Standard hull, the right half a Cut face (the neighbour across its
// x = 2 edge has another material; its other sides are the mesh border). At every LOD level, bit 24 + i of a Cut
// triangle's word is set exactly when its edge opposite corner i lies on the right half's outline (both ends on one side
// of the rectangle); hull triangles have no bits; simplified Cut clusters exist, so the rule is checked past LOD 0.
UNX_TEST(cut_face_border_edges)
{
    scene::Scene sc = oneMesh(heightfield(32, 4.0f, 0.0f, true));
    sc.materials[1].cls = scene::MaterialClass::Cut;
    sc.materials[1].cutScale = 2.0f;
    sc.materials[1].cutDamageWidth = 0.02f;
    scene::validate(sc);
    const render::ClusterData d = build(sc, settings());
    const scene::Mesh& m = sc.meshes[0];
    auto side = [](float3 p) {
        uint32_t s = 0;
        if (p.x == 2.0f) s |= 1;
        if (p.x == 4.0f) s |= 2;
        if (p.z == 0.0f) s |= 4;
        if (p.z == 4.0f) s |= 8;
        return s;
    };
    uint32_t cutTriangles = 0, flagged = 0, wrong = 0, hullBits = 0, simplifiedCut = 0;
    const auto& r = d.meshes[0];
    for (uint32_t ci = r.clusterOffset; ci < r.clusterOffset + r.clusterCount; ++ci)
    {
        const render::gpu::Cluster& c = d.clusters[ci];
        const bool cut = c.material == 1;
        simplifiedCut += cut && c.lodError > 0;
        const uint32_t tris = (c.counts >> 8) & 0xFFu;
        for (uint32_t t = 0; t < tris; ++t)
        {
            const uint32_t w = d.clusterTriangles[c.triangleOffset + t];
            const uint32_t bits = (w >> 24) & 7u;
            if (!cut)
            {
                hullBits += bits != 0 || (w >> 27) != 0;
                continue;
            }
            ++cutTriangles;
            float3 p[3];
            for (int k = 0; k < 3; ++k) p[k] = m.positions[d.clusterVertexIndices[c.vertexOffset + ((w >> (8 * k)) & 0xFFu)]];
            for (uint32_t i = 0; i < 3; ++i)
            {
                const bool want = (side(p[(i + 1) % 3]) & side(p[(i + 2) % 3])) != 0;
                const bool got = (bits >> i) & 1u;
                flagged += got;
                wrong += want != got;
            }
        }
    }
    logf("    cut triangles %u (simplified cut clusters %u), boundary edges flagged %u, wrong %u, hull triangles with bits %u\n", cutTriangles, simplifiedCut, flagged,
         wrong, hullBits);
    CHECK(cutTriangles > 0 && flagged > 0 && simplifiedCut > 0);
    CHECK(wrong == 0 && hullBits == 0);
}

UNX_TEST(brick_bake_of_a_blade_clump)
{
    // Bake, cache round trip, cut-out alpha, and the model against exact ray casting: ray bundles of one voxel's width
    // (the footprint the march is used at) through the clump, mean transmittance of the bundle, per level.
    QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    BrickSettings bs = BrickSettings::fromQuality(q);
    bs.maxFeatureWidth = 1.0f;  // the clump's blades are 4 mm wide
    bs.residualMax = 0;         // reported here; the gate value is set from these measurements
    const scene::Scene s = bladeClump(60, 7);
    std::vector<BrickMeshStats> st;
    const auto t0 = std::chrono::steady_clock::now();
    const std::filesystem::path cache = std::filesystem::temp_directory_path() / "unx_brick_test_cache";
    std::error_code ec;
    std::filesystem::remove_all(cache, ec);
    const BrickData d = bakeBricks(s, bs, cache.string(), &st);
    const double bakeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(st.size() == 1 && d.meshOf[0] == 0 && !d.headers.empty());
    const BrickMeshStats& m = st[0];
    logf("    clump of 60 blades: width %.4f m, v0 %.4f m, %u levels, %u bricks, %u voxels; residual P99/max r %.4f/%.4f, half %.4f/%.4f, brick shape %.4f/%.4f; "
         "bake %.0f ms\n",
         m.featureWidth, m.voxel0, m.levels, m.bricks, m.voxels, m.rP99, m.rMax, m.rHalfP99, m.rHalfMax, m.rBrickP99, m.rBrickMax, bakeMs);

    // Cache: the second bake loads the file and is byte-identical.
    std::vector<BrickMeshStats> st2;
    const BrickData d2 = bakeBricks(s, bs, cache.string(), &st2);
    CHECK(st2.size() == 1 && st2[0].cached);
    CHECK(d2.grid == d.grid && d2.density == d.density && d2.shape == d.shape && d2.occupancy == d.occupancy);
    CHECK(d2.headers.size() == d.headers.size() && std::memcmp(d2.headers.data(), d.headers.data(), d.headers.size() * sizeof(gpu::BrickHeader)) == 0);
    std::filesystem::remove_all(cache, ec);

    // Cut-out: a texture with alpha 0 under the cutoff leaves nothing to bake; alpha 255 bakes like opaque.
    std::vector<BrickMeshStats> cut, solid;
    CHECK(bakeBricks(bladeClump(60, 7, 0), bs, "", &cut).headers.empty());
    CHECK(bakeBricks(bladeClump(60, 7, 255), bs, "", &solid).headers.size() == d.headers.size());

    // Model against exact: per clump density and level, 200 bundles of 8 x 8 parallel rays across 1 or 2 voxels,
    // random directions and offsets through the clump's middle; mean transmittance of the bundle, modelled vs exact.
    for (uint32_t blades : { 60u, 240u, 960u })
    {
    const scene::Scene sd = bladeClump(blades, 7);
    const BrickData dd = bakeBricks(sd, bs, "", nullptr);
    const scene::Mesh& mesh = sd.meshes[0];
    logf("    clump of %u blades:\n", blades);
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    for (uint32_t variant = 0; variant < 2; ++variant)
    for (uint32_t level = 0; level < dd.levels.size(); ++level)
    {
        const float ratio = (float)(1u << (variant % 3));  // bundle width / voxel
        const bool trilinear = variant >= 3;
        const float voxel = dd.levels[level].voxel;
        std::vector<float> err;
        double meanExact = 0, signedSum = 0;
        double byVoxels[4] = {}, countByVoxels[4] = {};  // |error| by occupied voxels crossed: 1, 2-3, 4-7, 8+
        for (int bundle = 0; bundle < 200; ++bundle)
        {
            const float z = uni(rng) * 2 - 1, a = 6.2831853f * uni(rng);
            const float3 w{ std::sqrt(1 - z * z) * std::cos(a), z, std::sqrt(1 - z * z) * std::sin(a) };
            const float3 helper = std::fabs(w.x) < 0.9f ? float3{ 1, 0, 0 } : float3{ 0, 1, 0 };
            const float3 u = normalize(cross(helper, w)), v = cross(w, u);
            const float3 centre{ (uni(rng) - 0.5f) * 0.2f, 0.05f + uni(rng) * 0.2f, (uni(rng) - 0.5f) * 0.2f };
            const float reach = 1.0f;
            double exact = 0, model = 0;
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j)
                {
                    const float3 o = centre + u * ((i + 0.5f) / 8 - 0.5f) * voxel * ratio + v * ((j + 0.5f) / 8 - 0.5f) * voxel * ratio - w * reach;
                    exact += clearPath(mesh, o, w, 2 * reach) ? 1 : 0;
                    model += marchTransmittance(dd, 0, level, o, w, 2 * reach, trilinear);
                }
            exact /= 64;
            model /= 64;
            meanExact += exact;
            err.push_back((float)std::fabs(model - exact));
            signedSum += model - exact;
            // Occupied voxels the bundle's centre ray crosses (the path's composition length).
            uint32_t crossed = 0;
            {
                const gpu::BrickMesh& bm = dd.meshes[0];
                const gpu::BrickLevel& lv = dd.levels[bm.firstLevel + level];
                int last = -1;
                for (int k = 0; k < 4096; ++k)
                {
                    const float3 p = centre - w * reach + w * ((k + 0.5f) * 2 * reach / 4096) - bm.boundsMin;
                    const int vx = (int)std::floor(p.x / lv.voxel), vy = (int)std::floor(p.y / lv.voxel), vz = (int)std::floor(p.z / lv.voxel);
                    if (vx < 0 || vy < 0 || vz < 0 || (uint32_t)vx / 16 >= lv.dims[0] || (uint32_t)vy / 16 >= lv.dims[1] || (uint32_t)vz / 16 >= lv.dims[2]) continue;
                    const int id = vx + 4096 * (vy + 4096 * vz);
                    if (id == last) continue;
                    last = id;
                    const uint32_t brick = dd.grid[lv.gridOffset + vx / 16 + lv.dims[0] * (vy / 16 + lv.dims[1] * (vz / 16))];
                    if (brick != UINT32_MAX && dd.density[(size_t)brick * 4096 + (vx % 16) + 16 * ((vy % 16) + 16 * (vz % 16))] != 0) ++crossed;
                }
            }
            const int bin = crossed <= 1 ? 0 : crossed <= 3 ? 1 : crossed <= 7 ? 2 : 3;
            byVoxels[bin] += std::fabs(model - exact);
            countByVoxels[bin] += 1;
        }
        std::sort(err.begin(), err.end());
        double mean = 0;
        for (float e : err) mean += e;
        logf("    %s bundle %gx: level %u (voxel %.4f m): bundle transmittance |model - exact| mean %.4f, P90 %.4f, P99 %.4f, max %.4f; signed mean %+.4f (mean exact T %.3f); "
             "by occupied voxels crossed 1 / 2-3 / 4-7 / 8+: %.3f (%g) / %.3f (%g) / %.3f (%g) / %.3f (%g)\n",
             trilinear ? "trilinear" : "nearest  ", ratio, level, voxel, mean / err.size(), err[err.size() * 9 / 10], err[err.size() * 99 / 100], err.back(), signedSum / err.size(), meanExact / 200,
             countByVoxels[0] ? byVoxels[0] / countByVoxels[0] : 0.0, countByVoxels[0], countByVoxels[1] ? byVoxels[1] / countByVoxels[1] : 0.0, countByVoxels[1],
             countByVoxels[2] ? byVoxels[2] / countByVoxels[2] : 0.0, countByVoxels[2], countByVoxels[3] ? byVoxels[3] / countByVoxels[3] : 0.0, countByVoxels[3]);
    }
    }
}

UNX_TEST(brick_model_error_by_elements)
{
    // The brick model's error against exact ray casting as a function of the blades a pixel-sized bundle meets (the
    // realisation behind a pixel), on short paths (+-2.5 voxels) so the transmittance stays mid-range at every density:
    // clumps of 60 to 3840 blades, level 0 (voxel 16 mm), bundles of 8 x 8 rays one voxel wide. Report only (input to
    // the design revision's band C transition rule).
    QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    BrickSettings bs = BrickSettings::fromQuality(q);
    bs.maxFeatureWidth = 1.0f;
    bs.residualMax = 0;
    struct Bin
    {
        double absSum = 0, signedSum = 0;
        std::vector<float> errs;
    };
    std::map<uint32_t, Bin> byBlades;  // key: distinct blades met by the bundle (1, 2, 3-4, 5-8, 9-16, 17+)
    auto binOf = [](uint32_t n) { return n <= 2 ? n : n <= 4 ? 4u : n <= 8 ? 8u : n <= 16 ? 16u : 99u; };
    for (uint32_t blades : { 60u, 240u, 960u, 3840u })
    {
        const scene::Scene sd = bladeClump(blades, 7);
        const BrickData dd = bakeBricks(sd, bs, "", nullptr);
        const scene::Mesh& mesh = sd.meshes[0];
        const float voxel = dd.levels[0].voxel, reach = 2.5f * voxel;
        std::mt19937 rng(13 + blades);
        std::uniform_real_distribution<float> uni(0.0f, 1.0f);
        for (int bundle = 0; bundle < 400; ++bundle)
        {
            const float z = uni(rng) * 2 - 1, a = 6.2831853f * uni(rng);
            const float3 w{ std::sqrt(1 - z * z) * std::cos(a), z, std::sqrt(1 - z * z) * std::sin(a) };
            const float3 helper = std::fabs(w.x) < 0.9f ? float3{ 1, 0, 0 } : float3{ 0, 1, 0 };
            const float3 u = normalize(cross(helper, w)), v = cross(w, u);
            const float3 centre{ (uni(rng) - 0.5f) * 0.2f, 0.05f + uni(rng) * 0.2f, (uni(rng) - 0.5f) * 0.2f };
            double exact = 0, model = 0;
            std::vector<uint32_t> met;
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j)
                {
                    const float3 o = centre + u * ((i + 0.5f) / 8 - 0.5f) * voxel + v * ((j + 0.5f) / 8 - 0.5f) * voxel - w * reach;
                    const uint32_t hit = firstHit(mesh, o, w, 2 * reach);
                    exact += hit == UINT32_MAX ? 1 : 0;
                    if (hit != UINT32_MAX) met.push_back(hit / 6);  // 6 triangles per blade
                    model += marchTransmittance(dd, 0, 0, o, w, 2 * reach);
                }
            std::sort(met.begin(), met.end());
            const uint32_t distinct = (uint32_t)(std::unique(met.begin(), met.end()) - met.begin());
            if (distinct == 0) continue;  // the bundle met nothing: no realisation to compare
            exact /= 64;
            model /= 64;
            Bin& b = byBlades[binOf(distinct)];
            b.absSum += std::fabs(model - exact);
            b.signedSum += model - exact;
            b.errs.push_back((float)std::fabs(model - exact));
        }
    }
    for (auto& [key, b] : byBlades)
    {
        std::sort(b.errs.begin(), b.errs.end());
        const char* label = key == 1 ? "1" : key == 2 ? "2" : key == 4 ? "3-4" : key == 8 ? "5-8" : key == 16 ? "9-16" : "17+";
        logf("    blades met %-4s: %4zu bundles, |model - exact| mean %.4f, P90 %.4f, P99 %.4f; signed mean %+.4f\n", label, b.errs.size(), b.absSum / b.errs.size(),
             b.errs[b.errs.size() * 9 / 10], b.errs[b.errs.size() * 99 / 100], b.signedSum / b.errs.size());
    }
}

UNX_TEST(brick_canopy_occupancy)
{
    // Design revision 14.4 item 1 input: along sun rays from receivers inside the forest's tree crowns and grass clumps
    // (forest_thin's meshes; forest_combat places the same meshes),
    // the fraction of the brick cells (16 v), 4^3 cells (4 v) and voxels a receiver march crosses that hold geometry, at
    // level 0 (the receiver footprint ~ v0) and level 1. Occupancy is exact triangle-box overlap without the transmittance
    // fit (the bake's occupancy-only mode); alpha cut-outs and fully transparent overlaps could only empty more cells.
    // Rays start at uniform points in the mesh bounds and run to the bounds' exit; sun elevations 15, 30, 45, 70 degrees,
    // random azimuths. Report only.
#if !UNX_HAS_SCENEGEN
    logf("    (no scene generator in this build: skipped)\n");
#else
    scenegen::Request req;
    req.id = scenegen::SceneId::ForestThin;
    const scene::Scene s = scenegen::generate(req);
    QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    BrickSettings bs = BrickSettings::fromQuality(q);
    bs.maxFeatureWidth = 0.1f;  // leaves (6 cm) and blades (4 mm)
    bs.residualMax = 0;
    bs.occupancyOnly = true;
    std::vector<BrickMeshStats> stats;
    const auto t0 = std::chrono::steady_clock::now();
    const BrickData bd = bakeBricks(s, bs, "", &stats);
    logf("    occupancy bake of %zu meshes: %.1f s\n", stats.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    std::vector<uint64_t> instancesOf(s.meshes.size(), 0);
    for (const auto& inst : s.instances) ++instancesOf[inst.mesh];
    const uint32_t edge = bs.brickEdge;
    // 3D DDA over cells of size 'cell' from 'lo' with 'dims' cells, along o + t d for t in [t0, t1]: visit(cx, cy, cz, ta, tb).
    auto dda = [](float3 lo, float cell, const uint32_t dims[3], float3 o, float3 d, float ta, float tb, const std::function<void(int, int, int, float, float)>& visit) {
        const float3 p = o + d * (ta + 1e-6f) - lo;
        int c[3] = { (int)std::floor(p.x / cell), (int)std::floor(p.y / cell), (int)std::floor(p.z / cell) };
        const float dv[3] = { d.x, d.y, d.z }, pv[3] = { p.x, p.y, p.z };
        int step[3];
        float tNext[3], tDelta[3];
        for (int a = 0; a < 3; ++a)
        {
            c[a] = std::clamp(c[a], 0, (int)dims[a] - 1);
            step[a] = dv[a] > 0 ? 1 : -1;
            tDelta[a] = dv[a] != 0 ? cell / std::fabs(dv[a]) : FLT_MAX;
            const float boundary = (c[a] + (dv[a] > 0 ? 1 : 0)) * cell;
            tNext[a] = dv[a] != 0 ? ta + (boundary - pv[a]) / dv[a] : FLT_MAX;
        }
        float t = ta;
        while (t < tb)
        {
            const int a = tNext[0] < tNext[1] ? (tNext[0] < tNext[2] ? 0 : 2) : (tNext[1] < tNext[2] ? 1 : 2);
            const float te = std::min(tNext[a], tb);
            visit(c[0], c[1], c[2], t, te);
            t = te;
            c[a] += step[a];
            tNext[a] += tDelta[a];
            if (c[a] < 0 || c[a] >= (int)dims[a]) break;
        }
    };
    for (size_t bm = 0; bm < bd.meshes.size(); ++bm)
    {
        uint32_t sceneMesh = 0;
        for (uint32_t m = 0; m < bd.meshOf.size(); ++m)
            if (bd.meshOf[m] == bm) sceneMesh = m;
        const gpu::BrickMesh& mesh = bd.meshes[bm];
        const scene::Mesh& sm = s.meshes[sceneMesh];
        float3 hi = mesh.boundsMin;
        for (const float3& v : sm.positions) hi = { std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z) };
        for (uint32_t level = 0; level < std::min(mesh.levelCount, 2u); ++level)
        {
            const gpu::BrickLevel& L = bd.levels[mesh.firstLevel + level];
            const float voxel = L.voxel, brickSize = voxel * edge;
            std::mt19937 rng(17 + (uint32_t)bm * 7 + level);
            std::uniform_real_distribution<float> uni(0.0f, 1.0f);
            std::vector<float> brickRatio, cellRatio, voxelRatio;
            double voxelsCrossed = 0, voxelsInBricks = 0, voxelsInCells = 0, bricksCrossed = 0;
            for (const float elevation : { 15.0f, 30.0f, 45.0f, 70.0f })
                for (int r = 0; r < 500; ++r)
                {
                    const float az = 6.2831853f * uni(rng), el = elevation * 0.01745329f;
                    const float3 d{ std::cos(el) * std::cos(az), std::sin(el), std::cos(el) * std::sin(az) };
                    const float3 o = mesh.boundsMin + float3{ uni(rng) * (hi.x - mesh.boundsMin.x), uni(rng) * (hi.y - mesh.boundsMin.y), uni(rng) * (hi.z - mesh.boundsMin.z) };
                    // Exit of the bounds.
                    float tExit = FLT_MAX;
                    const float ov[3] = { o.x, o.y, o.z }, dv[3] = { d.x, d.y, d.z }, lov[3] = { mesh.boundsMin.x, mesh.boundsMin.y, mesh.boundsMin.z }, hv[3] = { hi.x, hi.y, hi.z };
                    for (int a = 0; a < 3; ++a)
                        if (dv[a] != 0) tExit = std::min(tExit, ((dv[a] > 0 ? hv[a] : lov[a]) - ov[a]) / dv[a]);
                    if (!(tExit > 0)) continue;
                    uint32_t nBricks = 0, nOccupied = 0, nCells = 0, nCellsOccupied = 0, nVoxels = 0, nVoxelsOccupied = 0;
                    dda(mesh.boundsMin, brickSize, L.dims, o, d, 0, tExit, [&](int bx, int by, int bz, float ta, float tb) {
                        ++nBricks;
                        const uint32_t brick = bd.grid[L.gridOffset + bx + L.dims[0] * (by + L.dims[1] * bz)];
                        const float3 bLo = mesh.boundsMin + float3{ bx * brickSize, by * brickSize, bz * brickSize };
                        const uint32_t four[3] = { 4, 4, 4 }, sixteen[3] = { edge, edge, edge };
                        uint32_t cellsHere = 0, voxelsHere = 0;
                        dda(bLo, 4 * voxel, four, o, d, ta, tb, [&](int, int, int, float, float) { ++cellsHere; });
                        dda(bLo, voxel, sixteen, o, d, ta, tb, [&](int, int, int, float, float) { ++voxelsHere; });
                        nCells += cellsHere;
                        nVoxels += voxelsHere;
                        if (brick == UINT32_MAX) return;
                        ++nOccupied;
                        const uint64_t occ = bd.occupancy[brick];
                        dda(bLo, 4 * voxel, four, o, d, ta, tb, [&](int cx, int cy, int cz, float ca, float cb) {
                            if (((occ >> (cx + 4 * (cy + 4 * cz))) & 1) == 0) return;
                            ++nCellsOccupied;
                            const float3 cLo = bLo + float3{ cx * 4 * voxel, cy * 4 * voxel, cz * 4 * voxel };
                            dda(cLo, voxel, four, o, d, ca, cb, [&](int vx, int vy, int vz, float, float) {
                                const uint32_t v = (cx * 4 + vx) + edge * ((cy * 4 + vy) + edge * (cz * 4 + vz));
                                voxelsInCells += 1;
                                if (bd.density[(size_t)brick * edge * edge * edge + v] != 0) ++nVoxelsOccupied;
                            });
                        });
                        voxelsInBricks += voxelsHere;
                    });
                    if (nBricks == 0 || nVoxels == 0) continue;
                    brickRatio.push_back((float)nOccupied / nBricks);
                    cellRatio.push_back((float)nCellsOccupied / nCells);
                    voxelRatio.push_back((float)nVoxelsOccupied / nVoxels);
                    voxelsCrossed += nVoxels;
                    bricksCrossed += nBricks;
                }
            auto pct = [](std::vector<float> v, double q) {
                if (v.empty()) return 0.0f;
                std::sort(v.begin(), v.end());
                return v[std::min(v.size() - 1, (size_t)(q * v.size()))];
            };
            logf("    %-14s (%llu inst) level %u (voxel %.3f m, %u x %u x %u bricks): per ray occupied bricks P10/P50/P90 %.2f/%.2f/%.2f, 4^3 cells %.2f/%.2f/%.2f, "
                 "voxels %.3f/%.3f/%.3f | mean voxels crossed %.1f, in occupied bricks %.1f, in occupied cells %.1f (%zu rays)\n",
                 sm.name.c_str(), (unsigned long long)instancesOf[sceneMesh], level, voxel, L.dims[0], L.dims[1], L.dims[2], pct(brickRatio, 0.1), pct(brickRatio, 0.5),
                 pct(brickRatio, 0.9), pct(cellRatio, 0.1), pct(cellRatio, 0.5), pct(cellRatio, 0.9), pct(voxelRatio, 0.1), pct(voxelRatio, 0.5), pct(voxelRatio, 0.9),
                 brickRatio.empty() ? 0.0 : voxelsCrossed / brickRatio.size(), brickRatio.empty() ? 0.0 : voxelsInBricks / brickRatio.size(),
                 brickRatio.empty() ? 0.0 : voxelsInCells / brickRatio.size(), brickRatio.size());
            (void)bricksCrossed;
        }
    }
#endif
}

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int failed = 0, run = 0;
    for (const TestCase& t : registry())
    {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++run;
        try
        {
            t.fn();
            logf("PASS %s\n", t.name);
        }
        catch (const std::exception& e)
        {
            ++failed;
            logf("FAIL %s: %s\n", t.name, e.what());
        }
    }
    logf("%d/%d passed\n", run - failed, run);
    return failed == 0 ? 0 : 1;
}
