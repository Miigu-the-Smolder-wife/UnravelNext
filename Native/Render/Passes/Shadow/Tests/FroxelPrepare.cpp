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
        if (timing) requireGpuLock("froxel preparation A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 64); if (timing) test.profiler = &profiler;
        scene::Scene scene; scene.materials.emplace_back(); scene.cameras.emplace_back(); test.setScene(scene);
        auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
        auto vsm = makeBuffer(test.device, 2048, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped = nullptr; D3D12_RANGE none{ 0, 0 };
        check(vsm->Map(0, &none, &mapped), "froxel test constants");
        std::memset(mapped, 0, 2048); static_cast<float*>(mapped)[7] = 30; static_cast<float*>(mapped)[9] = 1;
        vsm->Unmap(0, nullptr);
        const uint32_t vsmCbv = test.device.descriptors().allocateResource();
        const D3D12_CONSTANT_BUFFER_VIEW_DESC cbv{ vsm->GetGPUVirtualAddress(), 2048 };
        test.device.d3d()->CreateConstantBufferView(&cbv, test.device.descriptors().resourceCpu(vsmCbv));
        std::array<std::vector<double>, 2> times;
        uint32_t checked = 0;
        for (uint32_t run = 0; run < (timing ? 120u : 96u); ++run)
        {
            const uint32_t flags = timing ? 15 : run % 32, slices = timing ? 64 : std::array<uint32_t, 3>{1, 37, 64}[run / 32];
            const uint32_t gx = timing ? 80 : 7, gy = timing ? 45 : 5, cells = gx * gy * slices;
            std::vector<uint32_t> words(16 + cells * 2, 0);
            words[0] = gx; words[1] = gy; words[2] = slices; words[3] = 16;
            words[4] = bits(0.1f); words[5] = bits(100.0f); words[6] = bits(std::log2(1000.0f)); words[8] = 64;
            for (uint32_t i = 0; i < cells; ++i) words[16 + i * 2 + 1] = i % 11 == 0 ? 1 : 0;
            auto lists = uploadStatic(test.device, words.data(), words.size() * 4, L"froxel fixture lists");
            std::array<std::shared_ptr<std::vector<uint8_t>>, 6> outputs;
            test.run([&](FramePassContext& fc) {
                auto view = test.mainView(fc, gx * 16 - 3, gy * 16 - 1);
                if (flags & 16) { view.view.clipPlane = { 0, 0, 1, -20 }; view.frameConstants = fc.frameConstantsFor(view.view); }
                const auto lights = fc.graph.importBuffer(lists.Get(), { "froxel fixture lists", words.size() * 4, 0 });
                const auto readers = fc.graph.createTexture({ "froxel readers", gx, gy, 1, 1, DXGI_FORMAT_R32G32_FLOAT });
                const auto media = fc.graph.createTexture({ "froxel media", gx, gy, (uint16_t)(slices * 2), 1, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D });
                const auto params = fc.graph.createTexture({ "froxel params", 11, 1, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
                fc.graph.addPass("froxel inputs", QueueType::Compute,
                    [&](PassBuilder& b) { b.use(readers, Use::UavCompute); b.use(media, Use::UavCompute); b.use(params, Use::UavCompute); },
                    [=, &fc](PassContext& c) {
                        const uint32_t k[8] = { c.uav(readers), c.uav(media), c.uav(params), 0, gx, gy, slices, run % 3 };
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/Shadow/Tests/FroxelPrepareInput")); c.computeConstants(k, 8);
                        c.cmd->Dispatch((gx + 7) / 8, (gy + 7) / 8, 1);
                    });
                for (uint32_t order = 0; order < 2; ++order)
                {
                    const uint32_t variant = order ^ (run & 1);
                    const auto queue = fc.graph.createBuffer({ "froxel queue", 16 + (uint64_t)cells * 4, 0 });
                    const auto args = fc.graph.createBuffer({ "froxel args", 16, 0 });
                    const auto air = fc.graph.createBuffer({ "froxel air", (uint64_t)cells * 64, 0 });
                    fc.graph.addPass("froxel clear", QueueType::Compute, [&](PassBuilder& b) { b.use(queue, Use::UavCompute); },
                        [=, &fc](PassContext& c) {
                            const uint32_t k[4] = { c.uav(queue), 0, 0, 0 };
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/Atmosphere/FroxelQueueArgs.MODE0")); c.computeConstants(k, 4); c.cmd->Dispatch(1, 1, 1);
                        });
                    fc.graph.addPass(variant ? "prepare optimized" : "prepare reference", QueueType::Compute,
                        [&](PassBuilder& b) { b.use(lights, Use::SrvCompute); b.use(readers, Use::SrvCompute); b.use(media, Use::SrvCompute); b.use(params, Use::SrvCompute);
                            b.use(queue, Use::UavCompute); b.use(air, Use::UavCompute); b.keep(); },
                        [=, &fc](PassContext& c) {
                            uint32_t k[40] = {}; k[0] = c.srv(lights); k[2] = k[3] = c.srv(params); k[7] = flags & 8 ? vsmCbv : UINT32_MAX;
                            k[14] = flags & 4 ? c.srv(readers) : UINT32_MAX; k[16] = flags & 1 ? c.srv(media) : UINT32_MAX;
                            k[18] = c.uav(queue); k[19] = c.uav(air); k[24] = flags & 2 ? 1 : 0;
                            k[28] = bits(0.03f); k[29] = bits(0.15f); k[30] = bits(2.0f); k[35] = bits(5.0f);
                            c.cmd->SetPipelineState(fc.shaders.compute(variant ? "Passes/Atmosphere/FroxelQueuePrepare" : "Passes/Shadow/Tests/FroxelQueuePrepareReference"));
                            c.bindFrameConstants(view.frameConstants); c.computeConstants(k, 40); c.cmd->Dispatch(gx, gy, 1);
                        });
                    fc.graph.addPass("froxel args", QueueType::Compute, [&](PassBuilder& b) { b.use(queue, Use::UavCompute); b.use(args, Use::UavCompute); },
                        [=, &fc](PassContext& c) {
                            const uint32_t k[4] = { c.uav(queue), c.uav(args), 0, 0 };
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/Atmosphere/FroxelQueueArgs.MODE1")); c.computeConstants(k, 4); c.cmd->Dispatch(1, 1, 1);
                        });
                    if (!timing) { outputs[variant * 3] = test.readbackBuffer(fc, queue, 16 + (uint64_t)cells * 4);
                        outputs[variant * 3 + 1] = test.readbackBuffer(fc, args, 16); outputs[variant * 3 + 2] = test.readbackBuffer(fc, air, (uint64_t)cells * 64); }
                }
            });
            if (timing)
            {
                if (run >= 24) for (const auto& p : test.lastTiming.passes)
                    if (p.name == "prepare optimized" || p.name == "prepare reference") times[p.name == "prepare optimized"].push_back(p.durationMs());
                continue;
            }
            M_CHECK(*outputs[1] == *outputs[4] && *outputs[2] == *outputs[5], "froxel case %u args/air mismatch", run);
            M_CHECK(std::memcmp(outputs[0]->data(), outputs[3]->data(), 16) == 0, "froxel case %u count mismatch", run);
            uint32_t count; std::memcpy(&count, outputs[0]->data(), 4); M_CHECK(count <= cells, "froxel count overflow");
            std::array<std::vector<uint32_t>, 2> items;
            for (uint32_t v = 0; v < 2; ++v) { items[v].resize(count); std::memcpy(items[v].data(), outputs[v * 3]->data() + 16, count * 4); std::sort(items[v].begin(), items[v].end()); }
            M_CHECK(items[0] == items[1] && std::adjacent_find(items[1].begin(), items[1].end()) == items[1].end(), "froxel case %u membership/duplicate mismatch", run);
            ++checked;
        }
        test.device.descriptors().freeResource(vsmCbv);
        if (timing) { for (auto& t : times) std::sort(t.begin(), t.end()); logf("Froxel prepare GPU n=%zu reference=%.6f ms optimized=%.6f ms\n", times[0].size(), times[0][times[0].size()/2], times[1][times[1].size()/2]); }
        else logf("PASS froxel prepare: %u GBV cases, queue membership/counts, indirect args and FP32 air scratch exact; media/fog/sky/readers/shadow/clip branches\n", checked);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
