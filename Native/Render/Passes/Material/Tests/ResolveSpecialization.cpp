#include "MTestFrame.h"
#include <array>
#include <algorithm>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

int main()
{
    try
    {
        TestFrame test(true, true);
        scene::Scene scene; scene.materials.emplace_back(); scene.cameras.emplace_back();
        scene.cameras[0].position = {0.2f, 0.1f, 3}; scene.cameras[0].forward = normalize(float3{-0.05f, -0.03f, -1});
        scene::Texture color; color.name = "detail checker"; color.width = color.height = 2; color.format = scene::TextureFormat::Rgba8Srgb;
        color.texels = {255, 80, 30, 255, 40, 230, 100, 255, 25, 50, 230, 255, 200, 180, 100, 255};
        scene.textures.push_back(color);
        scene::Texture height; height.name = "parallax"; height.width = height.height = 2; height.format = scene::TextureFormat::R8Linear;
        height.texels = {0, 64, 128, 255}; scene.textures.push_back(height);
        scene::Mesh mesh; mesh.name = "resolve plane";
        mesh.positions = {{-2, -1, 0}, {2, -1, 0}, {2, 1, 0}, {-2, 1, 0}};
        mesh.normals.assign(4, float3{0, 0, 1}); mesh.tangents.assign(4, float4{1, 0, 0, 1});
        mesh.uv0 = {{0, 1}, {1, 1}, {1, 0}, {0, 0}}; mesh.indices = {0, 1, 2, 0, 2, 3}; mesh.submeshes.push_back({0, 6, 0});
        scene.meshes.push_back(mesh); scene.instances.emplace_back(); scene.instances[0].mesh = 0;
        test.setScene(scene);
        uint64_t checked = 0;
        for (uint32_t mode = 0; mode < 9; ++mode)
        {
            scene::Material material;
            material.name = "edited material"; material.baseColorTexture = 0; material.roughness = 0.34f;
            if (mode <= 2 || mode == 4 || mode == 8)
            {
                material.uvScale = {2, 1.3f}; material.uvRotation = 0.2f;
                material.detailColorTexture = 0; material.detailScale = {3, 2}; material.detailColorStrength = 0.4f;
            }
            if (mode == 1) { material.anisotropy = 0.8f; material.anisotropyRotation = 0.6f; material.clearcoat = 0.3f; }
            if (mode == 2) { material.sheenColor = {0.3f, 0.2f, 0.1f}; material.cloth = 0.4f; }
            if (mode == 3) { material.heightTexture = 1; material.heightScale = 0.07f; }
            if (mode == 5) { material.cls = scene::MaterialClass::Subsurface; material.eyeIrisRadius = 0.3f; }
            if (mode == 7) { material.cls = scene::MaterialClass::Cut; material.cutDamageWidth = 0; }
            test.sceneData.materials[0] = material;
            const uint32_t materialIndex = 0; test.gpuScene.setMaterials({&materialIndex, 1});
            const bool expectSimple = mode != 3 && mode != 5 && mode != 7;
            for (const auto [W, H] : std::array<std::pair<uint32_t, uint32_t>, 2>{{{513, 257}, {960, 540}}})
            {
                std::array<std::array<std::shared_ptr<std::vector<uint8_t>>, 5>, 2> result;
                std::array<bool, 2> hasAniso{};
                test.run([&](FramePassContext& fc) {
                    auto input = test.mainView(fc, W, H); test.vis.record(fc, input);
                    for (uint32_t variant = 0; variant < 2; ++variant)
                    {
                        auto view = input; view.frameConstants = fc.frameConstantsFor(view.view);
                        auto& debug = fc.state<material::ResolveDebug>("M.resolveDebug"); debug.forceFullKernel = variant == 0;
                        tracks::materialResolve(fc, view);
                        M_CHECK(debug.usedSimpleKernel == (variant != 0 && expectSimple), "wrong variant after material edit %u", mode);
                        const auto output = material::resolveOutputs(fc, view);
                        result[variant][0] = test.readback(fc, view.gbuffer);
                        result[variant][1] = test.readback(fc, output.materialWord);
                        result[variant][2] = test.readback(fc, view.reflectionLobeTiles);
                        result[variant][3] = test.readbackBuffer(fc, output.tileArgs, material::kShadeClassCount * output.bands * 12);
                        hasAniso[variant] = output.anisoWord.valid();
                        if (output.anisoWord.valid()) result[variant][4] = test.readback(fc, output.anisoWord);
                        debug.forceFullKernel = false;
                    }
                });
                for (uint32_t row = 0; row < H; ++row)
                    for (uint32_t output = 0; output < 2; ++output)
                    {
                        const uint32_t bytes = output ? 4 : 8, pitch = TestFrame::rowPitch(W, bytes);
                        M_CHECK(std::memcmp(result[0][output]->data() + row * pitch, result[1][output]->data() + row * pitch, W * bytes) == 0,
                            "resolve output %u differs: material %u, row %u", output, mode, row);
                    }
                const uint32_t tx = (W + 7) / 8, ty = (H + 7) / 8;
                for (uint32_t row = 0; row < ty; ++row)
                    M_CHECK(std::memcmp(result[0][2]->data() + row * TestFrame::rowPitch(tx, 1), result[1][2]->data() + row * TestFrame::rowPitch(tx, 1), tx) == 0,
                        "lobe classification differs");
                M_CHECK(*result[0][3] == *result[1][3], "class tile counts differ");
                M_CHECK(hasAniso[0] == hasAniso[1], "class-word allocation differs");
                if (hasAniso[0]) for (uint32_t y = 0; y < H; ++y) for (uint32_t x = 0; x < W; ++x)
                {
                    uint32_t word; std::memcpy(&word, result[0][1]->data() + y * TestFrame::rowPitch(W, 4) + x * 4, 4);
                    if ((word & 0xFFFFu) == 0xFFFFu) continue; // sky has no class word
                    const uint32_t at = y * TestFrame::rowPitch(W, 4) + x * 4;
                    M_CHECK(std::memcmp(result[0][4]->data() + at, result[1][4]->data() + at, 4) == 0, "anisotropy/eye/height word differs");
                }
                checked += uint64_t(W) * H;
            }
        }
        logf("PASS resolve specialization: %llu pixels; G-buffer/material/lobe/class words and counts exact; UV/detail, anisotropy/coat/sheen, height/eye/cut edits and return to ordinary material; GBV enabled\n", (unsigned long long)checked);
        return 0;
    }
    catch (const std::exception& e) { logf("FAIL %s\n", e.what()); return 1; }
}
