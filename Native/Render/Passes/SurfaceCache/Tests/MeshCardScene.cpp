// refl::MeshCardScene (MESH_CARDS_INTERFACE_KO.md 5), CPU: the level a card asks for at a distance, sub-allocation and
// whole pages, the per-frame capture budget, reallocation and hiding when the camera moves, the feedback's pages above
// the resident level (mapped where hits ask, kept while they ask, gone when they stop), and the atlas state's
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
#include <random>
#include <vector>

using namespace unx;
using namespace unx::render::refl;

namespace
{
uint32_t g_failures = 0;
uint64_t g_stateHash = 1469598103934665603ull;
uint32_t g_checkedStates = 0;

void hashBytes(const void* data, size_t bytes)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) g_stateHash = (g_stateHash ^ p[i]) * 1099511628211ull;
}

template<class T> void hashVector(const std::vector<T>& values)
{
    const uint64_t size = values.size();
    hashBytes(&size, sizeof(size));
    if (!values.empty()) hashBytes(values.data(), values.size() * sizeof(T));
}

void validateAndFingerprint(const MeshCardScene& s)
{
    s.validate();
    ++g_checkedStates;
    // These packed GPU mirrors have explicit padding fields, initialized by the
    // producer. Capture fields are hashed separately to exclude C++ bool padding.
    hashVector(s.instanceMap()); hashVector(s.meshCardsGpu()); hashVector(s.cardsGpu());
    hashVector(s.pagesGpu()); hashVector(s.pageTableGpu());
    const uint64_t count = s.captures().size();
    hashBytes(&count, sizeof(count));
    for (const auto& c : s.captures())
    {
        hashBytes(&c.page, sizeof(c.page)); hashBytes(&c.card, sizeof(c.card));
        hashBytes(&c.sceneInstance, sizeof(c.sceneInstance));
        hashBytes(c.captureRect, sizeof(c.captureRect)); hashBytes(c.atlasRect, sizeof(c.atlasRect));
        hashBytes(c.cardUvRect, sizeof(c.cardUvRect));
        hashBytes(&c.resample, sizeof(c.resample)); hashBytes(&c.refresh, sizeof(c.refresh));
    }
}
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
    validateAndFingerprint(mcs);
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
            // Reuse holes before and after the allocation cursor, exhaust a small
            // atlas, change suballocation sizes, and exercise feedback/refresh.
            McSettings settings;
            settings.atlasSize = 512;
            settings.capturesPerFrame = 128;
            settings.keepUnusedPagesFrames = 3;
            MeshCardScene s(settings);
            std::mt19937 random(5821);
            for (uint32_t frame = 0; frame < 160; ++frame)
            {
                if (frame == 80) s.clear();
                for (uint32_t edit = 0; edit < 8; ++edit)
                {
                    const uint32_t instance = random() % 96;
                    s.removeInstance(instance);
                    if (random() % 4 != 0)
                        s.addInstance(instance, cube, translation({float(random() % 21) - 10, 0, float(random() % 21) - 10},
                                                                0.5f + float(random() % 8)), 1, instance % 7 == 0);
                }
                std::vector<McFeedback> feedback;
                if (!s.cardsGpu().empty())
                    for (uint32_t hit = 0; hit < 8; ++hit)
                        feedback.push_back({random() % static_cast<uint32_t>(s.cardsGpu().size()), 8, 0, 0, 64});
                s.setFeedback(feedback, 1024);
                const float3 eyes[2] = {{float(frame % 23), 1, 0}, {-8, 2, float(frame % 17)}};
                s.update(eyes);
                validateAndFingerprint(s);
                s.takeDirty();
            }
        }
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
            validateAndFingerprint(s);
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
            validateAndFingerprint(s);
            CHECK(s.captures().size() == 6);
            for (const McCapture& c : s.captures()) CHECK(c.atlasRect[2] == 8 && c.atlasRect[3] == 8 && c.resample);
            CHECK(s.stats().allocatedTexels == 6 * 64);
            // 30 m away: 1 texel, below the minimum of 4 -> hidden, nothing left in the atlas
            eye = { 30.5f, 0, 0 };
            s.update(std::span<const float3>(&eye, 1));
            validateAndFingerprint(s);
            CHECK(s.captures().empty() && s.stats().visibleCards == 0 && s.stats().mappedPages == 0);
            CHECK(s.stats().freePhysicalPages == 32 * 32);
            CHECK((s.cardsGpu()[0].packed & (1u << 16)) == 0);
            // and back
            eye = { 4, 0, 0 };
            s.update(std::span<const float3>(&eye, 1));
            validateAndFingerprint(s);
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
                validateAndFingerprint(s);
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
                validateAndFingerprint(s);
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
            validateAndFingerprint(s);
            const uint32_t pages = s.stats().mappedPages;
            CHECK(pages == 120 && s.stats().refreshed == 0);  // (every page was captured in this frame)
            // the resident pages come round: 37 a frame, each once before any twice
            std::vector<uint32_t> seen(s.pageCount(), 0);
            uint32_t refreshed = 0;
            for (uint32_t frame = 0; frame < 4; ++frame)
            {
                s.update(std::span<const float3>(&eye, 1));
                validateAndFingerprint(s);
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
            validateAndFingerprint(s);
            CHECK(s.stats().mappedPages == pages - 6);
            CHECK(s.addInstance(40, cube, translation({ 7 * 1.5f, 0, 0 })) != mc::kNone);
            s.update(std::span<const float3>(&eye, 1));
            validateAndFingerprint(s);
            CHECK(s.stats().mappedPages == pages && s.stats().cards == cards && s.stats().meshCards == sets);
            uint32_t ofForty = 0;
            for (const McCapture& c : s.captures()) ofForty += c.sceneInstance == 40 && !c.refresh ? 1u : 0u;
            CHECK(ofForty == 6);
        }
        {
            // feedback: 40 m from a 40 m wall its large faces are resident at 64 texels (level 6, one element). A page of
            // level 9 (4 x 4 pages) the hits ask for is mapped and captured with the card's lighting carried over; the
            // level's other pages are sent to the resident page; the page stays while hits ask and leaves 4 feedback
            // frames after they stop.
            const scene::MeshCards wall = scene::buildMeshCards(boxMesh({ -20, -20, -0.1f }, { 20, 20, 0.1f }), std::vector<uint8_t>{});
            McSettings quiet;
            quiet.refreshFraction = 0;
            quiet.keepUnusedPagesFrames = 4;
            MeshCardScene s(quiet);
            s.addInstance(0, wall, translation({ 0, 0, 0 }));
            const float3 eye{ 0, 0, 40 };
            uint32_t frames = 0;
            do
            {
                s.update(std::span<const float3>(&eye, 1));
            } while (!s.captures().empty() && ++frames < 20);
            validateAndFingerprint(s);
            uint32_t face = mc::kNone;
            for (uint32_t i = 0; i < s.cardsGpu().size() && face == mc::kNone; ++i)
                if (s.cardsGpu()[i].extent[0] > 19 && s.cardsGpu()[i].extent[1] > 19 && (s.cardsGpu()[i].packed & (1u << 16)) != 0) face = i;
            CHECK(face != mc::kNone);
            if (face != mc::kNone)
            {
                const uint32_t residentPages = s.stats().mappedPages, resident = s.cardsGpu()[face].pageTableOffset;
                CHECK(s.cardsGpu()[face].sizeInPages == (1u | 1u << 16) && s.cardsGpu()[face].hiResPageTableOffset == resident);
                McFeedback f;
                f.card = face;
                f.resLevel = 9;
                f.pageX = 1;
                f.pageY = 2;
                // no more hits than the minimum: nothing is mapped
                f.hits = quiet.feedbackMinPageHits;
                s.setFeedback(std::span<const McFeedback>(&f, 1), 8100);
                s.update(std::span<const float3>(&eye, 1));
                CHECK(s.captures().empty() && s.stats().hiResPages == 0);
                f.hits = 100;
                s.setFeedback(std::span<const McFeedback>(&f, 1), 8100);
                s.update(std::span<const float3>(&eye, 1));
                validateAndFingerprint(s);
                CHECK(s.stats().hiResRequests == 1 && s.stats().hiResMapped == 1 && s.stats().hiResPages == 1 && s.captures().size() == 1);
                if (!s.captures().empty())
                    CHECK(s.captures()[0].resample && !s.captures()[0].refresh && s.captures()[0].atlasRect[2] == 128 && s.captures()[0].card == face);
                CHECK(s.stats().mappedPages == residentPages + 1 && s.stats().pending == 0);
                CHECK(s.cardsGpu()[face].hiResSizeInPages == (4u | 4u << 16) && s.cardsGpu()[face].pageTableOffset == resident);
                const uint32_t high = s.cardsGpu()[face].hiResPageTableOffset;
                CHECK(high != resident);
                CHECK(s.pageTableGpu()[high + 1 + 2 * 4].cardPage == high + 1 + 2 * 4);  // the page itself
                CHECK(s.pageTableGpu()[high].cardPage == resident);                      // another page of the level
                CHECK(((s.pageTableGpu()[high].packed >> 24) & 0xF) == 6);
                // while the hits keep asking the page stays, and nothing is captured again
                for (int i = 0; i < 8; ++i)
                {
                    s.setFeedback(std::span<const McFeedback>(&f, 1), 8100);
                    s.update(std::span<const float3>(&eye, 1));
                    CHECK(s.captures().empty() && s.stats().hiResPages == 1 && s.stats().hiResRequests == 0);
                }
                // no hit asks any more
                for (int i = 0; i < 5; ++i)
                {
                    s.setFeedback(std::span<const McFeedback>(), 8100);
                    s.update(std::span<const float3>(&eye, 1));
                    validateAndFingerprint(s);
                }
                CHECK(s.stats().hiResPages == 0 && s.stats().mappedPages == residentPages);
                CHECK(s.cardsGpu()[face].hiResPageTableOffset == s.cardsGpu()[face].pageTableOffset);
                // the resident level rising past the feedback's level takes the feedback pages with it
                s.setFeedback(std::span<const McFeedback>(&f, 1), 8100);
                s.update(std::span<const float3>(&eye, 1));
                CHECK(s.stats().hiResPages == 1);
                const float3 near{ 0, 0, 1.5f };
                for (int i = 0; i < 20; ++i)
                {
                    s.setFeedback(std::span<const McFeedback>(), 8100);
                    s.update(std::span<const float3>(&near, 1));
                    validateAndFingerprint(s);
                    if (s.captures().empty()) break;
                }
                CHECK(s.stats().hiResPages == 0 && s.stats().requests == 0);
            }
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
    // Recorded by this exact fixture against the original scan-from-zero
    // allocator before changing its search cursors (2026-10-04, MSVC Release).
    CHECK(g_checkedStates == 200 && g_stateHash == 0x57d42175954994beull);
    if (g_failures != 0)
    {
        std::fprintf(stderr, "FAIL %u checks\n", g_failures);
        return 1;
    }
    std::printf("PASS states=%u gpu_records_and_captures=%016llx\n", g_checkedStates, (unsigned long long)g_stateHash);
    return 0;
}
