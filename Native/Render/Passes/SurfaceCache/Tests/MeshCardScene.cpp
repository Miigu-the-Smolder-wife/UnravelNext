// refl::MeshCardScene (MESH_CARDS_INTERFACE_KO.md 5), CPU: the level a card asks for at a distance, sub-allocation and
// whole pages, the per-frame capture budget, reallocation and hiding when the camera moves, and the atlas state's
// consistency after every frame. With a .unxscene argument: the scene's cards filled from its first camera - frames
// until every card has its level, the atlas use, the time of an update.
#include "unx/core/Jobs.h"
#include "unx/core/Log.h"
#include "unx/refl/MeshCardScene.h"
#include "unx/scene/MeshCards.h"
#include "unx/scene/SceneData.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <vector>

using namespace unx;
using namespace unx::render::refl;

namespace
{
uint32_t g_failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)

void quad(scene::Mesh& m, float3 a, float3 b, float3 c, float3 d)
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

scene::Mesh boxMesh(float3 lo, float3 hi)
{
    scene::Mesh m;
    const float3 p[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
                          { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
    const int faces[6][4] = { { 0, 4, 7, 3 }, { 1, 2, 6, 5 }, { 0, 1, 5, 4 }, { 3, 7, 6, 2 }, { 0, 3, 2, 1 }, { 4, 5, 6, 7 } };
    for (int f = 0; f < 6; ++f)
    {
        const float3 a = p[faces[f][0]], b = p[faces[f][1]], c = p[faces[f][2]], d = p[faces[f][3]];
        float3 want{ 0, 0, 0 };
        (f / 2 == 0 ? want.x : (f / 2 == 1 ? want.y : want.z)) = (f & 1) != 0 ? 1.0f : -1.0f;
        if (dot(cross(b - a, c - a), want) > 0) quad(m, a, b, c, d); else quad(m, a, d, c, b);
    }
    scene::Submesh s;
    s.indexCount = (uint32_t)m.indices.size();
    m.submeshes.push_back(s);
    return m;
}

float3x4 translation(float3 t, float scale = 1.0f)
{
    float3x4 m;
    m.m[0][0] = m.m[1][1] = m.m[2][2] = scale;
    m.m[0][3] = t.x;
    m.m[1][3] = t.y;
    m.m[2][3] = t.z;
    return m;
}

int sceneReport(const char* path)
{
    const scene::Scene s = scene::load(path);
    std::vector<float> scale(s.meshes.size(), 0.0f);
    for (const scene::Instance& inst : s.instances)
        scale[inst.mesh] = std::max(scale[inst.mesh], length(float3{ inst.transform.m[0][0], inst.transform.m[1][0], inst.transform.m[2][0] }));
    std::vector<scene::MeshCards> cards(s.meshes.size());
    Jobs::instance().parallelFor((uint32_t)s.meshes.size(),
                                 [&](uint32_t i) { cards[i] = scene::buildMeshCards(s.meshes[i], s.materials, scene::kMaxMeshCards, scale[i] > 0 ? scale[i] : 1.0f); });
    MeshCardScene mcs;
    uint32_t rigid = 0, withCards = 0;
    for (uint32_t i = 0; i < s.instances.size(); ++i)
    {
        const scene::Instance& inst = s.instances[i];
        const scene::Mesh& mesh = s.meshes[inst.mesh];
        if ((inst.flags & (scene::InstanceSkinned | scene::InstanceWind)) != 0 || !mesh.blendShapes.empty() || mesh.vertexAnimation.framesPerSecond > 0) continue;
        ++rigid;
        if (mcs.addInstance(i, cards[inst.mesh], inst.transform) != mc::kNone) ++withCards;
    }
    const float3 eye = s.cameras.empty() ? float3{ 0, 1.6f, 0 } : s.cameras[0].position;
    std::printf("%s: %zu instances, %u rigid, %u with cards; camera (%.2f, %.2f, %.2f)\n", path, s.instances.size(), rigid, withCards, eye.x, eye.y, eye.z);
    double worstMs = 0, firstMs = 0;
    uint32_t frames = 0;
    for (; frames < 2000; ++frames)
    {
        const auto t0 = std::chrono::steady_clock::now();
        mcs.update(std::span<const float3>(&eye, 1));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (frames == 0) firstMs = ms;
        worstMs = std::max(worstMs, ms);
        const McStats& st = mcs.stats();
        if (frames < 4 || st.captures == 0 || frames % 50 == 0)
            std::printf("  frame %4u: requests %6u captures %3u texels %6u lowered %u | visible cards %u / %u, mapped pages %u, free physical pages %u\n", frames + 1,
                        st.requests, st.captures, st.capturedTexels, st.loweredAllocations, st.visibleCards, st.cards, st.mappedPages, st.freePhysicalPages);
        if (st.captures == 0) break;
    }
    mcs.validate();
    const McStats& st = mcs.stats();
    const double atlas = (double)mcs.settings().atlasSize * mcs.settings().atlasSize;
    std::printf("  settled after %u frames; requests left %u; texels in the atlas %llu (%.1f %% of %u^2), asked for %llu; update %.2f ms first, %.2f ms worst\n", frames + 1,
                st.requests, (unsigned long long)st.allocatedTexels, 100.0 * (double)st.allocatedTexels / atlas, mcs.settings().atlasSize,
                (unsigned long long)st.desiredTexels, firstMs, worstMs);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc > 1) return sceneReport(argv[1]);

        const scene::MeshCards cube = scene::buildMeshCards(boxMesh({ -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f }), std::vector<uint8_t>{});
        CHECK(cube.cards.size() == 6);
        {
            // one cube: levels by distance, sub-allocation, reallocation, hiding (no refresh captures: the counts below
            // are those of new pages)
            McSettings quiet;
            quiet.refreshFraction = 0;
            MeshCardScene s(quiet);
            CHECK(s.captureAtlasSize() == 512);
            CHECK(s.addInstance(3, cube, translation({ 0, 0, 0 })) == 0);
            CHECK(s.instanceMap().size() == 4 && s.instanceMap()[3] == 0 && s.instanceMap()[0] == mc::kNone);
            float3 eye{ 4, 0, 0 };
            s.update(std::span<const float3>(&eye, 1));
            s.validate();
            // extent 0.51 m: min(100 x 0.51 / d, 20 x 0.51) = 10 texels -> 16 (level 4) while d <= 5.1 m
            CHECK(s.captures().size() == 6);
            for (const McCapture& c : s.captures())
            {
                CHECK(c.atlasRect[2] == 16 && c.atlasRect[3] == 16 && c.captureRect[2] == 16 && c.captureRect[3] == 16);
                CHECK(!c.resample && c.sceneInstance == 3);
                CHECK(c.cardUvRect[0] == 0 && c.cardUvRect[1] == 0 && c.cardUvRect[2] == 1 && c.cardUvRect[3] == 1);
            }
            CHECK(s.stats().visibleCards == 6 && s.stats().mappedPages == 6 && s.stats().allocatedTexels == 6 * 256);
            CHECK(s.stats().freePhysicalPages == 32 * 32 - 1);  // six 16 x 16 elements share one physical page
            CHECK((s.cardsGpu()[0].packed & (1u << 16)) != 0);
            CHECK(s.cardsGpu()[0].sizeInPages == (1u | 1u << 16));
            const McPageTableGpu t = s.pageTableGpu()[s.cardsGpu()[0].pageTableOffset];
            CHECK(((t.packed >> 24) & 0xF) == 4 && ((t.packed >> 28) & 0xF) == 4);
            const MeshCardScene::Dirty d = s.takeDirty();
            CHECK(d.meshCards.size() == 1 && d.cards.size() == 6 && d.pages.size() == 6 && d.instanceMap);
            // nothing changes while the camera stays
            s.update(std::span<const float3>(&eye, 1));
            CHECK(s.captures().empty() && s.stats().requests == 0);
            CHECK(s.takeDirty().cards.empty());
            // 10 m away: 5 texels -> 8 (level 3); the cards are captured again, their lighting can be carried over
            eye = { 10.5f, 0, 0 };
            s.update(std::span<const float3>(&eye, 1));
            s.validate();
            CHECK(s.captures().size() == 6);
            for (const McCapture& c : s.captures()) CHECK(c.atlasRect[2] == 8 && c.atlasRect[3] == 8 && c.resample);
            CHECK(s.stats().allocatedTexels == 6 * 64);
            // 30 m away: 1 texel, below the minimum of 4 -> hidden, nothing left in the atlas
            eye = { 30.5f, 0, 0 };
            s.update(std::span<const float3>(&eye, 1));
            s.validate();
            CHECK(s.captures().empty() && s.stats().visibleCards == 0 && s.stats().mappedPages == 0);
            CHECK(s.stats().freePhysicalPages == 32 * 32);
            CHECK((s.cardsGpu()[0].packed & (1u << 16)) == 0);
            // and back
            eye = { 4, 0, 0 };
            s.update(std::span<const float3>(&eye, 1));
            s.validate();
            CHECK(s.captures().size() == 6 && !s.captures()[0].resample);
            // a rigid move: the records follow, no capture
            s.takeDirty();
            s.setTransform(3, translation({ 0.25f, 0, 0 }));
            CHECK(s.meshCardsGpu()[0].worldToLocal[0][3] == 0.25f);
            s.update(std::span<const float3>(&eye, 1));
            CHECK(s.captures().empty());
            CHECK(s.takeDirty().cards.size() == 6);
        }
        {
            // a 40 m wall next to the camera: each large face is 512 x 512 = 4 x 4 pages, the whole capture atlas, so the
            // faces come one per frame; interior page edges carry the half-texel border
            const scene::MeshCards wall = scene::buildMeshCards(boxMesh({ -20, -20, -0.1f }, { 20, 20, 0.1f }), std::vector<uint8_t>{});
            McSettings quiet;
            quiet.refreshFraction = 0;
            MeshCardScene s(quiet);
            s.addInstance(0, wall, translation({ 0, 0, 0 }));
            const float3 eye{ 0, 0, 1.5f };
            uint32_t full = 0, frames = 0;
            bool border = false;
            do
            {
                s.update(std::span<const float3>(&eye, 1));
                s.validate();
                CHECK(s.stats().capturedTexels <= 512u * 512u);
                for (const McCapture& c : s.captures())
                {
                    if (c.atlasRect[2] == 128 && c.atlasRect[3] == 128) ++full;
                    if (c.cardUvRect[0] > 0 && c.cardUvRect[0] < 0.25f) border = std::fabs(c.cardUvRect[0] - (0.25f - 0.5f * 0.25f / 128.0f)) < 1e-6f || border;
                }
                ++frames;
            } while (!s.captures().empty() && frames < 20);
            CHECK(full == 32);
            CHECK(border);
            CHECK(s.stats().requests == 0);
            CHECK(s.stats().visibleCards == s.stats().cards);
            std::printf("wall: %u cards, %u pages, settled in %u frames\n", s.stats().cards, s.stats().mappedPages, frames);
        }
        {
            // the frame budget: 3,600 cubes on a 1.2 m grid (21,600 cards; those within about 12.7 m are large enough to show)
            McSettings quiet;
            quiet.refreshFraction = 0;
            MeshCardScene s(quiet);
            uint32_t n = 0;
            for (int z = 0; z < 60; ++z)
                for (int x = 0; x < 60; ++x) s.addInstance(n++, cube, translation({ (float)x * 1.2f - 36.0f, 0, (float)z * 1.2f - 36.0f }));
            const float3 eye{ 0, 1.5f, 0 };
            uint32_t frames = 0, captures = 0, worst = 0;
            do
            {
                s.update(std::span<const float3>(&eye, 1));
                s.validate();
                captures += s.stats().captures;
                worst = std::max(worst, s.stats().captures);
                CHECK(s.stats().captures <= s.settings().capturesPerFrame);
                CHECK(s.stats().capturedTexels <= 512u * 512u);
                ++frames;
            } while (!s.captures().empty() && frames < 500);
            CHECK(s.stats().requests == 0);
            CHECK(captures == s.stats().visibleCards);
            CHECK(worst == s.settings().capturesPerFrame);
            std::printf("grid: %u cards, %u visible, %u captures in %u frames, texels in the atlas %llu\n", s.stats().cards, s.stats().visibleCards, captures, frames,
                        (unsigned long long)s.stats().allocatedTexels);
        }
        {
            // refresh captures, removal and re-use of the freed entries
            MeshCardScene s;  // (refreshFraction 0.125: 37 pages a frame)
            for (uint32_t i = 0; i < 20; ++i) s.addInstance(i, cube, translation({ (float)i * 1.5f, 0, 0 }));
            const float3 eye{ 14, 0, 3 };
            s.update(std::span<const float3>(&eye, 1));
            s.validate();
            const uint32_t pages = s.stats().mappedPages;
            CHECK(pages == 120 && s.stats().refreshed == 0);  // (every page was captured in this frame)
            // the resident pages come round: 37 a frame, each once before any twice
            std::vector<uint32_t> seen(s.pageCount(), 0);
            uint32_t refreshed = 0;
            for (uint32_t frame = 0; frame < 4; ++frame)
            {
                s.update(std::span<const float3>(&eye, 1));
                s.validate();
                CHECK(s.stats().refreshed == s.captures().size() && s.stats().refreshed <= 37);
                for (const McCapture& c : s.captures())
                {
                    CHECK(c.refresh && !c.resample && c.page < seen.size());
                    if (c.page < seen.size()) ++seen[c.page];
                    ++refreshed;
                }
            }
            CHECK(refreshed > pages);
            uint32_t most = 0, least = 0xFFFFFFFFu;
            for (uint32_t n : seen)
                if (n) most = std::max(most, n), least = std::min(least, n);
            CHECK(most <= 2 && least >= 1);
            // an instance asked for by name goes first
            s.refreshInstance(7);
            s.update(std::span<const float3>(&eye, 1));
            uint32_t ofSeven = 0;
            for (const McCapture& c : s.captures()) ofSeven += c.sceneInstance == 7 ? 1u : 0u;
            CHECK(ofSeven == 6);
            // removal frees the pages and the records; a new instance takes the freed slots
            const uint32_t cards = s.stats().cards, sets = s.stats().meshCards;
            s.removeInstance(7);
            CHECK(!s.hasInstance(7) && s.instanceMap()[7] == mc::kNone);
            s.update(std::span<const float3>(&eye, 1));
            s.validate();
            CHECK(s.stats().mappedPages == pages - 6);
            CHECK(s.addInstance(40, cube, translation({ 7 * 1.5f, 0, 0 })) != mc::kNone);
            s.update(std::span<const float3>(&eye, 1));
            s.validate();
            CHECK(s.stats().mappedPages == pages && s.stats().cards == cards && s.stats().meshCards == sets);
            uint32_t ofForty = 0;
            for (const McCapture& c : s.captures()) ofForty += c.sceneInstance == 40 && !c.refresh ? 1u : 0u;
            CHECK(ofForty == 6);
        }
        {
            // an instance too small for cards, and one shown at a scale
            MeshCardScene s;
            const scene::MeshCards tiny = scene::buildMeshCards(boxMesh({ -0.02f, -0.02f, -0.02f }, { 0.02f, 0.02f, 0.02f }), std::vector<uint8_t>{});
            CHECK(s.addInstance(0, tiny, translation({ 0, 0, 0 })) == mc::kNone);
            CHECK(s.addInstance(1, cube, translation({ 0, 0, 0 }, 4.0f)) == 0);
            CHECK(std::fabs(s.cardsGpu()[0].extent[0] - 2.04f) < 0.05f);
            bool refused = false;
            try
            {
                s.addInstance(1, cube, translation({ 0, 0, 0 }));
            }
            catch (const std::exception&)
            {
                refused = true;
            }
            CHECK(refused);
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
