// scene::Scene's weather (clouds, fog, fogVolumes) in the .unxscene file, CPU: a scene without weather writes the same
// bytes as a scene that never had the fields (no CLDS or FOGS block); a cloud layer, a fog and fog volumes round-trip
// exactly through their blocks, each alone and together; fog volumes without the height fog keep it disabled; validate
// refuses values outside the fields' ranges.
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

Scene plainScene()
{
    Scene s;
    s.name = "weather";
    Light l;
    l.position = { 1.0f, 2.0f, 3.0f };
    s.lights.push_back(l);
    return s;
}

bool contains(const std::vector<uint8_t>& bytes, const char* tag)
{
    for (size_t i = 0; i + 4 <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, tag, 4) == 0) return true;
    return false;
}

bool same(float3 a, float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

bool refused(const Scene& s)
{
    try
    {
        validate(s);
    }
    catch (const std::exception&)
    {
        return true;
    }
    return false;
}
} // namespace

int main()
{
    try
    {
        // no weather: no block
        const Scene plain = plainScene();
        const std::vector<uint8_t> plainBytes = serialize(plain);
        CHECK(!contains(plainBytes, "CLDS"));
        CHECK(!contains(plainBytes, "FOGS"));
        const Scene plainBack = deserialize(plainBytes);
        CHECK(plainBack.clouds.coverage == 0 && !plainBack.fog.enabled && plainBack.fogVolumes.empty());
        validate(plain);

        // a cloud layer alone
        Scene cloudy = plainScene();
        cloudy.clouds.coverage = 0.5f;
        cloudy.clouds.baseAltitude = 700.0f;
        cloudy.clouds.topAltitude = 2600.0f;
        cloudy.clouds.sigmaMax = 0.03f;
        cloudy.clouds.albedo = 0.97f;
        cloudy.clouds.windX = 4.0f;
        cloudy.clouds.windZ = -2.5f;
        validate(cloudy);
        const std::vector<uint8_t> cloudyBytes = serialize(cloudy);
        CHECK(contains(cloudyBytes, "CLDS") && !contains(cloudyBytes, "FOGS"));
        CHECK(cloudyBytes.size() == plainBytes.size() + 4 + 7 * 4);  // tag, 7 floats
        const Scene cloudyBack = deserialize(cloudyBytes);
        CHECK(cloudyBack.clouds.coverage == 0.5f && cloudyBack.clouds.baseAltitude == 700.0f && cloudyBack.clouds.topAltitude == 2600.0f);
        CHECK(cloudyBack.clouds.sigmaMax == 0.03f && cloudyBack.clouds.albedo == 0.97f && cloudyBack.clouds.windX == 4.0f && cloudyBack.clouds.windZ == -2.5f);
        CHECK(!cloudyBack.fog.enabled && cloudyBack.fogVolumes.empty());
        CHECK(serialize(cloudyBack) == cloudyBytes);
        CHECK(contentHash(plain) != contentHash(cloudy));

        // fog volumes alone: the block, the height fog stays disabled
        Scene misty = plainScene();
        FogVolume box;
        box.centre = { -38.0f, 2.5f, 80.0f };
        box.halfSize = { 8.0f, 2.5f, 30.0f };
        box.yaw = 0.3f;
        box.shape = 1;
        box.density = 0.06f;
        box.heightFalloff = 2.0f;
        box.edge = 0.4f;
        box.albedo = { 0.9f, 0.8f, 0.7f };
        misty.fogVolumes.push_back(box);
        misty.fogVolumes.push_back(FogVolume{});
        validate(misty);
        const std::vector<uint8_t> mistyBytes = serialize(misty);
        CHECK(contains(mistyBytes, "FOGS") && !contains(mistyBytes, "CLDS"));
        CHECK(mistyBytes.size() == plainBytes.size() + 4 + 4 + 11 * 4 + 8 + 2 * (13 * 4 + 4));  // tag, enabled, 11 floats, count, 2 volumes
        const Scene mistyBack = deserialize(mistyBytes);
        CHECK(!mistyBack.fog.enabled && mistyBack.fogVolumes.size() == 2);
        if (mistyBack.fogVolumes.size() == 2)
        {
            const FogVolume& v = mistyBack.fogVolumes[0];
            CHECK(same(v.centre, box.centre) && same(v.halfSize, box.halfSize) && same(v.albedo, box.albedo));
            CHECK(v.yaw == 0.3f && v.shape == 1 && v.density == 0.06f && v.heightFalloff == 2.0f && v.edge == 0.4f);
            const FogVolume& d = mistyBack.fogVolumes[1];
            CHECK(d.shape == 0 && d.density == FogVolume{}.density && d.edge == FogVolume{}.edge);
        }
        CHECK(serialize(mistyBack) == mistyBytes);
        CHECK(!contains(mistyBytes, "FVST"));

        // steam values on one volume: the FVST block names it, the other keeps its defaults
        Scene steamy = misty;
        steamy.fogVolumes[1].sourcePlane = 0.25f;
        steamy.fogVolumes[1].riseSpeed = 0.4f;
        steamy.fogVolumes[1].turbulence = 0.8f;
        steamy.fogVolumes[1].turbulenceScale = 0.3f;
        validate(steamy);
        const std::vector<uint8_t> steamyBytes = serialize(steamy);
        CHECK(contains(steamyBytes, "FVST"));
        CHECK(steamyBytes.size() == mistyBytes.size() + 4 + 8 + (4 + 4 * 4));  // tag, count, index + 4 floats
        const Scene steamyBack = deserialize(steamyBytes);
        CHECK(steamyBack.fogVolumes.size() == 2);
        if (steamyBack.fogVolumes.size() == 2)
        {
            CHECK(steamyBack.fogVolumes[0].riseSpeed == 0 && steamyBack.fogVolumes[0].turbulence == 0 && steamyBack.fogVolumes[0].sourcePlane == 0);
            const FogVolume& v = steamyBack.fogVolumes[1];
            CHECK(v.sourcePlane == 0.25f && v.riseSpeed == 0.4f && v.turbulence == 0.8f && v.turbulenceScale == 0.3f);
        }
        CHECK(serialize(steamyBack) == steamyBytes);
        Scene badSteam = steamy;
        badSteam.fogVolumes[1].turbulence = 1.5f;
        CHECK(refused(badSteam));

        // the height fog and the clouds together: both blocks, clouds first
        Scene both = cloudy;
        both.fog.enabled = true;
        both.fog.density = 0.004f;
        both.fog.heightFalloff = 0.03f;
        both.fog.height = 12.0f;
        both.fog.albedo = { 0.6f, 0.8f, 1.0f };
        both.fog.phaseG = 0.6f;
        both.fog.startDistance = 3.0f;
        both.fog.skyAmount = 0.5f;
        both.fog.noiseAmount = 0.2f;
        both.fog.noiseScale = 35.0f;
        validate(both);
        const std::vector<uint8_t> bothBytes = serialize(both);
        CHECK(contains(bothBytes, "CLDS") && contains(bothBytes, "FOGS"));
        CHECK(bothBytes.size() == cloudyBytes.size() + 4 + 4 + 11 * 4 + 8);
        const Scene bothBack = deserialize(bothBytes);
        CHECK(bothBack.clouds.coverage == 0.5f && bothBack.fog.enabled && bothBack.fogVolumes.empty());
        CHECK(bothBack.fog.density == 0.004f && bothBack.fog.heightFalloff == 0.03f && bothBack.fog.height == 12.0f && same(bothBack.fog.albedo, both.fog.albedo));
        CHECK(bothBack.fog.phaseG == 0.6f && bothBack.fog.startDistance == 3.0f && bothBack.fog.skyAmount == 0.5f && bothBack.fog.noiseAmount == 0.2f &&
              bothBack.fog.noiseScale == 35.0f);
        CHECK(serialize(bothBack) == bothBytes);
        CHECK(!contains(bothBytes, "FGL2"));

        // the fog's second layer: its own block after FOGS
        Scene layered = both;
        layered.fog.density2 = 0.02f;
        layered.fog.heightFalloff2 = 0.5f;
        layered.fog.height2 = -3.0f;
        validate(layered);
        const std::vector<uint8_t> layeredBytes = serialize(layered);
        CHECK(contains(layeredBytes, "FGL2"));
        CHECK(layeredBytes.size() == bothBytes.size() + 4 + 3 * 4);
        const Scene layeredBack = deserialize(layeredBytes);
        CHECK(layeredBack.fog.density2 == 0.02f && layeredBack.fog.heightFalloff2 == 0.5f && layeredBack.fog.height2 == -3.0f);
        CHECK(layeredBack.fog.density == 0.004f && layeredBack.clouds.coverage == 0.5f);
        CHECK(serialize(layeredBack) == layeredBytes);

        // values outside the fields' ranges are refused
        Scene bad = cloudy;
        bad.clouds.topAltitude = bad.clouds.baseAltitude;
        CHECK(refused(bad));
        bad = cloudy;
        bad.clouds.coverage = 1.5f;
        CHECK(refused(bad));
        bad = both;
        bad.fog.phaseG = 1.0f;
        CHECK(refused(bad));
        bad = misty;
        bad.fogVolumes[0].halfSize.y = 0.0f;
        CHECK(refused(bad));
        bad = misty;
        bad.fogVolumes[1].shape = 2;
        CHECK(refused(bad));
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "exception: %s\n", e.what());
        return 1;
    }
    if (g_failures) std::fprintf(stderr, "%u failures\n", g_failures);
    else std::printf("weather blocks: ok\n");
    return g_failures ? 1 : 0;
}
