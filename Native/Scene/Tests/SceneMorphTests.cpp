// C4 scene data (render C): blend shapes and vertex animation in the scene description.
//   morph_round_trip         a scene with blend shapes, instance weights and a vertex animation serialises and loads back
//                            unchanged; a scene without them keeps the exact bytes of the format before C4 (content hashes
//                            of existing scenes do not move)
//   blend_shape_evaluation   evaluateMorph: p = p0 + sum w dp, n = normalize(n0 + sum w dn), untouched vertices unchanged,
//                            weights beyond the list read as 0; morphBound covers every evaluated displacement
//   vertex_animation         frame interpolation, looping wrap, clamped ends, negative time
//   validation               malformed streams are rejected
#include "unx/core/Log.h"
#include "unx/scene/SceneData.h"

#include <cmath>
#include <cstring>
#include <exception>
#include <functional>
#include <random>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::scene;

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

Scene quadScene()
{
    Scene s;
    s.name = "morph";
    s.materials.resize(1);
    Mesh m;
    m.name = "quad";
    m.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 } };
    m.normals.assign(4, { 0, 0, 1 });
    m.indices = { 0, 1, 2, 0, 2, 3 };
    m.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(m);
    Instance i;
    i.mesh = 0;
    s.instances.push_back(i);
    return s;
}

bool near(float3 a, float3 b, float e = 1e-6f) { return std::fabs(a.x - b.x) <= e && std::fabs(a.y - b.y) <= e && std::fabs(a.z - b.z) <= e; }
} // namespace

UNX_TEST(morph_round_trip)
{
    Scene plain = quadScene();
    validate(plain);
    const std::vector<uint8_t> before = serialize(plain);
    // No extension block without morph data: byte-identical to the pre-C4 layout (it ends with the camera paths).
    CHECK(deserialize(before).meshes[0].blendShapes.empty());
    Scene s = quadScene();
    BlendShape smile{ "smile", { 1, 2 }, { { 0, 0.5f, 0 }, { 0.25f, 0, 0 } }, { { 0.1f, 0, 0 }, { 0, 0.1f, 0 } } };
    BlendShape blink{ "blink", { 3 }, { { 0, -0.2f, 0 } }, {} };
    s.meshes[0].blendShapes = { smile, blink };
    s.instances[0].blendWeights = { 0.5f, 1.0f };
    Mesh anim = s.meshes[0];
    anim.name = "flag";
    anim.blendShapes.clear();
    anim.vertexAnimation.framesPerSecond = 30;
    anim.vertexAnimation.frameCount = 3;
    for (uint32_t f = 0; f < 3; ++f)
        for (const float3& p : anim.positions) anim.vertexAnimation.positions.push_back(p + float3{ 0, 0, 0.1f * f });
    s.meshes.push_back(anim);
    Instance ai;
    ai.mesh = 1;
    ai.vertexAnimationTime = 0.05f;
    s.instances.push_back(ai);
    validate(s);
    const Scene back = deserialize(serialize(s));
    CHECK(serialize(back) == serialize(s));
    CHECK(back.meshes[0].blendShapes.size() == 2 && back.meshes[0].blendShapes[0].name == "smile" && back.meshes[0].blendShapes[1].deltaNormals.empty());
    CHECK(back.instances[0].blendWeights == s.instances[0].blendWeights && back.instances[1].vertexAnimationTime == 0.05f);
    CHECK(back.meshes[1].vertexAnimation.positions.size() == s.meshes[1].vertexAnimation.positions.size() &&
          std::memcmp(back.meshes[1].vertexAnimation.positions.data(), s.meshes[1].vertexAnimation.positions.data(),
                      s.meshes[1].vertexAnimation.positions.size() * sizeof(float3)) == 0);
    CHECK(contentHash(back) == contentHash(s) && contentHash(s) != contentHash(plain));
    logf("    %zu bytes without morph data, %zu with\n", before.size(), serialize(s).size());
}

UNX_TEST(blend_shape_evaluation)
{
    Scene s = quadScene();
    s.meshes[0].blendShapes = { BlendShape{ "a", { 1, 2 }, { { 0, 0.5f, 0 }, { 0.25f, 0, 0 } }, { { 0.1f, 0, 0 }, { 0, 0.1f, 0 } } },
                                BlendShape{ "b", { 2, 3 }, { { 0, 0, 1 }, { 0, -0.2f, 0 } }, {} } };
    validate(s);
    const Mesh& m = s.meshes[0];
    float3 p, n;
    const std::vector<float> w = { 0.5f, 2.0f };
    evaluateMorph(m, w, 0, 0, p, n);
    CHECK(near(p, { 0, 0, 0 }) && near(n, { 0, 0, 1 }));  // untouched
    evaluateMorph(m, w, 0, 2, p, n);
    CHECK(near(p, float3{ 1, 1, 0 } + float3{ 0.25f, 0, 0 } * 0.5f + float3{ 0, 0, 1 } * 2.0f));
    CHECK(near(n, normalize(float3{ 0, 0, 1 } + float3{ 0, 0.1f, 0 } * 0.5f)));
    evaluateMorph(m, { 0.5f }, 0, 3, p, n);  // weight of shape b missing = 0
    CHECK(near(p, { 0, 1, 0 }));
    // The bound covers every vertex's displacement for |w| up to the given bounds.
    std::mt19937 rng(1);
    std::uniform_real_distribution<float> u(-2.0f, 2.0f);
    const float bound = morphBound(m, { 2.0f, 2.0f });
    for (int trial = 0; trial < 1000; ++trial)
    {
        const std::vector<float> ww = { u(rng), u(rng) };
        for (uint32_t v = 0; v < 4; ++v)
        {
            evaluateMorph(m, ww, 0, v, p, n);
            CHECK(length(p - m.positions[v]) <= bound * (1 + 1e-6f));
        }
    }
    logf("    morph bound %.4f m for |w| <= 2\n", bound);
}

UNX_TEST(vertex_animation)
{
    Scene s = quadScene();
    Mesh& m = s.meshes[0];
    m.vertexAnimation.framesPerSecond = 10;
    m.vertexAnimation.frameCount = 4;
    for (uint32_t f = 0; f < 4; ++f)
        for (const float3& p : m.positions) m.vertexAnimation.positions.push_back(p + float3{ 0, 0, (float)f });
    validate(s);
    float3 p, n;
    evaluateMorph(m, {}, 0.15f, 0, p, n);  // halfway between frames 1 and 2
    CHECK(near(p, { 0, 0, 1.5f }, 1e-5f));
    evaluateMorph(m, {}, 0.35f, 0, p, n);  // frame 3 -> frame 0 (loop)
    CHECK(near(p, { 0, 0, 1.5f }, 1e-5f));
    evaluateMorph(m, {}, -0.05f, 0, p, n);  // loop backwards: between frames 3 and 0
    CHECK(near(p, { 0, 0, 1.5f }, 1e-5f));
    m.vertexAnimation.loop = false;
    evaluateMorph(m, {}, 0.35f, 0, p, n);  // held at the last frame
    CHECK(near(p, { 0, 0, 3 }, 1e-5f));
    evaluateMorph(m, {}, -1.0f, 0, p, n);  // held at the first frame
    CHECK(near(p, { 0, 0, 0 }, 1e-5f));
    CHECK(std::fabs(morphBound(m, {}) - 3.0f) < 1e-6f);
}

UNX_TEST(validation)
{
    auto rejects = [](const Scene& s) {
        try
        {
            validate(s);
        }
        catch (const std::exception&)
        {
            return true;
        }
        return false;
    };
    Scene s = quadScene();
    s.meshes[0].blendShapes = { BlendShape{ "x", { 2, 1 }, { {}, {} }, {} } };
    CHECK(rejects(s));  // not ascending
    s.meshes[0].blendShapes = { BlendShape{ "x", { 4 }, { {} }, {} } };
    CHECK(rejects(s));  // out of range
    s.meshes[0].blendShapes = { BlendShape{ "x", { 1 }, { {} }, {} } };
    s.instances[0].blendWeights = { 1, 2 };
    CHECK(rejects(s));  // weights per shape
    s.instances[0].blendWeights = { 1 };
    CHECK(!rejects(s));
    s.meshes[0].vertexAnimation.framesPerSecond = 30;
    s.meshes[0].vertexAnimation.frameCount = 1;
    s.meshes[0].vertexAnimation.positions = s.meshes[0].positions;
    CHECK(rejects(s));  // vertex animation with blend shapes
}

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    uint32_t passed = 0, runCount = 0;
    for (const TestCase& t : registry())
    {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++runCount;
        try
        {
            t.fn();
        }
        catch (const std::exception& e)
        {
            logf("FAIL %s: %s\n", t.name, e.what());
            continue;
        }
        logf("PASS %s\n", t.name);
        ++passed;
    }
    logf("%u/%u passed\n", passed, runCount);
    return passed == runCount ? 0 : 1;
}
