#include "../../Material/Tests/MTestFrame.h"
#include "unx/render/GpuLock.h"
#include "../TsrRejectPolicy.h"
#include <array>
#include <cstdio>
#include <fstream>
#include <sstream>
using namespace unx;
using namespace unx::render;
using namespace unx::mtest;
static_assert(shading::detail::useFusedTsrRejection(1280, 720, 15));
static_assert(shading::detail::useFusedTsrRejection(1706, 960, 15));
static_assert(!shading::detail::useFusedTsrRejection(1280, 720, 14)); // missing moire
static_assert(!shading::detail::useFusedTsrRejection(1280, 720, 13)); // missing thin geometry
static_assert(!shading::detail::useFusedTsrRejection(1280, 720, 11)); // missing layers
static_assert(shading::detail::useFusedTsrRejection(1280, 720, 7));  // production inputs, resurrection disabled
static_assert(shading::detail::useFusedTsrRejection(1706, 960, 7));
static_assert(!shading::detail::useFusedTsrRejection(1706, 960, 0));
static_assert(!shading::detail::useFusedTsrRejection(1279, 720, 15));
static_assert(!shading::detail::useFusedTsrRejection(1280, 719, 15));
static_assert(shading::detail::useFusedTsrRejection(1920, 1080, 0));
static_assert(shading::detail::useFusedTsrRejection(1920, 1080, 15));
int main(int argc, char **argv)
{
    try
    {
        bool timing = false, quick = false, fused = false;
        std::string evidencePath;
        uint32_t requestedWidth = 0, requestedHeight = 0, requestedOptions = UINT32_MAX;
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--timing") == 0) timing = true;
            else if (std::strcmp(argv[i], "--quick") == 0) quick = true;
            else if (std::strcmp(argv[i], "--fused") == 0) fused = true;
            else if (std::strcmp(argv[i], "--evidence") == 0 && i + 1 < argc) evidencePath = argv[++i];
            else if (std::strcmp(argv[i], "--options") == 0 && i + 1 < argc) requestedOptions = (uint32_t)std::stoul(argv[++i]);
            else if (std::strcmp(argv[i], "--width") == 0 && i + 1 < argc) requestedWidth = (uint32_t)std::stoul(argv[++i]);
            else if (std::strcmp(argv[i], "--height") == 0 && i + 1 < argc) requestedHeight = (uint32_t)std::stoul(argv[++i]);
            else fail("unknown argument %s", argv[i]);
        }
        if (requestedOptions != UINT32_MAX && requestedOptions > 15) fail("--options must be in [0,15]");
        if ((requestedWidth == 0) != (requestedHeight == 0) || requestedWidth > 16374 || requestedHeight > 16374)
            fail("--width and --height must both be supplied and fit the texture including its halo");
        if (timing)
            requireGpuLock("TSR global prefix A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 64);
        if (timing)
            test.profiler = &profiler;
        scene::Scene scene;
        scene.materials.emplace_back();
        test.setScene(scene);
        uint64_t checkedPixels = 0;
        struct Pair { uint32_t width, height, options, order; double reference, candidate; };
        std::vector<Pair> pairs;
        // Partial tiles, single pixels, tile boundaries, and all optional-input
        // combinations.
        std::vector<std::pair<uint32_t, uint32_t>> sizes = {{1, 1}, {15, 17}, {16, 16}, {17, 31}, {257, 35}, {1280, 720}, {1920, 1080}, {3840, 2160}};
        if (requestedWidth) sizes = {{requestedWidth, requestedHeight}};
        for (const auto [W, H] : sizes)
        {
            if (!requestedWidth && ((timing && W < 1280) || (!timing && W > 1920)))
                continue;
            if (!requestedWidth && quick && (timing ? W != 1920 : W == 1280)) continue;
            std::array<std::vector<double>, 4> times;
            std::array<std::vector<double>, 2> ratios;
            const uint32_t warmup = quick ? 4 : 30, measuredPairs = quick ? 16 : 120;
            for (uint32_t seed = 0; seed < (timing ? warmup + measuredPairs : W >= 1280 ? 2u : 32u); ++seed)
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
                            std::array<TextureRef, 4> prefix;
                            if (variant)
                                for (uint32_t stage = 0; stage < (fused ? 1u : 4u); ++stage)
                                {
                                    prefix[stage] = fc.graph.createTexture({"reject global prefix", W + (fused ? 10u : 14u), H + (fused ? 10u : 14u), 1, 1, DXGI_FORMAT_R32G32B32A32_UINT});
                                    fc.graph.addPass("tsr prefix " + std::to_string(stage), QueueType::Compute,
                                        [&](PassBuilder& b) {
                                            if (stage == 0) { b.use(in[0], Use::SrvCompute); b.use(in[1], Use::SrvCompute); }
                                            else b.use(prefix[stage - 1], Use::SrvCompute);
                                            if (stage == 2) b.use(prefix[0], Use::SrvCompute);
                                            b.use(prefix[stage], Use::UavCompute);
                                        },
                                        [&, prefix, stage, in](PassContext& c) {
                                            const uint32_t k[8] = { stage == 0 ? c.srv(in[0]) : 0u, stage == 0 ? c.srv(in[1]) : 0u,
                                                c.uav(prefix[stage]), W, H, stage == 2 ? c.srv(prefix[0]) : 0u,
                                                stage ? c.srv(prefix[stage - 1]) : 0u, 0 };
                                            c.cmd->SetPipelineState(fc.shaders.compute(fused ? "Passes/Shading/TsrRejectPrefix" : "Passes/Shading/Tests/TsrRejectSplitPrefix.MODE" + std::to_string(stage)));
                                            c.computeConstants(k, 8);
                                            if (fused) c.cmd->Dispatch((W + 25) / 16, (H + 25) / 16, 1);
                                            else c.cmd->Dispatch((W + 21) / 8, (H + 21) / 8, 1);
                                        });
                                }
                            fc.graph.addPass(
                                variant ? "tsr optimized" : "tsr reference", QueueType::Compute,
                                [&](PassBuilder &b)
                                {
                                    for (TextureRef t : in)
                                        b.use(t, Use::SrvCompute);
                                    if (variant)
                                    {
                                        if (fused) b.use(prefix[0], Use::SrvCompute);
                                        else { b.use(prefix[2], Use::SrvCompute); b.use(prefix[3], Use::SrvCompute); }
                                    }
                                    for (TextureRef t : out)
                                        b.use(t, Use::UavCompute);
                                    b.keep();
                                },
                                [&, in, out, variant, seed, prefix](PassContext &c)
                                {
                                    uint32_t k[16] = {
                                        c.srv(in[0]), c.srv(in[1]), c.srv(in[2]), c.uav(out[0]), c.uav(out[1]), c.uav(out[2]), W, H};
                                    float blend = 0.03f;
                                    std::memcpy(k + 8, &blend, 4);
                                    const uint32_t options = requestedOptions != UINT32_MAX ? requestedOptions : timing ? ((seed / 2) & 1u ? 15u : 0u) : W >= 1280 ? (seed ? 15u : 0u) : seed;
                                    for (uint32_t i = 3; i < 8; ++i)
                                        k[i + 6] = (options & (1u << std::min(i - 3, 3u))) ? c.srv(in[i]) : UINT32_MAX;
                                    if (variant) { k[14] = c.srv(prefix[fused ? 0 : 3]); k[15] = fused ? 0 : c.srv(prefix[2]); }
                                    c.cmd->SetPipelineState(fc.shaders.compute(variant ? (fused ? "Passes/Shading/TsrRejectFromPrefix" : "Passes/Shading/Tests/TsrRejectSplitTail")
                                                                                       : "Passes/Shading/TsrReject"));
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
                    if (seed >= warmup)
                    {
                        double sums[2] = {};
                        for (const auto& pass : test.lastTiming.passes)
                            if (pass.name == "tsr reference") sums[0] += pass.durationMs();
                            else if (pass.name == "tsr optimized" || pass.name.starts_with("tsr prefix ")) sums[1] += pass.durationMs();
                        M_CHECK(sums[0] > 0 && sums[1] > 0, "detailed pipeline timings unavailable");
                        for (uint32_t v = 0; v < 2; ++v) times[2 * ((seed / 2) & 1u) + v].push_back(sums[v]);
                        if (sums[0] > 0) ratios[(seed / 2) & 1u].push_back(sums[1] / sums[0]);
                        pairs.push_back({W, H, requestedOptions != UINT32_MAX ? requestedOptions : ((seed / 2) & 1u) ? 15u : 0u, seed & 1u, sums[0], sums[1]});
                    }
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
                    auto& ratio = ratios[options];
                    std::sort(ratio.begin(), ratio.end());
                    logf("TSR %s GPU %ux%u options=%u samples=%zu "
                         "reference_median_ms=%.6f optimized_median_ms=%.6f "
                         "reduction=%.2f%%\n",
                         fused ? "fused2pass" : "split5pass", W, H, requestedOptions != UINT32_MAX ? requestedOptions : options ? 15u : 0u, reference.size(), reference[reference.size() / 2], optimized[optimized.size() / 2],
                         100.0 * (1.0 - optimized[optimized.size() / 2] / reference[reference.size() / 2]));
                    logf("  same-input paired split/reference ratio median=%.4f range=[%.4f,%.4f] n=%zu; order alternated; all prefix+tail costs included\n",
                         ratio[ratio.size() / 2], ratio.front(), ratio.back(), ratio.size());
                }
        }
        if (timing)
        {
            if (!evidencePath.empty())
            {
                // Retain the sampler's current report before the lock wrapper
                // deletes it at process exit. A sub-second run may have none.
                char samplerPath[32768];
                std::string sampler;
                const DWORD length = GetEnvironmentVariableA("UNX_GPU_CONTENTION", samplerPath, (DWORD)sizeof samplerPath);
                if (length > 0 && length < sizeof samplerPath)
                {
                    std::ifstream input(samplerPath, std::ios::binary);
                    std::ostringstream text; text << input.rdbuf(); sampler = text.str();
                }
                std::ofstream out(evidencePath, std::ios::binary);
                if (!out) fail("cannot write evidence %s", evidencePath.c_str());
                out.precision(12);
                out << "{\"prototype\":\"" << (fused ? "fused2pass" : "split5pass")
                    << "\",\"productionKernels\":" << (fused ? "true" : "false") << ",\"cleanGpuAcceptance\":false,\"qualityHash\":\"" << test.quality.hash()
                    << "\",\"samplerScope\":\"whole process, including setup; unavailable is not zero contention\",\"contentionTelemetry\":"
                    << (sampler.empty() ? "null" : sampler) << ",\"pairs\":[";
                for (size_t i = 0; i < pairs.size(); ++i)
                {
                    const Pair& p = pairs[i];
                    if (i) out << ',';
                    out << "{\"width\":" << p.width << ",\"height\":" << p.height << ",\"options\":" << p.options
                        << ",\"candidateFirst\":" << (p.order ? "true" : "false") << ",\"referenceMs\":" << p.reference
                        << ",\"candidateMs\":" << p.candidate << ",\"ratio\":" << (p.reference > 0 ? p.candidate / p.reference : 0) << '}';
                }
                out << "]}\n";
            }
            return 0;
        }
        logf("PASS TSR %s global prefix clamp: %llu pixels, all three GPU outputs "
             "bit-identical; partial tiles, HDR/nonfinite, border, "
             "optional-input cases; GBV enabled\n",
             fused ? "fused2pass" : "split5pass", (unsigned long long)checkedPixels);
        return 0;
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
