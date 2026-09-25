// FX particle module correctness (no GPU lock; WORLD_VFX_DESIGN_KO.md 7 V1 gate items except the timing):
//   1. stream copies: Stream/{include,shaders,src} are byte-identical to the NativeVfx originals (SHA-256) when the
//      Unravel checkout is present;
//   2. reference: every tick the same packet (RppStream: RPP load + cascades, sources, transport, rebase, explicit
//      births, kill) goes to the GPU module and to the CPU reference executor (VfxStreamCpu.h: the stream's formulas in
//      double). Per tick: status 0, alive = alive_after = reference, CPU-assigned events (identity exact, state close),
//      collision event counts. Every --compare-every ticks and at the end: every live particle (identity exact),
//      position error |dp| / max(|p|, 1 m), velocity, age;
//   3. --check items (the reference kernel's): alive flags = counter, alive and dead lists a disjoint complete
//      partition, dying list = slots that died, sort keys non-decreasing and the values a permutation of the alive
//      slots with their keys, positions finite, age <= lifetime, spawn capacity never exhausted;
//   4. --determinism: the whole run twice from a fresh module; the GPU state (every state buffer, lists, sorted
//      keys/values, records) and the sorted events are bit identical at every compare tick.
// Options: --ticks N (600) --compare-every K (60) --particles P --emitters E --no-features --no-reference
//          --determinism --warp (WARP adapter: another implementation, 4-lane waves) --no-debug-layer --gbv
//          --yield (pause while a GPU measurement lock or the user's HOLD is present: CPU-heavy runs)
//          --allow-copy-drift (development only: a stream copy that differs from the original is a warning)
#include "RppStream.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/core/Sha256.h"
#include "unx/fx/Particles.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <tuple>

using namespace unx;
#define FX_LOG(fmt, ...) unx::logf(fmt "\n", ##__VA_ARGS__)
using namespace unx::render;
namespace fs = std::filesystem;

namespace
{
#define FX_CHECK(cond, ...)                                                                                           \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

struct Options
{
    uint32_t ticks = 600, compareEvery = 60;
    fx::test::RppConfig rpp;
    bool reference = true, determinism = false, warp = false, debugLayer = true, gbv = false, yield = false, strictCopies = true;
};

std::string sha(const std::vector<uint8_t>& bytes)
{
    Sha256 h;
    h.update(bytes.data(), bytes.size());
    const auto d = h.finish();
    std::string s;
    for (uint8_t b : d) s += format("%02x", b);
    return s;
}

// 1. byte-identical stream copies
void checkStreamCopies(bool strict)
{
    const fs::path mine = fs::path(UNX_SOURCE_DIR) / "Native/Render/Passes/FX/Stream";
    const fs::path original = fs::path(UNX_SOURCE_DIR) / "../Unravel/Native/NativeVfx";
    const char* files[] = { "include/NativeVfxStream.h", "shaders/VfxParticleMath.hlsli", "src/VfxStreamCpu.h" };
    for (const char* f : files)
    {
        const std::string a = sha(readBinaryFile(mine / f));
        if (!fs::exists(original / f)) { FX_LOG("stream copy %s sha256 %s (original not present)", f, a.c_str()); continue; }
        const std::string b = sha(readBinaryFile(original / f));
        if (a != b && !strict) { FX_LOG("WARNING stream copy %s differs from the NativeVfx original (%s vs %s)", f, a.c_str(), b.c_str()); continue; }
        FX_CHECK(a == b, "stream copy %s differs from the NativeVfx original (%s vs %s)", f, a.c_str(), b.c_str());
        FX_LOG("stream copy %s = original, sha256 %s", f, a.c_str());
    }
}

void waitIfMeasuring(bool yield)
{
    if (!yield) return;
    const fs::path dir = fs::path(UNX_SOURCE_DIR) / ".gpulock";
    bool said = false;
    while (fs::exists(dir / "current.json") || fs::exists(dir / "HOLD"))
    {
        if (!said) FX_LOG("yield: a GPU measurement or HOLD is active; pausing");
        said = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

template <typename T> T at(const std::vector<uint8_t>& b, size_t i) { T v; std::memcpy(&v, b.data() + i * sizeof(T), sizeof(T)); return v; }

struct Stats
{
    double maxPos = 0, maxVel = 0, maxAge = 0, sumPos = 0;
    uint64_t n = 0;
    double p99Pos = 0;
    int dumped = 0;
};

// One run of the stream through the module (and optionally the reference). Returns the state hashes per compare tick.
std::vector<std::string> run(Device& device, const Options& o, bool withReference, Stats& worst)
{
    ShaderLibrary shaders(device, executableDirectory() / "shaders");
    const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    GpuScene scene(device);
    RenderGraph graph(device);
    TrackState state;
    fx::ParticleSystem& ps = fx::particles(state, device, quality);
    fx::test::RppStream stream(o.rpp);
    nv_stream::CpuExecutor cpu;
    FrameContext frame;
    scene::Camera cam;
    cam.position = { 1060.0f, 12.0f, -2080.0f };
    cam.forward = normalize(float3{ -0.3f, -0.4f, 1.0f });
    frame.mainView = ViewDesc::fromCamera(cam, 3840, 2160, float4x4{});
    FrameServices services;
    std::vector<NV_StreamEvent> previous;
    std::vector<std::string> hashes;
    uint64_t events = 0, collisions = 0, collisionDiff = 0, childRows = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t t = 1; t <= o.ticks; ++t)
    {
        waitIfMeasuring(o.yield);
        const std::vector<uint8_t> packet = stream.next(t == 1 ? nullptr : &previous);
        const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet.data());
        ps.submit(packet.data(), packet.size());
        if (withReference) cpu.submit(packet.data(), packet.size());

        FrameResources resources;
        FramePassContext fc{ device, graph, shaders, quality, scene, frame, resources, services, [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state };
        tracks::simulation(fc);
        graph.execute(nullptr);
        ++frame.frameIndex;
        FX_CHECK(device.drainDebugMessages() == 0, "tick %u: D3D12 debug layer errors", t);

        const fx::TickReadback rb = ps.readback(h.stream, h.generation, h.tick);
        FX_CHECK(rb.counters.tick == h.tick && rb.counters.stream == h.stream && rb.counters.generation == h.generation, "tick %u: readback identity", t);
        FX_CHECK(rb.counters.status == 0, "tick %u: GPU status 0x%x (1 impact overflow, 2 nonfinite, 4 alive mismatch, 8 capacity)", t, rb.counters.status);
        FX_CHECK(rb.counters.alive == h.alive_after, "tick %u: GPU alive %u != alive_after %u", t, rb.counters.alive, h.alive_after);
        previous = rb.events;
        events += h.event_slots;
        collisions += rb.counters.collision_events;

        if (withReference)
        {
            const NV_StreamCounters& rc = cpu.counters();
            FX_CHECK(rc.status == 0, "tick %u: reference status 0x%x", t, rc.status);
            FX_CHECK(rc.alive == rb.counters.alive, "tick %u: alive GPU %u reference %u", t, rb.counters.alive, rc.alive);
            const auto& re = cpu.events();
            // CPU-assigned slots: identity exact, state close
            for (uint32_t i = 0; i < h.event_slots; ++i)
            {
                const NV_StreamEvent &g = rb.events[i], &r = re[i];
                FX_CHECK(g.emitter == r.emitter && g.birth == r.birth && g.kind == r.kind, "tick %u event %u: identity GPU (%u,%u,%u) reference (%u,%u,%u)", t, i,
                         g.emitter, g.birth, g.kind, r.emitter, r.birth, r.kind);
                double dp = 0, pp = 0;
                for (int a = 0; a < 3; ++a) { dp += (g.position[a] - r.position[a]) * (double)(g.position[a] - r.position[a]); pp += (double)r.position[a] * r.position[a]; }
                const double e = std::sqrt(dp) / std::max(std::sqrt(pp), 1.0);
                FX_CHECK(e <= 1e-5, "tick %u event %u (emitter %u birth %u kind %u): position error %.3g", t, i, g.emitter, g.birth, g.kind, e);
            }
            const uint32_t rcCollisions = rc.collision_events;
            collisionDiff += (uint64_t)std::abs((int64_t)rcCollisions - (int64_t)rb.counters.collision_events);
        }

        if (t % o.compareEvery == 0 || t == o.ticks)
        {
            // 3. structural checks
            const auto alive = ps.readState("alive"), aliveList = ps.readState("aliveList"), deadList = ps.readState("deadList");
            const auto counters = ps.readState("counters"), keys = ps.readState("keysSorted"), vals = ps.readState("valsSorted");
            const auto keyBySlot = ps.readState("keyBySlot"), posAge = ps.readState("posAge"), velocity = ps.readState("velocity"), meta = ps.readState("meta");
            const auto dying = ps.readState("dyingList"), records = ps.readState("records");
            const uint32_t cap = ps.capacity(), nAlive = at<uint32_t>(counters, 0), nDead = at<uint32_t>(counters, 1), nDying = at<uint32_t>(counters, 4);
            uint32_t flags = 0;
            for (uint32_t i = 0; i < cap; ++i) flags += at<uint32_t>(alive, i) == 1u;
            FX_CHECK(flags == nAlive && nAlive + nDead == cap, "tick %u: alive flags %u, counters alive %u dead %u, capacity %u", t, flags, nAlive, nDead, cap);
            std::vector<uint8_t> seen(cap, 0);
            for (uint32_t i = 0; i < nAlive; ++i)
            {
                const uint32_t s = at<uint32_t>(aliveList, i);
                FX_CHECK(s < cap && at<uint32_t>(alive, s) == 1u && !seen[s], "tick %u: alive list entry %u = slot %u", t, i, s);
                seen[s] = 1;
                if (i) FX_CHECK(at<uint32_t>(aliveList, i - 1) < s, "tick %u: alive list not in slot order", t);
            }
            for (uint32_t i = 0; i < nDead; ++i)
            {
                const uint32_t s = at<uint32_t>(deadList, i);
                FX_CHECK(s < cap && at<uint32_t>(alive, s) == 0u && !seen[s], "tick %u: dead list entry %u = slot %u", t, i, s);
                seen[s] = 1;
            }
            for (uint32_t i = 0; i < nDying; ++i)
            {
                const uint32_t s = at<uint32_t>(dying, i);
                FX_CHECK(s < cap && at<uint32_t>(alive, s) == 0u, "tick %u: dying list slot %u not dead", t, s);
            }
            std::vector<uint8_t> seen2(cap, 0);
            const uint32_t keyMask = ps.sortPasses() >= 3 ? 0xFFFFFFu : ((1u << (8 * ps.sortPasses())) - 1u);
            for (uint32_t i = 0; i < nAlive; ++i)
            {
                const uint32_t k = at<uint32_t>(keys, i), s = at<uint32_t>(vals, i);
                if (i) FX_CHECK((at<uint32_t>(keys, i - 1) & keyMask) <= (k & keyMask), "tick %u: sort keys decrease at %u", t, i);
                FX_CHECK(s < cap && at<uint32_t>(alive, s) == 1u && !seen2[s] && at<uint32_t>(keyBySlot, s) == k, "tick %u: sorted value %u = slot %u", t, i, s);
                seen2[s] = 1;
                if (i && (at<uint32_t>(keys, i - 1) & keyMask) == (k & keyMask)) FX_CHECK(at<uint32_t>(vals, i - 1) < s, "tick %u: sort not stable at %u", t, i);
            }
            for (uint32_t i = 0; i < nAlive; ++i)
            {
                const uint32_t s = at<uint32_t>(aliveList, i);
                const float4 pa = at<float4>(posAge, s);
                FX_CHECK(std::isfinite(pa.x) && std::isfinite(pa.y) && std::isfinite(pa.z) && std::isfinite(pa.w) && pa.w >= 0, "tick %u: slot %u state not finite", t, s);
                const fx::RenderRecord rr = at<fx::RenderRecord>(records, s);
                FX_CHECK(rr.position[0] == pa.x && rr.age == pa.w && rr.emitter == at<uint32_t>(meta, s * 2), "tick %u: record of slot %u", t, s);
            }

            // 2. reference comparison of every live particle
            if (withReference)
            {
                auto ref = cpu.checkpoint();
                auto gpu = ps.checkpoint(shaders);
                std::sort(gpu.begin(), gpu.end(), [](const NV_StreamParticle& a, const NV_StreamParticle& b) { return a.emitter != b.emitter ? a.emitter < b.emitter : a.birth < b.birth; });
                FX_CHECK(gpu.size() == ref.size(), "tick %u: %zu GPU particles, %zu reference", t, gpu.size(), ref.size());
                std::vector<double> errs;
                errs.reserve(gpu.size());
                for (size_t i = 0; i < gpu.size(); ++i)
                {
                    const auto &g = gpu[i];
                    const auto& r = ref[i];
                    FX_CHECK(g.emitter == r.row && g.birth == r.birth, "tick %u: particle %zu identity GPU (%u,%u) reference (%u,%u)", t, i, g.emitter, g.birth, r.row, r.birth);
                    double dp = 0, pp = 0, dv = 0, vv = 0;
                    for (int a = 0; a < 3; ++a)
                    {
                        dp += (g.position[a] - r.position[a]) * (g.position[a] - r.position[a]);
                        pp += r.position[a] * r.position[a];
                        dv += (g.velocity[a] - r.velocity[a]) * (g.velocity[a] - r.velocity[a]);
                        vv += r.velocity[a] * r.velocity[a];
                    }
                    const double ep = std::sqrt(dp) / std::max(std::sqrt(pp), 1.0), ev = std::sqrt(dv) / std::max(std::sqrt(vv), 1.0);
                    const double ea = std::abs(g.age - r.age) / std::max(r.age, 1.0);
                    errs.push_back(ep);
                    if (ep > 1e-5 && worst.dumped < 5)
                    {
                        ++worst.dumped;
                        FX_LOG("  particle (%u,%u): GPU p (%.6f %.6f %.6f) v (%.6f %.6f %.6f) age %.6f | ref p (%.6f %.6f %.6f) v (%.6f %.6f %.6f) age %.6f", g.emitter, g.birth,
                               g.position[0], g.position[1], g.position[2], g.velocity[0], g.velocity[1], g.velocity[2], g.age, r.position[0], r.position[1],
                               r.position[2], r.velocity[0], r.velocity[1], r.velocity[2], r.age);
                    }
                    worst.maxPos = std::max(worst.maxPos, ep);
                    worst.maxVel = std::max(worst.maxVel, ev);
                    worst.maxAge = std::max(worst.maxAge, ea);
                    worst.sumPos += ep;
                    ++worst.n;
                }
                std::sort(errs.begin(), errs.end());
                const double p99 = errs.empty() ? 0 : errs[std::min(errs.size() - 1, (size_t)(errs.size() * 0.99))];
                worst.p99Pos = std::max(worst.p99Pos, p99);
                FX_LOG("tick %u: %zu particles, rows %u, event slots %u, collisions %u; position error max %.3g p99 %.3g, velocity %.3g, age %.3g", t,
                        gpu.size(), stream.rows(), h.event_slots, rb.counters.collision_events, errs.empty() ? 0.0 : errs.back(), p99, worst.maxVel, worst.maxAge);
            }
            // 4. state hash
            Sha256 hs;
            for (const auto* b : { &posAge, &velocity, &meta, &alive, &aliveList, &deadList, &dying, &keys, &vals, &records }) hs.update(b->data(), b->size());
            std::vector<NV_StreamEvent> ev = rb.events;
            std::sort(ev.begin() + h.event_slots, ev.end(), [](const NV_StreamEvent& a, const NV_StreamEvent& b) { return std::tie(a.emitter, a.birth) < std::tie(b.emitter, b.birth); });
            hs.update(ev.data(), ev.size() * sizeof(NV_StreamEvent));
            std::string hex;
            for (uint8_t b : hs.finish()) hex += format("%02x", b);
            hashes.push_back(hex);
            if (!withReference) FX_LOG("tick %u: alive %u, rows %u, event slots %u, collisions %u, state %s", t, nAlive, stream.rows(), h.event_slots, rb.counters.collision_events, hex.substr(0, 16).c_str());
        }
    }
    childRows = stream.childRowsCreated();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    FX_LOG("run: %u ticks in %.1f s, CPU-assigned events %llu, collision events %llu (GPU vs reference count difference %llu), child rows %llu, max cascade depth %u",
            o.ticks, seconds, (unsigned long long)events, (unsigned long long)collisions, (unsigned long long)collisionDiff, (unsigned long long)childRows, stream.maxDepth());
    return hashes;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        Options o;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--ticks") o.ticks = (uint32_t)std::stoul(next());
            else if (a == "--compare-every") o.compareEvery = (uint32_t)std::stoul(next());
            else if (a == "--particles") o.rpp.particles = (uint32_t)std::stoul(next());
            else if (a == "--emitters") o.rpp.emitters = (uint32_t)std::stoul(next());
            else if (a == "--no-features") o.rpp.features = false;
            else if (a == "--no-reference") o.reference = false;
            else if (a == "--determinism") o.determinism = true;
            else if (a == "--warp") o.warp = true;
            else if (a == "--no-debug-layer") o.debugLayer = false;
            else if (a == "--gbv") o.gbv = true;
            else if (a == "--yield") o.yield = true;
            else if (a == "--allow-copy-drift") o.strictCopies = false;
            else if (a == "--no-box") o.rpp.boxShape = false;
            else fail("unknown option %s", a.c_str());
        }
        checkStreamCopies(o.strictCopies);

        DeviceOptions opts;
        ComPtr<ID3D12Device> warpDevice;
        if (o.warp)
        {
            // WARP: another D3D12 implementation (CPU rasterizer/compute, 4-lane waves). The module's Device runs on it as an
            // external device (no debug layer on external devices, INTERFACES 4.1).
            ComPtr<IDXGIFactory6> factory;
            check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
            // WARP's highest feature level is 12_1, but it has every capability the renderer requires (SM 6.8, mesh
            // shaders, DXR 1.1, enhanced barriers, binding tier 3, heap tier 2), which Device checks one by one.
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&warpDevice))))
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&warpDevice)), "WARP device");
            opts.externalDevice = warpDevice.Get();
            opts.debugLayer = false;
        }
        else
        {
            opts.debugLayer = o.debugLayer;
            opts.gpuValidation = o.gbv;
        }
        Device device(opts);
        FX_LOG("device: %s (%s)", device.caps().adapter.c_str(), o.warp ? "WARP" : "hardware");

        Stats worst;
        const auto a = run(device, o, o.reference, worst);
        if (o.reference)
        {
            FX_LOG("reference comparison: %llu particle states, position error max %.3g (p99 %.3g, mean %.3g), velocity %.3g, age %.3g", (unsigned long long)worst.n,
                    worst.maxPos, worst.p99Pos, worst.n ? worst.sumPos / worst.n : 0.0, worst.maxVel, worst.maxAge);
            FX_CHECK(worst.maxPos <= 1e-5, "position error %.3g exceeds 1e-5", worst.maxPos);
        }
        if (o.determinism)
        {
            Stats unused;
            const auto b = run(device, o, false, unused);
            FX_CHECK(a.size() == b.size(), "determinism: compare ticks differ");
            for (size_t i = 0; i < a.size(); ++i) FX_CHECK(a[i] == b[i], "determinism: state %zu differs between runs (%s vs %s)", i, a[i].c_str(), b[i].c_str());
            FX_LOG("determinism: %zu compare ticks bit identical between two runs", a.size());
        }
        FX_LOG("FX particle tests PASS");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
