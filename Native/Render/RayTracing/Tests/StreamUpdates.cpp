#include "../../Passes/Material/Tests/MTestFrame.h"
#include "unx/rt/RayScene.h"
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
        bool timing = false, fastTrace = false;
        for (int i = 1; i < argc; ++i)
        {
            timing |= std::strcmp(argv[i], "--timing") == 0;
            fastTrace |= std::strcmp(argv[i], "--fast-trace") == 0;
        }
        if (timing) requireGpuLock("fixed-topology stream RT A/B");
        TestFrame test(!timing, !timing);
        GpuProfiler profiler(test.device, 1, 256);
        if (timing) test.profiler = &profiler;
        scene::Scene scene; scene.materials.emplace_back(); test.setScene(scene);
        std::array<std::unique_ptr<rt::RayScene>, 2> rays;
        for (auto& r : rays) r = std::make_unique<rt::RayScene>(test.device, test.shaders, test.gpuScene, test.quality);
        std::array<std::vector<double>, 2> builds, traces;
        uint64_t checkedRays = 0;
        for (uint32_t frame = 0; frame < (timing ? 120u : 14u); ++frame)
        {
            const uint32_t cells = timing ? 256u : frame < 3 ? 4u : 8u;
            const uint32_t streams = timing ? 4u : frame == 8 ? 0u : frame == 2 || frame == 3 || frame >= 11 ? 2u : 1u;
            const bool fluid = !timing && (frame == 5 || frame == 6);
            const uint32_t flags = (!timing && frame == 5 ? 1u : !timing && frame == 9 ? 2u : 0u) | (timing || (frame & 1u) ? 4u : 0u);
            const float y = float(frame % 17) * 0.01f;
            std::array<std::shared_ptr<std::vector<uint8_t>>, 2> results;
            for (uint32_t order = 0; order < 2; ++order)
            {
                const uint32_t variant = order ^ (frame & 1u);
                test.quality.applyOverride(variant || fastTrace ? "raytracing.stream_refit=true" : "raytracing.stream_refit=false");
                test.quality.applyOverride(fastTrace && variant ? "raytracing.stream_fast_trace=true" : "raytracing.stream_fast_trace=false");
                const uint32_t side = timing ? 64 : 32;
                const uint32_t count = side * side * std::max(1u, streams);
                if (!timing && frame == 0)
                {
                    // A discarded recording must never make an unbuilt BLAS eligible for update.
                    RenderGraph abandoned(test.device);
                    FrameResources resources;
                    FrameServices services;
                    test.trackState.beginRecord();
                    FramePassContext fc{test.device, abandoned, test.shaders, test.quality, test.gpuScene, test.frame, resources, services,
                        [&](const ViewDesc& v) { return test.frameConstantsFor(v); }, &test.trackState};
                    TriangleStream stream;
                    stream.maxTriangles = cells * cells * 2;
                    stream.vertices = abandoned.createBuffer({"unsubmitted vertices", uint64_t(stream.maxTriangles) * 96, 0});
                    stream.fixedTopologyId = 1;
                    resources.triangleStreams.push_back(stream);
                    rays[variant]->record(fc);
                }
                test.trackState.beginRecord();
                test.run([&](FramePassContext& fc) {
                    for (uint32_t s = 0; s < streams; ++s)
                    {
                        const uint32_t triangles = cells * cells * 2;
                        const BufferRef vertices = fc.graph.createBuffer({"test water vertices", uint64_t(triangles) * 96, 0});
                        fc.graph.addPass("stream vertices", QueueType::Compute,
                            [&](PassBuilder& b) { b.use(vertices, Use::UavCompute); },
                            [&, vertices, s, flags, y, triangles](PassContext& c) {
                                const uint32_t k[8] = {c.uav(vertices), cells, s, flags, std::bit_cast<uint32_t>(y), 0, 0, 0};
                                c.cmd->SetPipelineState(fc.shaders.compute("RayTracing/Tests/StreamUpdateVertices"));
                                c.computeConstants(k, 8); c.cmd->Dispatch((triangles * 3 + 63) / 64, 1, 1);
                            });
                        TriangleStream stream;
                        stream.vertices = vertices; stream.maxTriangles = triangles;
                        stream.fixedTopologyId = fluid ? 0 : (frame >= 7 && !timing ? 10u : 1u) + ((!timing && frame == 12) ? streams - 1 - s : s);
                        fc.resources.triangleStreams.push_back(stream);
                    }
                    rays[variant]->record(fc);
                    const auto& stats = rays[variant]->stats();
                    if ((!variant && !fastTrace) || fluid || !frame || (!timing && frame == 8))
                        M_CHECK(stats.streamRefits == 0, "unsafe refit at frame %u variant %u", frame, variant);
                    if ((variant || fastTrace) && !timing && frame == 12) M_CHECK(stats.streamRefits == 0, "reordered owners reused stale slots");
                    if ((variant || fastTrace) && (timing ? frame > 0 : frame == 1 || frame == 10))
                        M_CHECK(stats.streamRefits == streams, "expected submitted refits at frame %u, got %u/%u", frame, stats.streamRefits, streams);
                    const BufferRef out = fc.graph.createBuffer({"stream ray results", uint64_t(count) * 24, 0});
                    fc.graph.addPass("stream rays", QueueType::Compute,
                        [&](PassBuilder& b) { rays[variant]->declareTraversal(b); b.use(out, Use::UavCompute); b.keep(); },
                        [&, out, count, variant](PassContext& c) {
                            uint32_t srvs[8]; rays[variant]->rootConstants(srvs);
                            const uint32_t k[4] = {srvs[1], c.uav(out), side, streams};
                            c.cmd->SetPipelineState(fc.shaders.compute("RayTracing/Tests/StreamUpdateProbe"));
                            c.computeConstants(k, 4); c.cmd->Dispatch((count + 63) / 64, 1, 1);
                        });
                    if (!timing) results[variant] = test.readbackBuffer(fc, out, uint64_t(count) * 24);
                });
                if (timing && frame >= 20) for (const auto& p : test.lastTiming.passes)
                {
                    if (p.name == "r.as.streams") builds[variant].push_back(p.durationMs());
                    if (p.name == "stream rays") traces[variant].push_back(p.durationMs());
                }
            }
            if (!timing)
            {
                M_CHECK(*results[0] == *results[1], "refit hit/distance/barycentrics differ at frame %u", frame);
                uint32_t hits = 0;
                for (size_t at = 0; at < results[0]->size(); at += 24)
                {
                    float distance; std::memcpy(&distance, results[0]->data() + at, 4);
                    if (distance >= 0)
                    {
                        ++hits;
                        M_CHECK(std::abs(distance - (2 - y)) < ((flags & 4u) ? 0.04001f : 1e-5f), "wrong analytic surface hit at frame %u", frame);
                    }
                }
                M_CHECK(streams ? hits > results[0]->size() / 24 * 9 / 10 : hits == 0, "missing/stale stream at frame %u", frame);
                checkedRays += results[0]->size() / 24;
            }
        }
        if (timing)
        {
            for (auto& a : builds) std::sort(a.begin(), a.end());
            for (auto& a : traces) std::sort(a.begin(), a.end());
            logf("Stream RT GPU n=%zu, 4 x 131072 triangles: build %.6f ms refit %.6f ms; traversal %.6f / %.6f ms; BLAS bytes %llu / %llu\n",
                builds[0].size(), builds[0][builds[0].size()/2], builds[1][builds[1].size()/2], traces[0][traces[0].size()/2], traces[1][traces[1].size()/2],
                (unsigned long long)rays[0]->stats().streamBlasBytes, (unsigned long long)rays[1]->stats().streamBlasBytes);
        }
        else logf("PASS stream RT %llu rays: hit, primitive, instance, facing and barycentrics bit-identical; unsubmitted record, moving vertices, growing pool, slot changes, NaN fluid deactivate/reactivate, disappear/recreate and finite degeneracy; GBV enabled\n", (unsigned long long)checkedRays);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
