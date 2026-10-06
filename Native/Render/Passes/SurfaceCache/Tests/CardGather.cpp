#include "../../Material/Tests/MTestFrame.h"
#include <array>
using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main()
{
    try
    {
        TestFrame test(true, true);
        scene::Scene scene; scene.materials.emplace_back(); scene.cameras.emplace_back(); test.setScene(scene);
        uint64_t checked = 0;
        for (const uint32_t size : {2u, 17u, 256u, 1024u})
        for (uint32_t seed = 0; seed < 4; ++seed)
        {
            std::shared_ptr<std::vector<uint8_t>> result;
            test.run([&](FramePassContext& fc) {
                const auto light = fc.graph.createTexture({"card light", size, size, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT});
                const auto depth = fc.graph.createTexture({"card depth", size, size, 1, 1, DXGI_FORMAT_R16_UNORM});
                const auto output = fc.graph.createBuffer({"card gather comparison", uint64_t(size)*size*4, 0});
                fc.graph.addPass("card gather inputs", QueueType::Compute,
                    [&](PassBuilder& b) { b.use(light, Use::UavCompute); b.use(depth, Use::UavCompute); },
                    [&, light, depth, size, seed](PassContext& c) {
                        const uint32_t k[8] = {c.uav(light), c.uav(depth), 0, size, seed, 0, 0, 0};
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/SurfaceCache/Tests/CardGatherProbe.STAGE0"));
                        c.computeConstants(k, 8); c.cmd->Dispatch((size+7)/8, (size+7)/8, 1);
                    });
                fc.graph.addPass("card gather comparison", QueueType::Compute,
                    [&](PassBuilder& b) { b.use(light, Use::SrvCompute); b.use(depth, Use::SrvCompute); b.use(output, Use::UavCompute); b.keep(); },
                    [&, light, depth, output, size, seed](PassContext& c) {
                        const uint32_t k[8] = {c.srv(light), c.srv(depth), c.uav(output), size, seed, 0, 0, 0};
                        c.cmd->SetPipelineState(fc.shaders.compute("Passes/SurfaceCache/Tests/CardGatherProbe.STAGE1"));
                        c.computeConstants(k, 8); c.cmd->Dispatch((size+7)/8, (size+7)/8, 1);
                    });
                result = test.readbackBuffer(fc, output, uint64_t(size)*size*4);
            });
            for (size_t i = 0; i < result->size(); i += 4)
            {
                uint32_t error; std::memcpy(&error, result->data()+i, 4);
                M_CHECK(error == 0, "card gather differs at size %u seed %u texel %zu (depth=1, RGB=2): %u", size, seed, i/4, error);
            }
            checked += uint64_t(size)*size;
        }
        logf("PASS card gathers: %llu footprints, R16_UNORM depth and R11G11B10 lighting bit-identical to four loads, page edges, invalid depth, HDR and partial groups; GBV enabled\n", (unsigned long long)checked);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
