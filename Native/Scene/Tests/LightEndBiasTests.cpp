// scene::Light::rayEndBias (INTERFACES v1.93) in the .unxscene file, CPU: lights without a value write the same bytes
// as a scene that never had the field (no LEND block); lights with a value round-trip exactly through the block, the
// others keep "none"; a block naming a light the scene does not have is refused.
#include "unx/core/Log.h"
#include "unx/scene/SceneData.h"

#include <cstdio>
#include <cstring>
#include <exception>
#include <vector>

using namespace unx;
using namespace unx::scene;

namespace
{
uint32_t g_failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)

Scene threeLights()
{
    Scene s;
    s.name = "light end bias";
    for (int i = 0; i < 3; ++i)
    {
        Light l;
        l.type = i == 1 ? LightType::Rect : LightType::Point;
        l.position = { (float)i, 2.0f, 0.0f };
        l.size = { 4.9f, 0.04f };
        l.castShadow = true;
        s.lights.push_back(l);
    }
    return s;
}

bool contains(const std::vector<uint8_t>& bytes, const char* tag)
{
    for (size_t i = 0; i + 4 <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, tag, 4) == 0) return true;
    return false;
}
} // namespace

int main()
{
    try
    {
        // no light with a value: no block, and the default is "none"
        const Scene plain = threeLights();
        const std::vector<uint8_t> plainBytes = serialize(plain);
        CHECK(!contains(plainBytes, "LEND"));
        const Scene plainBack = deserialize(plainBytes);
        CHECK(plainBack.lights.size() == 3);
        for (const Light& l : plainBack.lights) CHECK(l.rayEndBias < 0);

        // one light with a value (0 is a value): the block, exact round trip, the others stay "none"
        Scene biased = threeLights();
        biased.lights[1].rayEndBias = 0.4f;
        biased.lights[2].rayEndBias = 0.0f;
        const std::vector<uint8_t> biasedBytes = serialize(biased);
        CHECK(contains(biasedBytes, "LEND"));
        CHECK(biasedBytes.size() == plainBytes.size() + 4 + 8 + 2 * 8);  // tag, count, 2 x { index, float }
        const Scene back = deserialize(biasedBytes);
        CHECK(back.lights.size() == 3);
        CHECK(back.lights[0].rayEndBias < 0);
        CHECK(back.lights[1].rayEndBias == 0.4f);
        CHECK(back.lights[2].rayEndBias == 0.0f);
        CHECK(serialize(back) == biasedBytes);
        CHECK(contentHash(plain) != contentHash(biased));

        // a block naming a light the scene does not have is refused
        std::vector<uint8_t> broken = biasedBytes;
        const uint32_t bad = 7;
        std::memcpy(broken.data() + plainBytes.size() + 4 + 8, &bad, 4);
        bool refused = false;
        try
        {
            deserialize(broken);
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        CHECK(refused);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    std::printf(g_failures ? "LIGHT END BIAS TESTS FAILED (%u)\n" : "LIGHT END BIAS TESTS PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
