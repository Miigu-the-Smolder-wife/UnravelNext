// Cluster builder self-checks on procedural meshes (CPU only, seconds):
//   limits, crack-free uniform cuts (also across a submesh seam), monotone errors and enclosing spheres, hierarchy
//   coverage, minimum feature widths of known shapes, thin geometry kept out of LOD thinning, determinism.
//   unx_test_clusterbuilder [filter]
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/clusterbuilder/ClusterHierarchy.h"
#include "unx/core/Config.h"
#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <vector>

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

UNX_TEST(build_is_deterministic)
{
    const Built a = buildOne(heightfield(96, 20.0f, 1.0f, true));
    const Built b = buildOne(heightfield(96, 20.0f, 1.0f, true));
    CHECK(a.data.clusters.size() == b.data.clusters.size());
    CHECK(std::memcmp(a.data.clusters.data(), b.data.clusters.data(), a.data.clusters.size() * sizeof(render::gpu::Cluster)) == 0);
    CHECK(a.data.clusterVertexIndices == b.data.clusterVertexIndices);
    CHECK(a.data.clusterTriangles == b.data.clusterTriangles);
    CHECK(a.data.lodLevelClusters == b.data.lodLevelClusters);
    CHECK(a.data.named.size() == b.data.named.size());
    for (size_t i = 0; i < a.data.named.size(); ++i) CHECK(a.data.named[i].bytes == b.data.named[i].bytes);
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
