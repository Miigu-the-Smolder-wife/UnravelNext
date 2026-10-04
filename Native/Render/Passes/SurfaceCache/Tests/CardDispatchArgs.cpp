// Actual GPU argument generation, followed by indirect compute/ray consumers.
// Boundary cases include empty lists, partial groups, ray chunks and count clamps.
#include "../../Material/Tests/MTestFrame.h"
#include "unx/rt/RayPipeline.h"
#include <array>
#include <cstddef>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main()
{
    try
    {
        TestFrame test(true, true);
        scene::Scene scene;
        scene.materials.emplace_back(); scene.cameras.emplace_back();
        test.setScene(scene);
        rt::RayPipelineDesc pipelineDesc;
        pipelineDesc.library = "Passes/SurfaceCache/Tests/CardDispatchRays";
        pipelineDesc.rayGen = { "CardDispatchTestGen" };
        rt::RayPipeline pipeline(test.device, test.shaders, pipelineDesc);
        ComPtr<ID3D12CommandSignature> signature;
        const D3D12_INDIRECT_ARGUMENT_DESC argument{ D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH };
        const D3D12_COMMAND_SIGNATURE_DESC signatureDesc{ sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument, 0 };
        check(test.device.d3d()->CreateCommandSignature(&signatureDesc, nullptr, IID_PPV_ARGS(&signature)), "card test signature");
        constexpr uint32_t stride = rt::RayPipeline::kDispatchDescStride;
        constexpr uint32_t directChunk = 262144, radiosityChunk = 65536;
        const std::array<std::array<uint32_t, 4>, 12> cases = {{
            {0, 0, 1025, 1025}, {1, 1, 1025, 1025}, {63, 15, 1025, 1025}, {64, 16, 1025, 1025},
            {65, 17, 1025, 1025}, {455, 1023, 1025, 1025}, {456, 1024, 1025, 1025},
            {457, 1025, 1025, 1025}, {9999, 9999, 1025, 1025}, {0, 65535, 1, 65535},
            {65535, 0, 65535, 1}, {0xFFFFFFFFu, 0xFFFFFFFFu, 65535, 65535} }};
        uint64_t checkedThreads = 0;
        for (uint32_t ci = 0; ci < cases.size(); ++ci)
        {
            const auto values = cases[ci];
            const uint32_t dc = values[2], rc = values[3];
            const uint32_t d = std::min(values[0], dc), r = std::min(values[1], rc);
            const uint32_t dChunks = (dc * 576 + directChunk - 1) / directChunk;
            const uint32_t rChunks = (rc * 64 + radiosityChunk - 1) / radiosityChunk;
            const uint32_t dOffset = 64, rOffset = dOffset + dChunks * stride;
            const uint32_t bytes = rOffset + rChunks * stride;
            const bool execute = ci < 9;
            const uint32_t rayCapacity = dc * 576 + rc * 64;
            const uint32_t outputWords = execute ? rayCapacity + 4 * std::max(dc, rc) : 1;
            std::vector<uint32_t> input(16 + outputWords, 0);
            input[0] = values[0]; input[8] = values[1];
            auto upload = makeBuffer(test.device, input.size() * 4, D3D12_HEAP_TYPE_UPLOAD);
            void* mapped = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(upload->Map(0, &none, &mapped), "card test upload");
            std::memcpy(mapped, input.data(), input.size() * 4); upload->Unmap(0, nullptr);
            std::shared_ptr<std::vector<uint8_t>> argsResult, outputResult;
            test.run([&](FramePassContext& fc) {
                const BufferRef select = fc.graph.createBuffer({ "card test select", 64, 0 });
                const BufferRef args = fc.graph.createBuffer({ "card test arguments", bytes, 0 });
                const BufferRef output = fc.graph.createBuffer({ "card test output", (uint64_t)outputWords * 4, 0 });
                fc.graph.addPass("card test initialize", QueueType::Compute,
                    [&](PassBuilder& b) { b.use(select, Use::CopyDst); b.use(args, Use::CopyDst); b.use(output, Use::CopyDst); },
                    [=, &pipeline](PassContext& c) {
                        c.cmd->CopyBufferRegion(c.resource(select), 0, upload.Get(), 0, 64);
                        c.cmd->CopyBufferRegion(c.resource(output), 0, upload.Get(), 64, (uint64_t)outputWords * 4);
                        for (uint32_t chunk = 0; chunk < dChunks + rChunks; ++chunk)
                            c.cmd->CopyBufferRegion(c.resource(args), 64 + (uint64_t)chunk * stride, pipeline.dispatchTemplate(), 0, stride);
                    });
                fc.graph.addPass("card test arguments", QueueType::Compute,
                    [&](PassBuilder& b) { b.use(select, Use::SrvCompute); b.use(args, Use::UavCompute); },
                    [=, &fc](PassContext& c) {
                        const uint32_t k[12] = { c.srv(select), c.uav(args), dc, rc, dOffset, dChunks, rOffset, rChunks,
                            stride, (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width), directChunk, radiosityChunk };
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/SurfaceCache/CardDispatchArgs"));
                        c.computeConstants(k, 12); c.cmd->Dispatch(1, 1, 1);
                    });
                if (execute)
                {
                    fc.graph.addPass("card test indirect consumers", QueueType::Compute,
                        [&](PassBuilder& b) { b.use(args, Use::IndirectArgs); b.use(output, Use::UavGraphics); },
                        [=, &fc, &pipeline](PassContext& c) {
                            for (uint32_t context = 0; context < 2; ++context)
                            {
                                const uint32_t chunks = context ? rChunks : dChunks, chunkSize = context ? radiosityChunk : directChunk;
                                for (uint32_t chunk = 0; chunk < chunks; ++chunk)
                                {
                                    const uint32_t k[4] = { c.uav(output), context ? dc * 576 : 0, chunk * chunkSize, 0 };
                                    c.computeConstants(k, 4);
                                    pipeline.dispatchIndirect(c.cmd, c.resource(args), (context ? rOffset : dOffset) + (uint64_t)chunk * stride);
                                }
                            }
                            c.cmd->SetPipelineState(fc.shaders.compute("Passes/SurfaceCache/Tests/CardDispatchProbe"));
                            for (uint32_t consumer = 0; consumer < 4; ++consumer)
                            {
                                const uint32_t k[4] = { c.uav(output), rayCapacity + consumer * std::max(dc, rc), 0, 0 };
                                c.computeConstants(k, 4);
                                c.cmd->ExecuteIndirect(signature.Get(), 1, c.resource(args), consumer * 16, nullptr, 0);
                            }
                        });
                    outputResult = test.readbackBuffer(fc, output, (uint64_t)outputWords * 4);
                }
                argsResult = test.readbackBuffer(fc, args, bytes);
            });
            const uint32_t compute[4] = { (d + 63) / 64, d, (r * 4 + 63) / 64, r };
            for (uint32_t consumer = 0; consumer < 4; ++consumer)
            {
                uint32_t words[4]; std::memcpy(words, argsResult->data() + consumer * 16, 16);
                M_CHECK(words[0] == compute[consumer] && words[1] == 1 && words[2] == 1 && words[3] == 0,
                    "case %u compute consumer %u arguments", ci, consumer);
            }
            for (uint32_t context = 0; context < 2; ++context)
            {
                const uint32_t chunks = context ? rChunks : dChunks, chunkSize = context ? radiosityChunk : directChunk;
                const uint32_t count = context ? r * 64 : d * 576;
                for (uint32_t chunk = 0; chunk < chunks; ++chunk)
                {
                    const uint32_t first = chunk * chunkSize, width = count > first ? std::min(count - first, chunkSize) : 0;
                    auto expected = pipeline.dispatchDesc(0, width, width ? 1 : 0, width ? 1 : 0);
                    M_CHECK(std::memcmp(&expected, argsResult->data() + (context ? rOffset : dOffset) + chunk * stride,
                        sizeof(expected)) == 0, "case %u context %u chunk %u descriptor/template", ci, context, chunk);
                }
            }
            if (execute)
            {
                auto verify = [&](uint32_t base, uint32_t count, uint32_t capacity) {
                    for (uint32_t i = 0; i < capacity; ++i)
                    {
                        uint32_t value; std::memcpy(&value, outputResult->data() + (uint64_t)(base + i) * 4, 4);
                        M_CHECK(value == (i < count ? i + 1 : 0), "case %u output %u thread %u", ci, base, i);
                    }
                    checkedThreads += count;
                };
                verify(0, d * 576, dc * 576); verify(dc * 576, r * 64, rc * 64);
                for (uint32_t consumer = 0; consumer < 4; ++consumer)
                    verify(rayCapacity + consumer * std::max(dc, rc), compute[consumer], std::max(dc, rc));
            }
        }
        logf("PASS card indirect dispatch: 12 argument cases, 9 real compute/ray cases, %llu live threads; empty/boundary/clamped counts, GBV enabled\n",
            (unsigned long long)checkedThreads);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
