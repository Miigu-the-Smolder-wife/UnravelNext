// Actual WaterTrack reconstruction/refit versus unchanged NaN-tail FluidSurface/full BLAS builds.
// No draw/vertex/ray tolerance: active bytes and all hit fields must be identical.
#include "../../Passes/Material/Tests/MTestFrame.h"
#include "unx/rt/RayScene.h"
#include "unx/water/FluidSurface.h"
#include <array>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main()
{
    try
    {
        TestFrame test(true, true);
        scene::Scene scene; scene.materials.emplace_back(); test.setScene(scene);
        std::array<std::unique_ptr<rt::RayScene>, 2> rays;
        for (auto& r : rays) r = std::make_unique<rt::RayScene>(test.device, test.shaders, test.gpuScene, test.quality);
        std::unique_ptr<water::FluidSurface> reference;
        uint32_t referenceCapacity = 0;
        constexpr uint32_t side = 48, rayCount = 6 * side * side;
        uint64_t checkedRays = 0, refits = 0;
        const uint32_t counts[] = {512, 64, 1, 512, 512, 0, 512, 5000, 5000, 64, 512, 512};
        for (uint32_t frame = 0; frame < std::size(counts); ++frame)
        {
            const uint32_t count = counts[frame];
            const uint32_t width = count > 512 ? 18 : count > 64 ? 8 : 4;
            std::vector<std::array<float, 12>> records(std::max(1u, count));
            for (uint32_t i = 0; i < count; ++i)
            {
                auto& p = records[i];
                p[0] = 2.25f + float(i % width) * .5f + (frame == 4 ? 2.0f : 0);
                p[1] = 2.25f + float(i / width % width) * .5f;
                p[2] = 2.25f + float(i / (width * width)) * .5f;
                p[4] = .5f; p[5] = -.25f; p[6] = .125f;
                std::memcpy(&p[11], &i, 4);
            }
            auto particles = uploadStatic(test.device, records.data(), records.size() * sizeof(records[0]), L"fluid test particles");
            FluidFrame fluid;
            fluid.current = particles.Get(); fluid.count = count; fluid.stride = 48; fluid.dx = .05f; fluid.alpha = 1;
            fluid.domainCells[0] = fluid.domainCells[1] = fluid.domainCells[2] = 32;
            fluid.origin[0] = frame >= 8 ? -1024 : 0;
            fluid.frameVelocity[0] = .25f;
            if (frame == 11)
            {
                fluid.start = particles.Get(); fluid.startCount = count; fluid.startValid = true; fluid.alpha = .5f;
                std::copy(std::begin(fluid.origin), std::end(fluid.origin), fluid.startOrigin);
                fluid.startOrigin[1] = -.1;
            }
            test.frame.fluids = &fluid; test.frame.fluidCount = 1;
            test.frame.streamAxes[2] = frame >= 10 ? -1.0f : 1.0f;
            float origin[3];
            for (int a = 0; a < 3; ++a) origin[a] = float(fluid.startValid ? fluid.startOrigin[a] + (fluid.origin[a] - fluid.startOrigin[a]) * fluid.alpha : fluid.origin[a]);
            if (count && (!reference || count > referenceCapacity))
            {
                water::FluidSurfaceDesc d;
                d.nodes[0] = d.nodes[1] = d.nodes[2] = 64; d.h = .025f;
                d.maxParticles = std::max(4096u, count * 5 / 4); d.maxTriangles = 2 * d.maxParticles;
                referenceCapacity = d.maxParticles;
                reference = std::make_unique<water::FluidSurface>(test.device, test.shaders, d);
            }
            if (reference) reference->setOrigin(origin);
            auto produce = [&](FramePassContext& fc, uint32_t variant) {
                if (variant) { tracks::waterGeometry(fc); return; }
                if (!count) return;
                water::FluidSurfaceInput in;
                in.particles = fc.graph.importBuffer(particles.Get(), {"reference particles", particles->GetDesc().Width, 0});
                in.count = count; in.stride = 48; in.velocityOffset = 16; in.velocityScale = fluid.dx;
                in.frameVelocity[0] = fluid.frameVelocity[0]; in.axes[2] = test.frame.streamAxes[2];
                if (fluid.startValid) { in.previous = in.particles; in.previousSlotOffset = 44; in.alpha = fluid.alpha; }
                const auto out = reference->record(fc.graph, in);
                TriangleStream stream;
                stream.vertices = out.vertices; stream.velocities = out.velocities; stream.drawArgs = out.draw;
                stream.maxTriangles = reference->desc().maxTriangles; // zero topology ID: full build with NaN-inactive tail
                fc.resources.triangleStreams.push_back(stream);
            };
            std::array<std::shared_ptr<std::vector<uint8_t>>, 2> hits, vertices, velocities, draw;
            for (uint32_t variant = 0; variant < 2; ++variant)
            {
                if (frame == 0 || frame == 4 || frame == 7)
                {
                    // First use, changed contents, and growth recorded but never submitted.
                    RenderGraph abandoned(test.device); FrameResources resources; FrameServices services;
                    test.trackState.beginRecord();
                    FramePassContext fc{test.device, abandoned, test.shaders, test.quality, test.gpuScene, test.frame, resources, services,
                        [&](const ViewDesc& v) { return test.frameConstantsFor(v); }, &test.trackState};
                    produce(fc, variant); rays[variant]->record(fc);
                }
                test.trackState.beginRecord();
                test.run([&](FramePassContext& fc) {
                    produce(fc, variant);
                    if (count)
                    {
                        M_CHECK(fc.resources.triangleStreams.size() == 1, "missing fluid stream frame %u", frame);
                        const auto& stream = fc.resources.triangleStreams[0];
                        M_CHECK(bool(stream.fixedTopologyId) == bool(variant), "producer topology contract frame %u", frame);
                        draw[variant] = test.readbackBuffer(fc, stream.drawArgs, 16);
                        vertices[variant] = test.readbackBuffer(fc, stream.vertices, uint64_t(stream.maxTriangles) * 96);
                        velocities[variant] = test.readbackBuffer(fc, stream.velocities, uint64_t(stream.maxTriangles) * 48);
                    }
                    rays[variant]->record(fc);
                    const auto& stats = rays[variant]->stats();
                    if (!variant || frame == 0 || frame == 5 || frame == 6 || frame == 7)
                        M_CHECK(stats.streamRefits == 0, "unsafe fluid refit frame %u variant %u", frame, variant);
                    else { M_CHECK(stats.streamRefits == 1, "missing fluid refit frame %u", frame); ++refits; }
                    const BufferRef out = fc.graph.createBuffer({"fluid ray results", uint64_t(rayCount) * 24, 0});
                    fc.graph.addPass("fluid stream rays", QueueType::Compute,
                        [&](PassBuilder& b) { rays[variant]->declareTraversal(b); b.use(out, Use::UavCompute); b.keep(); },
                        [&, out, variant](PassContext& c) {
                            uint32_t srvs[8]; rays[variant]->rootConstants(srvs);
                            uint32_t k[12] = {srvs[1], c.uav(out), side, 0};
                            float lo[3], hi[3];
                            for (int a = 0; a < 3; ++a)
                            {
                                lo[a] = origin[a]; hi[a] = origin[a] + 1.6f;
                                if (test.frame.streamAxes[a] < 0) { const float old = lo[a]; lo[a] = -hi[a]; hi[a] = -old; }
                            }
                            std::memcpy(k + 4, lo, 12); std::memcpy(k + 8, hi, 12);
                            c.cmd->SetPipelineState(fc.shaders.compute("RayTracing/Tests/FluidStreamProbe"));
                            c.computeConstants(k, 12); c.cmd->Dispatch((rayCount + 63) / 64, 1, 1);
                        });
                    hits[variant] = test.readbackBuffer(fc, out, uint64_t(rayCount) * 24);
                });
            }
            M_CHECK(*hits[0] == *hits[1], "fluid ray hit/distance/primitive/facing/barycentric mismatch frame %u", frame);
            uint32_t hitCount = 0;
            for (size_t at = 0; at < hits[0]->size(); at += 24)
            {
                float distance; std::memcpy(&distance, hits[0]->data() + at, 4); hitCount += distance >= 0;
            }
            if (!count || count == 1) M_CHECK(hitCount == 0, "empty/degenerate fluid still intersects rays frame %u", frame);
            else M_CHECK(hitCount > 0, "nonempty fluid has no ray hits frame %u", frame);
            if (count)
            {
                M_CHECK(*draw[0] == *draw[1], "fluid draw arguments changed frame %u", frame);
                uint32_t drawn; std::memcpy(&drawn, draw[0]->data(), 4);
                M_CHECK(std::memcmp(vertices[0]->data(), vertices[1]->data(), size_t(drawn) * 32) == 0, "visible fluid position/normal bytes changed frame %u", frame);
                M_CHECK(std::memcmp(velocities[0]->data(), velocities[1]->data(), size_t(drawn) * 16) == 0, "visible fluid velocity bytes changed frame %u", frame);
                for (size_t at = size_t(drawn) * 32; at < vertices[1]->size(); at += 96)
                {
                    float p[3]; std::memcpy(p, vertices[1]->data() + at, 12);
                    M_CHECK(std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]), "nonfinite refit tail frame %u", frame);
                    M_CHECK(std::memcmp(vertices[1]->data() + at, vertices[1]->data() + at + 32, 12) == 0 &&
                            std::memcmp(vertices[1]->data() + at, vertices[1]->data() + at + 64, 12) == 0, "nondegenerate refit tail frame %u", frame);
                }
            }
            checkedRays += rayCount;
        }
        logf("PASS fluid stream updates: %llu exact rays, %llu refits; active vertex/normal/velocity/draw bytes identical; activation, retirement, empty, movement, growth, rebase, mirror, interpolation and discarded records; GBV enabled\n",
             (unsigned long long)checkedRays, (unsigned long long)refits);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
