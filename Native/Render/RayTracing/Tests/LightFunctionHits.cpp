// Light functions at ray hits (A8; E's LightFunction.hlsli through RayTracing/HitLocalLights.hlsli): R's local-light
// sample at a hit carries the sampled point or spot light's function toward the hit. At 2,000 points around a spot light
// (cookie with rotation and intensity keys) and a point light (IES profile over both angles, colour keys, flicker), the
// sample's weight with the functions over the same sample without them (same light choice and direction: the choice
// does not depend on f) against E's CPU reference lights::evaluate at the sample's direction and the frame time; the
// word the grid header carries is written per frame (frames without functions: ratio 1).
//   unx_test_raytracing_lightfunctionhits [--no-debug-layer]
#include "../../Passes/Atmosphere/Tests/TestFrame.h"

#if __has_include("unx/lights/LightFunctions.h")
#include "unx/lights/LightFunctions.h"
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
        sc.name = "light function hits";
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
        scene::Light spot;
        spot.type = scene::LightType::Spot;
        spot.position = { 0, 4, 0 };
        spot.forward = normalize(float3{ 0.2f, -1, 0.1f });
        spot.right = normalize(cross(spot.forward, float3{ 0, 0, 1 }));
        spot.intensity = 800;
        spot.range = 40;
        spot.spotInner = 0.4f;
        spot.spotOuter = 1.0f;
        scene::Light point;
        point.type = scene::LightType::Point;
        point.position = { 3, 2, -2 };
        point.forward = normalize(float3{ 1, -0.3f, 0.4f });
        point.right = normalize(cross(point.forward, float3{ 0, 1, 0 }));
        point.intensity = 500;
        point.range = 40;
        sc.lights = { spot, point };
        tf.setScene(sc);
        tf.frame.mainView = ViewDesc::fromCamera(cam, 320, 180, float4x4{});
        tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;
        tf.frame.time = 1.37f;

        lights::LightFunction cookie;
        cookie.profile = lights::Profile::Cookie;
        cookie.image.width = 4;
        cookie.image.height = 3;
        // Neighbouring texels differ by <= 0.2: the sampler's 8-bit filter fraction then stays within E's 2e-3 tolerance
        // (its error scales with the texel step).
        for (uint32_t k = 0; k < 12; ++k)
            for (int c = 0; c < 3; ++c) cookie.image.rgb.push_back(0.25f + 0.12f * (float)(k % 4) + 0.15f * (float)(k / 4) + 0.1f * (float)c);
        cookie.tanX = 0.6f;
        cookie.tanY = 0.45f;
        cookie.rotationSpeed = 0.7f;
        cookie.rotationPhase = 0.3f;
        cookie.intensityKeys = { { 0, 1 }, { 1, 0.3f }, { 2, 1.5f } };
        cookie.intensityPeriod = 2;
        lights::LightFunction ies;
        ies.profile = lights::Profile::Ies;
        ies.ies.vertical = { 0, 45, 90, 135, 180 };
        ies.ies.horizontal = { 0, 90, 180, 270, 360 };
        for (int v = 0; v < 5; ++v)
            for (int h = 0; h < 5; ++h) ies.ies.values.push_back(h == 4 ? 0.2f + 0.2f * v : 0.2f + 0.15f * v + 0.1f * h);  // 360 = 0
        for (int v = 0; v < 5; ++v) ies.ies.values[v * 5 + 4] = ies.ies.values[v * 5];
        ies.ies.peak = 1;
        ies.colorKeys = { { 0, 1, 0.8f, 0.6f }, { 3, 0.5f, 1, 0.9f } };
        ies.colorPeriod = 3;
        ies.flickerDepth = 0.4f;
        ies.flickerFrequency = 3;
        ies.flickerOctaves = 2;
        ies.flickerSeed = 7;
        lights::LightFunctions& set = lights::lightFunctions(tf.trackState);

        std::mt19937 rng(11);
        std::uniform_real_distribution<float> ux(-6, 8), uy(-2, 3), uz(-8, 5);
        std::vector<float4> points;
        for (int i = 0; i < 2000; ++i) points.push_back({ ux(rng), uy(rng), uz(rng), 0 });  // footprint 0: the sharpest mip
        const uint32_t count = (uint32_t)points.size();

        auto probe = [&]() {
            std::shared_ptr<std::vector<uint8_t>> out;
            tf.run([&](FramePassContext& fc) {
                tracks::lightFunctions(fc);
                rt::RayScene& rays = rt::RayScene::get(fc);
                rays.record(fc);
                uint32_t scene[8];
                rays.rootConstants(scene);
                const BufferRef input = tf.uploadBuffer(fc, points.data(), points.size() * 16, 0, "lf.hit.points");
                const BufferRef result = fc.graph.createBuffer(BufferDesc{ "lf.hit.result", (uint64_t)count * 32, 0 });
                ID3D12PipelineState* pso = fc.shaders.compute("RayTracing/Tests/LightFunctionHitProbe");
                const D3D12_GPU_VIRTUAL_ADDRESS cb = fc.frameConstantsFor(fc.frame.mainView);
                fc.graph.addPass("lf.hit.probe", QueueType::Compute,
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
                                     std::memcpy(&k[24], scene, sizeof scene);
                                     c.cmd->SetPipelineState(pso);
                                     c.bindFrameConstants(cb);
                                     c.computeConstants(k, 32);
                                     c.cmd->Dispatch((count + 63) / 64, 1, 1);
                                 });
                out = tf.readbackBuffer(fc, result, (uint64_t)count * 32);
            });
            return out;
        };
        struct Sample
        {
            float3 weight, wi;
            uint32_t light, valid;
        };
        auto decode = [&](const std::vector<uint8_t>& d) {
            std::vector<Sample> s(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint32_t* w = reinterpret_cast<const uint32_t*>(d.data()) + 8 * i;
                std::memcpy(&s[i].weight, w, 12);
                s[i].light = w[3];
                std::memcpy(&s[i].wi, w + 4, 12);
                s[i].valid = w[7];
            }
            return s;
        };

        set.set(0, cookie);
        set.set(1, ies);
        const std::vector<Sample> with = decode(*probe());
        set.clear(0);
        set.clear(1);
        const std::vector<Sample> without = decode(*probe());
        const scene::Light* L[2] = { &sc.lights[0], &sc.lights[1] };
        const lights::LightFunction* F[2] = { &cookie, &ies };
        // f at the GPU = weight with / without (same sample). Tolerance as E's LightFunctionTests: the cookie reads half-float
        // texels through the sampler's 8-bit filter fraction (2e-3 absolute), IES and keys are float (1e-4 x scale); a sample
        // at a discontinuity (the cookie frame) passes if a direction 1e-4 rad away agrees.
        uint32_t compared = 0, perLight[2] = {}, mismatch = 0, dark = 0, edges = 0;
        double worst[2] = {};
        const float time = (float)tf.frame.time;  // g_time: the float frame time
        auto maxAbs = [](float3 x, float3 y) { return std::max({ std::abs(x.x - y.x), std::abs(x.y - y.y), std::abs(x.z - y.z) }); };
        for (uint32_t i = 0; i < count; ++i)
        {
            const Sample &a = with[i], &b = without[i];
            if (!b.valid) continue;
            S_CHECK(a.valid == b.valid && a.light == b.light && a.wi.x == b.wi.x && a.wi.y == b.wi.y && a.wi.z == b.wi.z, "point %u: the sample changed with the function", i);
            if (a.light > 1)
            {
                ++mismatch;
                continue;
            }
            if (!(b.weight.x > 0 && b.weight.y > 0 && b.weight.z > 0)) continue;
            const float3 gpu{ a.weight.x / b.weight.x, a.weight.y / b.weight.y, a.weight.z / b.weight.z };
            const float3 fw = L[a.light]->forward, rt = L[a.light]->right, dir{ -b.wi.x, -b.wi.y, -b.wi.z };
            const float3 cpu = lights::evaluate(*F[a.light], fw, rt, dir, time);
            const float tol = a.light == 0 ? 2e-3f : 1e-4f * std::max({ 1.0f, cpu.x, cpu.y, cpu.z });
            const float e = maxAbs(cpu, gpu);
            ++perLight[a.light];
            ++compared;
            dark += cpu.x == 0 && cpu.y == 0 && cpu.z == 0;
            if (e <= tol)
            {
                worst[a.light] = std::max(worst[a.light], (double)e);
                continue;
            }
            float3 u = cross(dir, rt);
            u = u * (1.0f / std::sqrt(dot(u, u)));
            const float3 v = cross(dir, u);
            bool edge = false;
            for (int k = 0; k < 8 && !edge; ++k)
            {
                const float ang = k * 0.785398163f;
                float3 d2 = dir + (u * std::cos(ang) + v * std::sin(ang)) * 1e-4f;
                d2 = d2 * (1.0f / std::sqrt(dot(d2, d2)));
                edge = maxAbs(lights::evaluate(*F[a.light], fw, rt, d2, time), gpu) <= tol;
            }
            S_CHECK(edge, "point %u light %u dir (%.5f %.5f %.5f): GPU f (%.6f %.6f %.6f) CPU (%.6f %.6f %.6f)", i, a.light, dir.x, dir.y, dir.z, gpu.x, gpu.y, gpu.z,
                    cpu.x, cpu.y, cpu.z);
            ++edges;
        }
        std::printf("light functions at hits: %u samples compared (spot %u, point %u; %u where f = 0, %u at the cookie's frame), worst |f| error: cookie %.2e, IES %.2e\n",
                    compared, perLight[0], perLight[1], dark, edges, worst[0], worst[1]);
        S_CHECK(mismatch == 0 && perLight[0] > 200 && perLight[1] > 200, "too few samples per light (%u, %u)", perLight[0], perLight[1]);
        // Frames without functions after frames with them: the header word is reset (ratio 1).
        for (int k = 0; k < 3; ++k) probe();
        const std::vector<Sample> after = decode(*probe());
        uint32_t differ = 0;
        for (uint32_t i = 0; i < count; ++i)
            if (after[i].valid && (after[i].weight.x != without[i].weight.x || after[i].weight.y != without[i].weight.y || after[i].weight.z != without[i].weight.z)) ++differ;
        S_CHECK(differ == 0, "%u samples still carry a light function after it was cleared", differ);
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
#else
#include <cstdio>
int main()
{
    std::printf("SKIP: no light function module in this build\n");
    return 0;
}
#endif
