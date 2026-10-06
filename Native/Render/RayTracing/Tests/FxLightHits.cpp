// A3 FX particle lights at ray hits (S_STATUS_KO.md 10; HitLocalLights.hlsli rtFxWeight / rtFxChoose, RayScene
// recordFxLights, FxLightGroups.hlsl): two scene point lights in RayScene's grid and five FX point lights written into the
// scene light buffer's tail by a GPU pass (as the FX module will), one of them bright and far from every probe point, then
// 40 FX lights (two groups of the choice). At 96 points the
// mean of rtLocalLightSample's weight over K stratified choices equals the sum of every light's radiance at the point,
// sum_i I_i c_i window_i / d_i^2 (the estimator is unbiased: each light's weight is L_i / P(i), and stratification makes
// the choice frequencies P(i) up to 1/K); FX lights are chosen at points they reach, never where none reaches, and a frame
// with the count word at 0 gives the grid alone.
//   unx_test_raytracing_fxlighthits [--no-debug-layer]
#include "../../Passes/Atmosphere/Tests/TestFrame.h"
#include "unx/render/Tracks.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

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
        sc.name = "fx light hits";
        scene::Camera cam;
        cam.name = "main";
        cam.position = { 0, 2, 8 };
        cam.forward = normalize(float3{ 0, -0.2f, -1 });
        sc.cameras.push_back(cam);
        sc.materials.push_back({});
        {
            scene::Mesh mesh;
            mesh.name = "quad";
            for (float3 q : { float3{ -1, 0, -1 }, float3{ -1, 0, 1 }, float3{ 1, 0, 1 }, float3{ 1, 0, -1 } })
            {
                mesh.positions.push_back(q);
                mesh.normals.push_back({ 0, 1, 0 });
                mesh.uv0.push_back({ q.x, q.z });
            }
            mesh.indices = { 0, 1, 2, 0, 2, 3 };
            mesh.submeshes.push_back({ 0, 6, 0 });
            sc.meshes.push_back(mesh);
            scene::Instance a;
            a.transform.m[1][3] = -30;
            sc.instances.push_back(a);
        }
        auto pointLight = [](float3 p, float intensity, float3 color, float range) {
            scene::Light l;
            l.type = scene::LightType::Point;
            l.position = p;
            l.forward = { 0, 0, 1 };
            l.right = { 1, 0, 0 };
            l.intensity = intensity;
            l.color = color;
            l.range = range;
            return l;
        };
        sc.lights = { pointLight({ 0, 3, 0 }, 500, { 1, 0.9f, 0.8f }, 20), pointLight({ 4, 1, -3 }, 300, { 0.7f, 0.8f, 1 }, 15) };
        tf.setScene(sc);
        tf.frame.mainView = ViewDesc::fromCamera(cam, 320, 180, float4x4{});
        tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;
        S_CHECK(tf.gpuScene.setFxLightCapacity(64), "FX light capacity refused");

        // FX lights (gpu::Light records as the FX module writes them: point, cd x colour, range, no shadow slot).
        struct Fx
        {
            float3 p;
            float intensity;
            float3 color;
            float range;
        };
        std::vector<Fx> fx = { { { 1, 1, 1 }, 2000, { 1, 0.5f, 0.2f }, 6 },
                           { { -2, 0.5f, -1 }, 800, { 1, 0.8f, 0.3f }, 4 },
                           { { 2, 2, 2 }, 60, { 0.9f, 0.9f, 1 }, 8 },
                           { { 0, 0.2f, -4 }, 5000, { 1, 0.3f, 0.1f }, 3 },
                           { { 400, 0, 400 }, 1e6f, { 1, 1, 1 }, 10 } };  // far from every point: never reaches
        {
            // 35 more (lights 5..39: a second group of 32), small ones scattered over the probed region.
            std::mt19937 r(9);
            std::uniform_real_distribution<float> px(-3, 5), py(-0.5f, 3), pz(-6, 3), pi(20, 400), pr(1.5f, 5);
            for (int i = 0; i < 35; ++i) fx.push_back({ { px(r), py(r), pz(r) }, pi(r), { 1, 0.6f, 0.3f }, pr(r) });
        }
        std::vector<gpu::Light> records(fx.size());
        for (size_t i = 0; i < fx.size(); ++i)
        {
            gpu::Light& g = records[i];
            g = {};
            g.position = fx[i].p;
            g.typeFlags = (uint32_t)scene::LightType::Point | (0xFFFFu << 16);
            g.forward = { 0, 0, 1 };
            g.right = { 1, 0, 0 };
            g.range = fx[i].range;
            g.intensity = fx[i].intensity;
            g.color = fx[i].color;
            g.size = { 0.2f, 0 };
        }

        std::mt19937 rng(5);
        std::uniform_real_distribution<float> ux(-3, 5), uy(-0.5f, 3), uz(-6, 3);
        std::vector<float4> points;
        for (int i = 0; i < 96; ++i) points.push_back({ ux(rng), uy(rng), uz(rng), 0 });
        const uint32_t count = (uint32_t)points.size(), K = 16384;

        auto probe = [&](uint32_t fxCount) {
            std::shared_ptr<std::vector<uint8_t>> out;
            tf.run([&](FramePassContext& fc) {
                const GpuScene::FxLightRange range = tf.gpuScene.fxLightRange();
                fc.resources.fxLights = fc.graph.importBuffer(range.lightBuffer, BufferDesc{ "scene lights (FX tail)", (uint64_t)(range.first + range.capacity) * sizeof(gpu::Light), (uint32_t)sizeof(gpu::Light) });
                fc.resources.fxLightCount = fc.graph.importBuffer(range.countBuffer, BufferDesc{ "FX light count", 16, 4 });
                ID3D12PipelineState* pso = fc.shaders.compute("RayTracing/Tests/FxLightHitProbe");
                const BufferRef source = tf.uploadBuffer(fc, records.data(), records.size() * sizeof(gpu::Light), 0, "fx.lights.source");
                const BufferRef lights = fc.resources.fxLights, countWord = fc.resources.fxLightCount;
                fc.graph.addPass("fx.lights.write", QueueType::Compute,
                                 [&](PassBuilder& b) {
                                     b.use(source, Use::SrvCompute);
                                     b.use(lights, Use::UavCompute);
                                     b.use(countWord, Use::UavCompute);
                                 },
                                 [=](PassContext& c) {
                                     uint32_t k[8] = { c.srv(source), range.lightUav, range.countUav, range.first, 0, fxCount, (uint32_t)sizeof(gpu::Light), 0 };
                                     c.cmd->SetPipelineState(pso);
                                     c.computeConstants(k, 8);
                                     c.cmd->Dispatch((uint32_t)(records.size() * (sizeof(gpu::Light) / 4) + 63) / 64, 1, 1);
                                 });
                rt::RayScene& rays = rt::RayScene::get(fc);
                rays.record(fc);
                uint32_t scene[8];
                rays.rootConstants(scene);
                const BufferRef input = tf.uploadBuffer(fc, points.data(), points.size() * 16, 0, "fx.hit.points");
                const BufferRef result = fc.graph.createBuffer(BufferDesc{ "fx.hit.result", (uint64_t)count * 16, 0 });
                const D3D12_GPU_VIRTUAL_ADDRESS cb = fc.frameConstantsFor(fc.frame.mainView);
                fc.graph.addPass("fx.hit.probe", QueueType::Compute,
                                 [&](PassBuilder& b) {
                                     b.use(input, Use::SrvCompute);
                                     b.use(result, Use::UavCompute);
                                     rays.declareTraversal(b);
                                 },
                                 [=](PassContext& c) {
                                     uint32_t k[32] = {};
                                     k[0] = c.srv(input);
                                     k[1] = c.uav(result);
                                     k[2] = count;
                                     k[3] = K;
                                     k[4] = 1;
                                     std::memcpy(&k[24], scene, sizeof scene);
                                     c.cmd->SetPipelineState(pso);
                                     c.bindFrameConstants(cb);
                                     c.computeConstants(k, 32);
                                     c.cmd->Dispatch((count + 63) / 64, 1, 1);
                                 });
                out = tf.readbackBuffer(fc, result, (uint64_t)count * 16);
            });
            return out;
        };
        // The exact sum at x of the lights that are in (the grid's two, then the first fxCount FX lights).
        auto window = [](float d, float r) {
            const float q = d / r, q4 = q * q * q * q;
            const float w = std::clamp(1 - q4, 0.0f, 1.0f);
            return w * w;
        };
        auto exact = [&](float3 x, uint32_t fxCount, float3& fxPart) {
            float3 sum{ 0, 0, 0 };
            fxPart = { 0, 0, 0 };
            auto add = [&](float3 p, float intensity, float3 color, float range, bool isFx) {
                const float3 d = x - p;
                const float d2 = dot(d, d);
                if (d2 >= range * range || d2 <= 0) return;
                const float f = intensity * window(std::sqrt(d2), range) / d2;
                sum = sum + color * f;
                if (isFx) fxPart = fxPart + color * f;
            };
            for (const scene::Light& l : sc.lights) add(l.position, l.intensity, l.color, std::max(l.range, 1e-3f), false);
            for (uint32_t i = 0; i < fxCount; ++i) add(fx[i].p, fx[i].intensity, fx[i].color, fx[i].range, true);
            return sum;
        };
        auto check = [&](uint32_t fxCount, const char* what) {
            probe(fxCount);  // the first frame after a count change (the distribution is this frame's in every frame)
            const std::vector<uint8_t> d = *probe(fxCount);
            double worst = 0, sumRel = 0;
            uint32_t compared = 0, fxReached = 0, fxChosenWithout = 0, fxMissing = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const float* r = reinterpret_cast<const float*>(d.data()) + 4 * i;
                const float3 x{ points[i].x, points[i].y, points[i].z };
                float3 fxPart;
                const float3 e = exact(x, fxCount, fxPart);
                const bool reached = fxPart.x + fxPart.y + fxPart.z > 0;
                fxReached += reached;
                if (!reached && r[3] > 0) ++fxChosenWithout;
                if (reached && !(r[3] > 0)) ++fxMissing;
                const float m = std::max({ e.x, e.y, e.z });
                if (!(m > 0)) continue;
                const double rel = std::max({ std::abs(r[0] - e.x), std::abs(r[1] - e.y), std::abs(r[2] - e.z) }) / m;
                worst = std::max(worst, rel);
                sumRel += rel;
                ++compared;
            }
            std::printf("%s: %u points compared, mean weight vs the exact sum: mean %.2e, worst %.2e; FX lights reach %u points, chosen where none "
                        "reaches %u, missing where they reach %u\n",
                        what, compared, compared ? sumRel / compared : 0.0, worst, fxReached, fxChosenWithout, fxMissing);
            S_CHECK(compared > 60, "%s: too few points with light (%u)", what, compared);
            // Stratified choice: each light's frequency is within 1/K of its probability; weights up to the largest L_i / P(i).
            S_CHECK(worst < 5e-3, "%s: the mean weight differs from the sum of the lights by %.3g", what, worst);
            S_CHECK(fxChosenWithout == 0 && fxMissing == 0, "%s: FX choice where no FX light reaches (%u) or none where one does (%u)", what, fxChosenWithout,
                    fxMissing);
            if (fxCount > 0) S_CHECK(fxReached > 20, "%s: FX lights reach only %u points", what, fxReached);
        };
        check(5, "grid + 5 FX lights (one bright one out of reach), one group");
        check(40, "grid + 40 FX lights, two groups");
        check(0, "count word 0 (grid alone)");
        const uint32_t errors = tf.device.drainDebugMessages();
        S_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("PASS\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
