#include "../../Passes/Material/Tests/MTestFrame.h"
#include "unx/rt/RayScene.h"
#include <array>
#include <bit>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main()
{
    try
    {
        TestFrame test(true, true);
        test.quality.applyOverride("raytracing.emitters=false");
        test.quality.applyOverride("raytracing.far_field=false");
        auto baselineQuality = QualityConfig::parse(test.quality.canonical() + "\nraytracing.cache_unchanged_rigid = false\n", "uncached mobility reference");
        constexpr uint32_t count = 48;
        scene::Scene scene; scene.materials.emplace_back(); scene.cameras.emplace_back();
        scene::Mesh mesh; mesh.name = "rigid triangle";
        mesh.positions = {{-0.2f, 0, -0.2f}, {0, 0, 0.2f}, {0.2f, 0, -0.2f}};
        mesh.normals.assign(3, {0, 1, 0}); mesh.indices = {0, 1, 2}; mesh.submeshes.push_back({0, 3, 0}); scene.meshes.push_back(mesh);
        auto matrix = [](float x) { float3x4 m{}; m.m[0][0] = m.m[1][1] = m.m[2][2] = 1; m.m[0][3] = x; return m; };
        for (uint32_t i = 0; i < count; ++i) { scene::Instance in; in.mesh = 0; in.flags |= scene::InstanceDynamic; in.transform = matrix(float(i) * 2); scene.instances.push_back(in); }
        test.setScene(scene);
        test.frame.mainView = ViewDesc::fromCamera(scene.cameras[0], 64, 64, {});
        std::array<std::unique_ptr<rt::RayScene>, 2> rays;
        rays[0] = std::make_unique<rt::RayScene>(test.device, test.shaders, test.gpuScene, baselineQuality);
        rays[1] = std::make_unique<rt::RayScene>(test.device, test.shaders, test.gpuScene, test.quality);
        M_CHECK(rays[0]->stats().dynamicInstances == count && rays[1]->stats().dynamicInstances == 0 && rays[1]->stats().staticInstances == count,
            "initial upload did not cache movable rigid population");
        float originX = 0, offset0 = 0, offset1 = 0;
        bool hidden = false;
        uint64_t expectedBuilds = 0;
        for (uint32_t frame = 0; frame < 13; ++frame)
        {
            test.frame.originShift = {};
            const bool edit = frame == 1 || frame == 2 || frame == 4 || frame == 8 || frame == 9;
            if (edit)
            {
                const uint32_t index = frame == 4 ? 1 : 0;
                if (index == 0) offset0 = frame == 2 || frame == 8 ? 1.25f : 0.75f; else offset1 = 0.75f;
                const InstanceTransformUpdate update{ index, matrix(float(index) * 2 + (index ? offset1 : offset0) - originX), 0 };
                test.gpuScene.updateTransforms(test.frame.frameIndex, {&update, 1});
            }
            if (frame == 5 || frame == 6) { hidden = frame == 5; test.gpuScene.setInstanceVisible(2, !hidden); }
            if (frame == 7) { originX = 1024; test.gpuScene.rebase({1024, 0, 0}); test.frame.originShift = {1024, 0, 0}; }
            test.gpuScene.flushUpdates(test.frame.frameIndex, 2, test.shaders);
            if (frame == 4)
            {
                // Consume the CPU-side first-motion transition in a graph that
                // is never submitted. The next recording must still remove the
                // old cached instance and emit its GI invalidation bounds.
                RenderGraph abandoned(test.device); FrameResources resources; FrameServices services;
                test.trackState.beginRecord();
                FramePassContext fc{ test.device, abandoned, test.shaders, test.quality, test.gpuScene, test.frame, resources, services,
                    [&](const ViewDesc& v) { return test.frameConstantsFor(v); }, &test.trackState };
                rays[1]->record(fc);
                M_CHECK(rays[1]->stats().staticTlasBuildsTotal == expectedBuilds, "discarded cached-TLAS update counted submitted");
            }
            if (frame == 1 || frame == 4 || frame == 5 || frame == 6 || frame == 7) ++expectedBuilds;
            std::array<std::shared_ptr<std::vector<uint8_t>>, 2> results;
            test.trackState.beginRecord();
            test.run([&](FramePassContext& fc) {
                for (uint32_t variant = 0; variant < 2; ++variant)
                {
                    rays[variant]->record(fc);
                    if (frame == 4 && variant == 1) M_CHECK(rays[variant]->changes().size() >= 2, "discarded promotion lost GI invalidation");
                    const auto output = fc.graph.createBuffer({ "rigid hits", count * 2 * 8, 0 });
                    fc.graph.addPass("rigid probe", QueueType::Compute,
                        [&](PassBuilder& b) { rays[variant]->declareTraversal(b); b.use(output, Use::UavCompute); b.keep(); },
                        [&, output, variant](PassContext& c) {
                            uint32_t srvs[8]; rays[variant]->rootConstants(srvs);
                            const uint32_t k[8] = { srvs[0], srvs[1], c.uav(output), count, std::bit_cast<uint32_t>(originX), 0, 0, 0 };
                            c.cmd->SetPipelineState(fc.shaders.compute("RayTracing/Tests/RigidMobilityProbe")); c.computeConstants(k, 8); c.cmd->Dispatch((count * 2 + 63) / 64, 1, 1);
                        });
                    results[variant] = test.readbackBuffer(fc, output, count * 2 * 8);
                }
            });
            M_CHECK(*results[0] == *results[1], "cached/dynamic ray ownership differs at frame %u", frame);
            M_CHECK(rays[1]->stats().staticTlasBuildsTotal == expectedBuilds, "cached TLAS rebuilt unnecessarily or missed an invalidation at frame %u", frame);
            const uint32_t movers = frame >= 4 ? 2 : frame >= 1 ? 1 : 0;
            M_CHECK(rays[1]->stats().dynamicInstances == movers, "wrong dynamic population at frame %u", frame);
            for (uint32_t bank = 0; bank < 2; ++bank) for (uint32_t i = 0; i < count; ++i)
            {
                const float offset = i == 0 ? offset0 : i == 1 ? offset1 : 0;
                const bool hit = !(i == 2 && hidden) && std::abs(offset - bank * 0.75f) < 0.01f;
                uint32_t id; float t; const auto* p = results[1]->data() + (bank * count + i) * 8;
                std::memcpy(&id, p, 4); std::memcpy(&t, p + 4, 4);
                M_CHECK(id == (hit ? i : UINT32_MAX) && (hit ? std::abs(t - 2) < 1e-6 : t < 0), "stale/missing analytic hit frame %u instance %u bank %u", frame, i, bank);
            }
        }
        logf("PASS rigid mobility: 48 authoring-dynamic instances cached, only 2 observed movers promoted; 1248 exact ray results, hide/show/rebase/discard retry; 5 required cached TLAS submissions, GBV enabled\n");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
