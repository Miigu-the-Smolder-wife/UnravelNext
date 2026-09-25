// FX particle module timing gate (WORLD_VFX_DESIGN_KO.md 7 V1: <= 0.2 ms per tick at 524,288 particles, sort included).
// One harness frame = one simulation tick recorded in the C0 slot (tracks::simulation), timed by the harness (GPU lock
// required, 1.5 s warm-up during which the stream reaches its steady state of 524,288 live particles, medians and
// P95/P99 of the frame and of every pass). The main view (sort key frame) is 4K or 1440p; the simulation cost does not
// depend on it, both are measured as the rules require.
//   --load core     the design's RPP load (WORLD_VFX 3.3): 128 emitters, 524,288 live, 16 fields, gravity, wind, noise,
//                   drag, collisions against 6,912 rigid-body surfaces (1/8 of the programs), death events (1/4)
//   --load features core + cascades to depth 4, moving source, transport, rebase, explicit births (the fixture reads each
//                   tick's events back before writing the next packet, so frames do not overlap)
//   GpuLock.ps1 -Track FX -- unx_gate_fx_particlegate [--load core|features|both] [--resolution 4K|1440p|both]
//                                                     [--frames N] [--out DIR] [--set key=value ...]
#include "../Tests/RppStream.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/fx/Particles.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/render/Tracks.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
double passSum(const HarnessResult& r, const char* prefix)
{
    double sum = 0;
    for (const auto& [name, d] : r.passMs)
        if (name.rfind(prefix, 0) == 0) sum += d.median;
    return sum;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string load = "both", resolution = "both", out = std::string(UNX_SOURCE_DIR) + "/Results/FX/ParticleGate";
        uint32_t frames = 600;
        std::vector<std::string> overrides;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--load") load = next();
            else if (a == "--resolution") resolution = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--out") out = next();
            else if (a == "--set") overrides.push_back(next());
            else fail("unknown option %s", a.c_str());
        }
        requireGpuLock("fx.particles");
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const auto& o : overrides) quality.applyOverride(o);
        DeviceOptions opts;
        Device device(opts);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene scene(device);
        Harness harness(device, quality);

        std::vector<std::string> loads, resolutions;
        if (load == "both") loads = { "core", "features" }; else loads = { load };
        if (resolution == "both") resolutions = { "4K", "1440p" }; else resolutions = { resolution };
        for (const std::string& l : loads)
            for (const std::string& rn : resolutions)
            {
                const Resolution res = resolutionFromString(rn, quality);
                fx::test::RppConfig cfg;
                cfg.features = l == "features";
                cfg.killTick = UINT32_MAX;  // the population stays at the RPP load
                fx::test::RppStream stream(cfg);
                TrackState state;
                fx::ParticleSystem& ps = fx::particles(state, device, quality);
                FrameContext frame;
                scene::Camera cam;
                cam.position = { 1060.0f, 12.0f, -2080.0f };
                cam.forward = normalize(float3{ -0.3f, -0.4f, 1.0f });
                frame.mainView = ViewDesc::fromCamera(cam, res.width, res.height, float4x4{});
                FrameServices services;
                std::vector<NV_StreamEvent> previous;
                uint64_t lastTick = 0, lastStream = 0, lastGeneration = 0;

                HarnessOptions ho;
                ho.frames = frames;
                ho.label = "fx_particles_" + l;
                ho.outputDirectory = out;
                const HarnessResult r = harness.run(res, ho, [&](RenderGraph& graph, const Resolution&, uint64_t f) {
                    if (cfg.features && lastTick)
                    {
                        const fx::TickReadback rb = ps.readback(lastStream, lastGeneration, lastTick);
                        if (rb.counters.status) fail("tick %llu: status 0x%x", (unsigned long long)lastTick, rb.counters.status);
                        previous = rb.events;
                    }
                    const std::vector<uint8_t> packet = stream.next(lastTick ? &previous : nullptr);
                    const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet.data());
                    lastTick = h.tick;
                    lastStream = h.stream;
                    lastGeneration = h.generation;
                    ps.submit(packet.data(), packet.size());
                    frame.frameIndex = f;
                    FrameResources resources;
                    FramePassContext fc{ device, graph, shaders, quality, scene, frame, resources, services,
                                         [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state };
                    tracks::simulation(fc);
                });
                harness.printSummary(r);
                const fx::TickReadback last = ps.readback(lastStream, lastGeneration, lastTick);
                std::printf("FX_PARTICLE_GATE load=%s resolution=%s alive=%u status=0x%x collisions=%u rows=%u tick=%llu gpu_frame_ms_median=%.4f p95=%.4f p99=%.4f "
                            "passes_ms: integrate=%.4f spawn=%.4f child=%.4f compact=%.4f sort=%.4f grid=%.4f other=%.4f gate=%s\n",
                            l.c_str(), rn.c_str(), last.counters.alive, last.counters.status, last.counters.collision_events, stream.rows(),
                            (unsigned long long)lastTick, r.gpuFrameMs.median, r.gpuFrameMs.p95, r.gpuFrameMs.p99, passSum(r, "fx.particles.integrate"),
                            passSum(r, "fx.particles.spawn"), passSum(r, "fx.particles.child"), passSum(r, "fx.particles.compact"), passSum(r, "fx.particles.sort"),
                            passSum(r, "fx.particles.grid") + passSum(r, "fx.particles.surfaces"),
                            passSum(r, "fx.particles.upload") + passSum(r, "fx.particles.begin") + passSum(r, "fx.particles.readback"),
                            r.gpuFrameMs.median <= 0.2 ? "PASS" : "FAIL");
            }
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
