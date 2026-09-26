// Scene edits after commit through the host (render A item A2; INTERFACES 6.3 v1.44, UnxSceneEditInstances /
// UnxSceneEditMaterials): an appended material and instance are drawn in the next frame, a replaced material changes
// the drawn colour, a hidden instance's slot is reused at another place, the current scene save holds the edits, and
// invalid edits are refused before anything changes. Correctness run on the one-box scene (standalone HostRenderer,
// hardware GPU; run under GpuLock -Kind correctness), read back as RGB10A2.
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 640, kHeight = 360;

// Pixels whose display value is clearly of one hue, per third of the image (left, middle, right).
struct Thirds
{
    uint32_t red[3] = {}, blue[3] = {}, grey[3] = {};
};

Thirds classify(const std::vector<uint32_t>& pixels)
{
    Thirds t;
    for (uint32_t y = 0; y < kHeight; ++y)
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            const uint32_t p = pixels[y * kWidth + x];
            const float r = (p & 1023u) / 1023.0f, g = ((p >> 10) & 1023u) / 1023.0f, b = ((p >> 20) & 1023u) / 1023.0f;
            const uint32_t third = x * 3 / kWidth;
            if (r > 0.05f && r > 2.5f * g && r > 2.5f * b) ++t.red[third];
            else if (b > 0.05f && b > 2.5f * r && b > 1.5f * g) ++t.blue[third];
            else if (r > 0.05f && std::abs(r - g) < 0.08f * r && std::abs(r - b) < 0.12f * r) ++t.grey[third];
        }
    return t;
}
} // namespace

int main()
{
    try
    {
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        HostRenderer h(o);
        h.scene() = test::oneBox();
        h.scene().materials[0].baseColor = { 0.5f, 0.5f, 0.5f };
        const scene::Camera camera = h.scene().cameras[0];
        h.commit();
        uint64_t index = 0;
        std::vector<uint32_t> pixels(kWidth * kHeight);
        auto frames = [&](int n) {
            for (int i = 0; i < n; ++i)
            {
                FramePacket p;
                p.frameIndex = index++;
                p.deltaTime = 1.0f / 60;
                p.width = kWidth;
                p.height = kHeight;
                p.camera = camera;
                const uint64_t ticket = h.queueFrame(std::move(p));
                h.renderStandalone(ticket, i == n - 1 ? pixels.data() : nullptr, i == n - 1 ? pixels.size() * 4 : 0);
            }
            const Thirds t = classify(pixels);
            logf("    red %u %u %u, blue %u %u %u, grey %u %u %u\n", t.red[0], t.red[1], t.red[2], t.blue[0], t.blue[1], t.blue[2], t.grey[0], t.grey[1], t.grey[2]);
            return t;
        };
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-72s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        auto refused = [&](const char* what, auto&& edit) {
            bool threw = false;
            try { edit(); }
            catch (const std::exception& e) { threw = true; logf("    (refused: %s)\n", e.what()); }
            expect(what, threw);
        };
        auto at = [](float x) {
            scene::Instance i;
            i.mesh = 0;
            i.transform.m[0][3] = x;
            return i;
        };

        Thirds t = frames(8);
        expect("committed box: grey in the middle, nothing red", t.grey[1] > 1000 && t.red[0] + t.red[1] + t.red[2] == 0);

        // Append a red material and a box on the right using it.
        scene::Material red = h.scene().materials[0];
        red.name = "red";
        red.baseColor = { 0.8f, 0.03f, 0.03f };
        const std::pair<uint32_t, scene::Material> addRed[] = { { 1, red } };
        h.editMaterials(addRed);
        scene::Instance right = at(1.3f);
        right.materialOverrides = { 1 };
        const std::pair<uint32_t, scene::Instance> addRight[] = { { 1, right } };
        h.editInstances(addRight);
        expect("host counts include the edits at once", h.instanceCount() == 2 && h.materialCount() == 2);
        t = frames(8);
        expect("appended red box drawn on the right, middle still grey", t.red[2] > 500 && t.red[0] == 0 && t.grey[1] > 1000);

        // Replace material 0: the middle box turns blue.
        scene::Material blue = h.scene().materials[0];
        blue.baseColor = { 0.03f, 0.08f, 0.8f };
        const std::pair<uint32_t, scene::Material> setBlue[] = { { 0, blue } };
        h.editMaterials(setBlue);
        const Thirds before = t;
        t = frames(8);
        // The sky counts as blue in every third, so the box's change is measured against the frame before.
        expect("replaced material: middle box blue (blue +, grey - in the middle), red box unchanged",
               t.blue[1] > before.blue[1] + 5000 && t.grey[1] + 5000 < before.grey[1] && t.red[2] > 500 && t.blue[0] == before.blue[0]);

        // Remove the red box (hide) and reuse its slot on the left.
        h.setInstanceVisible(1, false);
        t = frames(4);
        expect("hidden: no red anywhere", t.red[0] + t.red[1] + t.red[2] == 0);
        scene::Instance left = at(-1.3f);
        left.materialOverrides = { 1 };
        const std::pair<uint32_t, scene::Instance> reuse[] = { { 1, left } };
        h.editInstances(reuse);
        t = frames(8);
        expect("reused slot: visible again, red on the left only", t.red[0] > 500 && t.red[2] == 0);

        // The host's current scene has the edits (the scene save writes it).
        const scene::Scene now = h.currentScene();
        expect("current scene: 2 instances, 2 materials, material 0 blue, instance 1 at x = -1.3",
               now.instances.size() == 2 && now.materials.size() == 2 && now.materials[0].baseColor.z == 0.8f && now.instances[1].transform.m[0][3] == -1.3f);

        // Invalid edits change nothing.
        refused("append out of order (index 5 of 2) is refused", [&] { const std::pair<uint32_t, scene::Instance> e[] = { { 5, at(0) } }; h.editInstances(e); });
        refused("a mesh that is not in the scene is refused", [&] { scene::Instance i = at(0); i.mesh = 3; const std::pair<uint32_t, scene::Instance> e[] = { { 2, i } }; h.editInstances(e); });
        refused("a skinned instance is refused", [&] { scene::Instance i = at(0); i.flags |= scene::InstanceSkinned; const std::pair<uint32_t, scene::Instance> e[] = { { 2, i } }; h.editInstances(e); });
        refused("a material override beyond the materials is refused", [&] { scene::Instance i = at(0); i.materialOverrides = { 7 }; const std::pair<uint32_t, scene::Instance> e[] = { { 2, i } }; h.editInstances(e); });
        refused("a texture that is not in the scene is refused", [&] { scene::Material m = red; m.normalTexture = 9; const std::pair<uint32_t, scene::Material> e[] = { { 2, m } }; h.editMaterials(e); });
        expect("refused edits left the counts alone", h.instanceCount() == 2 && h.materialCount() == 2);
        t = frames(2);
        expect("and the image: blue middle, red left", t.blue[1] > t.blue[0] + 5000 && t.red[0] > 500);
        const uint32_t debug = h.debugErrors();
        expect("D3D12 debug layer errors 0", debug == 0);

        logf(failures ? "HOST SCENE EDITS TEST FAILED (%u)\n" : "HOST SCENE EDITS TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
