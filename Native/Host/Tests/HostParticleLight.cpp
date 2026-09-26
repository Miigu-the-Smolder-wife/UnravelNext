// Lit particles in a real scene through the host (render A item A3, FX stage 2 in context): the RPP stand-in stream with
// lit sprites (material 1, medium_phase 0.6) inside a scene with its sun, atmosphere, GI cache, shadow pages and local
// lights, under automatic exposure. A particle's radiance must be finite in every channel: a NaN channel composites to 0
// on the display (saturate), which showed in D0 as floor-coloured shapes with whole channels missing.
// Check: no pixel of any frame has a channel at 0 where the frame before the first tick had it above 40 (10-bit) while
// another channel stayed lit (the NaN signature); the particles reach the image; no D3D12 debug-layer errors.
// Options: --width W --height H (960 x 540), --hitch N (frames every N-th tick), --scene file.unxscene (default: the one-box scene), --out DIR (PPM of both frames), --stream DIR (a recorded
// game stream, UNX_FX_RECORD packets, replayed in the scene's own coordinates from its camera 0 instead of the RPP
// stand-in). Without --stream the scene is moved so that its camera 0 looks at the stand-in's emitters (world-fixed at
// x 1000.., y 2, z -2000..).
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "../../Render/Passes/FX/Tests/RppStream.h"
#include "TestScenes.h"
#include "unx/core/File.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
uint32_t kWidth = 960, kHeight = 540;
constexpr uint32_t kTicks = 120;

int channel(uint32_t p, int c) { return (int)((p >> (10 * c)) & 1023u); }

void writePpm(const std::filesystem::path& path, const std::vector<uint32_t>& pixels)
{
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
    for (uint32_t p : pixels)
        for (int c = 0; c < 3; ++c) f.put((char)(channel(p, c) >> 2));
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string scenePath, out, streamDir;
        uint32_t every = 1;  // --hitch N: frames only every N-th tick; the ticks between are claimed by their readbacks (compute queue)
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--scene" && i + 1 < argc) scenePath = argv[++i];
            else if (a == "--out" && i + 1 < argc) out = argv[++i];
            else if (a == "--stream" && i + 1 < argc) streamDir = argv[++i];
            else if (a == "--width" && i + 1 < argc) kWidth = (uint32_t)std::atoi(argv[++i]);
            else if (a == "--height" && i + 1 < argc) kHeight = (uint32_t)std::atoi(argv[++i]);
            else if (a == "--hitch" && i + 1 < argc) every = (uint32_t)std::max(1, std::atoi(argv[++i]));
            else fail("unknown option %s", a.c_str());
        }
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-80s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        HostRenderer h(o);
        h.scene() = scenePath.empty() ? test::oneBox() : scene::load(scenePath);
        scene::Scene& s = h.scene();
        if (s.cameras.empty()) fail("the scene has no camera");
        // Move the scene so camera 0 stands 8 m beyond the last emitter row, looking back across the rows at its own height.
        scene::Camera camera = s.cameras[0];
        if (streamDir.empty())
        {
            const float3 eye0 = s.cameras[0].position;
            const float3 target{ 1006.0f, eye0.y, -1950.0f };  // rows at z -2000..-1958, looked at along -z
            const float3 shift = target - eye0;
            for (scene::Instance& in : s.instances)
                for (int r = 0; r < 3; ++r) in.transform.m[r][3] += (&shift.x)[r];
            for (scene::Light& l : s.lights) l.position = l.position + shift;
            camera.position = target;
            camera.forward = normalize(float3{ 0.0f, -0.15f, -1.0f });
        }
        camera.ev100 = std::numeric_limits<float>::quiet_NaN();  // automatic exposure, as the D0 game view
        h.commit();

        fx::test::RppConfig config;
        config.particles = 16384;
        config.material = 1;
        config.phase = 0.6f;
        fx::test::RppStream stream(config);
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
        // settle exposure and GI on the scene alone, then the frame before the first tick
        for (int i = 0; i < 30; ++i) render(nullptr);
        std::vector<uint32_t> before, after;
        render(&before);
        std::vector<NV_StreamEvent> previous;
        NV_StreamHeader header{};
        uint64_t changed = 0, nanSignature = 0;
        uint32_t worstFrame = 0, firstFrames = 0;
        // pixels of 'b' with a channel zeroed (it was above 40 in 'a') while another is lit
        auto signature = [&](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, uint64_t* differing) {
            uint64_t n = 0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (differing && a[i] != b[i]) ++*differing;
                bool zeroed = false, lit = false;
                for (int c = 0; c < 3; ++c)
                {
                    if (channel(b[i], c) == 0 && channel(a[i], c) > 40) zeroed = true;
                    if (channel(b[i], c) > 40) lit = true;
                }
                n += zeroed && lit;
            }
            return n;
        };
        std::vector<std::filesystem::path> recorded;
        if (!streamDir.empty())
        {
            for (const auto& e : std::filesystem::directory_iterator(streamDir))
                if (e.path().extension() == ".bin") recorded.push_back(e.path());
            std::sort(recorded.begin(), recorded.end());
            if (recorded.empty()) fail("no packet_*.bin in %s", streamDir.c_str());
        }
        const uint32_t ticks = recorded.empty() ? kTicks : (uint32_t)recorded.size();
        for (uint32_t t = 1; t <= ticks; ++t)
        {
            const std::vector<uint8_t> packet = recorded.empty() ? stream.next(t == 1 ? nullptr : &previous) : readBinaryFile(recorded[t - 1]);
            std::memcpy(&header, packet.data(), sizeof header);
            h.vfxSubmit(packet.data(), packet.size());
            // frames at several points of the tick interval, extrapolated ones included (a host's frames can run ahead of
            // a late tick): w = frame time after the tick before, over dt
            uint64_t n = 0;
            for (const double w : { 0.0, 0.5, 1.0, 1.5, 3.0 })
            {
                if (t % every != 0 && t != ticks) break;
                time = header.time - header.dt + w * header.dt;
                render(&after);
                n += signature(before, after, t == ticks && w == 3.0 ? &changed : nullptr);
            }
            if (n && !nanSignature) worstFrame = t;
            nanSignature += n;
            if (n && !out.empty() && firstFrames < 3)
            {
                std::filesystem::create_directories(out);
                writePpm(std::filesystem::path(out) / ("particle_light_nan_" + std::to_string(t) + ".ppm"), after);
                ++firstFrames;
            }
            const fx::TickReadback& r = h.vfxReadback(header.stream, header.generation, header.tick);
            if (r.counters.status != 0) fail("tick %u: stream status 0x%x", t, r.counters.status);
            previous = r.events;
        }
        logf("  %llu pixels changed by the particles (last frame), %llu with a channel zeroed while another is lit over %u frames (first in frame %u)\n",
             (unsigned long long)changed, (unsigned long long)nanSignature, ticks, worstFrame);
        if (!out.empty())
        {
            std::filesystem::create_directories(out);
            writePpm(std::filesystem::path(out) / "particle_light_before.ppm", before);
            writePpm(std::filesystem::path(out) / "particle_light_after.ppm", after);
        }
        expect("the lit particles reach the image", changed > 1000);
        expect("no channel of a lit pixel zeroed (non-finite particle radiance)", nanSignature == 0);
        expect("D3D12 debug layer errors 0", h.debugErrors() == 0);
        logf(failures ? "HOST PARTICLE LIGHT TEST FAILED (%u)\n" : "HOST PARTICLE LIGHT TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
