// Lit particles under shading.mega_lights through the host (render A; the first run of FxLayerSetup's ML1 variant):
// the RPP stand-in stream with lit sprite programs (material 1) next to two local lights (one casting shadows), in a
// standalone HostRenderer with shading.mega_lights on. The particle setup then takes the local lights from the froxel
// grid's sampled fluence and moment volumes (MegaLightsVolume.hlsl, s.ml.volume) and not from its loop over the froxel
// list with S's shadow maps (S assigns none under mega_lights).
// Checks: no stream status, no D3D12 debug-layer errors, and the lit particles reach the image (pixels change against
// the frame before the first tick). --lights-off: the same run with the two lights at intensity 0; the particles' pixels
// must then differ from the lit run's (the sampled volumes are what lights them at night).
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness). --out DIR: the frames as PPM.
#include "Renderer/HostRenderer.h"

#include "../../Render/Passes/FX/Tests/RppStream.h"
#include "TestScenes.h"
#include "unx/core/File.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 960, kHeight = 540, kTicks = 60;

struct RunResult
{
    std::vector<uint32_t> before, after;
    uint32_t debugErrors = 0;
};

RunResult run(bool megaLights, float lightIntensity)
{
    HostRendererOptions o;
    o.standalone = true;
    o.framesInFlight = 2;
    o.shaderDirectory = executableDirectory() / "shaders";
    o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
    if (megaLights) o.qualityOverrides = { "shading.mega_lights=true" };
    HostRenderer h(o);
    h.scene() = test::oneBox();
    // Night: the local lights are what lights the particles.
    h.scene().sun.direction = normalize(float3{ 0.3f, -0.5f, 0.2f });
    // Two point lights over the first emitters of the grid (x 1000.., z -2000.., y 2): a warm one casting shadows, a cool one without.
    scene::Light warm;
    warm.position = { 1004.0f, 5.0f, -2002.0f };
    warm.color = { 1.0f, 0.75f, 0.5f };
    warm.intensity = lightIntensity;
    warm.range = 30.0f;
    warm.castShadow = true;
    scene::Light cool = warm;
    cool.position = { 1009.0f, 3.0f, -2000.0f };
    cool.color = { 0.5f, 0.75f, 1.0f };
    cool.castShadow = false;
    h.scene().lights.push_back(warm);
    h.scene().lights.push_back(cool);
    h.commit();
    fx::test::RppConfig config;
    config.particles = 16384;
    config.material = 1;  // lit sprites (render stage 2)
    fx::test::RppStream stream(config);
    scene::Camera camera;
    camera.position = { 1006.0f, 4.0f, -2010.0f };
    const float3 target{ 1006.0f, 2.5f, -2001.0f };
    camera.forward = normalize(target - camera.position);
    camera.ev100 = 4.0f;
    RunResult result;
    uint64_t frame = 0;
    double time = 0;
    auto render = [&](std::vector<uint32_t>* pixels) {
        FramePacket p;
        p.frameIndex = frame++;
        p.time = time;
        p.deltaTime = 1.0f / 60;
        p.width = kWidth;
        p.height = kHeight;
        p.camera = camera;
        const uint64_t ticket = h.queueFrame(std::move(p));
        if (pixels) pixels->resize((size_t)kWidth * kHeight);
        h.renderStandalone(ticket, pixels ? pixels->data() : nullptr, pixels ? pixels->size() * 4 : 0);
    };
    render(&result.before);
    std::vector<NV_StreamEvent> previous;
    NV_StreamHeader header{};
    for (uint32_t t = 1; t <= kTicks; ++t)
    {
        const std::vector<uint8_t> packet = stream.next(t == 1 ? nullptr : &previous);
        std::memcpy(&header, packet.data(), sizeof header);
        h.vfxSubmit(packet.data(), packet.size());
        time = header.time - 0.5 * header.dt;
        render(t == kTicks ? &result.after : nullptr);
        const fx::TickReadback& r = h.vfxReadback(header.stream, header.generation, header.tick);
        if (r.counters.status != 0) fail("tick %u: stream status 0x%x", t, r.counters.status);
        previous = r.events;
    }
    result.debugErrors = h.debugErrors();
    return result;
}

uint32_t changedPixels(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
    uint32_t n = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
        const uint32_t x = a[i], y = b[i];
        const int d = std::abs((int)(x & 1023u) - (int)(y & 1023u)) + std::abs((int)((x >> 10) & 1023u) - (int)((y >> 10) & 1023u)) +
                      std::abs((int)((x >> 20) & 1023u) - (int)((y >> 20) & 1023u));
        if (d > 12) ++n;
    }
    return n;
}

void writePpm(const std::filesystem::path& path, const std::vector<uint32_t>& pixels)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6" << char(10) << kWidth << ' ' << kHeight << char(10) << "255" << char(10);
    for (uint32_t p : pixels)
    {
        const char rgb[3] = { (char)((p & 1023u) >> 2), (char)(((p >> 10) & 1023u) >> 2), (char)(((p >> 20) & 1023u) >> 2) };
        f.write(rgb, 3);
    }
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const RunResult lit = run(true, 4000.0f), dark = run(true, 0.0f);
        const uint32_t drawn = changedPixels(lit.before, lit.after), byLights = changedPixels(lit.after, dark.after);
        std::printf("HostVfxMegaLights: lit particles changed %u pixels against the empty frame; %u pixels differ from the run with the lights at 0; debug-layer errors %u + %u\n",
                    drawn, byLights, lit.debugErrors, dark.debugErrors);
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--out")
            {
                std::filesystem::create_directories(argv[i + 1]);
                writePpm(std::filesystem::path(argv[i + 1]) / "host_vfx_ml_lit.ppm", lit.after);
                writePpm(std::filesystem::path(argv[i + 1]) / "host_vfx_ml_dark.ppm", dark.after);
            }
        if (lit.debugErrors || dark.debugErrors) fail("D3D12 debug-layer errors");
        if (drawn == 0) fail("the lit particles did not reach the image");
        if (byLights == 0) fail("the local lights did not change the particles (the sampled volumes were not read)");
        std::printf("HostVfxMegaLights: ok\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "HostVfxMegaLights: FAILED: %s\n", e.what());
        return 1;
    }
}
