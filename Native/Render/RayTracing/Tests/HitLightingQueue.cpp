#include "../../Passes/Material/Tests/MTestFrame.h"
#include "unx/rt/HitLightingQueue.h"
#include "unx/rt/RayScene.h"
#include <array>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

struct TestRay { float3 origin; float maximum; float3 direction; float visibleMaximum; };

int main()
{
    try
    {
        TestFrame test(true, true);
        scene::Scene scene;
        scene.materials.emplace_back();
        scene.cameras.emplace_back();
        scene.cameras[0].position = {0, 0, 0}; scene.cameras[0].forward = {0, 0, 1};
        scene::Mesh mesh;
        mesh.positions = {{-2, -2, 3}, {2, -2, 3}, {2, 2, 3}, {-2, 2, 3}};
        mesh.normals.assign(4, {0, 0, -1});
        mesh.uv0 = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        mesh.indices = {0, 2, 1, 0, 3, 2}; mesh.submeshes.push_back({0, 6, 0});
        scene.meshes.push_back(mesh);
        scene::Instance instance; instance.mesh = 0; scene.instances.push_back(instance);
        test.setScene(scene);
        rt::RayScene rays(test.device, test.shaders, test.gpuScene, test.quality);
        auto& reference = rt::RayPipeline::get(test.device, test.shaders, rt::standardRayPipeline("RayTracing/Tests/TraceTest", {"TraceTestGen"}));
        auto& gather = rt::RayPipeline::get(test.device, test.shaders, rt::standardRayPipeline("RayTracing/Tests/TraceQueueGather", {"TraceTestGen"}));
        auto& lighting = rt::RayPipeline::get(test.device, test.shaders, rt::standardRayPipeline("RayTracing/Tests/TraceQueueLighting", {"TraceTestGen"}));
        uint64_t checked = 0, continued = 0;
        for (bool async : {false, true})
        {
        test.graph.setAsyncCompute(async);
        for (uint32_t count : {0u, 1u, 31u, 32u, 33u, 777u, 10000u, 0u, 63u})
        {
            std::vector<TestRay> input(std::max(1u, count));
            for (uint32_t i = 0; i < input.size(); ++i)
            {
                const float x = (float((i * 37u) % 101u) - 50) / 45.0f;
                const float y = (float((i * 19u) % 103u) - 51) / 46.0f;
                input[i] = {{0, 0, 0}, 10, normalize(float3{x, y, 1}), i & 1u ? 2.5f : 9.0f};
            }
            auto upload = uploadStatic(test.device, input.data(), input.size() * sizeof(TestRay), L"continuation rays");
            std::array<std::shared_ptr<std::vector<uint8_t>>, 6> results;
            test.run([&](FramePassContext& fc) {
                const auto view = test.mainView(fc, 256, 256);
                rays.record(fc);
                uint32_t sceneWords[8]; rays.rootConstants(sceneWords);
                const auto sceneSrvs = std::to_array(sceneWords);
                const auto source = fc.graph.importBuffer(upload.Get(), {"continuation input", input.size() * sizeof(TestRay), sizeof(TestRay)});
                const uint64_t bytes = uint64_t(std::max(1u, count)) * 32;
                const auto original = fc.graph.createBuffer({"monolithic results", bytes, 32});
                const auto deferred = fc.graph.createBuffer({"continuation results", bytes, 32});
                // Non-wave-aligned chunks exercise both the queue's tail and the
                // restoration of source identity after arbitrary append order.
                const auto queue = rt::beginHitLightingQueue(fc, "test.hitlighting", std::max(1u, count), 37, lighting);
                auto dispatch = [&](const char* name, rt::RayPipeline* pipeline, BufferRef output, uint32_t stage) {
                    fc.graph.addPass(name, QueueType::Compute,
                        [&](PassBuilder& b) {
                            b.use(source, Use::SrvGraphics); b.use(output, Use::UavGraphics); rays.declareTraversal(b);
                            if (stage) b.use(queue.records, stage == 1 ? Use::UavGraphics : Use::SrvGraphics);
                            if (stage == 2) b.use(queue.arguments, Use::IndirectArgs);
                        },
                        [=](PassContext& c) {
                            uint32_t k[32] = {c.srv(source), c.uav(output), count, stage == 1 ? c.uav(queue.records) : stage == 2 ? c.srv(queue.records) : gpu::kNone};
                            std::memcpy(k + 24, sceneSrvs.data(), 32);
                            c.bindFrameConstants(view.frameConstants);
                            if (stage != 2)
                            {
                                c.computeConstants(k, 32); pipeline->dispatch(c.cmd, 0, std::max(1u, count), 1, 1);
                            }
                            else for (uint32_t chunk = 0; chunk < queue.chunks; ++chunk)
                            {
                                k[4] = chunk * queue.chunkRays; c.computeConstants(k, 32);
                                pipeline->dispatchIndirect(c.cmd, c.resource(queue.arguments), uint64_t(chunk) * rt::RayPipeline::kDispatchDescStride);
                            }
                        });
                };
                dispatch("test.monolithic", &reference, original, 0);
                dispatch("test.minimal", &gather, deferred, 1);
                rt::prepareHitLightingQueue(fc, "test.hitlighting", queue);
                if (async)
                {
                    // The screen-probe compactor uses (0,0,0) for empty chunks.
                    // Exercise that real producer on the compute queue as well
                    // as the continuation producer's (0,1,1) on graphics.
                    fc.graph.addPass("test.compact.dimensions", QueueType::Compute,
                        [&](PassBuilder& b) { b.use(queue.records, Use::UavCompute); b.use(queue.arguments, Use::UavCompute); },
                        [=, &test](PassContext& c) {
                            const uint32_t stride = rt::RayPipeline::kDispatchDescStride;
                            const uint32_t k[12] = {c.uav(queue.records), queue.capacity, c.uav(queue.arguments),
                                uint32_t(offsetof(D3D12_DISPATCH_RAYS_DESC, Width)), stride, queue.chunkRays, queue.chunks, 1, stride, 0, 0, 0};
                            c.cmd->SetPipelineState(test.shaders.compute("RayTracing/CompactDispatch.MODE1"));
                            c.computeConstants(k, 12); gpuDispatch(c.cmd, 1, 1, 1);
                        });
                }
                dispatch("test.lighting", &lighting, deferred, 2);
                results[0] = test.readbackBuffer(fc, original, bytes);
                results[1] = test.readbackBuffer(fc, deferred, bytes);
                results[2] = test.readbackBuffer(fc, queue.records, 16 + uint64_t(queue.capacity) * 80);
                results[3] = test.readbackBuffer(fc, queue.arguments, uint64_t(queue.chunks) * rt::RayPipeline::kDispatchDescStride);

                const auto batched = fc.graph.createBuffer({"bounded continuation results", bytes, 32});
                // Reuse one small queue across many batches, including a partial
                // final batch. There is no intervening CPU or GPU readback wait.
                const auto bounded = rt::beginHitLightingQueue(fc, "test.bounded", 111, 37, lighting);
                for (uint32_t first = 0; first < std::max(1u, count); first += bounded.capacity)
                {
                    if (first) rt::resetHitLightingQueue(fc, "test.bounded", bounded);
                    const uint32_t width = std::max(1u, std::min(count - first, bounded.capacity));
                    fc.graph.addPass("test.bounded.trace", QueueType::Compute,
                        [&](PassBuilder& b) {
                            b.use(source, Use::SrvGraphics); b.use(batched, Use::UavGraphics);
                            b.use(bounded.records, Use::UavGraphics); rays.declareTraversal(b);
                        },
                        [=, &gather](PassContext& c) {
                            uint32_t k[32] = {c.srv(source), c.uav(batched), count, c.uav(bounded.records), 0, first};
                            std::memcpy(k + 24, sceneSrvs.data(), 32); c.bindFrameConstants(view.frameConstants);
                            c.computeConstants(k, 32); gather.dispatch(c.cmd, 0, width, 1, 1);
                        });
                    rt::prepareHitLightingQueue(fc, "test.bounded", bounded);
                    fc.graph.addPass("test.bounded.lighting", QueueType::Compute,
                        [&](PassBuilder& b) {
                            b.use(source, Use::SrvGraphics); b.use(batched, Use::UavGraphics);
                            b.use(bounded.records, Use::SrvGraphics); b.use(bounded.arguments, Use::IndirectArgs); rays.declareTraversal(b);
                        },
                        [=, &lighting](PassContext& c) {
                            uint32_t k[32] = {c.srv(source), c.uav(batched), count, c.srv(bounded.records)};
                            std::memcpy(k + 24, sceneSrvs.data(), 32); c.bindFrameConstants(view.frameConstants);
                            for (uint32_t chunk = 0; chunk < bounded.chunks; ++chunk)
                            {
                                k[4] = chunk * bounded.chunkRays; c.computeConstants(k, 32);
                                lighting.dispatchIndirect(c.cmd, c.resource(bounded.arguments), uint64_t(chunk) * rt::RayPipeline::kDispatchDescStride);
                            }
                        });
                }
                results[4] = test.readbackBuffer(fc, batched, bytes);
                results[5] = test.readbackBuffer(fc, bounded.records, 16);
            });
            M_CHECK(std::memcmp(results[0]->data(), results[1]->data(), uint64_t(count) * 32) == 0, "deferred ray results differ at count %u", count);
            M_CHECK(std::memcmp(results[0]->data(), results[4]->data(), uint64_t(count) * 32) == 0, "bounded ray results differ at count %u", count);
            uint32_t boundedEntries; std::memcpy(&boundedEntries, results[5]->data(), 4);
            M_CHECK(boundedEntries <= 111 && boundedEntries <= count, "reused continuation queue overflowed");
            uint32_t entries; std::memcpy(&entries, results[2]->data(), 4);
            M_CHECK(entries <= count, "continuation capacity exceeded");
            std::vector<bool> seen(count, false);
            for (uint32_t entry = 0; entry < entries; ++entry)
            {
                const uint8_t* record = results[2]->data() + 16 + uint64_t(entry) * 80;
                uint32_t identity[4]; std::memcpy(identity, record, 16);
                M_CHECK(identity[0] < count && !seen[identity[0]], "duplicated/lost continuation identity");
                seen[identity[0]] = true;
                M_CHECK(identity[1] == identity[0] * 9781u && identity[2] == 17 && identity[3] == 23, "continuation context changed");
                M_CHECK(std::memcmp(record + 48, &input[identity[0]].origin, 12) == 0 &&
                        std::memcmp(record + 64, &input[identity[0]].direction, 12) == 0, "ray origin/direction changed");
            }
            const uint32_t chunks = (std::max(1u, count) + 36) / 37;
            for (uint32_t chunk = 0; chunk < chunks; ++chunk)
            {
                uint32_t dimensions[3];
                std::memcpy(dimensions, results[3]->data() + uint64_t(chunk) * rt::RayPipeline::kDispatchDescStride + offsetof(D3D12_DISPATCH_RAYS_DESC, Width), 12);
                const uint32_t width = chunk * 37 < entries ? std::min(entries - chunk * 37, 37u) : 0u;
                const uint32_t height = async && width == 0 ? 0u : 1u;
                M_CHECK(dimensions[0] == width && dimensions[1] == height && dimensions[2] == height,
                    "continuation indirect dimensions differ");
            }
            checked += count; continued += entries;
        }
        }
        M_CHECK(test.device.drainDebugMessages() == 0, "GPU validation errors");
        logf("PASS hit-lighting queue: %llu rays, %llu exact continuations; graphics and compute queues, hit identity, normals, visibility, ray bits, context, empty/reset/full/partial chunks and 111-record bounded reuse; GBV enabled\n",
            (unsigned long long)checked, (unsigned long long)continued);
        return 0;
    }
    catch (const std::exception& error) { logf("FAIL %s\n", error.what()); return 1; }
}
