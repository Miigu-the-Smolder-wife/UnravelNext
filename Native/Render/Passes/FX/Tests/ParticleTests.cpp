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
//          --record DIR: GPU run only (inside the GPU lock); writes every packet, every tick's readback and the sorted
//                        checkpoints of the compare ticks to DIR
//          --replay DIR: CPU only (outside the lock); runs the recorded packets through the reference executor and
//                        compares with the recorded GPU events and checkpoints (the same items as the in-process run)
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
    std::string record, replay;
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

// 1. byte-identical stream copies of the pinned NativeVfx commit (the copies are updated together with this pin)
constexpr const char* kStreamCommit = "eca421d7";
void checkStreamCopies(bool strict)
{
    const fs::path mine = fs::path(UNX_SOURCE_DIR) / "Native/Render/Passes/FX/Stream";
    const fs::path original = fs::path(UNX_SOURCE_DIR) / "../Unravel/Native/NativeVfx";
    struct Pin { const char* file; const char* sha; };
    const Pin pins[] = { { "include/NativeVfxStream.h", "af9a7e0a475f9b4e8d30288da3e9797ab15c160bd0f124dc41cc6271bf9c6687" },
                         { "shaders/VfxParticleMath.hlsli", "6586633b3e631f2c39c4a06f4072daad8b25a61c2c56ff46b537f3e5617448a2" },
                         { "src/VfxStreamCpu.h", "fea7de8484e0fd44963bb25e0d63a2d850e991941faa48b267e3916c8769e6b8" } };
    for (const Pin& pin : pins)
    {
        const std::string a = sha(readBinaryFile(mine / pin.file));
        if (a != pin.sha && !strict) { FX_LOG("WARNING stream copy %s is not the pinned NativeVfx %s version", pin.file, kStreamCommit); continue; }
        FX_CHECK(a == pin.sha, "stream copy %s sha256 %s is not the pinned NativeVfx %s version %s", pin.file, a.c_str(), kStreamCommit, pin.sha);
        const bool present = fs::exists(original / pin.file);
        const std::string b = present ? sha(readBinaryFile(original / pin.file)) : std::string();
        FX_LOG("stream copy %s = NativeVfx %s (sha256 %.16s)%s", pin.file, kStreamCommit, a.c_str(),
               !present ? ", original not present" : (b == a ? "" : ", the NativeVfx working tree has a newer edit"));
    }
}

// CPU-heavy runs (WARP, the CPU reference) pause while a timing measurement holds the GPU lock (current.json kind
// "timing", or no kind) and while the user's HOLD exists; a correctness holder does not stop them (GpuLock v1.28).
bool timingHeld(const fs::path& dir)
{
    const fs::path current = dir / "current.json";
    if (!fs::exists(current)) return false;
    try
    {
        const std::vector<uint8_t> b = readBinaryFile(current);
        const std::string text(b.begin(), b.end());
        return text.find("\"kind\":\"correctness\"") == std::string::npos && text.find("\"kind\": \"correctness\"") == std::string::npos;
    }
    catch (...)
    {
        return true;
    }
}
void waitIfMeasuring(bool yield)
{
    if (!yield) return;
    const fs::path dir = fs::path(UNX_SOURCE_DIR) / ".gpulock";
    bool said = false;
    while (timingHeld(dir) || fs::exists(dir / "HOLD"))
    {
        if (!said) FX_LOG("yield: a GPU timing measurement or HOLD is active; pausing");
        said = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void writeFile(const fs::path& p, const void* data, size_t bytes)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.wstring().c_str(), L"wb") || !f) fail("cannot write %s", p.string().c_str());
    if (bytes && std::fwrite(data, 1, bytes, f) != bytes) fail("short write %s", p.string().c_str());
    std::fclose(f);
}
template <typename T> std::vector<T> readVector(const fs::path& p)
{
    const std::vector<uint8_t> b = readBinaryFile(p);
    std::vector<T> v(b.size() / sizeof(T));
    if (!v.empty()) std::memcpy(v.data(), b.data(), v.size() * sizeof(T));
    return v;
}
fs::path tickFile(const std::string& dir, const char* kind, uint32_t t) { return fs::path(dir) / format("%s_%04u.bin", kind, t); }

template <typename T> T at(const std::vector<uint8_t>& b, size_t i) { T v; std::memcpy(&v, b.data() + i * sizeof(T), sizeof(T)); return v; }

struct Stats
{
    double maxPos = 0, maxVel = 0, maxAge = 0, sumPos = 0;
    uint64_t n = 0;
    double p99Pos = 0, maxEvent = 0;
    uint64_t over = 0;
    int dumped = 0, dumpedEvents = 0;
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
        if (!o.record.empty()) writeFile(tickFile(o.record, "packet", t), packet.data(), packet.size());

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
        if (!o.record.empty())
        {
            std::vector<uint8_t> b(sizeof(NV_StreamCounters) + rb.events.size() * sizeof(NV_StreamEvent));
            std::memcpy(b.data(), &rb.counters, sizeof(NV_StreamCounters));
            if (!rb.events.empty()) std::memcpy(b.data() + sizeof(NV_StreamCounters), rb.events.data(), rb.events.size() * sizeof(NV_StreamEvent));
            writeFile(tickFile(o.record, "readback", t), b.data(), b.size());
        }
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
                double dv = 0, vv = 0;
                for (int a = 0; a < 3; ++a) { dv += (g.velocity[a] - r.velocity[a]) * (double)(g.velocity[a] - r.velocity[a]); vv += (double)r.velocity[a] * r.velocity[a]; }
                const double ev = std::sqrt(dv) / std::max(std::sqrt(vv), 1.0);
                if (ev > 1e-4 && worst.dumpedEvents < 6)
                {
                    ++worst.dumpedEvents;
                    FX_LOG("  tick %u event %u (emitter %u birth %u kind %u): GPU p (%.6f %.6f %.6f) v (%.6f %.6f %.6f) | ref p (%.6f %.6f %.6f) v (%.6f %.6f %.6f)", t, i, g.emitter,
                           g.birth, g.kind, g.position[0], g.position[1], g.position[2], g.velocity[0], g.velocity[1], g.velocity[2], r.position[0], r.position[1],
                           r.position[2], r.velocity[0], r.velocity[1], r.velocity[2]);
                }
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

            if (!o.record.empty())
            {
                auto gpu = ps.checkpoint(shaders);
                std::sort(gpu.begin(), gpu.end(), [](const NV_StreamParticle& a, const NV_StreamParticle& b) { return a.emitter != b.emitter ? a.emitter < b.emitter : a.birth < b.birth; });
                writeFile(tickFile(o.record, "checkpoint", t), gpu.data(), gpu.size() * sizeof(NV_StreamParticle));
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
            // geometry outputs: every live ribbon particle's point sits at output_base + (birth - death_birth) with its state;
            // every live volume particle's grid^3 cells are finite, non-empty cuboids
            if (h.ribbon_points || h.medium_cells)
            {
                const auto* table = reinterpret_cast<const NV_StreamEmitter*>(packet.data() + h.emitters);
                const auto points = ps.readState("ribbonPoints"), cellsBuf = ps.readState("mediumCells"), vertices = ps.readState("ribbonVertices");
                uint32_t checkedPoints = 0, checkedCells = 0;
                for (uint32_t i = 0; i < nAlive; ++i)
                {
                    const uint32_t slot = at<uint32_t>(aliveList, i), row = at<uint32_t>(meta, slot * 2), birth = at<uint32_t>(meta, slot * 2 + 1);
                    const NV_StreamEmitter& e = table[row];
                    const float4 pa = at<float4>(posAge, slot);
                    if (e.program == 13)
                    {
                        const uint32_t k = e.output_base + (birth - e.death_birth);
                        FX_CHECK(k < h.ribbon_points, "tick %u: ribbon point %u outside %u", t, k, h.ribbon_points);
                        float p[8];
                        std::memcpy(p, points.data() + (size_t)k * 32, 32);
                        FX_CHECK(p[0] == pa.x && p[1] == pa.y && p[2] == pa.z && p[4] == pa.w, "tick %u: ribbon point %u of (%u,%u) is not its state", t, k, row, birth);
                        float v[8];
                        std::memcpy(v, vertices.data() + (size_t)k * 64, 32);
                        FX_CHECK(std::isfinite(v[0]) && std::isfinite(v[3]) && std::isfinite(v[6]), "tick %u: ribbon vertex %u not finite", t, k);
                        ++checkedPoints;
                    }
                    if (e.program == 14)
                    {
                        const uint32_t k = e.output_base + (birth - e.death_birth) * 8;
                        for (uint32_t c = 0; c < 8; ++c)
                        {
                            float cell[24];
                            std::memcpy(cell, cellsBuf.data() + ((size_t)k + c) * 96, 96);
                            FX_CHECK(cell[4] < cell[8] && cell[5] < cell[9] && cell[6] < cell[10] && std::isfinite(cell[12]) && cell[16] >= 0,
                                     "tick %u: medium cell %u of (%u,%u) (first cell %u; row output_base %u death %u next %u; header cells %u) is not a finite cuboid: low %g %g %g high %g %g %g absorption %g scattering %g",
                                     t, c, row, birth, k, e.output_base, e.death_birth, e.next_birth, h.medium_cells, cell[4], cell[5], cell[6], cell[8], cell[9], cell[10], cell[12], cell[16]);
                        }
                        ++checkedCells;
                    }
                }
                FX_LOG("tick %u: outputs checked: %u ribbon points (of %u), %u volume particles (%u cells)", t, checkedPoints, h.ribbon_points, checkedCells, h.medium_cells);
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
    FX_LOG("stream: capacity changes %u (repacks without RESET), final capacity %u", stream.capacityChanges(), stream.capacity());
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    FX_LOG("run: %u ticks in %.1f s, CPU-assigned events %llu, collision events %llu (GPU vs reference count difference %llu), child rows %llu, max cascade depth %u",
            o.ticks, seconds, (unsigned long long)events, (unsigned long long)collisions, (unsigned long long)collisionDiff, (unsigned long long)childRows, stream.maxDepth());
    return hashes;
}
// CPU half of a recorded run: the reference executor on the recorded packets, compared with the recorded GPU results.
void replay(const Options& o, Stats& worst)
{
    nv_stream::CpuExecutor cpu;
    uint64_t collisionDiff = 0, eventsCompared = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t t = 1; t <= o.ticks; ++t)
    {
        const std::vector<uint8_t> packet = readBinaryFile(tickFile(o.replay, "packet", t));
        const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet.data());
        cpu.submit(packet.data(), packet.size());
        const std::vector<uint8_t> rbBytes = readBinaryFile(tickFile(o.replay, "readback", t));
        NV_StreamCounters gc;
        std::memcpy(&gc, rbBytes.data(), sizeof gc);
        std::vector<NV_StreamEvent> ge((rbBytes.size() - sizeof gc) / sizeof(NV_StreamEvent));
        if (!ge.empty()) std::memcpy(ge.data(), rbBytes.data() + sizeof gc, ge.size() * sizeof(NV_StreamEvent));
        const NV_StreamCounters& rc = cpu.counters();
        FX_CHECK(rc.status == 0, "tick %u: reference status 0x%x", t, rc.status);
        FX_CHECK(gc.status == 0 && gc.alive == rc.alive, "tick %u: GPU alive %u status 0x%x, reference alive %u", t, gc.alive, gc.status, rc.alive);
        const auto& re = cpu.events();
        for (uint32_t i = 0; i < h.event_slots; ++i)
        {
            const NV_StreamEvent &g = ge[i], &r = re[i];
            FX_CHECK(g.emitter == r.emitter && g.birth == r.birth && g.kind == r.kind, "tick %u event %u: identity GPU (%u,%u,%u) reference (%u,%u,%u)", t, i, g.emitter,
                     g.birth, g.kind, r.emitter, r.birth, r.kind);
            double dp = 0, pp = 0;
            for (int a = 0; a < 3; ++a) { dp += (g.position[a] - r.position[a]) * (double)(g.position[a] - r.position[a]); pp += (double)r.position[a] * r.position[a]; }
            const double e = std::sqrt(dp) / std::max(std::sqrt(pp), 1.0);
            worst.maxEvent = std::max(worst.maxEvent, e);
            ++eventsCompared;
        }
        collisionDiff += (uint64_t)std::abs((int64_t)rc.collision_events - (int64_t)gc.collision_events);
        const fs::path cp = tickFile(o.replay, "checkpoint", t);
        if (!fs::exists(cp)) continue;
        const auto gpu = readVector<NV_StreamParticle>(cp);
        const auto ref = cpu.checkpoint();
        FX_CHECK(gpu.size() == ref.size(), "tick %u: %zu GPU particles, %zu reference", t, gpu.size(), ref.size());
        std::vector<double> errs;
        errs.reserve(gpu.size());
        for (size_t i = 0; i < gpu.size(); ++i)
        {
            const auto& g = gpu[i];
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
            errs.push_back(ep);
            if (ep > 1e-5 && worst.dumped < 8)
            {
                ++worst.dumped;
                FX_LOG("  tick %u particle (%u,%u): GPU p (%.6f %.6f %.6f) v (%.6f %.6f %.6f) | ref p (%.6f %.6f %.6f) v (%.6f %.6f %.6f) age %.4f", t, g.emitter, g.birth,
                       g.position[0], g.position[1], g.position[2], g.velocity[0], g.velocity[1], g.velocity[2], r.position[0], r.position[1], r.position[2],
                       r.velocity[0], r.velocity[1], r.velocity[2], r.age);
            }
            worst.maxPos = std::max(worst.maxPos, ep);
            worst.maxVel = std::max(worst.maxVel, ev);
            worst.maxAge = std::max(worst.maxAge, std::abs(g.age - r.age) / std::max(r.age, 1.0));
            worst.sumPos += ep;
            ++worst.n;
            if (ep > 1e-5) ++worst.over;
        }
        std::sort(errs.begin(), errs.end());
        const double p99 = errs.empty() ? 0 : errs[std::min(errs.size() - 1, (size_t)(errs.size() * 0.99))];
        worst.p99Pos = std::max(worst.p99Pos, p99);
        FX_LOG("replay tick %u: %zu particles, event slots %u, collisions GPU %u ref %u; position error max %.3g p99 %.3g (over 1e-5: %llu so far)", t, gpu.size(), h.event_slots,
               gc.collision_events, rc.collision_events, errs.empty() ? 0.0 : errs.back(), p99, (unsigned long long)worst.over);
    }
    FX_LOG("replay: %u ticks in %.1f s, %llu CPU-assigned events compared (position error max %.3g), collision count difference %llu", o.ticks,
           std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), (unsigned long long)eventsCompared, worst.maxEvent, (unsigned long long)collisionDiff);
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
            else if (a == "--record") { o.record = next(); o.reference = false; }
            else if (a == "--replay") o.replay = next();
            else if (a == "--yield") o.yield = true;
            else if (a == "--allow-copy-drift") o.strictCopies = false;
            else if (a == "--no-box") o.rpp.boxShape = false;
            else if (a == "--no-child-noise") o.rpp.childNoise = false;
            else if (a == "--fields") o.rpp.fields = (uint32_t)std::stoul(next());
            else fail("unknown option %s", a.c_str());
        }
        checkStreamCopies(o.strictCopies);
        if (!o.replay.empty())
        {
            Stats worst;
            replay(o, worst);
            FX_LOG("reference comparison: %llu particle states, position error max %.3g (p99 %.3g, mean %.3g, %llu over 1e-5), velocity %.3g, age %.3g, events %.3g",
                   (unsigned long long)worst.n, worst.maxPos, worst.p99Pos, worst.n ? worst.sumPos / worst.n : 0.0, (unsigned long long)worst.over, worst.maxVel, worst.maxAge,
                   worst.maxEvent);
            FX_CHECK(worst.maxPos <= 1e-5 && worst.maxEvent <= 1e-5, "position error %.3g / event %.3g exceeds 1e-5", worst.maxPos, worst.maxEvent);
            FX_LOG("FX particle replay PASS");
            return 0;
        }
        if (!o.record.empty()) fs::create_directories(o.record);

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
