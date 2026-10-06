#include "../FroxelSystem.h"
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
        test.quality.applyOverride("atmosphere.froxels.tile_px=16");
        test.quality.applyOverride("atmosphere.froxels.depth_slices=64");
        uint32_t failures = 0;
        for (uint32_t count : {0u, 1u, 2u, 3u, 5u, 7u})
        for (uint32_t pose = 0; pose < 2; ++pose)
        {
            scene::Scene scene; scene.materials.emplace_back(); scene.cameras.emplace_back();
            scene.cameras[0].position = {0, 2.5f, 5};
            scene.cameras[0].forward = normalize(float3{pose ? 0.2f : 0, -0.12f, -1});
            for (uint32_t i = 0; i < count; ++i)
            {
                scene::Light l; l.position = {-1.4f + float(i) * 0.1f, 2.8f, 1}; l.range = 12; scene.lights.push_back(l);
            }
            test.setScene(scene);
            std::shared_ptr<std::vector<uint8_t>> result;
            uint64_t bound = 0; uint32_t froxels = 0;
            test.run([&](FramePassContext& fc) {
                const auto view = test.mainView(fc, 1024, 768);
                const auto grid = shadow::froxelGridFor(fc.quality, 1024, 768);
                froxels = grid.gridX * grid.gridY * grid.slices;
                bound = shadow::froxelListBound(grid, view.view, test.gpuScene.lights());
                shadow::recordFroxelLists(fc, view, gpu::kNone, false);
                result = test.readbackBuffer(fc, fc.resources.froxelLights, shadow::froxelListBytes(grid, shadow::froxelListCapacity(test.trackState)));
            });
            uint32_t header[16]; std::memcpy(header, result->data(), sizeof header);
            uint64_t rounded = 0, odd = 0;
            for (uint32_t f = 0; f < froxels; ++f)
            {
                uint32_t entries; std::memcpy(&entries, result->data() + header[8] + uint64_t(f) * 8 + 4, 4);
                rounded += (entries + 1u) & ~1u; odd += entries & 1u;
            }
            const bool ok = bound >= header[7] && header[7] <= header[10] && header[12] == 0 && header[13] == 0 && rounded == header[7];
            logf("Froxel capacity %u lights pose %u: CPU bound %llu, GPU need %u, capacity %u, odd lists %llu, cut %u, lost %u: %s\n",
                count, pose, (unsigned long long)bound, header[7], header[10], (unsigned long long)odd, header[12], header[13], ok ? "PASS" : "FAIL");
            failures += !ok;
            if (count == 1 && !odd) fail("single-light case did not exercise padding");
        }
        if (failures) fail("froxel capacity failed %u cases", failures);
        logf("PASS froxel capacity: empty/even/odd light lists, exact GPU padding, no lost scene lights; GBV enabled\n");
        return 0;
    }
    catch (const std::exception& e) { logf("FAIL %s\n", e.what()); return 1; }
}
