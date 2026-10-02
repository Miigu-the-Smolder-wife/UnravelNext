// scene::buildMeshCards (MESH_CARDS_INTERFACE_KO.md 2), CPU: card counts and directions on meshes whose answer follows
// from the generator's rules, and that every surface point a card can hold lies inside a card of its direction.
//   closed box           one outer card per direction
//   room shell (inward)  one card per direction behind the near wall (near plane > 0), none for the outside
//   one-sided floor      a single +Y card
//   box in a box         the inner box is inside geometry: no cards for it
//   two stacked slabs    +Y and -Y get a second card for the faces in the gap
//   two-sided quad       both directions of its axis
#include "unx/core/Jobs.h"
#include "unx/core/Log.h"
#include "unx/scene/MeshCards.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <vector>

using namespace unx;
using namespace unx::scene;

namespace
{
uint32_t g_failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)

// Quad a, b, c, d counter-clockwise seen from its front.
void quad(Mesh& m, float3 a, float3 b, float3 c, float3 d)
{
    const uint32_t base = (uint32_t)m.positions.size();
    const float3 n = normalize(cross(b - a, c - a));
    for (float3 p : { a, b, c, d })
    {
        m.positions.push_back(p);
        m.normals.push_back(n);
    }
    for (uint32_t i : { 0u, 1u, 2u, 0u, 2u, 3u }) m.indices.push_back(base + i);
}

// Axis-aligned box; outward = faces seen from outside, else from inside (a room).
void box(Mesh& m, float3 lo, float3 hi, bool outward)
{
    const float3 p[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
                          { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
    const int faces[6][4] = { { 0, 4, 7, 3 }, { 1, 2, 6, 5 }, { 0, 1, 5, 4 }, { 3, 7, 6, 2 }, { 0, 3, 2, 1 }, { 4, 5, 6, 7 } };  // -X +X -Y +Y -Z +Z
    for (int f = 0; f < 6; ++f)
    {
        const float3 a = p[faces[f][0]], b = p[faces[f][1]], c = p[faces[f][2]], d = p[faces[f][3]];
        float3 want{ 0, 0, 0 };
        (f / 2 == 0 ? want.x : (f / 2 == 1 ? want.y : want.z)) = (f & 1) != 0 ? 1.0f : -1.0f;
        const bool isOutward = dot(cross(b - a, c - a), want) > 0;
        if (isOutward == outward) quad(m, a, b, c, d); else quad(m, a, d, c, b);
    }
}

void finish(Mesh& m)
{
    Submesh s;
    s.indexCount = (uint32_t)m.indices.size();
    m.submeshes.push_back(s);
}

uint32_t count(const MeshCards& c, uint32_t direction)
{
    uint32_t n = 0;
    for (const MeshCard& card : c.cards) n += card.direction == direction ? 1u : 0u;
    return n;
}

bool inside(const MeshCard& card, float3 p)
{
    float3 x, y, z;
    meshCardAxes(card.direction, x, y, z);
    const float3 r = p - card.origin;
    const float e = 1e-4f;
    return std::fabs(dot(r, x)) <= card.extent.x + e && std::fabs(dot(r, y)) <= card.extent.y + e && std::fabs(dot(r, z)) <= card.extent.z + e;
}

// Fraction of the mesh's triangle centroids (front side only unless two-sided) that lie in a card of their normal's
// main direction.
float held(const Mesh& m, const MeshCards& cards, bool twoSided)
{
    uint32_t in = 0, all = 0;
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
    {
        const float3 a = m.positions[m.indices[t]], b = m.positions[m.indices[t + 1]], c = m.positions[m.indices[t + 2]];
        const float3 n = normalize(cross(b - a, c - a));
        const float3 centre = (a + b + c) / 3.0f;
        const float ax = std::fabs(n.x), ay = std::fabs(n.y), az = std::fabs(n.z);
        const uint32_t axis = ax >= ay && ax >= az ? 0u : (ay >= az ? 1u : 2u);
        const float sign = axis == 0 ? n.x : (axis == 1 ? n.y : n.z);
        const uint32_t direction = axis * 2 + (sign > 0 ? 1u : 0u);
        bool found = false;
        for (const MeshCard& card : cards.cards)
            if ((card.direction == direction || (twoSided && card.direction == (direction ^ 1u))) && inside(card, centre)) found = true;
        ++all;
        in += found ? 1u : 0u;
    }
    return all != 0 ? (float)in / (float)all : 0.0f;
}

void print(const char* name, const MeshCards& c, double ms)
{
    std::printf("%-16s cards %2zu (", name, c.cards.size());
    for (uint32_t d = 0; d < kMeshCardDirections; ++d) std::printf("%u%s", count(c, d), d + 1 < kMeshCardDirections ? " " : "");
    std::printf(")  surfels %5u  %.1f ms\n", c.surfels, ms);
}

MeshCards timed(const char* name, const Mesh& m, const std::vector<uint8_t>& twoSided = {})
{
    const auto t0 = std::chrono::steady_clock::now();
    const MeshCards c = buildMeshCards(m, twoSided);
    print(name, c, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    return c;
}
// With a scene file: the cards of every mesh of the scene (counts, time, how much of each mesh they hold).
int sceneReport(const char* path)
{
    const Scene scene = load(path);
    std::vector<MeshCards> cards(scene.meshes.size());
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<uint32_t> instances(scene.meshes.size(), 0);
    std::vector<float> scale(scene.meshes.size(), 0.0f);  // the largest scale a mesh is shown at (1 when it has no instance)
    for (const Instance& inst : scene.instances)
    {
        ++instances[inst.mesh];
        const float3 column{ inst.transform.m[0][0], inst.transform.m[1][0], inst.transform.m[2][0] };
        scale[inst.mesh] = std::max(scale[inst.mesh], length(column));
    }
    for (float& v : scale) v = v > 0 ? v : 1.0f;
    std::vector<float> meshMs(scene.meshes.size(), 0.0f);
    Jobs::instance().parallelFor((uint32_t)scene.meshes.size(), [&](uint32_t i)
                                 {
                                     const auto m0 = std::chrono::steady_clock::now();
                                     cards[i] = buildMeshCards(scene.meshes[i], scene.materials, kMaxMeshCards, scale[i]);
                                     meshMs[i] = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - m0).count();
                                 });
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    size_t total = 0, none = 0, trianglesAll = 0;
    double heldWeighted = 0;
    std::printf("mesh                              tris  inst   size (m)            cards (-X +X -Y +Y -Z +Z)  surfels  held      ms  (passes, bvh / columns / visibility / clusters ms, hemisphere rays)\n");
    for (size_t i = 0; i < scene.meshes.size(); ++i)
    {
        const Mesh& m = scene.meshes[i];
        const MeshCards& c = cards[i];
        const size_t triangles = m.indices.size() / 3;
        const float h = held(m, c, c.mostlyTwoSided);
        const float3 size = (c.boundsMax - c.boundsMin) * scale[i];
        std::printf("%-30.30s %7zu %5u   %5.2f %5.2f %5.2f   %2zu (%u %u %u %u %u %u) %s %6u  %.3f %7.0f  (%u, %.0f / %.0f / %.0f / %.0f, %llu)\n", m.name.c_str(), triangles, instances[i], size.x, size.y,
                    size.z, c.cards.size(), count(c, 0), count(c, 1), count(c, 2), count(c, 3), count(c, 4), count(c, 5), c.mostlyTwoSided ? "2s" : "  ",
                    c.surfels, h, meshMs[i], c.stats.passes, c.stats.bvhMs, c.stats.columnMs, c.stats.visibilityMs, c.stats.clusterMs,
                    (unsigned long long)c.stats.hemisphereRays);
        total += c.cards.size();
        none += c.cards.empty() ? 1 : 0;
        trianglesAll += triangles;
        heldWeighted += (double)h * (double)triangles;
    }
    std::printf("%zu meshes, %zu instances, %zu cards, %zu meshes without cards, triangle centroids held %.3f, %.0f ms on %u workers\n", scene.meshes.size(),
                scene.instances.size(), total, none, trianglesAll != 0 ? heldWeighted / (double)trianglesAll : 0.0, ms, Jobs::instance().workerCount());
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1) return sceneReport(argv[1]);
        {
            Mesh m;
            box(m, { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f }, true);
            finish(m);
            const MeshCards c = timed("closed box", m);
            CHECK(c.cards.size() == 6);
            for (uint32_t d = 0; d < kMeshCardDirections; ++d) CHECK(count(c, d) == 1);
            CHECK(!c.mostlyTwoSided);
            CHECK(held(m, c, false) == 1.0f);
            for (const MeshCard& card : c.cards)
            {
                // the outer card spans the whole face; its depth is the facing face's cell with half a voxel before and
                // one and a half behind (the side faces belong to their own directions)
                CHECK(std::fabs(card.extent.x - 0.51f) < 0.011f && std::fabs(card.extent.y - 0.51f) < 0.011f);
                CHECK(std::fabs(card.extent.z - 0.1f) < 0.011f);
            }
            // the same mesh gives the same cards
            const MeshCards again = buildMeshCards(m, std::vector<uint8_t>{});
            CHECK(again.cards.size() == c.cards.size());
            for (size_t i = 0; i < c.cards.size() && i < again.cards.size(); ++i)
                CHECK(again.cards[i].origin.x == c.cards[i].origin.x && again.cards[i].extent.z == c.cards[i].extent.z &&
                      again.cards[i].direction == c.cards[i].direction);
        }
        {
            Mesh m;
            box(m, { -2.0f, 0.0f, -2.0f }, { 2.0f, 3.0f, 2.0f }, false);
            finish(m);
            const MeshCards c = timed("room shell", m);
            CHECK(c.cards.size() == 6);
            for (uint32_t d = 0; d < kMeshCardDirections; ++d) CHECK(count(c, d) == 1);
            CHECK(held(m, c, false) == 1.0f);
            // each card starts behind the wall the capture looks through: it does not reach the near side of the bounds
            for (const MeshCard& card : c.cards)
            {
                float3 x, y, z;
                meshCardAxes(card.direction, x, y, z);
                const float front = dot(card.origin, z) + card.extent.z;  // the card's near plane along its normal
                const float boundsFront = std::max(dot(c.boundsMin, z), dot(c.boundsMax, z));
                CHECK(front < boundsFront - 0.04f);
            }
        }
        {
            Mesh m;
            quad(m, { -2, 0, -2 }, { -2, 0, 2 }, { 2, 0, 2 }, { 2, 0, -2 });  // faces +Y
            finish(m);
            CHECK(normalize(cross(m.positions[1] - m.positions[0], m.positions[2] - m.positions[0])).y > 0.99f);
            const MeshCards c = timed("floor", m);
            CHECK(c.cards.size() == 1);
            CHECK(count(c, 3) == 1);
            CHECK(held(m, c, false) == 1.0f);

            const MeshCards both = timed("two-sided quad", m, std::vector<uint8_t>(m.indices.size() / 3, 1));
            CHECK(both.mostlyTwoSided);
            CHECK(both.cards.size() == 2);
            CHECK(count(both, 2) == 1 && count(both, 3) == 1);
        }
        {
            Mesh m;
            box(m, { -1, -1, -1 }, { 1, 1, 1 }, true);
            box(m, { -0.4f, -0.4f, -0.4f }, { 0.4f, 0.4f, 0.4f }, true);
            finish(m);
            const MeshCards c = timed("box in a box", m);
            CHECK(c.cards.size() == 6);
            for (uint32_t d = 0; d < kMeshCardDirections; ++d) CHECK(count(c, d) == 1);
        }
        {
            Mesh m;
            box(m, { -0.5f, 0.0f, -0.5f }, { 0.5f, 0.2f, 0.5f }, true);
            box(m, { -0.5f, 0.8f, -0.5f }, { 0.5f, 1.0f, 0.5f }, true);
            finish(m);
            const MeshCards c = timed("stacked slabs", m);
            CHECK(count(c, 2) == 2 && count(c, 3) == 2);
            CHECK(count(c, 0) == 1 && count(c, 1) == 1 && count(c, 4) == 1 && count(c, 5) == 1);
            CHECK(c.cards.size() <= kMaxMeshCards);
            CHECK(held(m, c, false) == 1.0f);
        }
        {
            // a large mesh: the 64-voxel cap (the lobby's shell is tens of metres)
            Mesh m;
            box(m, { -20, 0, -12 }, { 20, 6, 12 }, false);
            for (int i = 0; i < 6; ++i) box(m, { -15.0f + 6.0f * (float)i, 0.0f, -0.4f }, { -14.2f + 6.0f * (float)i, 6.0f, 0.4f }, true);  // columns
            finish(m);
            const MeshCards c = timed("hall + columns", m);
            CHECK(!c.cards.empty() && c.cards.size() <= kMaxMeshCards);
            std::printf("  hall: centroids held %.3f\n", held(m, c, false));
        }
        {
            // a mesh authored in centimetres and shown at 0.01: the same cards as the metre box, in its own units
            Mesh m;
            box(m, { -50, -50, -50 }, { 50, 50, 50 }, true);
            finish(m);
            const MeshCards c = buildMeshCards(m, std::vector<uint8_t>{}, kMaxMeshCards, 0.01f);
            print("box in cm", c, 0.0);
            CHECK(c.cards.size() == 6);
            for (const MeshCard& card : c.cards) CHECK(std::fabs(card.extent.x - 51.0f) < 1.1f && std::fabs(card.extent.z - 10.0f) < 1.1f);
        }
        {
            Mesh empty;
            CHECK(buildMeshCards(empty, std::vector<uint8_t>{}).cards.empty());
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    if (g_failures != 0)
    {
        std::fprintf(stderr, "FAIL %u checks\n", g_failures);
        return 1;
    }
    std::printf("PASS\n");
    return 0;
}
