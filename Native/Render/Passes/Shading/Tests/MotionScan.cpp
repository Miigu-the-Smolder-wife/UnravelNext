#include "../../Material/Tests/MTestFrame.h"
#include "unx/render/GpuLock.h"
#include <array>
using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main(int argc, char** argv)
{
    try
    {
        const bool timing = argc == 2 && std::strcmp(argv[1], "--timing") == 0;
        if (timing) requireGpuLock("rotation scan A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 32);
        if (timing) test.profiler = &profiler;
        scene::Scene scene; scene.materials.emplace_back(); test.setScene(scene);
        uint64_t checked = 0;
        for (uint32_t width : {1u, 31u, 32u, 33u, 1023u, 1024u, 1025u, 1920u, 3840u, 16384u})
        {
            if (timing && width != 1920 && width != 3840) continue;
            const uint32_t height = timing ? 1080u : 17u;
            std::array<std::vector<double>, 2> times;
            for (uint32_t seed = 0; seed < (timing ? 140u : 4u); ++seed)
            {
                std::array<std::shared_ptr<std::vector<uint8_t>>, 2> result;
                test.run([&](FramePassContext& fc) {
                    const std::array<TextureRef, 2> map = {
                        fc.graph.createTexture({"reference scan", width, height, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT}),
                        fc.graph.createTexture({"production scan", width, height, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT})};
                    fc.graph.addPass("scan input", QueueType::Graphics,
                        [&](PassBuilder& b) { for (auto t : map) b.use(t, Use::UavCompute); },
                        [&, map, seed](PassContext& c) {
                            const uint32_t k[8] = {c.uav(map[0]), c.uav(map[1]), width, height, seed};
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shading/Tests/MotionScanInput"));
                            c.computeConstants(k, 8); c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                        });
                    for (uint32_t slot = 0; slot < 2; ++slot)
                    {
                        const uint32_t variant = slot ^ (seed & 1u);
                        fc.graph.addPass(variant ? "scan optimized" : "scan reference", QueueType::Graphics,
                            [&](PassBuilder& b) { b.use(map[variant], Use::UavCompute); b.keep(); },
                            [&, map, variant](PassContext& c) {
                                const uint32_t k[8] = {0, c.uav(map[variant]), 0, 0, width, height};
                                c.cmd->SetPipelineState(fc.shaders.compute(variant ? "Passes/Shading/MotionRotation.STEP1" : "Passes/Shading/Tests/MotionScanReference"));
                                c.computeConstants(k, 8); c.cmd->Dispatch(height, 1, 1);
                            });
                        if (!timing) result[variant] = test.readback(fc, map[variant]);
                    }
                });
                if (timing)
                {
                    if (seed >= 20)
                        for (const auto& p : test.lastTiming.passes)
                            if (p.name == "scan reference" || p.name == "scan optimized")
                                times[p.name == "scan optimized"].push_back(p.durationMs());
                }
                else
                {
                    for (uint32_t y = 0; y < height; ++y)
                        M_CHECK(std::memcmp(result[0]->data() + y * TestFrame::rowPitch(width, 16),
                                            result[1]->data() + y * TestFrame::rowPitch(width, 16), width * 16) == 0,
                                "rotation prefix differs: width %u row %u seed %u", width, y, seed);
                    checked += uint64_t(width) * height * 4;
                }
            }
            if (timing)
            {
                for (auto& values : times) { M_CHECK(values.size() == 120, "missing GPU timings"); std::sort(values.begin(), values.end()); }
                logf("rotation scan %ux%u reference %.6f ms optimized %.6f ms reduction %.2f%% (120 pairs, alternating order)\n",
                     width, height, times[0][60], times[1][60], 100 * (1 - times[1][60] / times[0][60]));
            }
        }
        M_CHECK(test.device.drainDebugMessages() == 0, "D3D12 validation errors");
        if (!timing) logf("rotation scan: %llu float32 values bit-identical; no D3D12 validation errors\n", (unsigned long long)checked);
        return 0;
    }
    catch (const std::exception& e) { logf("FAIL: %s\n", e.what()); return 1; }
}
