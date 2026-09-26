// Mesh particles (A3, FEATURES_GAME 0.A 7b; render C): the instances FxMeshInstances writes into the GPU instance range
// against the VFX CPU reference executor (VfxStreamCpu.h, double) on the RPP stream with its colliding programs turned
// into mesh programs (executor version 4 orientations, ground contacts damping the spin), renderer axes (1, 1, -1).
// Per tick three frames, w = 0, 0.37, 1 (frame time between the previous tick's end and the latest's):
//   1. transforms: every instance against the reference particle evaluated by the render rules in double (Hermite
//      position, q(w) = transport^w exp(w0 w dt / 2) q0, births and deaths extrapolated, size x sizeScale x size curve);
//      instance count = live mesh particles at the frame time; the unmapped program's particles counted, not drawn;
//   2. tick-boundary continuity: the w = 0 frame of tick n + 1 draws what the w = 1 frame of tick n drew (spin carried
//      across contacts included);
//   3. previous transforms: w = 0 (a new tick, mode 2) = the previous frame's drawn records (+ the origin shift at the
//      tick it happens); w = 0.37 and 1 (same tick, mode 1) = the previous frame's transforms.
// Options: --ticks N (90) --particles P (16384) --emitters E (16)
#include "RppStream.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/fx/MeshParticles.h"
#include "unx/fx/Particles.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
#define MP_CHECK(cond, ...)                                                                                           \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

struct Q { double x, y, z, w; };
Q mul(Q a, Q b)
{
    return { a.w * b.x + b.w * a.x + (a.y * b.z - a.z * b.y), a.w * b.y + b.w * a.y + (a.z * b.x - a.x * b.z),
             a.w * b.z + b.w * a.z + (a.x * b.y - a.y * b.x), a.w * b.w - (a.x * b.x + a.y * b.y + a.z * b.z) };
}
Q normalized(Q q)
{
    const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return { q.x / n, q.y / n, q.z / n, q.w / n };
}
// exp(w h / 2) q (the stream's advance, exact in double)
Q advance(Q q, const double w[3], double h)
{
    const double speed = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), half = 0.5 * speed * h;
    const double s = speed > 0 ? std::sin(half) / speed : 0.5 * h;
    return normalized(mul({ w[0] * s, w[1] * s, w[2] * s, std::cos(half) }, q));
}
Q power(Q r, double t)
{
    if (r.w < 0) r = { -r.x, -r.y, -r.z, -r.w };
    const double s = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z), half = std::atan2(s, r.w) * t;
    const double k = s > 1e-300 ? std::sin(half) / s : t;
    return { r.x * k, r.y * k, r.z * k, std::cos(half) };
}
// rotation matrix of q in the renderer's axes: M R M
std::array<double, 9> rendererRotation(Q q, const double axes[3])
{
    const double x = q.x, y = q.y, z = q.z, w = q.w;
    const double R[9] = { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y), 2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                          2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) };
    std::array<double, 9> r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r[3 * i + j] = axes[i] * R[3 * i + j] * axes[j];
    return r;
}

struct Pose
{
    double position[3];
    std::array<double, 9> rotation;  // unit
    double scale;
};
// the instance's (current or previous) transform
Pose poseOf(const float4 rows[3])
{
    Pose p;
    const float* m = &rows[0].x;
    p.scale = std::sqrt((double)m[0] * m[0] + (double)m[4] * m[4] + (double)m[8] * m[8]);
    for (int i = 0; i < 3; ++i)
    {
        p.position[i] = m[4 * i + 3];
        for (int j = 0; j < 3; ++j) p.rotation[3 * i + j] = m[4 * i + j] / p.scale;
    }
    return p;
}
// rotation angle between two rotations: |A - B|_F = 2 sqrt(2) sin(angle / 2) (well conditioned at small angles, unlike
// acos of the trace, whose float floor is ~sqrt(eps))
double angle(const std::array<double, 9>& a, const std::array<double, 9>& b)
{
    double f = 0;
    for (int i = 0; i < 9; ++i) f += (a[i] - b[i]) * (a[i] - b[i]);
    return 2 * std::asin(std::min(1.0, std::sqrt(f) / (2 * std::sqrt(2.0))));
}
double norm(const double p[3]) { return std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]); }
double distance(const double a[3], const double b[3])
{
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
}

scene::Mesh casing()
{
    scene::Mesh m;
    m.name = "casing";
    const float3 half{ 0.006f, 0.006f, 0.02f };
    for (int k = 0; k < 8; ++k) m.positions.push_back({ (k & 1) ? half.x : -half.x, (k & 2) ? half.y : -half.y, (k & 4) ? half.z : -half.z });
    for (const float3& p : m.positions) m.normals.push_back(normalize(p));
    m.indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    return m;
}

ComPtr<ID3D12Resource> hostBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "test buffer");
    return r;
}

// Explicit copies around the scene's range buffers (they sit outside the graph): the frame's count zeroed before the
// writer (the scene update's job in a renderer), the range read back after it.
void rangeCopy(RenderGraph& graph, const GpuScene::GpuInstanceRange& range, ID3D12Resource* zero, ID3D12Resource* readback, bool clear)
{
    graph.addPass(clear ? "test.mesh.zero" : "test.mesh.readback", QueueType::Graphics, [](PassBuilder& b) { b.keep(); },
                  [=](PassContext& c) {
                      D3D12_BUFFER_BARRIER before[2], after[2];
                      ID3D12Resource* targets[2] = { range.instanceBuffer, range.countBuffer };
                      const D3D12_BARRIER_ACCESS access = clear ? D3D12_BARRIER_ACCESS_COPY_DEST : D3D12_BARRIER_ACCESS_COPY_SOURCE;
                      for (int k = 0; k < 2; ++k)
                      {
                          before[k] = { D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, access, targets[k], 0, UINT64_MAX };
                          after[k] = { D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_ALL_SHADING, access, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, targets[k], 0, UINT64_MAX };
                      }
                      D3D12_BARRIER_GROUP g{ D3D12_BARRIER_TYPE_BUFFER, 2 };
                      g.pBufferBarriers = before;
                      c.cmd->Barrier(1, &g);
                      if (clear) c.cmd->CopyBufferRegion(range.countBuffer, range.countByteOffset, zero, 0, 16);
                      else
                      {
                          c.cmd->CopyBufferRegion(readback, 0, range.countBuffer, range.countByteOffset, 16);
                          c.cmd->CopyBufferRegion(readback, 16, range.instanceBuffer, (uint64_t)range.first * sizeof(gpu::Instance), (uint64_t)range.capacity * sizeof(gpu::Instance));
                      }
                      g.pBufferBarriers = after;
                      c.cmd->Barrier(1, &g);
                  });
}

struct Drawn
{
    Pose now, before;
};
// nearest instance of a list by current position (grid of 5 mm cells)
struct Index
{
    std::unordered_map<int64_t, std::vector<uint32_t>> cells;
    const std::vector<Drawn>* list = nullptr;
    static int64_t key(int64_t x, int64_t y, int64_t z) { return (x * 73856093) ^ (y * 19349663) ^ (z * 83492791); }
    static int64_t cell(double v) { return (int64_t)std::floor(v / 0.005); }
    explicit Index(const std::vector<Drawn>& l) : list(&l)
    {
        for (uint32_t i = 0; i < l.size(); ++i) cells[key(cell(l[i].now.position[0]), cell(l[i].now.position[1]), cell(l[i].now.position[2]))].push_back(i);
    }
    uint32_t nearest(const double p[3], double& best) const
    {
        uint32_t found = UINT32_MAX;
        best = 1e30;
        const int64_t cx = cell(p[0]), cy = cell(p[1]), cz = cell(p[2]);
        for (int64_t dx = -1; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz)
                {
                    auto it = cells.find(key(cx + dx, cy + dy, cz + dz));
                    if (it == cells.end()) continue;
                    for (uint32_t i : it->second)
                    {
                        const double d = distance((*list)[i].now.position, p);
                        if (d < best) best = d, found = i;
                    }
                }
        return found;
    }
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        uint32_t ticks = 90;
        fx::test::RppConfig rpp;
        rpp.emitters = 16;
        rpp.particles = 16384;
        rpp.features = false;
        rpp.bodies = 0;
        rpp.heightfield = false;
        rpp.sheet = false;
        rpp.delta = false;  // whole emitter table in every packet (the test reads rows directly)
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--ticks") ticks = (uint32_t)std::stoul(next());
            else if (a == "--particles") rpp.particles = (uint32_t)std::stoul(next());
            else if (a == "--emitters") rpp.emitters = (uint32_t)std::stoul(next());
            else fail("unknown option %s", a.c_str());
        }
        DeviceOptions opts;
        opts.debugLayer = true;
        Device device(opts);
        logf("device: %s\n", device.caps().adapter.c_str());
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");

        scene::Scene s;
        s.name = "mesh particles";
        s.materials.resize(1);
        s.meshes = { casing() };
        scene::Instance anchorInstance;
        anchorInstance.mesh = 0;
        anchorInstance.transform.m[1][3] = -100;
        s.instances.push_back(anchorInstance);
        s.cameras.push_back(scene::Camera{});
        scene::validate(s);
        GpuScene scene(device);
        RuntimeCapacity rc;
        rc.gpuInstances = (uint32_t)quality.integer("fx.particles.mesh_instances_max");
        scene.reserveRuntime(rc);
        scene.upload(s);
        const GpuScene::GpuInstanceRange range = scene.gpuInstanceRange();
        MP_CHECK(range.capacity == rc.gpuInstances && range.capacity > 0, "GPU instance range %u", range.capacity);

        RenderGraph graph(device);
        TrackState state;
        fx::ParticleSystem& ps = fx::particles(state, device, quality);
        fx::test::RppStream stream(rpp);
        nv_stream::CpuExecutor cpu;
        // program 0's asset draws mesh 0; program 8's key is left unmapped (counted, not drawn)
        constexpr uint64_t kDrawnAsset = 0x4d455348u, kUnmappedAsset = 0x4d455348u + 8;
        fx::meshAssets(state) = { { kDrawnAsset, 0 } };

        const double axes[3] = { 1, 1, -1 };  // the Unity World's axes
        FrameContext frame;
        frame.deltaTime = 1.0f / 165;
        for (int a = 0; a < 3; ++a) frame.streamAxes[a] = (float)axes[a];
        FrameServices services;
        const ComPtr<ID3D12Resource> zero = hostBuffer(device, 256, D3D12_HEAP_TYPE_UPLOAD);
        {
            uint8_t* p = nullptr;
            check(zero->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map zero");
            std::memset(p, 0, 256);
            zero->Unmap(0, nullptr);
        }
        const ComPtr<ID3D12Resource> readback = hostBuffer(device, 16 + (uint64_t)range.capacity * sizeof(gpu::Instance), D3D12_HEAP_TYPE_READBACK);

        std::vector<NV_StreamProgram> programs;
        std::vector<NV_StreamCurveKey> keys;
        std::vector<NV_StreamEvent> previous;
        std::map<std::pair<uint32_t, uint32_t>, nv_stream::ExactParticle> refPrev, refCur;
        double anchorPrev[3] = {}, anchorCur[3] = {};
        std::vector<Drawn> lastFrame;
        double lastOrigin[3] = {};
        double maxPos = 0, maxTurn = 0, maxScale = 0, maxContinuityPos = 0, maxContinuityTurn = 0, maxPrevPos = 0, maxPrevTurn = 0;
        uint64_t compared = 0, continuity = 0, prevCompared = 0, unmappedSeen = 0, turning = 0;
        int countSlack = 0;
        const double ws[3] = { 0.0, 0.37, 1.0 };
        for (uint32_t t = 1; t <= ticks; ++t)
        {
            std::vector<uint8_t> packet = stream.next(t == 1 ? nullptr : &previous);
            NV_StreamHeader& h = *reinterpret_cast<NV_StreamHeader*>(packet.data());
            // the colliding oriented programs become mesh programs (NV_MESH_SHAPE, output 1)
            if (h.programs && h.program_count)
            {
                auto* p = reinterpret_cast<NV_StreamProgram*>(packet.data() + h.programs);
                for (uint32_t k = 0; k < h.program_count; ++k)
                    if (p[k].flags & NV_STREAM_PROGRAM_ORIENTATION) p[k].output = 1;
                programs.assign(p, p + h.program_count);
            }
            if (h.curve_keys && h.curve_key_count)
            {
                const auto* k = reinterpret_cast<const NV_StreamCurveKey*>(packet.data() + h.curve_keys);
                keys.assign(k, k + h.curve_key_count);
            }
            MP_CHECK(!(h.flags & NV_STREAM_EMITTER_DELTA) && h.emitter_count == h.emitter_table, "tick %u: expected a whole emitter table", t);
            const std::vector<NV_StreamEmitter> table(reinterpret_cast<const NV_StreamEmitter*>(packet.data() + h.emitters),
                                                      reinterpret_cast<const NV_StreamEmitter*>(packet.data() + h.emitters) + h.emitter_count);
            ps.submit(packet.data(), packet.size());
            cpu.submit(packet.data(), packet.size());
            {
                FrameResources resources;
                FramePassContext fc{ device, graph, shaders, quality, scene, frame, resources, services, [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state };
                tracks::simulation(fc);
                graph.execute(nullptr);
                ++frame.frameIndex;
            }
            const fx::TickReadback rb = ps.readback(h.stream, h.generation, h.tick);
            MP_CHECK(rb.counters.alive == h.alive_after, "tick %u: alive %u, expected %u", t, rb.counters.alive, h.alive_after);
            previous = rb.events;
            refPrev = std::move(refCur);
            refCur.clear();
            for (const auto& x : cpu.checkpoint()) refCur[{ x.row, x.birth }] = x;
            std::memcpy(anchorPrev, anchorCur, sizeof anchorPrev);
            std::memcpy(anchorCur, h.anchor, sizeof anchorCur);
            if (t == 1) std::memcpy(anchorPrev, h.anchor, sizeof anchorPrev);
            // the frame origin follows the anchor; one origin shift (1024 m) half way
            for (int a = 0; a < 3; ++a) frame.worldOrigin[a] = std::floor(axes[a] * h.anchor[a]) + (a == 0 && t > ticks / 2 ? 1024.0 : 0.0);

            auto curve = [&](uint32_t first, uint32_t count, double u) {
                if (count < 2) return 1.0;
                uint32_t right = first + 1;
                while (right + 1 < first + count && !(keys[right].t > u)) ++right;
                const auto &a = keys[right - 1], &b = keys[right];
                const double w = std::clamp((u - a.t) / ((double)b.t - a.t), 0.0, 1.0);
                return (1 - w) * a.value[0] + w * b.value[0];
            };

            for (int f = 0; f < 3; ++f)
            {
                const double w = ws[f];
                frame.time = h.time - h.dt + w * h.dt;
                {
                    FrameResources resources;
                    FramePassContext fc{ device, graph, shaders, quality, scene, frame, resources, services, [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state };
                    rangeCopy(graph, range, zero.Get(), readback.Get(), true);
                    tracks::particleMeshes(fc);
                    rangeCopy(graph, range, zero.Get(), readback.Get(), false);
                    graph.execute(nullptr);
                    ++frame.frameIndex;
                }
                device.waitIdle();
                MP_CHECK(device.drainDebugMessages() == 0, "tick %u frame %d: D3D12 debug layer errors", t, f);
                const fx::MeshParticleStats st = fx::findMeshParticles(state)->stats();
                if (t == 1)
                {
                    // the render inputs start with the second tick (a previous tick's state): nothing is drawn yet
                    MP_CHECK(!st.recorded, "tick 1: mesh particles recorded before a tick pair exists");
                    continue;
                }
                MP_CHECK(st.recorded && st.status == 0 && st.overflow == 0, "tick %u frame %d: recorded %d status %u overflow %u", t, f, st.recorded, st.status, st.overflow);
                const uint32_t expectMode = t == 2 && f == 0 ? 0u : f == 0 ? 2u : 1u;
                MP_CHECK(st.mode == expectMode, "tick %u frame %d: previous-transform mode %u, expected %u", t, f, st.mode, expectMode);

                std::vector<Drawn> drawn;
                {
                    uint8_t* p = nullptr;
                    check(readback->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map readback");
                    uint32_t count;
                    std::memcpy(&count, p, 4);
                    MP_CHECK(count == st.instances && count <= range.capacity, "tick %u frame %d: count %u, written %u", t, f, count, st.instances);
                    for (uint32_t i = 0; i < count; ++i)
                    {
                        gpu::Instance in;
                        std::memcpy(&in, p + 16 + (size_t)i * sizeof(gpu::Instance), sizeof in);
                        MP_CHECK(in.mesh == 0 && in.flags == 3u && in.materialRemap == gpu::kNone && in.bonePalette == gpu::kNone && in.morph == gpu::kNone &&
                                     in.patch == gpu::kNone,
                                 "tick %u frame %d: instance %u fields mesh %u flags %u", t, f, i, in.mesh, in.flags);
                        drawn.push_back({ poseOf(in.objectToWorld), poseOf(in.prevObjectToWorld) });
                    }
                    readback->Unmap(0, nullptr);
                }

                // 1. against the reference, evaluated by the render rules in double
                const double dt = h.dt;
                uint32_t expected = 0, unmapped = 0;
                const Index index(drawn);
                std::vector<uint8_t> used(drawn.size(), 0);
                auto visit = [&](const nv_stream::ExactParticle* p0, const nv_stream::ExactParticle* p1) {
                    const nv_stream::ExactParticle& x = p1 ? *p1 : *p0;
                    const NV_StreamEmitter& e = table.at(x.row);
                    const NV_StreamProgram& prog = programs.at(e.program);
                    if (prog.output != 1) return;
                    double pos[3], age;
                    Q q;
                    const double wdt = w * dt, rest = (1 - w) * dt;
                    auto world = [&](const double anchor[3], const nv_stream::ExactParticle& s, int a) -> double {
                        return anchor[a] + e.origin_anchor[a] + s.position[a];
                    };
                    if (!p1)
                    {
                        if (!(wdt < prog.lifetime - p0->age)) return;
                        for (int a = 0; a < 3; ++a) pos[a] = world(anchorPrev, *p0, a) + p0->velocity[a] * wdt;
                        age = p0->age + wdt;
                        q = advance({ p0->rotation[0], p0->rotation[1], p0->rotation[2], p0->rotation[3] }, p0->spin, wdt);
                    }
                    else
                    {
                        age = p1->age - rest;
                        const Q q1{ p1->rotation[0], p1->rotation[1], p1->rotation[2], p1->rotation[3] };
                        if (p0)
                        {
                            const double w2 = w * w, w3 = w2 * w;
                            for (int a = 0; a < 3; ++a)
                                pos[a] = (2 * w3 - 3 * w2 + 1) * world(anchorPrev, *p0, a) + (w3 - 2 * w2 + w) * dt * p0->velocity[a] + (3 * w2 - 2 * w3) * world(anchorCur, *p1, a) +
                                         (w3 - w2) * dt * p1->velocity[a];
                            const Q q0{ p0->rotation[0], p0->rotation[1], p0->rotation[2], p0->rotation[3] };
                            const Q end = advance(q0, p0->spin, dt);
                            const Q transport = mul(q1, { -end.x, -end.y, -end.z, end.w });
                            q = normalized(mul(power(transport, w), advance(q0, p0->spin, wdt)));
                        }
                        else
                        {
                            if (age < 0) return;
                            for (int a = 0; a < 3; ++a) pos[a] = world(anchorCur, *p1, a) - p1->velocity[a] * rest;
                            const double back[3] = { -p1->spin[0], -p1->spin[1], -p1->spin[2] };
                            q = advance(q1, back, rest);
                        }
                    }
                    const uint64_t asset = prog.mesh_asset[0] | ((uint64_t)prog.mesh_asset[1] << 32);
                    if (asset != kDrawnAsset)
                    {
                        MP_CHECK(asset == kUnmappedAsset, "unexpected asset %llx", (unsigned long long)asset);
                        ++unmapped;
                        return;
                    }
                    ++expected;
                    Pose ref;
                    for (int a = 0; a < 3; ++a) ref.position[a] = axes[a] * pos[a] - frame.worldOrigin[a];
                    ref.rotation = rendererRotation(q, axes);
                    ref.scale = prog.size * e.size_scale * curve(prog.size_keys, prog.size_count, std::clamp(age / prog.lifetime, 0.0, 1.0));
                    double d;
                    const uint32_t k = index.nearest(ref.position, d);
                    if (k == UINT32_MAX || d > 2e-3 || used[k]) return;  // (counted below)
                    used[k] = 1;
                    const Pose& g = drawn[k].now;
                    maxPos = std::max(maxPos, d / std::max(1.0, norm(ref.position)));
                    maxTurn = std::max(maxTurn, angle(g.rotation, ref.rotation));
                    maxScale = std::max(maxScale, std::abs(g.scale - ref.scale) / ref.scale);
                    const double spin = std::sqrt(x.spin[0] * x.spin[0] + x.spin[1] * x.spin[1] + x.spin[2] * x.spin[2]);
                    if (spin > 0) ++turning;
                    ++compared;
                };
                for (const auto& [id, p1] : refCur)
                {
                    auto it = refPrev.find(id);
                    visit(it == refPrev.end() ? nullptr : &it->second, &p1);
                }
                for (const auto& [id, p0] : refPrev)
                    if (!refCur.count(id)) visit(&p0, nullptr);
                const uint32_t matched = (uint32_t)std::count(used.begin(), used.end(), 1);
                // float vs double at a birth or death instant can differ by a particle; anything more is a defect
                const int slack = std::abs((int)drawn.size() - (int)expected) + (int)(expected - matched);
                countSlack = std::max(countSlack, slack);
                MP_CHECK(slack <= 2, "tick %u frame %d: %zu instances, %u expected, %u matched", t, f, drawn.size(), expected, matched);
                MP_CHECK(std::abs((int)st.unmapped - (int)unmapped) <= 2, "tick %u frame %d: unmapped %u, expected %u", t, f, st.unmapped, unmapped);
                unmappedSeen += st.unmapped;

                // 2./3. continuity and previous transforms against the previous frame
                if (!lastFrame.empty())
                {
                    const Index before(lastFrame);
                    double shift[3];
                    for (int a = 0; a < 3; ++a) shift[a] = lastOrigin[a] - frame.worldOrigin[a];
                    for (const Drawn& d : drawn)
                    {
                        double p[3];
                        for (int a = 0; a < 3; ++a) p[a] = d.before.position[a] - shift[a];  // in the previous frame's coordinates
                        double dist;
                        const uint32_t k = before.nearest(p, dist);
                        if (k == UINT32_MAX || dist > 1e-3) continue;  // born since
                        const Drawn& o = lastFrame[k];
                        maxPrevPos = std::max(maxPrevPos, dist / std::max(1.0, norm(d.before.position)));  // (float in this frame's coordinates)
                        maxPrevTurn = std::max(maxPrevTurn, angle(d.before.rotation, o.now.rotation));
                        ++prevCompared;
                        if (f == 0)
                        {
                            // the frame at w = 1 of the last tick and this one at w = 0 draw the same state
                            double now[3];
                            for (int a = 0; a < 3; ++a) now[a] = d.now.position[a] - shift[a];
                            maxContinuityPos = std::max(maxContinuityPos, distance(now, o.now.position) / std::max(1.0, norm(d.now.position)));
                            maxContinuityTurn = std::max(maxContinuityTurn, angle(d.now.rotation, o.now.rotation));
                            ++continuity;
                        }
                    }
                }
                lastFrame = std::move(drawn);
                std::memcpy(lastOrigin, frame.worldOrigin, sizeof lastOrigin);
            }
        }
        logf("mesh particles: %llu transforms vs reference (%llu spinning): position %.3g (/ max(1 m, |p|)), rotation %.3g rad, scale %.3g; count slack %d; unmapped seen %llu\n",
             (unsigned long long)compared, (unsigned long long)turning, maxPos, maxTurn, maxScale, countSlack, (unsigned long long)unmappedSeen);
        logf("tick boundary: %llu particles, position %.3g (relative), rotation %.3g rad; previous transforms: %llu, position %.3g (relative), rotation %.3g rad\n",
             (unsigned long long)continuity, maxContinuityPos, maxContinuityTurn, (unsigned long long)prevCompared, maxPrevPos, maxPrevTurn);
        MP_CHECK(compared > 1000 && turning > 100 && continuity > 100 && prevCompared > 1000 && unmappedSeen > 0, "too few comparisons");
        MP_CHECK(maxPos <= 1e-5 && maxTurn <= 1e-4 && maxScale <= 1e-5, "transform error position %.3g rotation %.3g scale %.3g", maxPos, maxTurn, maxScale);
        MP_CHECK(maxContinuityPos <= 2.5e-7 && maxContinuityTurn <= 2e-6, "tick boundary jump position %.3g rotation %.3g", maxContinuityPos, maxContinuityTurn);
        MP_CHECK(maxPrevPos <= 2.5e-7 && maxPrevTurn <= 2e-6, "previous transform differs from the frame drawn: position %.3g rotation %.3g", maxPrevPos, maxPrevTurn);
        logf("PASS\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL: %s\n", e.what());
        return 1;
    }
}
