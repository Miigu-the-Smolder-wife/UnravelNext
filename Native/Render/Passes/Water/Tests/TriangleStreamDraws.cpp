#include "../../Material/Tests/MTestFrame.h"
#include "unx/render/GpuLock.h"
#include "unx/water/WaterSunMap.h"
#include <array>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main(int argc, char** argv)
{
    try
    {
        const bool timing = argc == 2 && std::strcmp(argv[1], "--timing") == 0;
        if (timing) requireGpuLock("triangle stream live dispatch A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 128);
        if (timing) test.profiler = &profiler;
        scene::Scene scene; scene.materials.emplace_back(); test.setScene(scene);
        const uint32_t capacities[] = {1, 31, 32, 33, 4096, 2097120, 2097121, 1u << 24};
        const uint32_t vertices[] = {0, 1, 3, 93, 96, 99, 12288, 6291363, UINT32_MAX};
        if (!timing) for (uint32_t frame = 0; frame < 4; ++frame)
        {
            std::array<ComPtr<ID3D12Resource>, kMaxTriangleStreams> input;
            std::vector<uint32_t> active;
            for (uint32_t i = 0; i < input.size(); ++i)
            {
                const uint32_t args[4] = {vertices[(i + frame) % std::size(vertices)], 1, 0, 0};
                input[i] = uploadStatic(test.device, args, sizeof args, L"test draw count");
                if (!(frame == 2 && i % 3 == 0)) active.push_back(i);
            }
            std::shared_ptr<std::vector<uint8_t>> result;
            test.run([&](FramePassContext& fc) {
                for (uint32_t i = 0; i < input.size(); ++i)
                {
                    TriangleStream stream;
                    stream.drawArgs = fc.graph.importBuffer(input[i].Get(), {"draw count", 16, 0});
                    stream.vertices = stream.drawArgs; // unused by argument generation
                    stream.maxTriangles = capacities[i % std::size(capacities)];
                    if (frame == 2 && i % 3 == 0)
                    {
                        stream.fixedTopologyId = i + 1;
                        stream.knownTriangleCount = std::min(vertices[(i + frame) % std::size(vertices)] / 3, stream.maxTriangles);
                    }
                    fc.resources.triangleStreams.push_back(stream);
                }
                prepareTriangleStreamDraws(fc);
                const auto args = fc.resources.triangleStreams[active[0]].meshArgs;
                prepareTriangleStreamDraws(fc); // already prepared in this recording
                M_CHECK(fc.resources.triangleStreams[active[0]].meshArgs.id == args.id, "prepared arguments replaced");
                for (const auto& stream : fc.resources.triangleStreams)
                    if (stream.fixedTopologyId) M_CHECK(!stream.meshArgs.valid(), "fixed-capacity producer got redundant GPU arguments");
                result = test.readbackBuffer(fc, args, active.size() * 12);
            });
            for (uint32_t slot = 0; slot < active.size(); ++slot)
            {
                const uint32_t i = active[slot];
                uint32_t got[3]; std::memcpy(got, result->data() + slot * 12, 12);
                const uint64_t live = std::min(vertices[(i + frame) % std::size(vertices)] / 3, capacities[i % std::size(capacities)]);
                const uint64_t groups = (live + 31) / 32;
                M_CHECK(got[0] == std::min<uint64_t>(groups, 65535) && got[1] == std::max<uint64_t>(1, (groups + 65534) / 65535) && got[2] == 1,
                    "stream %u frame %u: wrong dispatch %u,%u,%u", i, frame, got[0], got[1], got[2]);
            }
        }
        // Actual raster: tiny live surface in a large valid allocation. Compare all
        // sun-map inputs consumed by underwater lighting, alternating execution order.
        constexpr uint32_t capacity = 1u << 20;
        const float data[6][8] = {
            {-.2f, 0, -.2f, 1, 0, 1, 0, 0}, {.2f, .04f, -.2f, 1, 0, 1, 0, 0}, {.2f, .04f, .2f, 1, 0, 1, 0, 0},
            {-.2f, 0, -.2f, 1, 0, 1, 0, 0}, {.2f, .04f, .2f, 1, 0, 1, 0, 0}, {-.2f, 0, .2f, 1, 0, 1, 0, 0}};
        auto vertex = makeBuffer(test.device, uint64_t(capacity) * 96, D3D12_HEAP_TYPE_DEFAULT);
        auto upload = uploadStatic(test.device, data, sizeof data, L"surface upload");
        auto command = test.device.acquireCommandList(QueueType::Graphics);
        command.list->CopyBufferRegion(vertex.Get(), 0, upload.Get(), 0, sizeof data);
        test.device.queue(QueueType::Graphics).waitCpu(test.device.submit(command));
        water::WaterSunMap maps[2] = {water::WaterSunMap(test.device), water::WaterSunMap(test.device)};
        std::array<std::vector<double>, 2> times;
        std::vector<double> argumentTimes, indirectTotalTimes;
        for (uint32_t iteration = 0; iteration < (timing ? 240u : 3u); ++iteration)
        {
            const uint32_t args[4] = {timing ? 6u : iteration * 3, 1, 0, 0};
            auto draw = uploadStatic(test.device, args, sizeof args, L"surface count");
            std::array<std::shared_ptr<std::vector<uint8_t>>, 6> results;
            uint32_t side = 0;
            test.run([&](FramePassContext& fc) {
                TriangleStream stream;
                stream.vertices = fc.graph.importBuffer(vertex.Get(), {"surface", uint64_t(capacity) * 96, 0});
                stream.drawArgs = fc.graph.importBuffer(draw.Get(), {"surface draw", 16, 0});
                stream.maxTriangles = capacity; stream.boundsMin = {-.2f, 0, -.2f}; stream.boundsMax = {.2f, .04f, .2f};
                fc.resources.triangleStreams.push_back(stream);
                prepareTriangleStreamDraws(fc);
                for (uint32_t order = 0; order < 2; ++order)
                {
                    const uint32_t variant = order ^ (iteration & 1u);
                    water::WaterSunStream in;
                    in.stream = variant ? fc.resources.triangleStreams[0] : stream;
                    const auto out = maps[variant].record(fc.graph, fc.shaders, fc.frame.frameIndex, {in}, {0, 1, 0});
                    side = out.texels;
                    if (timing) fc.graph.addPass("keep sun map", QueueType::Graphics,
                        [&](PassBuilder& b) { b.use(out.normal, Use::SrvCompute); b.keep(); }, [](PassContext&) {});
                    else
                    {
                        results[variant * 3] = test.readback(fc, out.depth);
                        results[variant * 3 + 1] = test.readback(fc, out.normal);
                        results[variant * 3 + 2] = test.readback(fc, out.medium);
                    }
                }
            });
            if (timing)
            {
                uint32_t order = 0;
                double argumentMs = 0, indirectMs = 0;
                for (const auto& p : test.lastTiming.passes)
                    if (p.name == "stream draw arguments") argumentMs += p.durationMs();
                for (const auto& p : test.lastTiming.passes) if (p.name == "w.sun map raster")
                {
                    const uint32_t variant = order ^ (iteration & 1u);
                    if (variant) indirectMs += p.durationMs();
                    if (iteration >= 40) times[variant].push_back(p.durationMs());
                    ++order;
                }
                if (iteration >= 40)
                {
                    argumentTimes.push_back(argumentMs);
                    indirectTotalTimes.push_back(argumentMs + indirectMs);
                }
            }
            else for (uint32_t output = 0; output < 3; ++output)
            {
                const uint32_t bytes = output == 0 ? 4 : 8, pitch = TestFrame::rowPitch(side, bytes);
                for (uint32_t y = 0; y < side; ++y)
                    M_CHECK(std::memcmp(results[output]->data() + y * pitch, results[output + 3]->data() + y * pitch, side * bytes) == 0,
                        "sun map differs: live %u, output %u, row %u", args[0] / 3, output, y);
            }
        }
        if (timing)
        {
            for (auto& v : times) std::sort(v.begin(), v.end());
            std::sort(argumentTimes.begin(), argumentTimes.end());
            std::sort(indirectTotalTimes.begin(), indirectTotalTimes.end());
            logf("Stream raster GPU n=%zu capacity %u live 2: direct %.6f ms indirect %.6f ms reduction %.2f%%\n",
                times[0].size(), capacity, times[0][times[0].size()/2], times[1][times[1].size()/2],
                100 * (1 - times[1][times[1].size()/2] / times[0][times[0].size()/2]));
            logf("Stream argument generation %.6f ms; indirect raster plus generation %.6f ms (%.2f%% less than direct raster)\n",
                argumentTimes[argumentTimes.size()/2], indirectTotalTimes[indirectTotalTimes.size()/2],
                100 * (1 - indirectTotalTimes[indirectTotalTimes.size()/2] / times[0][times[0].size()/2]));
        }
        else logf("PASS stream dispatch: 63 streams x 4 frames, zero/partial/overflow/two-dimensional counts; sun-map depth/normal/medium bit-identical for 0/1/2 live triangles; GBV enabled\n");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
