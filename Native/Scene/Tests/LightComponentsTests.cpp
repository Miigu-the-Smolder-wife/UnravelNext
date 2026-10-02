// scene::Light's light components (scales, source texture, barn doors, lighting channels, draw distance, colour
// temperature, falloff exponent) in the .unxscene file and their CPU helpers:
//   - lights that set none write the same bytes as a scene that never had the fields (no LCMP block);
//   - a light that sets one round-trips every value exactly through the block, the others keep the defaults; the block
//     follows the ray end bias block; a block naming a light the scene does not have is refused;
//   - an instance's lighting channels ride in its flags (default: channel 0) and round-trip with them;
//   - colorTemperatureTint: luminance 1, red over blue below 6,500 K and blue over red above it; lightColor keeps the
//     colour's luminance.
#include "unx/core/Log.h"
#include "unx/scene/SceneData.h"

#include <cmath>
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
    s.name = "light components";
    for (int i = 0; i < 3; ++i)
    {
        Light l;
        l.type = i == 1 ? LightType::Rect : LightType::Point;
        l.position = { (float)i, 2.0f, 0.0f };
        l.size = { 1.2f, 0.6f };
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
float luminance(float3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }
} // namespace

int main()
{
    try
    {
        const Scene plain = threeLights();
        const std::vector<uint8_t> plainBytes = serialize(plain);
        CHECK(!contains(plainBytes, "LCMP"));
        for (const Light& l : deserialize(plainBytes).lights) CHECK(!hasLightComponents(l));

        Scene set = threeLights();
        Light& a = set.lights[1];
        a.specularScale = 0.0f;
        a.diffuseScale = 0.5f;
        a.volumetricScattering = 2.0f;
        a.indirectIntensity = 0.25f;
        a.barnDoorAngle = 0.7f;
        a.barnDoorLength = 0.2f;
        a.lightingChannels = 6;
        a.maxDrawDistance = 40.0f;
        a.maxDistanceFadeRange = 5.0f;
        a.temperature = 3200.0f;
        set.lights[2].falloffExponent = 8.0f;
        set.lights[2].rayEndBias = 0.1f;  // (the LEND block comes first)
        const std::vector<uint8_t> setBytes = serialize(set);
        CHECK(contains(setBytes, "LCMP"));
        CHECK(contains(setBytes, "LEND"));
        const Scene back = deserialize(setBytes);
        CHECK(back.lights.size() == 3);
        CHECK(!hasLightComponents(back.lights[0]));
        const Light& b = back.lights[1];
        CHECK(b.specularScale == 0.0f && b.diffuseScale == 0.5f && b.volumetricScattering == 2.0f && b.indirectIntensity == 0.25f);
        CHECK(b.barnDoorAngle == 0.7f && b.barnDoorLength == 0.2f && b.lightingChannels == 6);
        CHECK(b.maxDrawDistance == 40.0f && b.maxDistanceFadeRange == 5.0f && b.temperature == 3200.0f && b.sourceTexture == kNone);
        CHECK(back.lights[2].falloffExponent == 8.0f && back.lights[2].rayEndBias == 0.1f);
        CHECK(serialize(back) == setBytes);
        CHECK(contentHash(plain) != contentHash(set));

        // a block naming a light the scene does not have is refused
        Scene one = threeLights();
        one.lights[0].diffuseScale = 0.5f;
        std::vector<uint8_t> broken = serialize(one);
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

        // an instance's channels in its flags
        CHECK(instanceLightingChannels(InstanceCastShadow) == 1);
        const uint32_t flags = withLightingChannels(InstanceCastShadow | InstanceDynamic, 6);
        CHECK(instanceLightingChannels(flags) == 6 && (flags & (InstanceCastShadow | InstanceDynamic)) == (InstanceCastShadow | InstanceDynamic));
        CHECK(withLightingChannels(flags, 1) == (InstanceCastShadow | InstanceDynamic));
        CHECK(instanceLightingChannels(withLightingChannels(0, 0)) == 0);

        // the colour temperature's tint
        for (float kelvin : { 1500.0f, 3200.0f, 6500.0f, 12000.0f })
        {
            const float3 t = colorTemperatureTint(kelvin);
            CHECK(std::fabs(luminance(t) - 1.0f) < 1e-4f);
            CHECK(t.x >= 0 && t.y >= 0 && t.z >= 0);
            if (kelvin < 6000.0f) CHECK(t.x > t.z);
            if (kelvin > 7000.0f) CHECK(t.z > t.x);
        }
        const float3 white = colorTemperatureTint(6500.0f);
        CHECK(std::fabs(white.x - 1.0f) < 0.1f && std::fabs(white.y - 1.0f) < 0.1f && std::fabs(white.z - 1.0f) < 0.1f);
        Light warm;
        warm.color = { 0.9f, 1.0f, 1.1f };
        CHECK(lightColor(warm).x == warm.color.x);  // (no temperature: the colour itself)
        warm.temperature = 2700.0f;
        CHECK(std::fabs(luminance(lightColor(warm)) - luminance(warm.color)) < 1e-4f);
        CHECK(lightColor(warm).x > lightColor(warm).z);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
    std::printf(g_failures ? "LIGHT COMPONENTS TESTS FAILED (%u)\n" : "LIGHT COMPONENTS TESTS PASSED\n", g_failures);
    return g_failures ? 1 : 0;
}
