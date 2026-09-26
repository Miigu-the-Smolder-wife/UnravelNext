// C5 terrain deformation patches (render C): a cooked terrain tile with holes, footprints in a 5 cm D window (some across
// block lines, one across the tile edge, one on a stitched side's neighbour), for both z orientations of the tile (the
// host's axis mapping may mirror z). The tile's source triangles inside replaced blocks are dropped (as V does) and the
// patches added; the result must be watertight (every edge used twice except the tile's outer border and hole rims),
// cover exactly the source's projected area, put every patch vertex on the source surface plus D, keep source vertices
// bit for bit where D is 0 on stitched sides, and face the source's way.
#include "unx/core/Log.h"
#include "unx/scene/TerrainPatch.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

using namespace unx;
using namespace unx::scene;

namespace
{
uint32_t g_failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)

constexpr uint32_t kCells = 32;
constexpr float kCell = 0.5f;

float heightAt(float x, float z) { return 1.5f * std::sin(x * 0.31f) * std::cos(z * 0.23f) + 0.4f * std::sin(x * 1.3f + z * 0.7f); }

// Tile like UnravelNextTerrainCook: cells [x0, x0 + kCells) with terrain-space positions; zSign -1 mirrors z (and the
// winding) as an axis mapping would; hole cells get no triangles.
Mesh tile(uint32_t x0, float zSign)
{
    Mesh m;
    m.name = "tile";
    for (uint32_t j = 0; j <= kCells; ++j)
        for (uint32_t i = 0; i <= kCells; ++i)
        {
            const float x = (x0 + i) * kCell, z = j * kCell;
            m.positions.push_back({ x, heightAt(x, z), zSign * z });
            const float gx = (heightAt(x + kCell, z) - heightAt(x - kCell, z)) / (2 * kCell), gz = (heightAt(x, z + kCell) - heightAt(x, z - kCell)) / (2 * kCell);
            m.normals.push_back(normalize(float3{ -gx, 1, -zSign * gz }));
            m.uv0.push_back({ x / 64.0f, z / 64.0f });
        }
    for (uint32_t j = 0; j < kCells; ++j)
        for (uint32_t i = 0; i < kCells; ++i)
        {
            if ((i == 5 && j == 6) || (i == 20 && j == 21)) continue;  // holes (one inside a replaced block)
            const uint32_t a = j * (kCells + 1) + i, b = a + 1, c = a + kCells + 1, d = c + 1;
            if (zSign > 0) m.indices.insert(m.indices.end(), { a, c, d, a, d, b });
            else m.indices.insert(m.indices.end(), { a, d, c, a, b, d });
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 3 });
    return m;
}

// Footprints (ellipsoidal dents up to 6 cm) in world (terrain) coordinates; the window covers x [4, 20.2) z [2, 18.2).
std::vector<float> g_heights;

TerrainDeformation footprints(float zSign)
{
    TerrainDeformation d;
    d.spacing = 0.05f;
    d.size = 324;
    d.originX = 4.0f;
    d.originZ = zSign > 0 ? 2.0f : -2.0f - (d.size - 1) * d.spacing;  // object z of texel 0 (rows run along +z)
    g_heights.assign((size_t)d.size * d.size, 0.0f);
    d.height = g_heights.data();
    const float prints[][3] = { { 6.1f, 4.3f, 0.06f }, { 8.0f, 6.0f, 0.05f }, { 11.3f, 9.9f, 0.04f }, { 15.95f, 10.0f, 0.05f }, { 10.7f, 11.2f, 0.03f }, { 10.9f, 3.4f, 0.04f } };  // the last one over the hole cell (5, 6)
    for (uint32_t tz = 0; tz < d.size; ++tz)
        for (uint32_t tx = 0; tx < d.size; ++tx)
        {
            const float x = d.originX + tx * d.spacing, oz = d.originZ + tz * d.spacing, z = zSign * oz;
            float h = 0;
            for (const auto& p : prints)
            {
                const float u = (x - p[0]) / 0.16f, v = (z - p[1]) / 0.3f, r2 = u * u + v * v;
                if (r2 < 1) h = std::min(h, -p[2] * (1 - r2));
            }
            g_heights[(size_t)tz * d.size + tx] = h;
        }
    return d;
}

// Source surface height at object (x, z) of the tile (the tile's own triangles, diagonal a-d).
float sourceHeight(const Mesh& m, float x, float z, float zSign)
{
    const float fi = x / kCell - 16.0f, fj = zSign * z / kCell;
    const uint32_t i = (uint32_t)std::min(std::max(std::floor(fi), 0.0f), (float)kCells - 1), j = (uint32_t)std::min(std::max(std::floor(fj), 0.0f), (float)kCells - 1);
    const float fu = fi - i, fv = fj - j;
    const uint32_t a = j * (kCells + 1) + i, b = a + 1, c = a + kCells + 1, dd = c + 1;
    if (fv >= fu) return m.positions[a].y * (1 - fv) + m.positions[c].y * (fv - fu) + m.positions[dd].y * fu;
    return m.positions[a].y * (1 - fu) + m.positions[b].y * (fu - fv) + m.positions[dd].y * fv;
}

void run(float zSign)
{
    const Mesh src = tile(16, zSign);  // cells 16..48: x 8..24 m; the window covers its left part, one print crosses x = 8
    const TerrainGrid grid = terrainGrid(src);
    const TerrainDeformation d = footprints(zSign);
    CHECK(texelsPerCell(grid, d) == 10);
    const std::vector<uint32_t> active = activePatchBlocks(grid, d);
    const uint32_t blocks = patchBlocksPerSide(grid);
    logf("  z sign %+.0f: %zu of %u blocks replaced\n", zSign, active.size(), blocks * blocks);
    CHECK(!active.empty() && active.size() < blocks * blocks);
    std::vector<uint8_t> replaced((size_t)blocks * blocks, 0);
    for (uint32_t b : active) replaced[b] = 1;

    // Combined surface: source triangles outside replaced blocks (by centroid) + the patches.
    struct Tri
    {
        float3 p[3];
        bool patch;
    };
    std::vector<Tri> tris;
    for (size_t t = 0; t < src.indices.size(); t += 3)
    {
        const float3 a = src.positions[src.indices[t]], b = src.positions[src.indices[t + 1]], c = src.positions[src.indices[t + 2]];
        const float cx = (a.x + b.x + c.x) / 3, cz = zSign * (a.z + b.z + c.z) / 3;
        const uint32_t bi = (uint32_t)((cx / kCell - 16.0f) / kPatchBlockCells), bj = (uint32_t)((cz / kCell) / kPatchBlockCells);
        if (!replaced[bj * blocks + bi]) tris.push_back({ { a, b, c }, false });
    }
    uint64_t patchVertices = 0, patchTriangles = 0;
    double worstHeight = 0;
    for (uint32_t b : active)
    {
        const Mesh p = buildTerrainPatch(grid, d, b % blocks, b / blocks);
        patchVertices += p.positions.size();
        patchTriangles += p.indices.size() / 3;
        CHECK(p.submeshes.size() == 1 && p.submeshes[0].material == 3);
        for (size_t t = 0; t < p.indices.size(); t += 3) tris.push_back({ { p.positions[p.indices[t]], p.positions[p.indices[t + 1]], p.positions[p.indices[t + 2]] }, true });
        // Every patch vertex: source surface + D at its texel.
        for (const float3& v : p.positions)
        {
            const long tx = std::lround((v.x - d.originX) / d.spacing), tz = std::lround((v.z - d.originZ) / d.spacing);
            const float h = (tx >= 0 && tz >= 0 && tx < (long)d.size && tz < (long)d.size) ? d.height[(size_t)tz * d.size + tx] : 0.0f;
            worstHeight = std::max(worstHeight, (double)std::fabs(v.y - h - sourceHeight(src, v.x, v.z, zSign)));
        }
    }
    logf("  patches: %llu vertices, %llu triangles; worst |y - (source + D)| %.2e m\n", (unsigned long long)patchVertices, (unsigned long long)patchTriangles, worstHeight);
    CHECK(worstHeight < 2e-6);

    // Watertight: edges by exact endpoint bits. Border edges (used once) lie on the tile's outer border or around holes.
    auto key = [](const float3& p) { return std::make_tuple(p.x, p.y, p.z); };
    std::map<std::pair<std::tuple<float, float, float>, std::tuple<float, float, float>>, int> edges;
    double area = 0;
    uint32_t flipped = 0;
    for (const Tri& t : tris)
    {
        const float3 n = cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
        area += 0.5 * std::fabs(n.y);
        if (n.y < -1e-9f) ++flipped;  // both orientations of the test tile face +y
        for (int e = 0; e < 3; ++e)
        {
            auto a = key(t.p[e]), b = key(t.p[(e + 1) % 3]);
            if (b < a) std::swap(a, b);
            ++edges[{ a, b }];
        }
    }
    auto onBorderOrHole = [&](const std::tuple<float, float, float>& p) {
        const float x = std::get<0>(p), z = zSign * std::get<2>(p);
        const float fi = x / kCell - 16.0f, fj = z / kCell;
        if (fi <= 0 || fj <= 0 || fi >= kCells || fj >= kCells) return true;
        for (auto [hi, hj] : { std::pair{ 5.0f, 6.0f }, std::pair{ 20.0f, 21.0f } })
            if (fi >= hi - 1e-4f && fi <= hi + 1 + 1e-4f && fj >= hj - 1e-4f && fj <= hj + 1 + 1e-4f) return true;
        return false;
    };
    uint32_t open = 0, overused = 0;
    for (const auto& [e, count] : edges)
    {
        if (count > 2) ++overused;
        if (count == 1 && !(onBorderOrHole(e.first) && onBorderOrHole(e.second))) ++open;
    }
    logf("  combined: %zu triangles, %u open edges inside, %u edges used 3+ times, %u flipped\n", tris.size(), open, overused, flipped);
    CHECK(open == 0 && overused == 0 && flipped == 0);
    const double expected = (kCells * kCells - 2) * kCell * kCell;
    logf("  projected area %.6f m^2 (source %.6f)\n", area, expected);
    CHECK(std::fabs(area - expected) < 1e-4 * expected);
}
} // namespace

int main()
{
    try
    {
        run(1.0f);
        run(-1.0f);
        // No deformation: nothing is replaced.
        {
            const Mesh src = tile(16, 1.0f);
            TerrainDeformation d = footprints(1.0f);
            std::fill(g_heights.begin(), g_heights.end(), 0.0f);
            CHECK(activePatchBlocks(terrainGrid(src), d).empty());
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    logf(g_failures ? "TERRAIN PATCH TEST FAILED (%u)\n" : "TERRAIN PATCH TEST PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
