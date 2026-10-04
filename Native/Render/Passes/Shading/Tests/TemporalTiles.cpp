// Compare complete temporal-filter outputs while changing only the output tile
// footprint (all halos and filter stages retained). Timing rotates dispatch order.
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
        if (timing) requireGpuLock("TSR output-tile A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 64);
        if (timing) test.profiler = &profiler;
        scene::Scene scene; scene.materials.emplace_back(); scene.cameras.emplace_back(); test.setScene(scene);
        const char* names[3] = { "TsrReject", "TsrFlicker", "TsrThin" };
        constexpr uint32_t tiles[3] = { 16, 17, 18 };
        uint64_t checkedPixels = 0;
        for (auto [w, h] : std::array<std::pair<uint32_t, uint32_t>, 6>{{{1, 1}, {29, 25}, {257, 35}, {1280, 720}, {1920, 1080}, {3840, 2160}}})
        {
            if (timing ? w < 1280 : w == 1920 || w == 3840) continue;
            std::array<std::vector<double>, 9> times;
            const uint32_t iterations = timing ? 90 : w == 1280 ? 2 : 32;
            for (uint32_t seed = 0; seed < iterations; ++seed)
            {
                std::array<std::shared_ptr<std::vector<uint8_t>>, 27> results;
                test.frame.deltaTime = (seed & 1u) ? 1.0f / 30 : 1.0f / 60;
                test.run([&](FramePassContext& fc) {
                    const auto view = test.mainView(fc, w, h);
                    const DXGI_FORMAT temporalFormats[10] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                        DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R8_UNORM };
                    const DXGI_FORMAT rejectFormats[8] = { DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM,
                        DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                        DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM };
                    std::array<TextureRef, 10> temporal;
                    std::array<TextureRef, 8> reject;
                    for (uint32_t i = 0; i < 10; ++i) temporal[i] = fc.graph.createTexture({ "temporal tile input", w, h, 1, 1, temporalFormats[i] });
                    for (uint32_t i = 0; i < 8; ++i) reject[i] = fc.graph.createTexture({ "reject tile input", w, h, 1, 1, rejectFormats[i] });
                    fc.graph.addPass("temporal tile inputs", QueueType::Compute,
                        [&](PassBuilder& b) { for (auto t : temporal) b.use(t, Use::UavCompute); for (auto t : reject) b.use(t, Use::UavCompute); },
                        [=, &fc](PassContext& c) {
                            uint32_t k[16] = {};
                            for (uint32_t i = 0; i < 10; ++i) k[i] = c.uav(temporal[i]);
                            k[12] = w; k[13] = h; k[14] = seed;
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shading/Tests/TemporalPattern"));
                            c.computeConstants(k, 16); c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                            for (uint32_t i = 0; i < 8; ++i) k[i] = c.uav(reject[i]);
                            k[8] = w; k[9] = h; k[10] = seed;
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shading/Tests/TsrCodeInput"));
                            c.computeConstants(k, 12); c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                        });
                    for (uint32_t shader = 0; shader < 3; ++shader)
                    for (uint32_t order = 0; order < 3; ++order)
                    {
                        const uint32_t variant = (order + seed) % 3, tile = tiles[variant], count = shader == 0 ? 3 : 2;
                        std::array<TextureRef, 3> out;
                        const DXGI_FORMAT formats[3][3] = {{ DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8_UNORM },
                            { DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_UNKNOWN },
                            { DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_UNKNOWN }};
                        for (uint32_t i = 0; i < count; ++i) out[i] = fc.graph.createTexture({ "tile result", w, h, 1, 1, formats[shader][i] });
                        const std::string name = std::string(names[shader]) + "." + std::to_string(tile);
                        fc.graph.addPass(name, QueueType::Compute,
                            [&](PassBuilder& b) {
                                if (shader == 0) { for (auto t : reject) b.use(t, Use::SrvCompute); }
                                else { for (auto t : temporal) b.use(t, Use::SrvCompute); }
                                for (uint32_t i = 0; i < count; ++i) b.use(out[i], Use::UavCompute); b.keep();
                            },
                            [=, &fc](PassContext& c) {
                                auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
                                uint32_t k[20] = {};
                                if (shader == 0)
                                {
                                    k[0] = c.srv(reject[0]); k[1] = c.srv(reject[1]); k[2] = c.srv(reject[2]);
                                    k[3] = c.uav(out[0]); k[4] = c.uav(out[1]); k[5] = c.uav(out[2]); k[6] = w; k[7] = h; k[8] = bits(0.03f);
                                    const uint32_t options = timing || w == 1280 ? seed & 1 ? 15 : 0 : seed;
                                    for (uint32_t i = 3; i < 8; ++i) k[i + 6] = options & (1u << std::min(i - 3, 3u)) ? c.srv(reject[i]) : UINT32_MAX;
                                }
                                else if (shader == 1)
                                {
                                    k[0] = c.srv(temporal[0]); k[1] = c.srv(temporal[4]); k[2] = c.srv(temporal[3]); k[3] = c.srv(temporal[5]);
                                    k[4] = c.uav(out[0]); k[5] = c.uav(out[1]); k[6] = w; k[7] = h;
                                    k[8] = seed & 1u; k[9] = seed & 2u ? c.srv(temporal[6]) : UINT32_MAX;
                                }
                                else
                                {
                                    k[0] = seed & 2u ? c.srv(temporal[7]) : UINT32_MAX; k[1] = c.srv(temporal[8]); k[2] = c.srv(temporal[9]); k[3] = c.srv(temporal[3]);
                                    k[4] = c.uav(out[0]); k[5] = c.uav(out[1]); k[6] = w; k[7] = h;
                                    k[8] = bits(1.0f); k[9] = bits(0.2f); k[10] = seed; k[11] = seed & 1u;
                                    k[12] = seed & 4u ? c.srv(temporal[0]) : UINT32_MAX; k[13] = bits(0.05f); k[14] = bits(0.2f); k[15] = bits(0.05f); k[16] = bits(0.25f);
                                }
                                c.cmd->SetPipelineState(fc.shaders.compute(std::string("Passes/Shading/Tests/") + names[shader] + "Tiles.TILE_SIZE" + std::to_string(tile)));
                                c.bindFrameConstants(view.frameConstants); c.computeConstants(k, 20);
                                c.cmd->Dispatch((w + tile - 1) / tile, (h + tile - 1) / tile, 1);
                            });
                        if (!timing) for (uint32_t i = 0; i < count; ++i) results[shader * 9 + variant * 3 + i] = test.readback(fc, out[i]);
                    }
                });
                if (timing)
                {
                    if (seed >= 18) for (const auto& p : test.lastTiming.passes)
                        for (uint32_t shader = 0; shader < 3; ++shader) for (uint32_t variant = 0; variant < 3; ++variant)
                            if (p.name == std::string(names[shader]) + "." + std::to_string(tiles[variant])) times[shader * 3 + variant].push_back(p.durationMs());
                    continue;
                }
                checkedPixels += uint64_t(w) * h;
                for (uint32_t shader = 0; shader < 3; ++shader)
                for (uint32_t variant = 1; variant < 3; ++variant)
                for (uint32_t i = 0; i < (shader == 0 ? 3u : 2u); ++i)
                {
                    const uint32_t bytes = shader == 0 ? i == 2 ? 2 : 4 : shader == 1 ? i == 0 ? 2 : 4 : 1;
                    const uint32_t pitch = TestFrame::rowPitch(w, bytes);
                    for (uint32_t y = 0; y < h; ++y)
                        M_CHECK(std::memcmp(results[shader * 9 + i]->data() + y * pitch,
                            results[shader * 9 + variant * 3 + i]->data() + y * pitch, w * bytes) == 0,
                            "%s %ux%u seed %u tile %u output %u row %u differs", names[shader], w, h, seed, tiles[variant], i, y);
                }
            }
            if (timing) for (uint32_t shader = 0; shader < 3; ++shader)
            {
                for (uint32_t variant = 0; variant < 3; ++variant) std::sort(times[shader * 3 + variant].begin(), times[shader * 3 + variant].end());
                logf("%s %ux%u n=%zu GPU ms tile16=%.6f tile17=%.6f tile18=%.6f\n", names[shader], w, h, times[shader * 3].size(),
                    times[shader * 3][times[shader * 3].size()/2], times[shader * 3 + 1][times[shader * 3 + 1].size()/2], times[shader * 3 + 2][times[shader * 3 + 2].size()/2]);
            }
        }
        if (!timing) logf("PASS temporal tile footprints: %llu pixels per shader, tile 16/17/18 all seven outputs bit-identical; GBV enabled\n", (unsigned long long)checkedPixels);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
