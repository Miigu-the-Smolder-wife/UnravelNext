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

uint32_t g_watchEmitter = UINT32_MAX, g_watchBirth = 0;  // diagnostic: --watch E B logs that particle's error at every compare

struct Options
{
    uint32_t ticks = 600, compareEvery = 60;
    fx::test::RppConfig rpp;
    std::string record, replay, overflowDump;
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
constexpr const char* kStreamCommit = "92aada32";
void checkStreamCopies(bool strict)
{
    const fs::path mine = fs::path(UNX_SOURCE_DIR) / "Native/Render/Passes/FX/Stream";
    const fs::path original = fs::path(UNX_SOURCE_DIR) / "../Unravel/Native/NativeVfx";
    struct Pin { const char* file; const char* sha; };
    const Pin pins[] = { { "include/NativeVfxStream.h", "6c7a6ce17dcf1bbe3a2743aad25b1f7b2275b785a01fb4e4d14465678d236201" },
                         { "shaders/VfxParticleMath.hlsli", "54cee51763671e0742087c1eabc294b77e86359211b0f3369004ee1ba81efb64" },
                         { "src/VfxStreamCpu.h", "dfb1165d13171b95b10e72ab91fbcf5a86f65b78632e775b9e00349f8e07b3ee" } };
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

// Sequential ribbon strips in double (the old VfxRibbonShader walk) on the GPU's points of one range: the reference of
// the parallel strip scan (FxRibbon.hlsl). Returns per point {vertex 0, vertex 1, uv.x, link}.
struct D3 { double x, y, z; };
D3 operator+(D3 a, D3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
D3 operator-(D3 a, D3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
D3 operator*(D3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double dotd(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 crossd(D3 a, D3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double lend(D3 a) { return std::sqrt(dotd(a, a)); }
D3 unitd(D3 a) { const double l = lend(a); return l > 0 ? a * (1 / l) : a; }
D3 initiald(D3 tangent, D3 normal)
{
    D3 side = crossd(tangent, normal);
    if (lend(side) < 1e-6)
    {
        const D3 a = { std::abs(tangent.x), std::abs(tangent.y), std::abs(tangent.z) };
        const D3 alt = a.x <= a.y && a.x <= a.z ? D3{ 1, 0, 0 } : (a.y <= a.z ? D3{ 0, 1, 0 } : D3{ 0, 0, 1 });
        side = crossd(tangent, alt);
    }
    return unitd(side);
}
D3 transportd(D3 before, D3 after, D3 side)
{
    const double c = std::clamp(dotd(before, after), -1.0, 1.0);
    if (c > -1.0 + 1e-6)
    {
        const D3 axis = crossd(before, after), first = crossd(axis, side);
        side = side + first + crossd(axis, first) * (1.0 / (1.0 + c));
    }
    side = side - after * dotd(side, after);
    return lend(side) > 1e-6 ? unitd(side) : initiald(after, before);
}
struct RibbonRef { D3 v0, v1; double u; uint32_t link; bool written; };
// A break test |segment| <= limit on float points is a threshold on a continuous value: within a few float ulps of the
// limit, float and double arithmetic (and two GPUs) may decide differently, and no implementation can promise
// otherwise. There (|segment - limit| <= 1e-6 limit) the walk adopts the GPU's decision (link of the later point) and
// counts it; every other decision and all geometry are compared exactly as before.
std::vector<RibbonRef> ribbonReference(const std::vector<D3>& p, const std::vector<double>& width, uint32_t base, D3 normal, double limit, double uvScale,
                                       const std::vector<uint8_t>& gpuLinks, uint32_t& ambiguous)
{
    auto gpuLinked = [&](uint32_t later, uint32_t earlier) {
        uint32_t l;
        std::memcpy(&l, gpuLinks.data() + (size_t)(base + later) * 4, 4);
        return l == base + earlier;
    };
    auto broken = [&](double length, uint32_t later, uint32_t earlier) {
        if (std::abs(length - limit) <= 1e-6 * limit) { ++ambiguous; return !gpuLinked(later, earlier); }
        return length > limit;
    };
    const uint32_t n = (uint32_t)p.size();
    std::vector<RibbonRef> out(n);
    for (uint32_t j = 0; j < n; ++j) out[j] = { p[j], p[j], 0, 0xFFFFFFFFu, false };
    auto last = [&](uint32_t i) { while (i + 1 < n && p[i + 1].x == p[i].x && p[i + 1].y == p[i].y && p[i + 1].z == p[i].z) ++i; return i; };
    D3 prior = normal, side = normal;
    double distance = 0;
    uint32_t previous = 0xFFFFFFFFu;
    if (!n) return out;
    uint32_t cur = last(0);
    while (cur < n)
    {
        const uint32_t next = cur + 1 < n ? last(cur + 1) : n;
        bool before = previous != 0xFFFFFFFFu, after = next < n;
        D3 in{}, outv{};
        double seg = 0;
        if (before) { in = p[cur] - p[previous]; seg = lend(in); if (broken(seg, cur, previous)) before = false; else in = in * (1 / seg); }
        if (after) { outv = p[next] - p[cur]; const double span = lend(outv); if (broken(span, next, cur)) after = false; else outv = outv * (1 / span); }
        if (before || after)
        {
            const D3 t = before ? (after ? (lend(in + outv) > 1e-12 ? unitd(in + outv) : outv) : in) : outv;
            if (before) { side = transportd(prior, t, side); distance += seg; out[cur].link = base + previous; }
            else { distance = 0; side = initiald(t, normal); }
            prior = t;
            const double h = 0.5 * width[cur];
            out[cur].v0 = p[cur] - side * h;
            out[cur].v1 = p[cur] + side * h;
            out[cur].u = distance / uvScale;
            out[cur].written = true;
        }
        previous = cur;
        cur = next;
    }
    return out;
}

// Whole emitter table after a packet (a delta updates the listed rows; unsent rows lose their per-tick fields).
void applyEmitters(std::vector<NV_StreamEmitter>& table, const std::vector<uint8_t>& packet)
{
    // the stream's own table model (blocks, patches, per-tick fields of unsent rows read as absent)
    const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet.data());
    nv_stream::apply_emitter_table(table, h, packet.data(), packet.size());
}

template <typename T> T at(const std::vector<uint8_t>& b, size_t i) { T v; std::memcpy(&v, b.data() + i * sizeof(T), sizeof(T)); return v; }

struct Stats
{
    double maxPos = 0, maxVel = 0, maxAge = 0, sumPos = 0;
    uint64_t n = 0;
    double p99Pos = 0, maxEvent = 0, maxRibbon = 0;
    uint64_t ribbonPoints = 0;
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
    std::vector<uint8_t> firstPacket;
    std::vector<NV_StreamEmitter> emitterTable;  // whole table after each packet
    std::vector<std::string> hashes;
    uint64_t events = 0, collisions = 0, collisionDiff = 0, childRows = 0, impactOverflowTicks = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t t = 1; t <= o.ticks; ++t)
    {
        waitIfMeasuring(o.yield);
        const std::vector<uint8_t> packet = stream.next(t == 1 ? nullptr : &previous);
        if (t == 1) firstPacket = packet;
        applyEmitters(emitterTable, packet);
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
        // NV_STREAM_STATUS_IMPACT_OVERFLOW (1) is a reported condition of the contract (a particle needed a fifth impact in
        // one interval and stays at its fourth contact); the reference must report it in the same ticks. Any other bit
        // is a defect.
        FX_CHECK((rb.counters.status & ~1u) == 0, "tick %u: GPU status 0x%x (1 impact overflow, 2 nonfinite, 4 alive mismatch, 8 capacity, 16 range, 32 watchdog)", t, rb.counters.status);
        if (rb.counters.status & 1u) ++impactOverflowTicks;
        if ((rb.counters.status & 1u) && !o.overflowDump.empty())
        {
            // the inputs of every overflowing nv_integrate call of this tick (Particles.hlsli OverflowRecord, 432 B each)
            const auto counters = ps.readState("counters"), records = ps.readState("overflow");
            const uint32_t n = std::min<uint32_t>(at<uint32_t>(counters, 10), (uint32_t)(records.size() / 432));
            std::vector<uint8_t> b(4 + (size_t)n * 432);
            std::memcpy(b.data(), &n, 4);
            std::memcpy(b.data() + 4, records.data(), (size_t)n * 432);
            fs::create_directories(o.overflowDump);
            writeFile(tickFile(o.overflowDump, "overflow", t), b.data(), b.size());
            FX_LOG("tick %u: IMPACT_OVERFLOW in %u slots (inputs dumped)", t, at<uint32_t>(counters, 10));
        }
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
            FX_CHECK((rc.status & ~1u) == 0, "tick %u: reference status 0x%x", t, rc.status);
            if ((rc.status & 1u) != (rb.counters.status & 1u)) FX_LOG("tick %u: impact overflow reported by %s only", t, (rc.status & 1u) ? "the reference" : "the GPU");
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
            const auto dying = ps.readState("dyingList"), posAgePrev = ps.readState("posAgePrev");
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
                // interpolation pair: a slot that existed in the previous tick (its input age is not a new birth's -elapsed)
                // is one dt older now
                const float4 pv = at<float4>(posAgePrev, s);
                if (h.dt > 0 && !std::signbit(pv.w))
                    FX_CHECK(std::abs(((double)pa.w - (double)pv.w) - h.dt) <= 1e-5 * std::max<double>(pa.w, 1.0), "tick %u: slot %u previous age %.9g, age %.9g, dt %.9g",
                             t, s, pv.w, pa.w, h.dt);
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
                    if (g.emitter == g_watchEmitter && g.birth == g_watchBirth)
                        FX_LOG("  watch tick %u particle (%u,%u): position error %.3g, velocity error %.3g", t, g.emitter, g.birth, ep, ev);
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
            // tick surfaces: the GPU's float anchor-space body surfaces against the double transform of the same inputs
            // (reported, not checked: the stream defines surfaces in float anchor space)
            if (h.body_count)
            {
                const NV_StreamHeader& h0 = *reinterpret_cast<const NV_StreamHeader*>(firstPacket.data());
                const auto* table0 = reinterpret_cast<const NV_StreamSurface*>(firstPacket.data() + h0.surfaces);
                const auto* bodies = reinterpret_cast<const NV_StreamBody*>(packet.data() + h.bodies);
                const auto gpuSurfaces = ps.readState("tickSurfaces");
                double worstAbs = 0, worstAt = 0;
                for (uint32_t i = 0; i < h0.surface_count; ++i)
                {
                    const NV_StreamSurface& s = table0[i];
                    if (s.body == NV_STREAM_NONE) continue;
                    const NV_StreamBody& b = bodies[s.body];
                    const double qx = b.rotation[0], qy = b.rotation[1], qz = b.rotation[2], qw = b.rotation[3];
                    NV_StreamSurface g;
                    std::memcpy(&g, gpuSurfaces.data() + (size_t)i * sizeof(NV_StreamSurface), sizeof g);
                    for (int v = 0; v < 3; ++v)
                    {
                        const float* local = v == 0 ? s.a : v == 1 ? s.b : s.c;
                        const float* gpuPoint = v == 0 ? g.a : v == 1 ? g.b : g.c;
                        const D3 u = { qx, qy, qz }, x = { local[0], local[1], local[2] };
                        const D3 tq = crossd(u, x) * 2.0, r = x + tq * qw + crossd(u, tq);
                        // tick surface = offsets from the centre of mass (NativeVfxStream.h)
                        const D3 p = { r.x + ((double)b.position[0] - b.center[0]), r.y + ((double)b.position[1] - b.center[1]),
                                       r.z + ((double)b.position[2] - b.center[2]) };
                        const double e = lend(D3{ gpuPoint[0] - p.x, gpuPoint[1] - p.y, gpuPoint[2] - p.z });
                        if (e > worstAbs) { worstAbs = e; worstAt = std::max(lend(p), 1e-30); }
                    }
                }
                FX_LOG("tick %u: body surface offsets GPU float vs double: max error %.3g m at |offset| %.3f m (%.2f ulp of |offset|)", t, worstAbs, worstAt,
                       worstAt > 0 ? worstAbs / std::ldexp(1.0, std::ilogb(worstAt) - 23) : 0.0);
            }
            // geometry outputs: every live ribbon particle's point sits at output_base + (birth - death_birth) with its state;
            // every live volume particle's grid^3 cells are finite, non-empty cuboids
            if (h.ribbon_points || h.medium_cells)
            {
                const NV_StreamEmitter* table = emitterTable.data();
                const auto points = ps.readState("ribbonPoints"), cellsBuf = ps.readState("mediumCells"), vertices = ps.readState("ribbonVertices");
                uint32_t checkedPoints = 0, checkedCells = 0;
                for (uint32_t i = 0; i < nAlive; ++i)
                {
                    const uint32_t slot = at<uint32_t>(aliveList, i), row = at<uint32_t>(meta, slot * 2), birth = at<uint32_t>(meta, slot * 2 + 1);
                    const NV_StreamEmitter& e = table[row];
                    const float4 pa = at<float4>(posAge, slot);
                    const NV_StreamProgram& outProgram = reinterpret_cast<const NV_StreamProgram*>(
                        firstPacket.data() + reinterpret_cast<const NV_StreamHeader*>(firstPacket.data())->programs)[e.program];
                    if (outProgram.output == 2)
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
                    if (outProgram.output == 3)
                    {
                        const uint32_t g = outProgram.medium_grid, cellsPer = g * g * g;
                        const uint32_t k = e.output_base + (birth - e.death_birth) * cellsPer;
                        for (uint32_t c = 0; c < cellsPer; ++c)
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
                // strips: the parallel scan against the sequential walk in double, per ribbon range
                const auto links = ps.readState("ribbonLinks");
                const auto* programTable = reinterpret_cast<const NV_StreamProgram*>(firstPacket.data() + reinterpret_cast<const NV_StreamHeader*>(firstPacket.data())->programs);
                double worstRibbon = 0;
                uint32_t ambiguousBreaks = 0;
                for (uint32_t r = 0; r < h.emitter_table; ++r)
                {
                    const NV_StreamEmitter& e = table[r];
                    const uint32_t count = e.next_birth - e.death_birth;
                    if (!(e.flags & NV_STREAM_EMITTER_ACTIVE) || !count || programTable[e.program].output != 2) continue;
                    const NV_StreamProgram& pr = programTable[e.program];
                    std::vector<D3> pts(count);
                    std::vector<double> widths(count);
                    for (uint32_t j = 0; j < count; ++j)
                    {
                        float q[8];
                        std::memcpy(q, points.data() + (size_t)(e.output_base + j) * 32, 32);
                        pts[j] = { q[0], q[1], q[2] };
                        widths[j] = q[3];
                    }
                    const auto ref = ribbonReference(pts, widths, e.output_base, { pr.ribbon_normal[0], pr.ribbon_normal[1], pr.ribbon_normal[2] }, pr.ribbon_break,
                                                     pr.ribbon_uv > 0 ? pr.ribbon_uv : 1.0, links, ambiguousBreaks);
                    for (uint32_t j = 0; j < count; ++j)
                    {
                        const uint32_t k = e.output_base + j;
                        FX_CHECK(at<uint32_t>(links, k) == ref[j].link, "tick %u: ribbon point %u link %u, reference %u", t, k, at<uint32_t>(links, k), ref[j].link);
                        if (!ref[j].written) continue;
                        float v[16];
                        std::memcpy(v, vertices.data() + (size_t)k * 64, 64);
                        const D3 g0 = { v[0], v[1], v[2] }, g1 = { v[8], v[9], v[10] };
                        const double scale = std::max(lend(pts[j]), 1.0);
                        const double err = std::max({ lend(g0 - ref[j].v0) / scale, lend(g1 - ref[j].v1) / scale, std::abs(v[6] - ref[j].u) / std::max(ref[j].u, 1.0) });
                        worstRibbon = std::max(worstRibbon, err);
                    }
                    worst.ribbonPoints += count;
                }
                worst.maxRibbon = std::max(worst.maxRibbon, worstRibbon);
                FX_LOG("tick %u: ribbon strips vs sequential double walk: links exact (%u break tests within 1e-6 of the limit took the GPU's decision), vertex/uv error max %.3g",
                       t, ambiguousBreaks, worstRibbon);
                FX_CHECK(worstRibbon <= 1e-5, "tick %u: ribbon vertex error %.3g exceeds 1e-5", t, worstRibbon);
            }
            // 4. state hash of the defined state: every slot's alive flag, the live slots' identity and state (this tick and
            // the previous tick, in alive-list order), and each list up to its count. Bytes past a count (a list's stale
            // tail, a dead slot's state) are undefined: they depend on the memory a buffer received and are never read.
            Sha256 hs;
            std::string parts;
            std::vector<uint8_t> live[4];
            for (uint32_t i = 0; i < nAlive; ++i)
            {
                const uint32_t s = at<uint32_t>(aliveList, i);
                const std::vector<uint8_t>* src[4] = { &posAge, &velocity, &meta, &posAgePrev };
                const size_t stride[4] = { 16, 16, 8, 16 };
                for (int k = 0; k < 4; ++k) live[k].insert(live[k].end(), src[k]->data() + s * stride[k], src[k]->data() + (s + 1) * stride[k]);
            }
            auto prefix = [](const std::vector<uint8_t>& b, uint32_t n) { return std::vector<uint8_t>(b.begin(), b.begin() + std::min<size_t>(b.size(), (size_t)n * 4)); };
            const std::vector<uint8_t> parts2[] = { live[0], live[1], live[2], prefix(alive, cap), prefix(aliveList, nAlive), prefix(deadList, nDead),
                                                    prefix(dying, nDying), prefix(keys, nAlive), prefix(vals, nAlive), live[3] };
            for (const auto& b : parts2)
            {
                hs.update(b.data(), b.size());
                Sha256 one;
                one.update(b.data(), b.size());
                const auto d = one.finish();
                parts += format(" %02x%02x", d[0], d[1]);  // per-part fingerprint (posAge velocity meta alive aliveList dead dying keys vals prev)
            }
            if (o.determinism) FX_LOG("tick %u: buffer fingerprints%s", t, parts.c_str());
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
    FX_LOG("stream: capacity changes %u (repacks without RESET), final capacity %u, peak rows %u, ticks with impact overflow %llu", stream.capacityChanges(),
           stream.capacity(), stream.rows(), (unsigned long long)impactOverflowTicks);
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
        FX_CHECK((rc.status & ~1u) == 0, "tick %u: reference status 0x%x", t, rc.status);
        FX_CHECK((gc.status & ~1u) == 0 && gc.alive == rc.alive, "tick %u: GPU alive %u status 0x%x, reference alive %u", t, gc.alive, gc.status, rc.alive);
        if ((rc.status & 1u) != (gc.status & 1u)) FX_LOG("tick %u: impact overflow reported by %s only", t, (rc.status & 1u) ? "the reference" : "the GPU");
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
        uint32_t ribbonPoints = 0;
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
            else if (a == "--overflow-dump") o.overflowDump = next();
            else if (a == "--ribbon-points") ribbonPoints = (uint32_t)std::stoul(next());
            else if (a == "--watch") { g_watchEmitter = (uint32_t)std::stoul(next()); g_watchBirth = (uint32_t)std::stoul(next()); }
            else if (a == "--anchor-shift") { o.rpp.anchorShift[0] = std::stod(next()); o.rpp.anchorShift[1] = std::stod(next()); o.rpp.anchorShift[2] = std::stod(next()); }
            else fail("unknown option %s", a.c_str());
        }
        // a reduced root population (WARP, short runs) reduces the ribbon points and volume particles in the same ratio;
        // the structure (256 ribbons, 16 volumes) stays
        if (o.rpp.particles < 524288u)
        {
            const double f = o.rpp.particles / 524288.0;
            o.rpp.ribbonPoints = std::max<uint32_t>(8u, (uint32_t)(o.rpp.ribbonPoints * f));
            o.rpp.volumeParticles = std::max<uint32_t>(8u, (uint32_t)(o.rpp.volumeParticles * f));
        }
        if (ribbonPoints) o.rpp.ribbonPoints = ribbonPoints;  // e.g. long ribbons (several strip chunks) at a small root count
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
