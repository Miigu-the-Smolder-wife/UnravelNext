// S's rain shadow map and rainExposure (WeatherField.hlsli, B6) through the frame path (tracks::atmosphere with
// FrameContext::weather; the ray scene of the test scene): a 10 x 10 m roof 4 m over a 200 m ground.
//  1. Vertical rain: ground under the roof 0 (a margin of 2 texels inside its edge), ground outside 1 (2 texels outside),
//     the roof's top 1.
//  2. Rain tilted by 26.6 deg (direction (0.5, -1, 0) normalised): the roof's shadow on the ground moves 4 m x 0.5 = 2 m
//     towards +x -- x = 6.5 is under it, x = -4.0 is not.
//  3. No weather: no record (rainExposure 1 everywhere, the pass not recorded).
#include "TestFrame.h"

#include "unx/core/Log.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace
{
void addQuad(scene::Mesh& m, float3 a, float3 b, float3 c, float3 d, float3 n)
{
    const uint32_t base = (uint32_t)m.positions.size();
    for (float3 p : { a, b, c, d })
    {
        m.positions.push_back(p);
        m.normals.push_back(n);
        m.uv0.push_back({ p.x, p.z });
    }
    m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}
} // namespace

int main()
{
    try
    {
        TestFrame tf(true);
        scene::Scene sc;
        sc.name = "rain shadow test";
        scene::Material m;
        m.name = "grey";
        sc.materials.push_back(m);
        scene::Mesh mesh;
        mesh.name = "ground and roof";
        addQuad(mesh, { -100, 0, -100 }, { -100, 0, 100 }, { 100, 0, 100 }, { 100, 0, -100 }, { 0, 1, 0 });
        addQuad(mesh, { -5, 4, -5 }, { -5, 4, 5 }, { 5, 4, 5 }, { 5, 4, -5 }, { 0, 1, 0 });
        mesh.submeshes.push_back({ 0, 12, 0 });
        sc.meshes.push_back(mesh);
        sc.instances.push_back({});
        scene::Camera cam;
        cam.name = "main";
        cam.position = { 0.3f, 1.7f, 12.1f };
        cam.forward = { 0, 0, -1 };
        sc.cameras.push_back(cam);
        tf.setScene(sc);
        tf.frame.mainView = ViewDesc::fromCamera(cam, 1920, 1080, float4x4{});
        tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;

        auto exposures = [&](float3 rainDirection, const std::vector<float4>& q, bool weather) {
            tf.frame.weather = {};
            if (weather)
            {
                tf.frame.weather.rainRate = 5;
                tf.frame.weather.rainDirection = normalize(rainDirection);
            }
            std::shared_ptr<std::vector<uint8_t>> out;
            uint32_t record = UINT32_MAX;
            tf.run([&](FramePassContext& fc) {
                tracks::atmosphere(fc);
                tracks::accelerationStructures(fc);  // R traces the rain shadow map after its ray scene record
                record = fc.resources.weather;
                const BufferRef in = tf.uploadBuffer(fc, q.data(), q.size() * 16, 16, "rain queries");
                const BufferRef o = fc.graph.createBuffer(BufferDesc{ "rain probe out", q.size() * 4ull, 4 });
                const TextureRef map = fc.resources.rainShadow;
                const uint32_t n = (uint32_t)q.size(), srv = fc.resources.weather;
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Atmosphere/Tests/RainProbe");
                fc.graph.addPass("s.test.rain", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     if (map.valid()) b.use(map, Use::SrvCompute);
                                     b.use(in, Use::SrvCompute);
                                     b.use(o, Use::UavCompute);
                                 },
                                 [=](PassContext& c) {
                                     const uint32_t k[4] = { srv, c.srv(in), c.uav(o), n };
                                     c.cmd->SetPipelineState(pso);
                                     c.computeConstants(k, 4);
                                     c.cmd->Dispatch((n + 63) / 64, 1, 1);
                                 });
                out = tf.readbackBuffer(fc, o, q.size() * 4ull);
            });
            std::vector<float> e(q.size());
            std::memcpy(e.data(), out->data(), e.size() * 4);
            return std::pair{ e, record };
        };
        bool pass = true;
        auto check = [&](bool ok, const char* what, double got, double want) {
            logf("  %-60s %.3f (want %.3f) %s\n", what, got, want, ok ? "ok" : "FAIL");
            pass = pass && ok;
        };
        const std::vector<float4> q = { { 0, 0.001f, 0, 0 }, { 4.4f, 0.001f, -4.4f, 0 }, { 5.6f, 0.001f, 0, 0 }, { -30, 0.001f, 20, 0 }, { 0, 4.001f, 0, 0 },
                                        { 6.5f, 0.001f, 0, 0 },  { -4.0f, 0.001f, 0, 0 } };
        {
            const auto [e, record] = exposures({ 0, -1, 0 }, q, true);
            check(record != UINT32_MAX, "vertical rain: a weather record", record != UINT32_MAX, 1);
            check(e[0] == 0, "vertical rain: ground under the roof's centre", e[0], 0);
            check(e[1] == 0, "vertical rain: ground 0.6 m inside the roof's corner", e[1], 0);
            check(e[2] == 1, "vertical rain: ground 0.6 m outside the roof's edge", e[2], 1);
            check(e[3] == 1, "vertical rain: open ground far away", e[3], 1);
            check(e[4] == 1, "vertical rain: the roof's top", e[4], 1);
        }
        {
            const auto [e, record] = exposures({ 0.5f, -1, 0 }, q, true);
            check(e[5] == 0, "tilted rain: x = 6.5 (under the shadow moved +2 m)", e[5], 0);
            check(e[6] == 1, "tilted rain: x = -4.0 (the shadow left it)", e[6], 1);
            check(e[4] == 1, "tilted rain: the roof's top", e[4], 1);
        }
        {
            const auto [e, record] = exposures({ 0, -1, 0 }, q, false);
            bool all = record == UINT32_MAX;
            for (float x : e) all = all && x == 1;
            check(all, "no weather: no record, exposure 1 everywhere", all, 1);
        }
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
