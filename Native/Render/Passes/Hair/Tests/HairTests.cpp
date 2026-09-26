// Track E hair correctness (B10; no GPU lock needed after the first run of new kernels).
//   1. fibre BSDF (HairBsdf.hlsli), 36 configurations (outgoing elevation, h, roughness) x 1M sample pairs:
//      - a non-absorbing fibre scatters all incident energy: the uniform-sphere estimate of the directional albedo is 1
//        (white furnace) within 3 standard errors + 0.3 %;
//      - the sampling pdf integrates to 1 (same tolerance), and hairSample draws from it: the sampled mean of wi.x equals
//        the pdf-weighted uniform estimate (within 4 standard errors + 2e-3);
//      - an absorbing fibre (sigma_a = 0.5, 1, 2) keeps its albedo below 1 and ordered by absorption.
//   unx_test_hair_hairtests [--no-debug-layer]
#include "../../Atmosphere/Tests/TestFrame.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

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
        sc.name = "hair test";
        scene::Camera cam;
        cam.name = "main";
        sc.cameras.push_back(cam);
        tf.setScene(sc);

        // ---- 1. fibre BSDF
        {
            struct Config
            {
                float c[12];
                bool absorbing;
            };
            std::vector<Config> configs;
            const float elevations[3] = { -0.5f, 0.0f, 0.7f }, hs[3] = { -0.6f, 0.0f, 0.8f };
            const float roughness[4][2] = { { 0.3f, 0.3f }, { 0.3f, 0.6f }, { 0.6f, 0.3f }, { 0.6f, 0.6f } };
            for (float e : elevations)
                for (float h : hs)
                    for (const auto& r : roughness)
                    {
                        const float c = std::sqrt(1 - e * e);
                        configs.push_back({ { e, c * 0.6f, c * 0.8f, h, 0, 0, 0, 1.55f, r[0], r[1], 0.0349f, 0 }, false });
                    }
            configs.push_back({ { 0.2f, 0.9f, 0.3f, 0.3f, 0.5f, 1.0f, 2.0f, 1.55f, 0.3f, 0.3f, 0.0349f, 0 }, true });
            const uint32_t count = (uint32_t)configs.size(), perThread = 4096, threads = 256;
            std::vector<float> packed;
            for (const Config& c : configs) packed.insert(packed.end(), c.c, c.c + 12);
            std::shared_ptr<std::vector<uint8_t>> results;
            tf.run([&](FramePassContext& fc) {
                const BufferRef cfg = tf.uploadBuffer(fc, packed.data(), packed.size() * 4, 16, "hair.test.configs");
                const BufferRef out = fc.graph.createBuffer(BufferDesc{ "hair.test.results", (uint64_t)count * threads * 64, 16 });
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Hair/Tests/HairBsdfProbe");
                fc.graph.addPass("hair.test.bsdf", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     b.use(cfg, Use::SrvCompute);
                                     b.use(out, Use::UavCompute);
                                 },
                                 [=](PassContext& c) {
                                     const uint32_t k[4] = { c.srv(cfg), c.uav(out), perThread, count };
                                     c.cmd->SetPipelineState(pso);
                                     c.computeConstants(k, 4);
                                     c.cmd->Dispatch(count, 1, 1);
                                 });
                results = tf.readbackBuffer(fc, out, (uint64_t)count * threads * 64);
            });
            const float* r = reinterpret_cast<const float*>(results->data());
            const double N = (double)threads * perThread;
            double worstFurnace = 0, worstPdf = 0, worstMoment = 0;
            for (uint32_t k = 0; k < count; ++k)
            {
                // per-thread partial sums -> mean and standard error of the mean (threads as batches)
                double sum[9] = {}, sq[9] = {};
                for (uint32_t t = 0; t < threads; ++t)
                {
                    const float* v = r + 16 * ((size_t)k * threads + t);
                    const double x[9] = { v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8] };  // albedoU rgb, pdfU, albedoI rgb, momentU, momentI
                    for (int i = 0; i < 9; ++i)
                    {
                        const double m = x[i] / perThread;
                        sum[i] += m;
                        sq[i] += m * m;
                    }
                }
                double mean[9], se[9];
                for (int i = 0; i < 9; ++i)
                {
                    mean[i] = sum[i] / threads;
                    se[i] = std::sqrt(std::max(0.0, sq[i] / threads - mean[i] * mean[i]) / threads);
                }
                if (!configs[k].absorbing)
                {
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        const double e = std::abs(mean[ch] - 1);
                        worstFurnace = std::max(worstFurnace, e);
                        S_CHECK(e <= 3 * se[ch] + 3e-3, "config %u: white furnace albedo[%d] = %.5f (se %.1e)", k, ch, mean[ch], se[ch]);
                    }
                    const double pe = std::abs(mean[3] - 1);
                    worstPdf = std::max(worstPdf, pe);
                    S_CHECK(pe <= 3 * se[3] + 3e-3, "config %u: pdf integrates to %.5f (se %.1e)", k, mean[3], se[3]);
                }
                const double me = std::abs(mean[8] - mean[7]);
                worstMoment = std::max(worstMoment, me);
                S_CHECK(me <= 4 * std::sqrt(se[7] * se[7] + se[8] * se[8]) + 2e-3, "config %u: sampled E[wi.x] %.5f vs pdf %.5f", k, mean[8], mean[7]);
                if (configs[k].absorbing)
                {
                    S_CHECK(mean[0] < 1 && mean[1] < mean[0] && mean[2] < mean[1], "absorbing fibre albedo (%.4f, %.4f, %.4f) not below 1 and ordered", mean[0], mean[1], mean[2]);
                    S_CHECK(std::abs(mean[4] - mean[0]) <= 4 * std::sqrt(se[0] * se[0] + se[4] * se[4]) + 3e-3, "absorbing: importance %.5f vs uniform %.5f", mean[4], mean[0]);
                    std::printf("absorbing fibre (sigma_a 0.5 / 1 / 2): albedo %.4f / %.4f / %.4f\n", mean[0], mean[1], mean[2]);
                }
            }
            std::printf("hair bsdf: %u configurations x %.0f samples - white furnace worst |1 - albedo| %.2e, pdf normalization %.2e, sampler moment %.2e\n", count, N,
                        worstFurnace, worstPdf, worstMoment);
        }
        const uint32_t errors = tf.device.drainDebugMessages();
        S_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("hair tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
