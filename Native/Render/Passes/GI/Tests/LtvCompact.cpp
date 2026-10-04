#include "../../Material/Tests/MTestFrame.h"
#include "unx/rt/RayPipeline.h"
#include <array>
#include <bit>
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
        rt::RayPipelineDesc desc;
        desc.rayGen = { "LtvCompactTestGen" };
        desc.library = "Passes/GI/Tests/LtvCompactProbe.MODE0";
        rt::RayPipeline reference(test.device, test.shaders, desc);
        desc.library = "Passes/GI/Tests/LtvCompactProbe.MODE1";
        rt::RayPipeline compact(test.device, test.shaders, desc);
        // Partial 4x4x4 groups, a partial screen cell, and ray chunks that cut
        // through a cell's nine samples. This exercises both ID reconstructions.
        constexpr uint32_t gx = 9, gy = 7, gz = 13, w = 17, h = 13, shift = 1;
        constexpr uint32_t capacity = gx * gy * gz, rays = capacity * 9, chunkSize = 257;
        constexpr uint32_t chunks = (rays + chunkSize - 1) / chunkSize, stride = rt::RayPipeline::kDispatchDescStride;
        std::vector<uint8_t> zero(rays * 16, 0);
        auto upload = makeBuffer(test.device, zero.size(), D3D12_HEAP_TYPE_UPLOAD);
        void* mapped = nullptr; D3D12_RANGE none{ 0, 0 };
        check(upload->Map(0, &none, &mapped), "ltv test zero upload");
        std::memcpy(mapped, zero.data(), zero.size()); upload->Unmap(0, nullptr);
        for (uint32_t pattern = 0; pattern < 3; ++pattern)
        for (uint32_t jitterCase = 0; jitterCase < 3; ++jitterCase)
        {
            std::array<std::shared_ptr<std::vector<uint8_t>>, 2> results;
            std::shared_ptr<std::vector<uint8_t>> listResult, argsResult, clearResult;
            test.run([&](FramePassContext& fc) {
                const auto view = test.mainView(fc, w, h);
                const auto depth = fc.graph.createTexture({ "ltv depth", w, h, 1, 1, DXGI_FORMAT_R32_FLOAT });
                const auto hiz = fc.graph.createTexture({ "ltv hiz", gx, gy, 1, 1, DXGI_FORMAT_R32_FLOAT });
                const auto trace = fc.graph.createTexture({ "ltv clear", gx * 3, gy * 3, gz, 1, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D });
                const auto cells = fc.graph.createBuffer({ "ltv cells", 16 + capacity * 16, 0 });
                const auto args = fc.graph.createBuffer({ "ltv args", chunks * stride, 0 });
                std::array<BufferRef, 2> outputs;
                for (auto& out : outputs) out = fc.graph.createBuffer({ "ltv probe", rays * 16, 0 });
                auto words = [=](PassContext& c, uint32_t* k) {
                    const float j = jitterCase == 0 ? 0.0f : jitterCase == 1 ? 0.5f : 0.9999f;
                    k[1] = c.srv(depth), k[2] = c.srv(hiz);
                    k[16] = gx, k[17] = gy, k[18] = gz | shift << 16;
                    k[32] = std::bit_cast<uint32_t>(j), k[33] = std::bit_cast<uint32_t>(1.0f - j), k[34] = std::bit_cast<uint32_t>(j);
                    k[38] = std::bit_cast<uint32_t>(jitterCase == 0 ? 0.0f : jitterCase == 1 ? 1.0f : 64.0f);
                    c.bindFrameConstants(view.frameConstants);
                };
                fc.graph.addPass("ltv test initialize", QueueType::Compute,
                    [&](PassBuilder& b) {
                        b.use(depth, Use::UavCompute); b.use(hiz, Use::UavCompute); b.use(cells, Use::UavCompute); b.use(args, Use::CopyDst);
                        for (auto out : outputs) b.use(out, Use::CopyDst);
                    },
                    [=, &fc, &compact](PassContext& c) {
                        for (auto out : outputs) c.cmd->CopyBufferRegion(c.resource(out), 0, upload.Get(), 0, zero.size());
                        for (uint32_t chunk = 0; chunk < chunks; ++chunk)
                            c.cmd->CopyBufferRegion(c.resource(args), chunk * stride, compact.dispatchTemplate(), 0, stride);
                        uint32_t k[8] = { c.uav(depth), c.uav(hiz), w, h, gx, gy, pattern, 0 };
                        c.bindFrameConstants(view.frameConstants);
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/GI/Tests/LtvCompactInput"));
                        c.computeConstants(k, 8); c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                        k[0] = c.uav(cells);
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/GI/LumenTranslucencyVolumeCompact.MODE0"));
                        c.computeConstants(k, 4); c.cmd->Dispatch(1, 1, 1);
                    });
                fc.graph.addPass("ltv test compact", QueueType::Compute,
                    [&](PassBuilder& b) { b.use(depth, Use::SrvCompute); b.use(hiz, Use::SrvCompute); b.use(cells, Use::UavCompute); b.use(trace, Use::UavCompute); },
                    [=, &fc](PassContext& c) {
                        uint32_t k[40] = {}; words(c, k); k[0] = c.uav(cells); k[3] = c.uav(trace);
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/GI/LumenTranslucencyVolumeCompact.MODE1"));
                        c.computeConstants(k, 40); c.cmd->Dispatch((gx + 3) / 4, (gy + 3) / 4, (gz + 3) / 4);
                    });
                fc.graph.addPass("ltv test args", QueueType::Compute,
                    [&](PassBuilder& b) { b.use(cells, Use::UavCompute); b.use(args, Use::UavCompute); },
                    [=, &fc](PassContext& c) {
                        uint32_t k[20] = { c.uav(cells), 0, 0, 0, c.uav(args), stride,
                            (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width), chunks, chunkSize };
                        k[16] = gx; k[17] = gy; k[18] = gz | shift << 16;
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/GI/LumenTranslucencyVolumeCompact.MODE2"));
                        c.computeConstants(k, 20); c.cmd->Dispatch(1, 1, 1);
                    });
                fc.graph.addPass("ltv test ray inputs", QueueType::Compute,
                    [&](PassBuilder& b) {
                        b.use(depth, Use::SrvGraphics); b.use(hiz, Use::SrvGraphics); b.use(cells, Use::SrvGraphics); b.use(args, Use::IndirectArgs);
                        for (auto out : outputs) b.use(out, Use::UavGraphics);
                    },
                    [=, &reference, &compact](PassContext& c) {
                        uint32_t k[44] = {}; words(c, k); k[0] = c.uav(outputs[0]);
                        c.computeConstants(k, 44); reference.dispatch(c.cmd, 0, gx * 3, gy * 3, gz);
                        k[0] = c.uav(outputs[1]); k[40] = c.srv(cells);
                        for (uint32_t chunk = 0; chunk < chunks; ++chunk)
                        {
                            k[19] = chunk * chunkSize; c.computeConstants(k, 44);
                            compact.dispatchIndirect(c.cmd, c.resource(args), chunk * stride);
                        }
                    });
                for (uint32_t i = 0; i < 2; ++i) results[i] = test.readbackBuffer(fc, outputs[i], rays * 16);
                listResult = test.readbackBuffer(fc, cells, 16 + capacity * 16);
                argsResult = test.readbackBuffer(fc, args, chunks * stride);
                clearResult = test.readback(fc, trace);
            });
            M_CHECK(*results[0] == *results[1], "ltv pattern %u jitter %u input/identity mismatch", pattern, jitterCase);
            uint32_t visible; std::memcpy(&visible, listResult->data(), 4);
            uint32_t expectedRays = 0;
            for (uint32_t ray = 0; ray < rays; ++ray)
            {
                uint32_t identity; std::memcpy(&identity, results[0]->data() + ray * 16 + 12, 4);
                expectedRays += identity != 0;
            }
            M_CHECK(visible * 9 == expectedRays, "ltv visible list count differs");
            for (uint32_t chunk = 0; chunk < chunks; ++chunk)
            {
                const uint32_t first = chunk * chunkSize, width = expectedRays > first ? std::min(expectedRays - first, chunkSize) : 0;
                const auto expected = compact.dispatchDesc(0, width, width ? 1 : 0, width ? 1 : 0);
                M_CHECK(std::memcmp(&expected, argsResult->data() + chunk * stride, sizeof expected) == 0, "ltv chunk %u args", chunk);
            }
            const uint32_t pitch = TestFrame::rowPitch(gx * 3, 4);
            for (uint32_t z = 0; z < gz; ++z)
            for (uint32_t y = 0; y < gy * 3; ++y)
            for (uint32_t x = 0; x < gx * 3 * 4; ++x)
                M_CHECK((*clearResult)[(z * gy * 3 + y) * pitch + x] == 0, "ltv trace not initialized");
        }
        logf("PASS translucency cell compaction: 9 cases, ray input positions and identities bit-exact; visibility/counts/chunks/zero initialization, GBV enabled\n");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
