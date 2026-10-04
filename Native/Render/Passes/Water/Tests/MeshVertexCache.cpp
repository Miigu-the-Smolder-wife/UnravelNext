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
        if (timing) requireGpuLock("pool vertex cache A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 64); if (timing) test.profiler = &profiler;
        scene::Scene scene; scene.materials.emplace_back(); test.setScene(scene);
        std::array<std::vector<double>, 4> times;
        for (uint32_t seed = 0; seed < (timing ? 240u : 6u); ++seed)
        {
            std::array<std::shared_ptr<std::vector<uint8_t>>, 12> result;
            test.run([&](FramePassContext& fc) {
                const auto field = fc.graph.createTexture({"water field", 512, 257, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT});
                const auto previous = fc.graph.createBuffer({"previous heights", 512 * 257 * 4, 0});
                const auto centre = fc.graph.createBuffer({"centre", 16, 0});
                fc.graph.addPass("water inputs", QueueType::Compute,
                    [&](PassBuilder& b) { b.use(field, Use::UavCompute); b.use(previous, Use::UavCompute); b.use(centre, Use::UavCompute); },
                    [&, field, previous, centre, seed](PassContext& c) {
                        const uint32_t k[4] = {c.uav(field), c.uav(previous), c.uav(centre), seed};
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/Water/Tests/MeshFieldPattern"));
                        c.computeConstants(k, 4); c.cmd->Dispatch(64, 33, 1);
                    });
                for (uint32_t shape = 0; shape < 2; ++shape)
                for (uint32_t order = 0; order < 2; ++order)
                {
                    const uint32_t variant = order ^ (seed & 1u);
                    const uint32_t count = shape ? 3 * 512 * 255 : 6 * 256 * 256;
                    const uint32_t storedCount = !shape && variant ? 257 * 257 : count;
                    const auto vertices = fc.graph.createBuffer({"surface vertices", uint64_t(storedCount) * 32, 0});
                    const auto velocities = fc.graph.createBuffer({"surface velocities", uint64_t(storedCount) * 16, 0});
                    const auto args = fc.graph.createBuffer({"draw args", 16, 0});
                    const std::string name = std::string(shape ? "round" : "pool") + (variant ? " cached" : " reference");
                    fc.graph.addPass(name, QueueType::Compute,
                        [&](PassBuilder& b) { b.use(field, Use::SrvCompute); b.use(previous, Use::SrvCompute); b.use(centre, Use::SrvCompute);
                            b.use(vertices, Use::UavCompute); b.use(velocities, Use::UavCompute); b.use(args, Use::UavCompute); b.keep(); },
                        [&, field, previous, centre, vertices, velocities, args, shape, variant, count, storedCount, seed](PassContext& c) {
                            auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
                            const float angle = float(seed % 5) * 0.37f, co = std::cos(angle), si = std::sin(angle);
                            uint32_t k[20] = {c.srv(field), c.srv(previous), c.uav(vertices), c.uav(velocities), c.uav(args), bits(seed % 3 ? 60.0f : 0.0f), bits(3.5f)};
                            k[8] = bits(seed & 1u ? 10000.0f : 0.0f);
                            if (shape)
                            { k[7] = c.srv(centre); k[9] = bits(-2.7f); k[10] = bits(2.1f); }
                            else
                            { k[10] = bits(-2.7f); k[11] = bits(5.0f / 256); k[16] = bits(3.0f / 256); }
                            k[12] = bits(co); k[13] = bits(-si); k[14] = bits(si); k[15] = bits(co);
                            const std::string kernel = std::string("Passes/Water/") + (variant ? "" : "Tests/") + (shape ? "RoundMesh" : "PoolMesh") + (variant ? "" : "Reference");
                            c.cmd->SetPipelineState(fc.shaders.compute(kernel)); c.computeConstants(k, 20);
                            if (!variant) c.cmd->Dispatch((count + 63) / 64, 1, 1);
                            else if (shape) c.cmd->Dispatch(64, 16, 1);
                            else c.cmd->Dispatch((storedCount + 63) / 64, 1, 1);
                        });
                    if (!timing)
                    {
                        const uint32_t base = shape * 6 + variant * 3;
                        result[base] = test.readbackBuffer(fc, vertices, uint64_t(storedCount) * 32);
                        result[base + 1] = test.readbackBuffer(fc, velocities, uint64_t(storedCount) * 16);
                        result[base + 2] = test.readbackBuffer(fc, args, 16);
                    }
                }
            });
            if (timing)
            {
                if (seed >= 40) for (const auto& p : test.lastTiming.passes)
                    for (uint32_t shape = 0; shape < 2; ++shape) for (uint32_t variant = 0; variant < 2; ++variant)
                        if (p.name == std::string(shape ? "round" : "pool") + (variant ? " cached" : " reference")) times[shape * 2 + variant].push_back(p.durationMs());
            }
            else for (uint32_t shape = 0; shape < 2; ++shape) for (uint32_t output = 0; output < 3; ++output)
            {
                if (!shape && output < 2)
                {
                    // Expand the indexed producer to the original corner order for the unchanged reference shader oracle.
                    const uint32_t stride = output == 0 ? 32 : 16;
                    const uint32_t dx[6] = {0,0,1,1,0,1}, dz[6] = {0,1,0,0,1,1};
                    auto& actual = *result[output + 3];
                    std::vector<uint8_t> expanded(size_t(6 * 256 * 256) * stride);
                    for (uint32_t corner = 0; corner < 6 * 256 * 256; ++corner)
                    {
                        const uint32_t cell = corner / 6;
                        const uint32_t vertex = (cell / 256 + dz[corner % 6]) * 257 + cell % 256 + dx[corner % 6];
                        M_CHECK(size_t(vertex + 1) * stride <= actual.size(), "pool index range");
                        std::memcpy(expanded.data() + size_t(corner) * stride, actual.data() + size_t(vertex) * stride, stride);
                    }
                    actual = std::move(expanded);
                }
                M_CHECK(*result[shape * 6 + output] == *result[shape * 6 + output + 3], "%s seed %u output %u differs", shape ? "round" : "pool", seed, output);
            }
        }
        if (timing) for (uint32_t shape = 0; shape < 2; ++shape)
        {
            auto& a = times[shape * 2]; auto& b = times[shape * 2 + 1]; std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
            logf("%s vertex GPU n=%zu reference %.6f ms cached %.6f ms reduction %.2f%%\n", shape ? "round" : "pool", a.size(), a[a.size()/2], b[b.size()/2], 100 * (1 - b[b.size()/2] / a[a.size()/2]));
        }
        else logf("PASS rectangular/round mesh positions, normals, velocities and draw counts bit-identical: six flat/wavy/rotated/far-origin/paused inputs; seam, fan and last partial ring; GBV enabled\n");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
