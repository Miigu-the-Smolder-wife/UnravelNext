// V3 through the host (render A item A3; WORLD_VFX 3.7 (f)): the renderer's FX particle module as the VFX stream
// executor. The same particle stream (the RPP stand-in authority, FX Tests/RppStream.h) runs twice:
//   C0      every tick is submitted, then a frame records it (the normal path), then its readback feeds the next packet;
//   hitches the ticks' readbacks come before any frame (two fixed steps in one Unity frame): the host claims them and runs
//           them at once on its compute queue; frames render only every third tick and wait for those ticks on the GPU.
// Checks: the hitch run claimed its ticks (immediate count = ticks), the C0 run none; the final checkpoints (every live
// particle's position, velocity and age) are bit identical between the runs, so where a tick runs does not change it;
// the particles reach the image through M's shading composite (pixels change against the frame before the first tick);
// no D3D12 debug-layer errors. Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
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
constexpr uint32_t kWidth = 960, kHeight = 540, kTicks = 90;

struct RunResult
{
    std::vector<NV_StreamParticle> checkpoint;
    std::vector<uint32_t> before, after;  // RGB10A2 frames before the first tick and after the last
    uint64_t immediate = 0;
    uint32_t debugErrors = 0;
};

RunResult run(bool hitches)
{
    HostRendererOptions o;
    o.standalone = true;
    o.framesInFlight = 2;
    o.shaderDirectory = executableDirectory() / "shaders";
    o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
    HostRenderer h(o);
    h.scene() = test::oneBox();
    // Night (the sun 30 degrees below the horizon): stage 1 of the particle pass draws emission only, which a daylight
    // exposure would not show.
    h.scene().sun.direction = normalize(float3{ 0.3f, -0.5f, 0.2f });
    h.commit();
    fx::test::RppConfig config;
    config.particles = 16384;
    fx::test::RppStream stream(config);
    // A camera 8 m from the first emitters of the grid (x 1000.., z -2000.., y 2), exposed for their emissive sprites
    // (stage 1 of the particle pass: emission only).
    scene::Camera camera;
    camera.position = { 1006.0f, 4.0f, -2010.0f };
    const float3 target{ 1006.0f, 2.5f, -2001.0f };
    camera.forward = normalize(target - camera.position);
    camera.ev100 = 0.0f;
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
        // Frames render between the last two ticks (the particle stream's clock).
        time = header.time - 0.5 * header.dt;
        if (!hitches) render(t == kTicks ? &result.after : nullptr);
        else if (t % 3 == 0) render(nullptr);
        const fx::TickReadback& r = h.vfxReadback(header.stream, header.generation, header.tick);
        if (r.counters.status != 0) fail("tick %u: stream status 0x%x", t, r.counters.status);
        previous = r.events;
    }
    if (hitches) render(&result.after);  // after the claimed ticks: waits for them on the GPU
    result.checkpoint = h.vfxCheckpoint(header.stream, header.generation, header.tick);
    result.immediate = h.vfxImmediateTicks();
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
} // namespace

// --out DIR: the C0 run's frames before and after as binary PPM (display-encoded, 8 bits of the 10).
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

int main(int argc, char** argv)
{
    try
    {
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-78s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        const RunResult c0 = run(false);
        const RunResult hitch = run(true);
        logf("  C0: %zu live particles, %llu ticks claimed; hitches: %zu live, %llu claimed\n", c0.checkpoint.size(), (unsigned long long)c0.immediate,
             hitch.checkpoint.size(), (unsigned long long)hitch.immediate);
        expect("C0 run: every tick recorded by a frame (none claimed)", c0.immediate == 0);
        // (every third tick a frame records the tick just submitted before its readback: C0 for those)
        expect("hitch run: the ticks no frame recorded were claimed by their readbacks", hitch.immediate == kTicks - kTicks / 3);
        expect("live particles in the checkpoint", !c0.checkpoint.empty());
        bool same = c0.checkpoint.size() == hitch.checkpoint.size();
        for (size_t i = 0; same && i < c0.checkpoint.size(); ++i)
            same = std::memcmp(&c0.checkpoint[i], &hitch.checkpoint[i], sizeof(NV_StreamParticle)) == 0;
        expect("final state bit identical whether ticks ran in frames or at once on compute", same);
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--out")
            {
                writePpm(std::filesystem::path(argv[i + 1]) / "host_vfx_before.ppm", c0.before);
                writePpm(std::filesystem::path(argv[i + 1]) / "host_vfx_after.ppm", c0.after);
            }
        const uint32_t changedC0 = changedPixels(c0.before, c0.after), changedHitch = changedPixels(hitch.before, hitch.after);
        logf("  pixels changed by the particles: C0 %u, hitches %u (of %u)\n", changedC0, changedHitch, kWidth * kHeight);
        expect("the particles reach the image (shading composite of the particle layer)", changedC0 > 2000 && changedHitch > 2000);
        expect("D3D12 debug layer errors 0", c0.debugErrors == 0 && hitch.debugErrors == 0);
        logf(failures ? "HOST VFX TEST FAILED (%u)\n" : "HOST VFX TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
