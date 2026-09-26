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
//                                                     [--frames N] [--out DIR] [--set key=value ...] [--no-heightfield]
//   The load includes the RppStream terrain (60 x 30 cells of 1.6 m, holes) unless --no-heightfield.
#include "../Tests/RppStream.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/fx/Particles.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
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
        bool delta = true, patchesOn = true, heightfield = true;
        bool passTimestamps = true;  // --no-pass-timestamps: frame timing only (the per-pass queries serialise the queue)  // emitter table as NV_STREAM_EMITTER_DELTA packets (--no-delta: whole table every tick, A/B)
        // --packets DIR: submit a recorded stream (ParticleTests --record: packet_NNNN.bin) open loop. The GPU tick is then
        // timed without the stand-in authority's CPU in the loop (a closed-loop features run leaves the GPU idle while the
        // fixture builds the next packet, and its clocks drop). The module is deterministic on one GPU and driver, so the
        // recorded packets stay consistent with the events this run produces. Runs out of packets = failure (never loops).
        std::string packets;
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
            else if (a == "--no-delta") delta = false;
            else if (a == "--no-heightfield") heightfield = false;
            else if (a == "--no-patches") patchesOn = false;
            else if (a == "--no-pass-timestamps") passTimestamps = false;
            else if (a == "--packets") packets = next();
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
        if (!packets.empty()) loads = { "recorded" };
        if (resolution == "both") resolutions = { "4K", "1440p" }; else resolutions = { resolution };
        for (const std::string& l : loads)
            for (const std::string& rn : resolutions)
            {
                const Resolution res = resolutionFromString(rn, quality);
                fx::test::RppConfig cfg;
                cfg.features = l == "features";
                cfg.heightfield = heightfield;
                cfg.killTick = UINT32_MAX;  // the population stays at the RPP load
                cfg.delta = delta;
                cfg.patches = patchesOn;
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
                std::vector<double> cpuStream, cpuSubmit, cpuRecord, packetKB, blocks, patchesPerTick;
                uint64_t lastTick = 0, lastStream = 0, lastGeneration = 0, overflowTicks = 0, firstOverflow = 0;
                uint32_t tableRows = 0;

                HarnessOptions ho;
                ho.frames = frames;
                ho.passTimestamps = passTimestamps;
                ho.label = "fx_particles_" + l + (passTimestamps ? "" : "_nopass");
                ho.outputDirectory = out;
                const HarnessResult r = harness.run(res, ho, [&](RenderGraph& graph, const Resolution&, uint64_t f) {
                    if ((cfg.features || !packets.empty()) && lastTick)
                    {
                        const fx::TickReadback rb = ps.readback(lastStream, lastGeneration, lastTick);
                        // IMPACT_OVERFLOW (bit 1) is a reported condition of the stream (counted); every other bit is a failure
                        if (rb.counters.status & ~uint32_t(NV_STREAM_STATUS_IMPACT_OVERFLOW)) fail("tick %llu: status 0x%x", (unsigned long long)lastTick, rb.counters.status);
                        if (rb.counters.status & NV_STREAM_STATUS_IMPACT_OVERFLOW)
                        {
                            if (!overflowTicks) firstOverflow = lastTick;
                            ++overflowTicks;
                        }
                        previous = rb.events;
                    }
                    const auto c0 = std::chrono::steady_clock::now();
                    std::vector<uint8_t> packet;
                    if (packets.empty()) packet = stream.next(lastTick ? &previous : nullptr);
                    else
                    {
                        const std::filesystem::path file = std::filesystem::path(packets) / format("packet_%04llu.bin", (unsigned long long)(lastTick + 1));
                        if (!std::filesystem::exists(file)) fail("--packets: %s is missing (the recording is shorter than warm-up + %u frames)", file.string().c_str(), frames);
                        packet = readBinaryFile(file);
                    }
                    const auto c1 = std::chrono::steady_clock::now();
                    const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet.data());
                    lastTick = h.tick;
                    lastStream = h.stream;
                    lastGeneration = h.generation;
                    packetKB.push_back(packet.size() / 1024.0);
                    blocks.push_back(h.emitter_count);
                    patchesPerTick.push_back(h.emitter_patch_count);
                    tableRows = h.emitter_table;
                    ps.submit(packet.data(), packet.size());
                    const auto c2 = std::chrono::steady_clock::now();
                    frame.frameIndex = f;
                    FrameResources resources;
                    FramePassContext fc{ device, graph, shaders, quality, scene, frame, resources, services,
                                         [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state };
                    tracks::simulation(fc);
                    const auto c3 = std::chrono::steady_clock::now();
                    cpuStream.push_back(std::chrono::duration<double, std::milli>(c1 - c0).count());
                    cpuSubmit.push_back(std::chrono::duration<double, std::milli>(c2 - c1).count());
                    cpuRecord.push_back(std::chrono::duration<double, std::milli>(c3 - c2).count());
                });
                auto median = [](std::vector<double> v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
                std::printf("FX_PARTICLE_GATE_CPU load=%s resolution=%s delta=%d fixture_ms=%.3f submit_ms=%.3f record_ms=%.3f packet_kb=%.1f emitter_blocks=%.0f patches=%.0f of %u rows "
                            "(medians; the fixture stands in for the VFX authority)\n",
                            l.c_str(), rn.c_str(), delta ? 1 : 0, median(cpuStream), median(cpuSubmit), median(cpuRecord), median(packetKB), median(blocks), median(patchesPerTick), tableRows);
                {
                    const double* sm = stream.sectionMs();
                    const double n = (double)std::max<uint64_t>(lastTick, 1);
                    std::printf("FX_PARTICLE_GATE_FIXTURE load=%s ms/tick: children %.3f rows %.3f deaths %.3f births %.3f cascade %.3f totals %.3f packet %.3f ends %.3f\n",
                                l.c_str(), sm[1] / n, sm[2] / n, sm[3] / n, sm[4] / n, sm[5] / n, sm[6] / n, sm[7] / n, sm[8] / n);
                }
                if (cfg.features)
                    std::printf("FX_PARTICLE_GATE_OVERFLOW load=%s resolution=%s ticks_with_impact_overflow=%llu first_tick=%llu (reported condition, NV_STREAM_STATUS_IMPACT_OVERFLOW)\n",
                                l.c_str(), rn.c_str(), (unsigned long long)overflowTicks, (unsigned long long)firstOverflow);
                harness.printSummary(r);
                const fx::TickReadback last = ps.readback(lastStream, lastGeneration, lastTick);
                std::printf("FX_PARTICLE_GATE load=%s resolution=%s alive=%u status=0x%x collisions=%u rows=%u tick=%llu gpu_frame_ms_median=%.4f p95=%.4f p99=%.4f "
                            "passes_ms: upload=%.4f emitters=%.4f integrate=%.4f spawn=%.4f child=%.4f compact=%.4f sort=%.4f grid=%.4f other=%.4f gate=%s\n",
                            l.c_str(), rn.c_str(), last.counters.alive, last.counters.status, last.counters.collision_events, tableRows,
                            (unsigned long long)lastTick, r.gpuFrameMs.median, r.gpuFrameMs.p95, r.gpuFrameMs.p99, passSum(r, "fx.particles.upload"),
                            passSum(r, "fx.particles.emitters"), passSum(r, "fx.particles.integrate"),
                            passSum(r, "fx.particles.spawn"), passSum(r, "fx.particles.child"), passSum(r, "fx.particles.compact"), passSum(r, "fx.particles.sort"),
                            passSum(r, "fx.particles.grid") + passSum(r, "fx.particles.surfaces"),
                            passSum(r, "fx.particles.upload") + passSum(r, "fx.particles.begin") + passSum(r, "fx.particles.readback"),
                            r.gpuFrameMs.median <= 0.2 ? "PASS" : "FAIL");
                {
                    // the last tick's work counts (cost attribution): colliders queued, grid entries, large-list surfaces
                    const std::vector<uint8_t> counters = ps.readState("counters");
                    auto word = [&](size_t k) { uint32_t v = 0; if (counters.size() >= (k + 1) * 4) std::memcpy(&v, counters.data() + k * 4, 4); return v; };
                    std::printf("FX_PARTICLE_GATE_WORK load=%s colliders=%u grid_entries=%u large_surfaces=%u collide_ms=%.4f ribbon_ms=%.4f begin_ms=%.4f readback_ms=%.4f "
                                "capacity=%u resident_mb=%.2f\n",
                                l.c_str(), word(11), word(6), word(5), passSum(r, "fx.particles.collide"), passSum(r, "fx.particles.ribbon"),
                                passSum(r, "fx.particles.begin"), passSum(r, "fx.particles.readback"), ps.capacity(), ps.residentBytes() / 1048576.0);
                }
            }
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
