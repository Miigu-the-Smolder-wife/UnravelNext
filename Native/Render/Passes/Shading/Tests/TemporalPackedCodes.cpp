#include "../../Material/Tests/MTestFrame.h"
#include "unx/render/GpuLock.h"
#include <array>
#include <bit>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main(int argc, char** argv)
{
    try
    {
        const bool timing = argc == 2 && std::strcmp(argv[1], "--timing") == 0;
        if (timing) requireGpuLock("temporal packed-code A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 64);
        if (timing) test.profiler = &profiler;
        scene::Scene scene;
        scene.materials.emplace_back(); scene.cameras.emplace_back();
        test.setScene(scene);
        const char* names[3] = {"TsrResurrect", "TsrFlicker", "TsrThin"};
        uint64_t checkedPixels = 0;
        for (const auto [w, h] : std::array<std::pair<uint32_t, uint32_t>, 4>{{{1, 1}, {17, 31}, {257, 35}, {1920, 1080}}})
        {
            if (timing && w != 1920) continue;
            std::array<std::vector<double>, 6> times;
            for (uint32_t seed = 0; seed < (timing ? 240u : w == 1920 ? 8u : 32u); ++seed)
            {
                std::array<std::shared_ptr<std::vector<uint8_t>>, 12> results;
                test.frame.deltaTime = (seed & 1u) ? 1.0f / 30 : 1.0f / 60;
                test.run([&](FramePassContext& fc) {
                    const ViewResources view = test.mainView(fc, w, h);
                    const DXGI_FORMAT formats[10] = {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                        DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8_UNORM};
                    std::array<TextureRef, 10> in;
                    for (uint32_t i = 0; i < 10; ++i) in[i] = fc.graph.createTexture({"temporal input", w, h, 1, 1, formats[i]});
                    fc.graph.addPass("temporal inputs", QueueType::Compute,
                        [&](PassBuilder& b) { for (auto t : in) b.use(t, Use::UavCompute); },
                        [&, in, seed](PassContext& c) {
                            uint32_t k[16] = {};
                            for (uint32_t i = 0; i < 10; ++i) k[i] = c.uav(in[i]);
                            k[12] = w; k[13] = h; k[14] = seed;
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shading/Tests/TemporalPattern"));
                            c.computeConstants(k, 16); c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                        });
                    for (uint32_t shader = 0; shader < 3; ++shader)
                    for (uint32_t order = 0; order < 2; ++order)
                    {
                        const uint32_t variant = order ^ (seed & 1u);
                        const uint32_t resultBase = shader * 4 + variant * 2;
                        const auto format = shader == 0 ? DXGI_FORMAT_R8G8B8A8_UNORM : shader == 1 ? DXGI_FORMAT_R16_FLOAT : DXGI_FORMAT_R8_UNORM;
                        const TextureRef out = fc.graph.createTexture({"temporal result", w, h, 1, 1, format});
                        const TextureRef history = fc.graph.createTexture({"temporal history", w, h, 1, 1,
                            shader == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM});
                        const std::string pass = std::string(names[shader]) + (variant ? " optimized" : " reference");
                        fc.graph.addPass(pass, QueueType::Compute,
                            [&](PassBuilder& b) { for (auto t : in) b.use(t, Use::SrvCompute); b.use(out, Use::UavCompute);
                                if (shader != 0) b.use(history, Use::UavCompute); b.keep(); },
                            [&, in, out, history, view, shader, variant, seed](PassContext& c) {
                                auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
                                uint32_t k[20] = {};
                                if (shader == 0)
                                {
                                    k[0] = c.srv(in[0]); k[1] = c.srv(in[1]); k[2] = c.srv(in[2]); k[3] = c.srv(in[3]);
                                    k[4] = c.uav(out); k[5] = w; k[6] = h;
                                }
                                else if (shader == 1)
                                {
                                    k[0] = c.srv(in[0]); k[1] = c.srv(in[4]); k[2] = c.srv(in[3]); k[3] = c.srv(in[5]);
                                    k[4] = c.uav(out); k[5] = c.uav(history); k[6] = w; k[7] = h;
                                    k[8] = seed & 1u; k[9] = seed & 2u ? c.srv(in[6]) : UINT32_MAX;
                                }
                                else
                                {
                                    k[0] = seed & 2u ? c.srv(in[7]) : UINT32_MAX; k[1] = c.srv(in[8]);
                                    k[2] = c.srv(in[9]); k[3] = c.srv(in[3]); k[4] = c.uav(out); k[5] = c.uav(history); k[6] = w; k[7] = h;
                                    k[8] = bits(1.0f); k[9] = bits(0.2f); k[10] = seed; k[11] = seed & 1u;
                                    k[12] = seed & 4u ? c.srv(in[0]) : UINT32_MAX; k[13] = bits(0.05f);
                                    k[14] = bits(0.2f); k[15] = bits(0.05f); k[16] = bits(0.25f);
                                }
                                const std::string kernel = std::string("Passes/Shading/") + (variant ? "" : "Tests/") + names[shader] + (variant ? "" : "Reference");
                                c.cmd->SetPipelineState(fc.shaders.compute(kernel)); c.bindFrameConstants(view.frameConstants);
                                c.computeConstants(k, 20); c.cmd->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
                            });
                        if (!timing)
                        {
                            results[resultBase] = test.readback(fc, out);
                            if (shader != 0) results[resultBase + 1] = test.readback(fc, history);
                        }
                    }
                });
                if (timing)
                {
                    if (seed >= 40) for (const auto& p : test.lastTiming.passes)
                        for (uint32_t shader = 0; shader < 3; ++shader)
                        for (uint32_t variant = 0; variant < 2; ++variant)
                            if (p.name == std::string(names[shader]) + (variant ? " optimized" : " reference")) times[shader * 2 + variant].push_back(p.durationMs());
                    continue;
                }
                for (uint32_t shader = 0; shader < 3; ++shader)
                for (uint32_t output = 0; output < (shader == 0 ? 1u : 2u); ++output)
                {
                    const uint32_t bytes = shader == 0 ? 4 : shader == 1 ? (output == 0 ? 2 : 4) : 1;
                    const uint32_t pitch = TestFrame::rowPitch(w, bytes), base = shader * 4 + output;
                    for (uint32_t y = 0; y < h; ++y)
                        M_CHECK(std::memcmp(results[base]->data() + y * pitch, results[base+2]->data() + y * pitch, w * bytes) == 0,
                            "%s %ux%u seed %u output %u row %u differs", names[shader], w, h, seed, output, y);
                }
                checkedPixels += uint64_t(w) * h;
            }
            if (timing) for (uint32_t shader = 0; shader < 3; ++shader)
            {
                auto& a = times[2 * shader]; auto& b = times[2 * shader + 1];
                std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
                logf("%s GPU n=%zu reference %.6f ms optimized %.6f ms reduction %.2f%%\n", names[shader], a.size(), a[a.size()/2], b[b.size()/2], 100 * (1 - b[b.size()/2] / a[a.size()/2]));
            }
        }
        if (!timing) logf("PASS temporal packed codes: %llu pixels per shader, all five outputs bit-identical; resets, moving, HDR/nonfinite, partial tiles, optional inputs; GBV enabled\n", (unsigned long long)checkedPixels);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
