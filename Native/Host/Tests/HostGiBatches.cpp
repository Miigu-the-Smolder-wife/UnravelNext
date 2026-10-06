// Compare the actual GI/lighting/post chain with exact continuations on/off.
// A moving occluder, animated local light, sun edit and cut exercise history.
// At 1024x768/tile 8 the trace atlas exceeds one bounded continuation batch.
#include "Renderer/HostRenderer.h"
#include "TestScenes.h"
#include "unx/core/File.h"
#include "unx/lights/LightFunctions.h"
#include "unx/refl/ReflectionSystem.h"
#include "../../Render/Passes/Shadow/FroxelSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t W = 1024, H = 768;
bool reflectionCase = false;
bool probeCacheCase = false;
bool probeCacheMonolithic = false;
constexpr std::array<uint32_t, 10> captures{15, 16, 19, 27, 28, 31, 39, 40, 43, 51};
using Images = std::array<std::vector<uint32_t>, captures.size()>;

scene::Scene room()
{
    auto s = test::oneBox();
    s.name = "GI bounded continuation history";
    s.materials[0].baseColor = {0.65f, 0.55f, 0.45f};
    if (reflectionCase) s.materials[0].roughness = 0.08f;
    for (auto& position : s.meshes[0].positions) position.y *= 2;
    s.instances[0].transform.m[1][3] = 1;
    auto box = [&](float3 centre, float3 size) {
        auto mesh = test::oneBox().meshes[0];
        for (auto& p : mesh.positions) { p.x *= size.x; p.y *= size.y; p.z *= size.z; }
        scene::Instance i; i.mesh = uint32_t(s.meshes.size()); s.meshes.push_back(std::move(mesh));
        i.transform.m[0][3] = centre.x; i.transform.m[1][3] = centre.y; i.transform.m[2][3] = centre.z;
        s.instances.push_back(i);
    };
    box({0, -0.1f, 0}, {12, 0.2f, 12});
    box({0, 3, -3}, {12, 6, 0.2f});
    box({-5, 3, 0}, {0.2f, 6, 6});
    box({5, 3, 0}, {0.2f, 6, 6});
    s.sun.direction = normalize(float3{0.25f, 0.8f, 0.4f});
    s.sun.illuminance = 1000;
    scene::Light light; light.position = {-1.4f, 2.8f, 1}; light.intensity = 3000; light.range = 12; light.castShadow = true;
    s.lights.push_back(light);
    s.cameras[0].position = {0, 2.5f, 5};
    s.cameras[0].forward = normalize(float3{0, -0.12f, -1});
    s.cameras[0].ev100 = 8;
    return s;
}

Images run(bool deferred, const std::vector<std::string>& preset)
{
    if (probeCacheCase) SetEnvironmentVariableA("UNX_GI_PROBE_CACHE_MODE", deferred ? "3" : "0");
    HostRendererOptions o;
    o.debugLayer = true;
    o.shaderDirectory = executableDirectory() / "shaders";
    o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
    o.qualityOverrides = preset;
    o.qualityOverrides.push_back("gi.lumen_tile=8");
    o.qualityOverrides.push_back(!probeCacheMonolithic && (deferred || reflectionCase || probeCacheCase) ? "gi.defer_hit_lighting=true" : "gi.defer_hit_lighting=false");
    HostRenderer h(o);
    h.scene() = room(); h.commit();
    if (reflectionCase) h.trackStateForTest().get<refl::ReflectionReuseDebug>("R.reuse.debug").forceUncachedRays = !deferred;
    if (reflectionCase)
    {
        HostRenderer::PoolInput pool{};
        pool.id = 7; pool.material = 0; pool.sizeX = 4; pool.sizeZ = 3; pool.depth = 0.6f; pool.surfaceFilm = 1;
        pool.centre[0] = 0; pool.centre[1] = 0.4; pool.centre[2] = 1;
        h.setPools({&pool, 1});
    }
    lights::LightFunction animated;
    animated.intensityKeys = {{0, 1}, {0.25f, 1}, {0.26666667f, 0.2f}, {0.6f, 0.2f}, {0.7f, 1}};
    lights::lightFunctions(h.trackStateForTest()).set(0, animated);
    Images images;
    size_t capture = 0;
    scene::Camera camera = h.scene().cameras[0];
    const uint32_t width = reflectionCase ? 1536 : W, height = reflectionCase ? 1024 : H;
    if (reflectionCase) camera.forward = normalize(float3{0, -0.4f, -1});
    for (uint32_t f = 0; f <= captures.back(); ++f)
    {
        if (f == 16) { auto sun = h.scene().sun; sun.direction = normalize(float3{-0.6f, 0.4f, 0.3f}); h.setSun(sun); }
        if (f == 28)
        {
            InstanceTransformUpdate move; move.instance = 0; move.objectToWorld = h.scene().instances[0].transform;
            move.objectToWorld.m[0][3] = 2.5f; h.setTransforms({&move, 1});
        }
        if (f == 40) { camera.position.x = -1.5f; camera.forward = normalize(float3{0.2f, -0.12f, -1}); h.setDiscontinuity(kDiscontinuityCut); }
        FramePacket p; p.frameIndex = f; p.time = double(f) / 60; p.deltaTime = 1.0f / 60;
        p.width = width; p.height = height; p.camera = camera;
        if (reflectionCase)
        {
            FramePacket::PoolSource ripple;
            ripple.pool = 7; ripple.source = PoolSourceFrame{0.3 * std::sin(0.5 * f), 1, 0.08f, 0.5f, 2e-4f};
            h.addPoolSources({&ripple, 1});
        }
        const bool take = capture < captures.size() && f == captures[capture];
        if (take) images[capture].resize(size_t(width) * height);
        h.renderStandalone(h.queueFrame(std::move(p)), take ? images[capture].data() : nullptr, take ? images[capture].size() * 4 : 0);
        if (reflectionCase && h.trackStateForTest().get<refl::ReflectionReuseDebug>("R.reuse.debug").usedCachedRays != deferred)
            fail("Reflection comparison did not exercise the requested ray-cache path");
        const auto& lists = shadow::froxelStats(h.trackStateForTest());
        if (lists.overflowLists || lists.droppedLights || lists.needed > lists.sceneBound)
            fail("GI comparison lost scene-light entries");
        if (take) ++capture;
    }
    if (h.debugErrors()) fail("GI batch integration debug errors");
    return images;
}
}

int main(int argc, char** argv)
{
    try
    {
        std::vector<std::string> preset;
        if (argc > 1)
        {
            std::ifstream in(argv[1]); if (!in) fail("preset override file is missing");
            for (std::string line; std::getline(in, line);)
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) preset.push_back(line);
            }
        }
        const bool self = argc > 2 && std::string(argv[2]) == "--self";
        reflectionCase = argc > 2 && std::string(argv[2]) == "--reflection";
        probeCacheMonolithic = argc > 2 && std::string(argv[2]) == "--probe-cache-monolithic";
        probeCacheCase = probeCacheMonolithic || (argc > 2 && std::string(argv[2]) == "--probe-cache");
        if (probeCacheCase) logf("Probe cache case: original replay twice versus near-first + prepared lookup, same GI continuations and quality\n");
        const uint32_t width = reflectionCase ? 1536 : W, height = reflectionCase ? 1024 : H;
        if (reflectionCase) logf("Reflection case: 1536x1024, glossy room and moving pool surface; original replay twice versus cached rays, same GI path\n");
        const auto original = run(false, preset), control = run(false, preset);
        const auto bounded = self ? control : run(true, preset);
        if (self) logf("Control comparison: monolithic versus monolithic\n");
        uint32_t failures = 0;
        for (size_t frame = 0; frame < captures.size(); ++frame)
        {
            uint64_t sum = 0, overTwo = 0; uint32_t worst = 0;
            for (size_t i = 0; i < original[frame].size(); ++i)
                for (uint32_t c = 0; c < 3; ++c)
                {
                    const uint32_t a = (original[frame][i] >> (10 * c)) & 1023, b = (bounded[frame][i] >> (10 * c)) & 1023;
                    const uint32_t d = a > b ? a - b : b - a;
                    sum += d; overTwo += d > 2; worst = std::max(worst, d);
                }
            const double count = double(width) * height * 3, mean = sum / count, fraction = overTwo / count;
            // Independent runs reorder adaptive/cache work even with the same
            // path. Compare spatial energy over 8x8 tiles against that measured
            // control, allowing at most one extra 8-bit display code. The frame
            // bias must itself stay within one display code (no energy drift).
            std::vector<double> tileErrors, controlErrors;
            double signedSum = 0;
            for (uint32_t y = 0; y < height; y += 8)
                for (uint32_t x = 0; x < width; x += 8)
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        int delta = 0, controlDelta = 0;
                        for (uint32_t dy = 0; dy < 8; ++dy) for (uint32_t dx = 0; dx < 8; ++dx)
                        {
                            const size_t i = size_t(y + dy) * width + x + dx;
                            delta += int((bounded[frame][i] >> (10 * c)) & 1023) - int((original[frame][i] >> (10 * c)) & 1023);
                            controlDelta += int((control[frame][i] >> (10 * c)) & 1023) - int((original[frame][i] >> (10 * c)) & 1023);
                        }
                        tileErrors.push_back(std::abs(delta / 64.0)); signedSum += delta;
                        controlErrors.push_back(std::abs(controlDelta / 64.0));
                    }
            std::sort(tileErrors.begin(), tileErrors.end());
            std::sort(controlErrors.begin(), controlErrors.end());
            const double p95 = tileErrors[size_t(tileErrors.size() * 0.95)], bias = signedSum / count;
            const double controlP95 = controlErrors[size_t(controlErrors.size() * 0.95)];
            logf("GI batches frame %u: pixel mean %.6f codes, >2 %.6f%%, worst %u; tile P95 %.6f, control P95 %.6f, signed bias %.6f\n",
                captures[frame], mean, fraction * 100, worst, p95, controlP95, bias);
            if (p95 > controlP95 + 1023.0 / 255 || std::abs(bias) > 1023.0 / 255) ++failures;
        }
        // The edits must actually change the rendered signal, not compare two
        // unchanging or empty frames. Compare each settled edit to its predecessor.
        for (const auto [a, b] : std::array<std::pair<size_t, size_t>, 3>{{{0, 3}, {3, 6}, {6, 9}}})
        {
            uint64_t changed = 0;
            for (size_t i = 0; i < original[a].size(); ++i) changed += original[a][i] != original[b][i];
            if (changed < 1000) fail("GI test edit produced no visible change");
            logf("GI history edit %zu -> %zu: %llu pixels changed\n", a, b, (unsigned long long)changed);
        }
        if (failures) fail("GI batch comparison failed on %u captures", failures);
        logf("PASS GI batches: actual lighting/post at tile 8, animated local light/sun, moving occluder, cut 1/4 and settled frames; tile P95 <= control + one 8-bit display code, frame bias <= one code; no lost lights; D3D12 errors 0\n");
        return 0;
    }
    catch (const std::exception& error) { logf("FAIL %s\n", error.what()); return 1; }
}
