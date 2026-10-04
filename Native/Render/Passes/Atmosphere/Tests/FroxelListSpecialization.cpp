// The unordered fill must produce exactly the old headers, statistics and lists.
#include "../../Material/Tests/MTestFrame.h"
#include "unx/render/GpuLock.h"
#include <array>
#include <bit>
#include <random>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main(int argc, char** argv)
{
    try
    {
        const bool timing = argc >= 2 && std::strcmp(argv[1], "--timing") == 0;
        const bool orderedTiming = argc == 3 && std::strcmp(argv[2], "--ordered") == 0;
        if (timing) requireGpuLock("froxel specialization A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 64);
        if (timing) test.profiler = &profiler;
        const uint32_t gx = timing ? 120 : 8, gy = timing ? 68 : 4, slices = 64;
        const uint32_t froxels = gx * gy * slices, blocks = (froxels + 2047) / 2048;
        std::mt19937 rng(531);
        std::uniform_real_distribution<float> unit(0, 1);
        std::array<std::vector<double>, 2> times;
        const std::vector<uint32_t> counts = timing ? std::vector<uint32_t>{128} : std::vector<uint32_t>{0, 1, 31, 254, 255, 700, 1025};
        uint64_t checkedLists = 0;
        for (uint32_t lightCount : counts)
        {
            scene::Scene scene;
            scene.materials.emplace_back();
            scene::Camera camera;
            camera.forward = {0, 0, 1};
            scene.cameras.push_back(camera);
            for (uint32_t i = 0; i < lightCount; ++i)
            {
                scene::Light l;
                l.type = (scene::LightType)(i % 6);
                l.position = {80 * unit(rng) - 40, 40 * unit(rng) - 20, 1 + 80 * unit(rng)};
                l.forward = normalize(float3{unit(rng) - 0.5f, unit(rng) - 0.5f, -0.1f - unit(rng)});
                l.right = normalize(cross(l.forward, float3{0, 1, 0.01f}));
                l.size = {0.1f + 2 * unit(rng), 0.1f + unit(rng)};
                l.range = timing ? 8 + 30 * unit(rng) : 500; // stored-candidate overflow in the dense cases
                l.intensity = 100 + 5000 * unit(rng);
                l.falloffExponent = i % 3 == 0 ? 2.5f : 0.0f;
                scene.lights.push_back(l);
            }
            test.setScene(scene);
            const uint32_t capacity = froxels * ((std::min(lightCount, 1024u) + 1u) & ~1u) + 2;
            const uint64_t bytes = 64 + uint64_t(froxels) * 8 + uint64_t(capacity) * 2;
            for (uint32_t iteration = 0; iteration < (timing ? 240u : 8u); ++iteration)
            {
                std::array<std::shared_ptr<std::vector<uint8_t>>, 2> results;
                test.run([&](FramePassContext& fc) {
                    const ViewResources view = test.mainView(fc, gx * 16, gy * 16);
                    for (uint32_t order = 0; order < 2; ++order)
                    {
                        const uint32_t variant = order ^ (iteration & 1u);
                        const BufferRef list = fc.graph.createBuffer({"lists", bytes, 0});
                        const BufferRef sums = fc.graph.createBuffer({"block sums", 16384, 0});
                        const BufferRef sceneAlloc = fc.graph.createBuffer({"scene allocation", uint64_t(froxels) * 4, 0});
                        const BufferRef candidates = fc.graph.createBuffer({"candidates", uint64_t(gx) * gy * 512, 0});
                        const bool cache = timing || (iteration & 1u) != 0;
                        const uint32_t fallback = timing ? 0u : ((iteration >> 1) & 1u);
                        const uint32_t flags = (timing ? orderedTiming : (iteration & 4u) != 0) ? 0u : 1u;
                        fc.graph.addPass("begin", QueueType::Compute,
                            [&](PassBuilder& b) { b.use(list, Use::UavCompute); },
                            [&, list](PassContext& c) {
                                const uint32_t k[8] = {c.uav(list), gx, gy, slices, 16, std::bit_cast<uint32_t>(0.1f), std::bit_cast<uint32_t>(100.0f), capacity};
                                c.cmd->SetPipelineState(fc.shaders.compute("Passes/Atmosphere/FroxelBegin"));
                                c.computeConstants(k, 8); c.cmd->Dispatch(1, 1, 1);
                            });
                        fc.graph.addPass("count", QueueType::Compute,
                            [&](PassBuilder& b) { b.use(list, Use::UavCompute); if (cache) b.use(candidates, Use::UavCompute); },
                            [&, list, candidates, cache, view](PassContext& c) {
                                const uint32_t k[12] = {c.uav(list), 96, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, 0,
                                    cache ? c.uav(candidates) : UINT32_MAX, 1, 0, 0, 0};
                                c.cmd->SetPipelineState(fc.shaders.compute("Passes/Atmosphere/FroxelLists.MODE0"));
                                c.bindFrameConstants(view.frameConstants); c.computeConstants(k, 12); c.cmd->Dispatch(gx, gy, 1);
                            });
                        for (uint32_t scan = 0; scan < 2; ++scan)
                            fc.graph.addPass("scan", QueueType::Compute,
                                [&](PassBuilder& b) { b.use(list, Use::UavCompute); b.use(sums, Use::UavCompute); b.use(sceneAlloc, Use::UavCompute); },
                                [&, list, sums, sceneAlloc, scan](PassContext& c) {
                                    const uint32_t k[4] = {c.uav(list), c.uav(sums), scan ? blocks : froxels, c.uav(sceneAlloc)};
                                    c.cmd->SetPipelineState(fc.shaders.compute(scan ? "Passes/Atmosphere/FroxelScan.MODE1" : "Passes/Atmosphere/FroxelScan.MODE0"));
                                    c.computeConstants(k, 4); c.cmd->Dispatch(scan ? 1 : blocks, 1, 1);
                                });
                        fc.graph.addPass(variant ? "specialized fill" : "reference fill", QueueType::Compute,
                            [&](PassBuilder& b) { b.use(list, Use::UavCompute); b.use(sums, Use::SrvCompute); b.use(sceneAlloc, Use::SrvCompute);
                                if (cache) b.use(candidates, Use::SrvCompute); b.keep(); },
                            [&, list, sums, sceneAlloc, candidates, cache, fallback, view, variant, flags](PassContext& c) {
                                const uint32_t k[12] = {c.uav(list), 96, UINT32_MAX, UINT32_MAX, c.srv(sums), fallback, c.srv(sceneAlloc),
                                    cache ? c.srv(candidates) : UINT32_MAX, flags, 0, 0, 0};
                                const char* kernel = !variant ? "Passes/Atmosphere/Tests/FroxelListsReference.MODE1" :
                                    flags ? "Passes/Atmosphere/FroxelListsUnordered" : "Passes/Atmosphere/FroxelLists.MODE1";
                                c.cmd->SetPipelineState(fc.shaders.compute(kernel));
                                c.bindFrameConstants(view.frameConstants); c.computeConstants(k, 12); c.cmd->Dispatch(gx, gy, 1);
                            });
                        if (!timing) results[variant] = test.readbackBuffer(fc, list, bytes);
                    }
                });
                if (timing)
                {
                    if (iteration >= 40) for (const auto& p : test.lastTiming.passes)
                        if (p.name == "reference fill" || p.name == "specialized fill") times[p.name == "specialized fill" ? 1 : 0].push_back(p.durationMs());
                    continue;
                }
                M_CHECK(std::memcmp(results[0]->data(), results[1]->data(), 64 + size_t(froxels) * 8) == 0,
                    "froxel header/statistics differ for %u lights, case %u", lightCount, iteration);
                for (uint32_t f = 0; f < froxels; ++f)
                {
                    uint32_t header[2];
                    std::memcpy(header, results[0]->data() + 64 + f * 8, 8);
                    const uint64_t offset = 64 + uint64_t(froxels) * 8 + uint64_t(header[0]) * 2;
                    M_CHECK(offset + header[1] * 2 <= bytes, "froxel list out of bounds");
                    M_CHECK(std::memcmp(results[0]->data() + offset, results[1]->data() + offset, header[1] * 2) == 0,
                        "froxel %u differs for %u lights, case %u", f, lightCount, iteration);
                }
                checkedLists += froxels;
            }
        }
        if (timing)
        {
            for (auto& v : times) std::sort(v.begin(), v.end());
            logf("Froxel GPU %s paired n=%zu: reference %.6f ms, specialized %.6f ms, reduction %.2f%%\n",
                orderedTiming ? "ordered" : "unordered", times[0].size(), times[0][times[0].size()/2], times[1][times[1].size()/2],
                100 * (1 - times[1][times[1].size()/2] / times[0][times[0].size()/2]));
        }
        else logf("PASS %llu froxel lists bit-identical: ordered/unordered, 0/1/31/254/255/700/1025 lights, six light types, cached/recull, fallback, partial words; GBV enabled\n",
            (unsigned long long)checkedLists);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
