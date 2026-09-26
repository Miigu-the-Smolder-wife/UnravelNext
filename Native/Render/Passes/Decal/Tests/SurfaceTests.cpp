// Track E surface state field correctness (A7; no GPU lock needed after the first run of new kernels). The renderer's
// copy of the VFX surface state (SurfaceField + tracks::surfaceState) against a CPU reference of SurfaceState.hlsli:
//   bricks in a 12^3 m region around the origin (negative keys included), random values and times, three deltas over
//   three frames (new bricks; removals and replacements, reusing slots and shifting hash runs; more new bricks), then
//   2,048 random points and every brick face: surfaceStateAt vs trilinear over the voxel centres of the quantized
//   records (encode), each brick decayed from its t0 to now with the channel half-lives (|d| <= 2e-6 + 2e-6 |ref|).
//   Also: the GPU table equals the CPU table after every frame, and every live key is found within maxProbe + 1 probes of
//   its hash (the GPU loop bound). surface.max_bricks = 512 (1,024 entries) so keys collide and deletions shift runs.
//   unx_test_decal_surfacetests [--no-debug-layer]
#include "../../Atmosphere/Tests/TestFrame.h"

#include "unx/decal/SurfaceState.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <random>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace
{
using Key = std::array<int32_t, 3>;

int floorDiv4(int v) { return v >= 0 ? v / 4 : -((-v + 3) / 4); }

// Reference sample from the quantized records.
std::array<double, 6> reference(const std::map<Key, std::vector<uint8_t>>& bricks, const std::array<double, 6>& halfLife, double now, const double p[3])
{
    std::array<double, 6> out{};
    double q[3], b[3], f[3];
    for (int a = 0; a < 3; ++a)
    {
        q[a] = (double)(float)((float)p[a] / 0.25f - 0.5f);  // the GPU's float arithmetic for the voxel coordinate
        b[a] = std::floor(q[a]);
        f[a] = q[a] - b[a];
    }
    for (int corner = 0; corner < 8; ++corner)
    {
        const int o[3] = { corner & 1, (corner >> 1) & 1, corner >> 2 };
        int voxel[3];
        double w = 1;
        for (int a = 0; a < 3; ++a)
        {
            voxel[a] = (int)b[a] + o[a];
            w *= o[a] ? f[a] : 1 - f[a];
        }
        const Key key{ floorDiv4(voxel[0]), floorDiv4(voxel[1]), floorDiv4(voxel[2]) };
        auto it = bricks.find(key);
        if (it == bricks.end()) continue;
        const uint8_t* r = it->second.data();
        float t0;
        std::memcpy(&t0, r + 12, 4);
        const int l[3] = { voxel[0] - key[0] * 4, voxel[1] - key[1] * 4, voxel[2] - key[2] * 4 };
        const int v = (l[0] * 4 + l[1]) * 4 + l[2];
        for (int c = 0; c < 6; ++c)
        {
            double value;
            if (c < 5) value = r[16 + 5 * v + c] / 255.0;
            else
            {
                int16_t s;
                std::memcpy(&s, r + 336 + 2 * v, 2);
                value = s / 2048.0;
            }
            const double decay = halfLife[c] > 0 ? std::exp2(-(now - t0) / halfLife[c]) : 1.0;
            out[c] += w * value * decay;
        }
    }
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--no-debug-layer") debugLayer = false;
            else fail("unknown argument %s", a.c_str());
        }
        TestFrame tf(debugLayer);
        scene::Scene sc;
        sc.name = "surface test";
        scene::Camera cam;
        cam.name = "main";
        sc.cameras.push_back(cam);
        tf.setScene(sc);
        tf.frame.mainView = ViewDesc::fromCamera(cam, 64, 64, float4x4{});
        // a small table (512 bricks -> 1,024 entries): runs of colliding keys, so probing and backward-shift deletion are
        // exercised; the first frame also rebuilds from the default capacity
        tf.quality.applyOverride("surface.max_bricks=512");
        surface::SurfaceField& field = surface::surfaceField(tf.trackState);
        const std::array<double, 6> halfLife{ 60, 0, 180, 5, 0, 0 };
        field.setHalfLives(halfLife);

        std::mt19937 rng(20260926);
        std::uniform_real_distribution<double> U(0, 1);
        std::uniform_int_distribution<int> K(-6, 5);
        std::map<Key, std::vector<uint8_t>> live;  // the reference's records
        auto makeBrick = [&](Key k, double t0) {
            surface::BrickInput b{};
            b.key[0] = k[0];
            b.key[1] = k[1];
            b.key[2] = k[2];
            b.t0 = t0;
            for (uint32_t v = 0; v < 64; ++v)
                for (uint32_t c = 0; c < 6; ++c) b.value[v * 6 + c] = (float)(c < 5 ? U(rng) : (U(rng) * 4 - 2));
            return b;
        };
        auto frame = [&]() { tf.run([&](FramePassContext& fc) { tracks::surfaceState(fc); }); };

        // delta 1: 400 new bricks
        std::vector<surface::BrickInput> changed;
        std::vector<int32_t> removed;
        while (live.size() < 400)
        {
            const Key k{ K(rng), K(rng), K(rng) };
            if (live.count(k)) continue;
            changed.push_back(makeBrick(k, 10 * U(rng)));
            std::vector<uint8_t> r(surface::kBrickBytes);
            surface::encode(changed.back(), r.data());
            live[k] = r;
        }
        field.apply(changed.data(), changed.size(), nullptr, 0);
        field.setTime(12.0);
        // the GPU table equals the CPU table after each frame's upload
        auto checkTable = [&](const char* label) {
            std::shared_ptr<std::vector<uint8_t>> t1;
            tf.run([&](FramePassContext& fc) {
                tracks::surfaceState(fc);
                t1 = tf.readbackBuffer(fc, fc.resources.surfaceTable, 256 + (uint64_t)field.tableSize() * 16);
            });
            uint32_t bad = 0, firstBad = UINT32_MAX;
            for (uint32_t i = 0; i < field.tableSize(); ++i)
                if (std::memcmp(t1->data() + 256 + 16 * (size_t)i, field.table()[i].data(), 16) != 0) { ++bad; if (firstBad == UINT32_MAX) firstBad = i; }
            S_CHECK(bad == 0, "%s: %u of %u GPU table entries differ from the CPU table (first %u)", label, bad, field.tableSize(), firstBad);
        };
        checkTable("frame 1");
        // delta 2: remove 150, replace 100 (new values and times)
        changed.clear();
        removed.clear();
        {
            std::vector<Key> keys;
            for (const auto& [k, r] : live) keys.push_back(k);
            std::shuffle(keys.begin(), keys.end(), rng);
            for (int n = 0; n < 150; ++n)
            {
                removed.insert(removed.end(), keys[n].begin(), keys[n].end());
                live.erase(keys[n]);
            }
            for (int n = 150; n < 250; ++n)
            {
                changed.push_back(makeBrick(keys[n], 12 + U(rng)));
                std::vector<uint8_t> r(surface::kBrickBytes);
                surface::encode(changed.back(), r.data());
                live[keys[n]] = r;
            }
        }
        field.apply(changed.data(), changed.size(), removed.data(), removed.size() / 3);
        field.setTime(14.0);
        checkTable("frame 2");
        // delta 3: 200 more new bricks (into freed slots and past them)
        changed.clear();
        while (changed.size() < 200)
        {
            const Key k{ K(rng), K(rng), K(rng) };
            if (live.count(k)) continue;
            changed.push_back(makeBrick(k, 14 + U(rng)));
            std::vector<uint8_t> r(surface::kBrickBytes);
            surface::encode(changed.back(), r.data());
            live[k] = r;
        }
        field.apply(changed.data(), changed.size(), nullptr, 0);
        const double now = 17.5;
        field.setTime(now);
        S_CHECK(field.bricks() == live.size(), "field holds %zu bricks, reference %zu", field.bricks(), live.size());

        // table invariant: every live key within maxProbe + 1 probes of its hash
        {
            const auto& table = field.table();
            const uint32_t mask = field.tableSize() - 1;
            for (const auto& [k, r] : live)
            {
                uint32_t i = surface::hash(k[0], k[1], k[2]) & mask;
                bool found = false;
                for (uint32_t p = 0; p <= field.maxProbe() && !found; ++p, i = (i + 1) & mask)
                    found = table[i][0] == k[0] && table[i][1] == k[1] && table[i][2] == k[2] && (uint32_t)table[i][3] != surface::kEmpty;
                S_CHECK(found, "brick (%d, %d, %d) not within %u probes", k[0], k[1], k[2], field.maxProbe() + 1);
            }
        }

        // sample points: random, plus points on brick faces and voxel-centre planes
        std::vector<std::array<float, 4>> points;
        std::uniform_real_distribution<double> P(-6.2, 6.2);
        std::uniform_int_distribution<int> V(-24, 23);
        for (int n = 0; n < 2048; ++n) points.push_back({ (float)P(rng), (float)P(rng), (float)P(rng), 0 });
        // brick faces (whole metres) and voxel-centre planes (0.125 + 0.25 k)
        for (int n = 0; n < 512; ++n) points.push_back({ (float)K(rng), (float)P(rng), (float)(0.125 + 0.25 * V(rng)), 0 });
        const uint32_t count = (uint32_t)points.size();
        std::shared_ptr<std::vector<uint8_t>> results, gpuTable, gpuPool;
        tf.run([&](FramePassContext& fc) {
            tracks::surfaceState(fc);
            const FrameResources& r = fc.resources;
            S_CHECK(r.surfaceConstants.valid() && r.surfaceTable.valid() && r.surfacePool.valid(), "no surface field resources");
            const BufferRef pts = tf.uploadBuffer(fc, points.data(), points.size() * 16, 16, "surface.test.points");
            const BufferRef out = fc.graph.createBuffer(BufferDesc{ "surface.test.results", (uint64_t)count * 32, 16 });
            const BufferRef cb = r.surfaceConstants, table = r.surfaceTable, pool = r.surfacePool;
            ID3D12PipelineState* pso = fc.shaders.compute("Passes/Decal/Tests/SurfaceProbe");
            fc.graph.addPass("surface.test.probe", QueueType::Graphics,
                             [&](PassBuilder& b) {
                                 b.use(pts, Use::SrvCompute);
                                 b.use(out, Use::UavCompute);
                                 for (BufferRef x : { cb, table, pool }) b.use(x, Use::SrvCompute);
                             },
                             [=](PassContext& c) {
                                 const uint32_t k[8] = { c.srv(pts), c.uav(out), count, 0, c.srv(cb), c.srv(table), c.srv(pool), 0 };
                                 c.cmd->SetPipelineState(pso);
                                 c.computeConstants(k, 8);
                                 c.cmd->Dispatch((count + 63) / 64, 1, 1);
                             });
            results = tf.readbackBuffer(fc, out, (uint64_t)count * 32);
            // one field buffer: constants at 0, the table at 256, the pool after it
            gpuTable = tf.readbackBuffer(fc, table, 256 + (uint64_t)field.tableSize() * 16 + (uint64_t)field.capacity() * surface::kBrickBytes);
            gpuPool = gpuTable;
        });
        // the GPU copy equals the CPU copy: every table entry, every live brick record
        {
            const auto& t = field.table();
            for (uint32_t i = 0; i < field.tableSize(); ++i)
                S_CHECK(std::memcmp(gpuTable->data() + 256 + 16 * (size_t)i, t[i].data(), 16) == 0, "table entry %u differs (cpu slot %d)", i, t[i][3]);
            for (uint32_t s : field.liveSlots())
                S_CHECK(std::memcmp(gpuPool->data() + 256 + 16 * (size_t)field.tableSize() + (size_t)s * surface::kBrickBytes, field.records().data() + (size_t)s * surface::kBrickBytes, surface::kBrickBytes) == 0,
                        "brick record in slot %u differs", s);
        }
        const float* g = reinterpret_cast<const float*>(results->data());
        double worst = 0;
        uint32_t nonzero = 0;
        for (uint32_t n = 0; n < count; ++n)
        {
            const double p[3] = { points[n][0], points[n][1], points[n][2] };
            const std::array<double, 6> ref = reference(live, halfLife, (double)(float)now, p);
            const float got[6] = { g[8 * n], g[8 * n + 1], g[8 * n + 2], g[8 * n + 3], g[8 * n + 4], g[8 * n + 5] };
            bool any = false;
            for (int c = 0; c < 6; ++c)
            {
                const double e = std::abs(got[c] - ref[c]);
                worst = std::max(worst, e / (1e-6 + std::abs(ref[c])));
                any |= ref[c] != 0;
                S_CHECK(e <= 2e-6 + 2e-6 * std::abs(ref[c]) + 1e-5 * std::abs(ref[c]), "point %u (%.4f, %.4f, %.4f) channel %d: %.7g vs %.7g", n, p[0], p[1], p[2], c, got[c], ref[c]);
            }
            nonzero += any;
        }
        S_CHECK(nonzero > 500, "only %u sample points met bricks", nonzero);
        // The VFX World mirrored in z against the renderer and the frame shifted by a world origin (render A's host-boundary
        // rule: stream = streamAxes x (frame + worldOrigin)): the same stream-space points, given in frame coordinates
        // (quantised to 1/1024 m so they are exact in float at these distances), read the same values.
        {
            const float axes[3] = { 1, 1, -1 };
            const double origin[3] = { 1024, -2048, 3072 };
            std::vector<std::array<float, 4>> framePoints(count);
            std::vector<std::array<double, 3>> streamPoints(count);
            for (uint32_t n = 0; n < count; ++n)
                for (int a = 0; a < 3; ++a)
                {
                    streamPoints[n][a] = std::round((double)points[n][a] * 1024.0) / 1024.0;
                    framePoints[n][a] = (float)(axes[a] * streamPoints[n][a] - origin[a]);
                }
            for (int a = 0; a < 3; ++a) { tf.frame.streamAxes[a] = axes[a]; tf.frame.worldOrigin[a] = origin[a]; }
            std::shared_ptr<std::vector<uint8_t>> mirrored;
            tf.run([&](FramePassContext& fc) {
                tracks::surfaceState(fc);
                const FrameResources& r = fc.resources;
                const BufferRef pts = tf.uploadBuffer(fc, framePoints.data(), framePoints.size() * 16, 16, "surface.test.points");
                const BufferRef out = fc.graph.createBuffer(BufferDesc{ "surface.test.results", (uint64_t)count * 32, 16 });
                const BufferRef cb = r.surfaceConstants, table = r.surfaceTable, pool = r.surfacePool;
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Decal/Tests/SurfaceProbe");
                fc.graph.addPass("surface.test.probe", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     b.use(pts, Use::SrvCompute);
                                     b.use(out, Use::UavCompute);
                                     for (BufferRef x : { cb, table, pool }) b.use(x, Use::SrvCompute);
                                 },
                                 [=](PassContext& c) {
                                     const uint32_t k[8] = { c.srv(pts), c.uav(out), count, 0, c.srv(cb), c.srv(table), c.srv(pool), 0 };
                                     c.cmd->SetPipelineState(pso);
                                     c.computeConstants(k, 8);
                                     c.cmd->Dispatch((count + 63) / 64, 1, 1);
                                 });
                mirrored = tf.readbackBuffer(fc, out, (uint64_t)count * 32);
            });
            for (int a = 0; a < 3; ++a) { tf.frame.streamAxes[a] = 1; tf.frame.worldOrigin[a] = 0; }
            const float* m = reinterpret_cast<const float*>(mirrored->data());
            double worstMirror = 0;
            uint32_t met = 0;
            for (uint32_t n = 0; n < count; ++n)
            {
                const double q[3] = { streamPoints[n][0], streamPoints[n][1], streamPoints[n][2] };
                const std::array<double, 6> ref = reference(live, halfLife, (double)(float)now, q);
                bool any = false;
                for (int c = 0; c < 6; ++c)
                {
                    const double e = std::abs(m[8 * n + c] - ref[c]);
                    worstMirror = std::max(worstMirror, e / (1e-6 + std::abs(ref[c])));
                    any |= ref[c] != 0;
                    S_CHECK(e <= 2e-6 + 2e-6 * std::abs(ref[c]) + 1e-5 * std::abs(ref[c]), "mirrored point %u (stream %.4f, %.4f, %.4f) channel %d: %.7g vs %.7g", n, q[0], q[1], q[2], c,
                            m[8 * n + c], ref[c]);
                }
                met += any;
            }
            S_CHECK(met > 500, "mirrored: only %u sample points met bricks", met);
            std::printf("surface: World mirrored in z and origin (1024, -2048, 3072) m: %u points (%u inside bricks) match the stream-space reference (worst rel %.2e)\n",
                        count, met, worstMirror);
        }
        const uint32_t errors = tf.device.drainDebugMessages();
        S_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("surface: %zu bricks after 3 deltas (max probe %u), %u points (%u inside bricks) match the reference (worst rel %.2e)\n", live.size(),
                    field.maxProbe(), count, nonzero, worst);
        std::printf("surface tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
