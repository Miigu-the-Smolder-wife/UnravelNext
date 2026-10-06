#pragma once
#include "unx/render/Frame.h"
#include "unx/render/GpuWorkload.h"
#include "unx/rt/RayPipeline.h"

namespace unx::render::rt
{
// Frame-owned continuation storage. One record per input ray is the hard upper
// bound; shader append order does not alter the stored source identity or seed.
struct HitLightingQueue
{
    BufferRef records, arguments;
    uint32_t capacity = 0, chunks = 0, chunkRays = 0;
    bool valid() const { return records.valid(); }
};
inline bool hitLightingStorageFits(uint64_t rays)
{
    // A high-resolution/large-cache view retains its monolithic path instead of
    // letting continuation storage consume an unbounded part of the VRAM budget.
    return rays > 0 && 16 + rays * 80 <= 256ull * 1024 * 1024;
}

inline HitLightingQueue beginHitLightingQueue(FramePassContext& fc, const std::string& name,
                                              uint32_t capacity, uint32_t chunkRays, RayPipeline& lighting)
{
    HitLightingQueue queue;
    queue.capacity = capacity;
    queue.chunkRays = chunkRays;
    queue.chunks = (capacity + chunkRays - 1) / chunkRays;
    queue.records = fc.graph.createBuffer({ (name + " records").c_str(), 16 + uint64_t(capacity) * 80, 0 });
    queue.arguments = fc.graph.createBuffer({ (name + " arguments").c_str(), uint64_t(queue.chunks) * RayPipeline::kDispatchDescStride, 0 });
    ID3D12Resource* descriptor = lighting.dispatchTemplate();
    ID3D12PipelineState* clear = fc.shaders.compute("RayTracing/HitLightingQueueArgs.MODE0");
    fc.graph.addPass(name + ".begin", QueueType::Compute,
        [&](PassBuilder& b) { b.use(queue.records, Use::UavCompute); b.use(queue.arguments, Use::CopyDst); },
        [=](PassContext& c) {
            for (uint32_t chunk = 0; chunk < queue.chunks; ++chunk)
                c.cmd->CopyBufferRegion(c.resource(queue.arguments), uint64_t(chunk) * RayPipeline::kDispatchDescStride,
                    descriptor, 0, RayPipeline::kDispatchDescStride);
            const uint32_t k[4] = { c.uav(queue.records), 0, 0, 0 };
            c.cmd->SetPipelineState(clear); c.computeConstants(k, 4); gpuDispatch(c.cmd, 1, 1, 1);
        });
    return queue;
}

// Reuse bounded continuation storage only after the preceding lighting pass has
// consumed it. The graph's write-after-read edge owns that ordering; no CPU wait.
inline void resetHitLightingQueue(FramePassContext& fc, const std::string& name, const HitLightingQueue& queue)
{
    ID3D12PipelineState* clear = fc.shaders.compute("RayTracing/HitLightingQueueArgs.MODE0");
    fc.graph.addPass(name + ".reset", QueueType::Compute,
        [&](PassBuilder& b) { b.use(queue.records, Use::UavCompute); },
        [=](PassContext& c) {
            const uint32_t k[4] = { c.uav(queue.records), 0, 0, 0 };
            c.cmd->SetPipelineState(clear); c.computeConstants(k, 4); gpuDispatch(c.cmd, 1, 1, 1);
        });
}

inline void prepareHitLightingQueue(FramePassContext& fc, const std::string& name, const HitLightingQueue& queue)
{
    ID3D12PipelineState* arguments = fc.shaders.compute("RayTracing/HitLightingQueueArgs.MODE1");
    fc.graph.addPass(name + ".args", QueueType::Compute,
        [&](PassBuilder& b) { b.use(queue.records, Use::SrvCompute); b.use(queue.arguments, Use::UavCompute); },
        [=](PassContext& c) {
            const uint32_t k[8] = { c.srv(queue.records), c.uav(queue.arguments), RayPipeline::kDispatchDescStride,
                uint32_t(offsetof(D3D12_DISPATCH_RAYS_DESC, Width)), queue.chunks, queue.chunkRays, queue.capacity, 0 };
            c.cmd->SetPipelineState(arguments); c.computeConstants(k, 8); gpuDispatch(c.cmd, (queue.chunks + 63) / 64, 1, 1);
        });
}
}
