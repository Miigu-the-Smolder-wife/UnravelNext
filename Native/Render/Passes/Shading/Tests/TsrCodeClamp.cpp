#include "../../Material/Tests/MTestFrame.h"
#include "unx/render/GpuLock.h"
#include <array>
#include <cstdio>
using namespace unx;
using namespace unx::render;
using namespace unx::mtest;
int main(int argc, char **argv)
{
    try
    {
        const bool timing = argc == 2 && std::strcmp(argv[1], "--timing") == 0;
        if (timing)
            requireGpuLock("TSR packed-code A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 64);
        if (timing)
            test.profiler = &profiler;
        scene::Scene scene;
        scene.materials.emplace_back();
        test.setScene(scene);
        if (!timing)
        {
            std::shared_ptr<std::vector<uint8_t>> codes;
            test.run(
                [&](FramePassContext &fc)
                {
                    const BufferRef checks = fc.graph.createBuffer({"TSR code checks", 2048 * 4, 4});
                    fc.graph.addPass(
                        "TSR code order", QueueType::Compute, [&](PassBuilder &b) { b.use(checks, Use::UavCompute); },
                        [&, checks](PassContext &c)
                        {
                            const uint32_t k = c.uav(checks);
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shading/Tests/TsrCodeOrder"));
                            c.computeConstants(&k, 1);
                            c.cmd->Dispatch(32, 1, 1);
                        });
                    codes = test.readbackBuffer(fc, checks, 2048 * 4);
                });
            for (uint32_t i = 0; i < 2048; ++i)
            {
                uint32_t error;
                std::memcpy(&error, codes->data() + i * 4, 4);
                M_CHECK(error == 0, "TSR code %u violates roundtrip/order: %u", i, error);
            }
            logf("PASS GPU exhaustive channel codes: exact roundtrip and strict order\n");
        }
        uint64_t checkedPixels = 0;
        // Partial tiles, single pixels, tile boundaries, and all optional-input
        // combinations.
        for (const auto [W, H] :
             std::array<std::pair<uint32_t, uint32_t>, 6>{{{1, 1}, {15, 17}, {16, 16}, {17, 31}, {257, 35}, {1920, 1080}}})
        {
            if (timing && W != 1920)
                continue;
            std::array<std::vector<double>, 4> times;
            for (uint32_t seed = 0; seed < (timing ? 660u : W == 1920 ? 2u : 32u); ++seed)
            {
                std::array<std::shared_ptr<std::vector<uint8_t>>, 6> results;
                test.run(
                    [&](FramePassContext &fc)
                    {
                        const DXGI_FORMAT formats[8] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                                                        DXGI_FORMAT_R8G8_UNORM,         DXGI_FORMAT_R16_FLOAT,
                                                        DXGI_FORMAT_R8_UNORM,           DXGI_FORMAT_R8G8B8A8_UNORM,
                                                        DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_R10G10B10A2_UNORM};
                        std::array<TextureRef, 8> in;
                        for (uint32_t i = 0; i < 8; ++i)
                            in[i] = fc.graph.createTexture({"tsr input", W, H, 1, 1, formats[i]});
                        fc.graph.addPass(
                            "tsr synthetic inputs", QueueType::Compute,
                            [&](PassBuilder &b)
                            {
                                for (TextureRef t : in)
                                    b.use(t, Use::UavCompute);
                            },
                            [&, in, seed](PassContext &c)
                            {
                                uint32_t k[12] = {};
                                for (uint32_t i = 0; i < 8; ++i)
                                    k[i] = c.uav(in[i]);
                                k[8] = W;
                                k[9] = H;
                                k[10] = seed;
                                c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shading/Tests/TsrCodeInput"));
                                c.computeConstants(k, 12);
                                c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                            });
                        for (uint32_t slot = 0; slot < 2; ++slot)
                        {
                            // Alternate dispatch order to remove the first-pass/cache
                            // advantage.
                            const uint32_t variant = slot ^ (seed & 1u);
                            std::array<TextureRef, 3> out = {fc.graph.createTexture({"reject", W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM}),
                                                             fc.graph.createTexture({"guide", W, H, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM}),
                                                             fc.graph.createTexture({"AA", W, H, 1, 1, DXGI_FORMAT_R8G8_UNORM})};
                            fc.graph.addPass(
                                variant ? "tsr optimized" : "tsr reference", QueueType::Compute,
                                [&](PassBuilder &b)
                                {
                                    for (TextureRef t : in)
                                        b.use(t, Use::SrvCompute);
                                    for (TextureRef t : out)
                                        b.use(t, Use::UavCompute);
                                    b.keep();
                                },
                                [&, in, out, variant, seed](PassContext &c)
                                {
                                    uint32_t k[16] = {
                                        c.srv(in[0]), c.srv(in[1]), c.srv(in[2]), c.uav(out[0]), c.uav(out[1]), c.uav(out[2]), W, H};
                                    float blend = 0.03f;
                                    std::memcpy(k + 8, &blend, 4);
                                    const uint32_t options = timing ? ((seed / 2) & 1u ? 15u : 0u) : W == 1920 ? (seed ? 15u : 0u) : seed;
                                    for (uint32_t i = 3; i < 8; ++i)
                                        k[i + 6] = (options & (1u << std::min(i - 3, 3u))) ? c.srv(in[i]) : UINT32_MAX;
                                    c.cmd->SetPipelineState(fc.shaders.compute(variant ? "Passes/Shading/TsrReject"
                                                                                       : "Passes/Shading/Tests/TsrRejectReference"));
                                    c.computeConstants(k, 16);
                                    c.cmd->Dispatch((W + 15) / 16, (H + 15) / 16, 1);
                                });
                            if (!timing)
                                for (uint32_t i = 0; i < 3; ++i)
                                    results[variant * 3 + i] = test.readback(fc, out[i]);
                        }
                    });
                if (timing)
                {
                    if (seed >= 60)
                        for (const auto &pass : test.lastTiming.passes)
                            if (pass.name == "tsr reference" || pass.name == "tsr optimized")
                                times[2 * ((seed / 2) & 1u) + (pass.name == "tsr optimized" ? 1 : 0)].push_back(pass.durationMs());
                    continue;
                }
                checkedPixels += uint64_t(W) * H;
                for (uint32_t i = 0; i < 3; ++i)
                {
                    const uint32_t bytes = i == 2 ? 2 : 4, pitch = TestFrame::rowPitch(W, bytes);
                    for (uint32_t y = 0; y < H; ++y)
                        M_CHECK(std::memcmp(results[i]->data() + y * pitch, results[i + 3]->data() + y * pitch, W * bytes) == 0,
                                "TSR seed %u output %u row %u differs", seed, i, y);
                }
            }
            if (timing)
                for (uint32_t options = 0; options < 2; ++options)
                {
                    auto &reference = times[2 * options];
                    auto &optimized = times[2 * options + 1];
                    std::sort(reference.begin(), reference.end());
                    std::sort(optimized.begin(), optimized.end());
                    logf("TSR GPU 1920x1080 options=%s samples=%zu "
                         "reference_median_ms=%.6f optimized_median_ms=%.6f "
                         "reduction=%.2f%%\n",
                         options ? "all" : "none", reference.size(), reference[reference.size() / 2], optimized[optimized.size() / 2],
                         100.0 * (1.0 - optimized[optimized.size() / 2] / reference[reference.size() / 2]));
                }
        }
        if (timing)
            return 0;
        logf("PASS TSR packed-code clamp: %llu pixels, all three GPU outputs "
             "bit-identical; partial tiles, HDR/nonfinite, border, all "
             "optional-input combinations; GBV enabled\n",
             (unsigned long long)checkedPixels);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
