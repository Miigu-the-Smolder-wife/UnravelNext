// The K-path maps atlas (view.screenProbeMaps, INTERFACES v1.13) against the in-block maps it replaces: identical random
// RGB9E5 maps written into both layouts (ProbeMapsAtlasFill), then 1 M random (probe, level, uv) samples with uv beyond
// [0, 1] (clamping) through the software bilinear and the hardware-filtered atlas (ProbeMapsAtlasCompare). They may
// differ only by the filter's weight precision (D3D: at least 8 bits of subtexel precision): the check is |hw - sw| <=
// 1/128 of the largest of the sample's four texels, for every sample. A 37 x 23 probe grid (neither a power of two).
//
//   unx_test_gi_probemapsatlas [--validate]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <cstring>

using namespace unx;
using namespace unx::render;

int main(int argc, char** argv)
{
    try
    {
        bool validate = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--validate") validate = true;
            else fail("unknown argument %s", a.c_str());
        }
        DeviceOptions options;
        options.debugLayer = validate;
        options.gpuValidation = validate;
        Device device(options);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        const uint32_t probesX = 37, probesY = 23, samples = 1u << 20;

        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, readbackHeap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 256;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> result, readback;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&result)), "result");
        d.Flags = D3D12_RESOURCE_FLAG_NONE;
        check(device.d3d()->CreateCommittedResource3(&readbackHeap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)),
              "readback");

        RenderGraph graph(device);
        const TextureRef probes = graph.createTexture({ "test probes", probesX * 8, probesY * 5 + 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT });
        TextureDesc atlasDesc{ "test maps atlas", probesX * 14, probesY * 8, 1, 1, DXGI_FORMAT_R32_UINT };
        atlasDesc.srvFormat = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        const TextureRef atlas = graph.createTexture(atlasDesc);
        const BufferRef resultRef = graph.importBuffer(result.Get(), { "test result", 256, 0 });
        const BufferRef readbackRef = graph.importBuffer(readback.Get(), { "test readback", 256, 0 });
        graph.addPass("test.clear", QueueType::Compute, [&](PassBuilder& b) { b.use(resultRef, Use::UavCompute); },
                      [&, resultRef](PassContext& c) {
                          // Zero via a copy of nothing is not available: the compare kernel's max/count start from a cleared buffer.
                          const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p[2] = { { c.resource(resultRef)->GetGPUVirtualAddress(), 0 },
                                                                            { c.resource(resultRef)->GetGPUVirtualAddress() + 4, 0 } };
                          c.cmd->WriteBufferImmediate(2, p, nullptr);
                      });
        graph.addPass("test.fill", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(probes, Use::UavCompute);
                          b.use(atlas, Use::UavCompute);
                      },
                      [&, probes, atlas](PassContext& c) {
                          const uint32_t k[4] = { c.uav(probes), c.uav(atlas), probesX, probesY };
                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/Tests/ProbeMapsAtlasFill"));
                          c.computeConstants(k, 4);
                          c.cmd->Dispatch((probesX + 7) / 8, (probesY + 7) / 8, 1);
                      });
        graph.addPass("test.compare", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(probes, Use::SrvCompute);
                          b.use(atlas, Use::SrvCompute);
                          b.use(resultRef, Use::UavCompute);
                      },
                      [&, probes, atlas, resultRef](PassContext& c) {
                          const uint32_t k[8] = { c.srv(probes), c.srv(atlas), probesX, probesY, c.uav(resultRef), samples, 0, 0 };
                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/Tests/ProbeMapsAtlasCompare"));
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch((samples + 63) / 64, 1, 1);
                      });
        graph.addPass("test.readback", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(resultRef, Use::CopySrc);
                          b.use(readbackRef, Use::CopyDst);
                          b.keep();
                      },
                      [resultRef, readbackRef](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(readbackRef), 0, c.resource(resultRef), 0, 8); });
        graph.execute(nullptr);
        device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));
        device.waitIdle();
        uint32_t words[2];
        void* mapped = nullptr;
        D3D12_RANGE all{ 0, 8 };
        check(readback->Map(0, &all, &mapped), "map readback");
        std::memcpy(words, mapped, 8);
        D3D12_RANGE none{ 0, 0 };
        readback->Unmap(0, &none);
        float maxError;
        std::memcpy(&maxError, &words[0], 4);
        bool pass = words[1] == 0 && maxError <= 1.0f / 128;
        logf("probe maps atlas: %u samples, largest |hardware - software| / largest texel %.6f (1/%.0f), %u beyond 1/128 -> %s\n", samples, maxError,
             maxError > 0 ? 1 / maxError : 0.0, words[1], pass ? "PASS" : "FAIL");
        const uint32_t errors = device.drainDebugMessages();
        if (validate) logf("debug layer + GPU-based validation errors: %u\n", errors);
        pass = pass && errors == 0;
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
