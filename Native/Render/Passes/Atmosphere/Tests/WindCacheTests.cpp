// S's wind cache (WindCache.hlsli, B6) through the frame path (tracks::atmosphere with FrameContext::wind):
//  1. windSample at cell centres of the 64^3 grid = windAt of the records (up to the cache's fp16);
//  2. windSample between centres = the trilinear blend of the eight centres' windAt (hardware filtering, 1/256 steps);
//  3. windExact = windAt at any point (records relative to the reference, the tick time);
//  4. the grid follows the camera in 8 m steps (its origin = floor(camera / 8) x 8 - 256).
#include "TestFrame.h"

#include "unx/core/Log.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "../WindField.hlsli"

using namespace unx;
using namespace unx::render;
using namespace unx::stest;
using wind::WindRecord;

namespace
{
WindRecord record(uint32_t op, uint32_t shape, float3 origin, float scale, float3 value, float amplitude, float length, float timeScale, uint32_t octaves, uint32_t seed)
{
    WindRecord r{};
    r.origin = origin;
    r.opShape = op | (shape << 8);
    const float inv = 1 / scale;
    r.row0 = { inv, 0, 0, value.x };
    r.row1 = { 0, inv, 0, value.y };
    r.row2 = { 0, 0, inv, value.z };
    const uint32_t bits = octaves | (seed << 8);
    float fbits;
    std::memcpy(&fbits, &bits, 4);
    r.turbulence = { amplitude, length, timeScale, fbits };
    return r;
}
} // namespace

int main()
{
    try
    {
        TestFrame tf(true);
        scene::Scene sc;
        sc.name = "wind cache test";
        scene::Camera cam;
        cam.name = "main";
        cam.position = { 100.5f, 3.2f, -40.2f };
        cam.forward = { 0, 0, -1 };
        sc.cameras.push_back(cam);
        tf.setScene(sc);
        tf.frame.mainView = ViewDesc::fromCamera(cam, 1920, 1080, float4x4{});
        tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;
        const double reference[3] = { 100, 0, -40 };
        std::vector<WindRecord> rs = {
            record(wind::kWindAdd, wind::kWindGlobal, { 0, 0, 0 }, 1, { 4, 0, 1 }, 1.5f, 40.0f, 9.0f, 2, 3),
            record(wind::kWindReplace, wind::kWindSphere, { 30, 0, 10 }, 25, { -2, 0, 5 }, 0.8f, 16.0f, 4.0f, 1, 9),
        };
        tf.frame.wind.records = rs.data();
        tf.frame.wind.count = (uint32_t)rs.size();
        tf.frame.wind.time = 12.25;
        std::memcpy(tf.frame.wind.reference, reference, sizeof reference);

        const float spacing = 8;
        const float3 origin{ std::floor(cam.position.x / spacing) * spacing - 256, std::floor(cam.position.y / spacing) * spacing - 256,
                             std::floor(cam.position.z / spacing) * spacing - 256 };
        const float3 ref{ (float)reference[0], (float)reference[1], (float)reference[2] };
        std::vector<float4> queries;
        std::vector<int> kind;  // 0 centre, 1 between centres
        for (int i = 0; i < 200; ++i)
        {
            const int cx = 3 + (i * 7) % 58, cy = 2 + (i * 13) % 60, cz = 5 + (i * 29) % 56;
            const float3 c = origin + float3{ cx + 0.5f, cy + 0.5f, cz + 0.5f } * spacing;
            queries.push_back({ c.x, c.y, c.z, 0 });
            kind.push_back(0);
            const float3 b = c + float3{ 0.3f, 0.55f, 0.8f } * spacing;
            queries.push_back({ b.x, b.y, b.z, 0 });
            kind.push_back(1);
        }
        std::shared_ptr<std::vector<uint8_t>> out;
        uint32_t headerSrv = UINT32_MAX;
        tf.run([&](FramePassContext& fc) {
            tracks::atmosphere(fc);
            headerSrv = fc.resources.wind;
            const BufferRef in = tf.uploadBuffer(fc, queries.data(), queries.size() * 16, 16, "wind queries");
            const BufferRef o = fc.graph.createBuffer(BufferDesc{ "wind probe out", queries.size() * 32ull, 16 });
            const TextureRef cache = fc.resources.windCache;
            const uint32_t n = (uint32_t)queries.size(), header = fc.resources.wind;
            ID3D12PipelineState* pso = fc.shaders.compute("Passes/Atmosphere/Tests/WindCacheProbe");
            fc.graph.addPass("s.test.wind", QueueType::Graphics,
                             [&](PassBuilder& b) {
                                 b.use(cache, Use::SrvCompute);
                                 b.use(in, Use::SrvCompute);
                                 b.use(o, Use::UavCompute);
                             },
                             [=](PassContext& c) {
                                 const uint32_t k[4] = { header, c.srv(in), c.uav(o), n };
                                 c.cmd->SetPipelineState(pso);
                                 c.computeConstants(k, 4);
                                 c.cmd->Dispatch((n + 63) / 64, 1, 1);
                             });
            out = tf.readbackBuffer(fc, o, queries.size() * 32ull);
        });
        bool pass = headerSrv != UINT32_MAX;
        auto cpu = [&](float3 x) { return wind::windAt(rs.data(), (uint32_t)rs.size(), x - ref, 12.25f); };
        double centre = 0, between = 0, exact = 0;
        for (size_t i = 0; i < queries.size(); ++i)
        {
            float3 s, e;
            std::memcpy(&s, out->data() + i * 32, 12);
            std::memcpy(&e, out->data() + i * 32 + 16, 12);
            const float3 x{ queries[i].x, queries[i].y, queries[i].z };
            const float3 w = cpu(x);
            const double scale = std::max(1.0, std::sqrt((double)dot(w, w)));
            if (i < 2)
                logf("  query %zu (%.1f, %.1f, %.1f): windSample (%.4f, %.4f, %.4f), windExact (%.4f, %.4f, %.4f), CPU windAt (%.4f, %.4f, %.4f)" "%c", i, x.x, x.y, x.z,
                     s.x, s.y, s.z, e.x, e.y, e.z, w.x, w.y, w.z, 10);
            auto finite = [](float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
            if (!finite(s) || !finite(e)) pass = false;  // a non-finite value fails (std::max would drop a NaN error)
            exact = std::max(exact, std::sqrt((double)dot(e - w, e - w)) / scale);
            if (kind[i] == 0) centre = std::max(centre, std::sqrt((double)dot(s - w, s - w)) / scale);
            else
            {
                // The eight centres around x, trilinear.
                const float3 g = (x - origin) * (1 / spacing) - float3{ 0.5f, 0.5f, 0.5f };
                const float3 g0{ std::floor(g.x), std::floor(g.y), std::floor(g.z) };
                const float3 f = g - g0;
                float3 t{ 0, 0, 0 };
                for (int k = 0; k < 8; ++k)
                {
                    const float3 o{ (float)(k & 1), (float)((k >> 1) & 1), (float)(k >> 2) };
                    const float wgt = (o.x != 0 ? f.x : 1 - f.x) * (o.y != 0 ? f.y : 1 - f.y) * (o.z != 0 ? f.z : 1 - f.z);
                    t = t + cpu(origin + (g0 + o + float3{ 0.5f, 0.5f, 0.5f }) * spacing) * wgt;
                }
                between = std::max(between, std::sqrt((double)dot(s - t, s - t)) / scale);
            }
        }
        auto report = [&](bool ok, const char* what, double v, double limit) {
            logf("  %-58s %.3e (limit %.1e) %s\n", what, v, limit, ok ? "ok" : "FAIL");
            pass = pass && ok;
        };
        report(centre < 2e-3, "windSample at cell centres vs windAt (relative, fp16 cache)", centre, 2e-3);
        report(between < 1e-2, "windSample between centres vs trilinear of centres (1/256 weights)", between, 1e-2);
        report(exact < 1e-4, "windExact vs windAt (relative)", exact, 1e-4);
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
