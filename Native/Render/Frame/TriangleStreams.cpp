#include "unx/render/Frame.h"
#include <algorithm>
#include <atomic>

namespace unx::render
{
uint64_t allocateTriangleStreamTopologyId()
{
    static std::atomic<uint64_t> next{1};
    const uint64_t id = next.fetch_add(1, std::memory_order_relaxed);
    if (!id) fail("triangle stream topology identity exhausted");
    return id;
}

void prepareTriangleStreamDraws(FramePassContext& fc)
{
    auto& streams = fc.resources.triangleStreams;
    if (streams.empty()) return;
    if (streams.size() > kMaxTriangleStreams) fail("triangle stream draw arguments: too many streams");
    struct Input { BufferRef draw; uint32_t capacity; };
    // Pool/RoundPool already draw their full fixed topology. Their direct dispatch
    // needs no GPU count conversion. Shaders still bound any external producer's live count.
    std::vector<Input> inputs;
    for (auto& stream : streams)
        if (stream.vertices.valid() && stream.drawArgs.valid() && stream.maxTriangles && stream.knownTriangleCount == UINT32_MAX && !stream.meshArgs.valid())
            inputs.push_back({stream.drawArgs, stream.maxTriangles});
    if (inputs.empty()) return;
    const BufferRef args = fc.graph.createBuffer({"triangle stream mesh dispatch", uint64_t(inputs.size()) * 12, 0});
    uint32_t slot = 0;
    for (auto& stream : streams)
        if (stream.vertices.valid() && stream.drawArgs.valid() && stream.maxTriangles && stream.knownTriangleCount == UINT32_MAX && !stream.meshArgs.valid())
        {
            stream.meshArgs = args;
            stream.meshArgsOffset = slot++ * 12;
        }
    auto* pso = fc.shaders.compute("Passes/Common/TriangleStreamArgs");
    fc.graph.addPass("stream draw arguments", QueueType::Graphics,
        [&](PassBuilder& b) {
            for (const auto& input : inputs) b.use(input.draw, Use::SrvCompute);
            b.use(args, Use::UavCompute);
        },
        [inputs, args, pso](PassContext& c) {
            c.cmd->SetPipelineState(pso);
            // 22 (descriptor, capacity) pairs fit beside four header constants.
            for (uint32_t base = 0; base < inputs.size(); base += 22)
            {
                const uint32_t count = std::min(22u, uint32_t(inputs.size()) - base);
                uint32_t k[48] = {c.uav(args), count, base, 0};
                for (uint32_t i = 0; i < count; ++i)
                {
                    k[4 + 2 * i] = c.srv(inputs[base + i].draw);
                    k[5 + 2 * i] = inputs[base + i].capacity;
                }
                c.computeConstants(k, 48);
                gpuDispatch(c.cmd, 1, 1, 1); // disjoint output records between chunks
            }
        });
}
}
