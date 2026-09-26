// First-person view models through the host (A12 host path, INTERFACES v1.54; E's viewmodel::ViewModels):
// HostRenderer::viewModelAdd / viewModelSetPose / viewModelRemove (the UnxViewModel* path) reach the renderer with the
// frames they are queued with. The one-box scene's camera looks along +x (the box at the origin is out of view); the box
// becomes a view model 2 m ahead of the camera (view space (0, 0, -2)) while the camera turns and moves every frame:
//   1. every frame the box covers the image centre (the G-buffer's grey through debug.view = "albedo") and the corners
//      are sky, whatever the camera did (the pose is composed with each rendered frame's camera);
//   2. a new pose (0.5 m right) moves the box's image right by the projected amount (the covered interval of the centre row);
//   3. after viewModelRemove the box is back at its scene transform (out of view): the centre is sky;
//   4. no D3D12 debug-layer errors.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 640, kHeight = 360;

float3x4 translation(float x, float y, float z)
{
    float3x4 m;
    m.m[0][3] = x;
    m.m[1][3] = y;
    m.m[2][3] = z;
    return m;
}
} // namespace

int main()
{
    try
    {
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-86s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        o.qualityOverrides = { "gi.deterministic=true", "debug.view=\"albedo\"" };
        HostRenderer h(o);
        h.scene() = test::oneBox();
        h.commit();
        const scene::Camera base = h.scene().cameras[0];
        uint64_t frame = 0;
        std::vector<uint32_t> pixels((size_t)kWidth * kHeight);
        // one frame with the camera at 'position' looking at yaw 'yaw' from +x (about +y); the albedo bytes (0 = sky)
        auto render = [&](float3 position, float yaw) {
            FramePacket p;
            p.frameIndex = frame++;
            p.deltaTime = 1.0f / 60;
            p.width = kWidth;
            p.height = kHeight;
            p.camera = base;
            p.camera.position = position;
            p.camera.forward = { std::cos(yaw), 0, -std::sin(yaw) };
            p.camera.up = { 0, 1, 0 };
            h.renderStandalone(h.queueFrame(std::move(p)), pixels.data(), pixels.size() * 4);
        };
        auto albedo = [&](uint32_t x, uint32_t y) { return (int)std::lround((pixels[(size_t)y * kWidth + x] & 1023u) * 255.0 / 1023.0); };
        auto columnCentre = [&](uint32_t y) {
            double sum = 0;
            uint32_t n = 0;
            for (uint32_t x = 0; x < kWidth; ++x)
                if (albedo(x, y) > 0) { sum += x + 0.5; ++n; }
            return n ? sum / n : -1.0;
        };

        render({ 0, 0.5f, 3 }, 0);
        const bool emptyBefore = albedo(kWidth / 2, kHeight / 2) == 0;
        const uint32_t id = h.viewModelAdd(0, translation(0, 0, -2));
        uint32_t held = 0, cornersSky = 0;
        const int frames = 12;
        for (int f = 0; f < frames; ++f)
        {
            render({ 0.3f * f, 0.5f + 0.05f * f, 3 - 0.2f * f }, 0.13f * f);
            held += albedo(kWidth / 2, kHeight / 2) == 188 && albedo(kWidth / 2 - 20, kHeight / 2) == 188 && albedo(kWidth / 2 + 20, kHeight / 2) == 188;
            cornersSky += albedo(0, 0) == 0 && albedo(kWidth - 1, kHeight - 1) == 0;
        }
        logf("  before: centre %s; with the view model: centre covered in %u of %d turning, moving frames, corners sky in %u\n",
             emptyBefore ? "sky" : "covered", held, frames, cornersSky);
        expect("before the add the centre is sky (the box is out of view)", emptyBefore);
        expect("the view model covers the image centre in every frame of a turning, moving camera", held == (uint32_t)frames && cornersSky == (uint32_t)frames);

        // 2. 0.5 m right: the centre row crosses the box between the extremes of its corners' x / depth (faces at 1.5 and
        //    2.5 m), so the covered interval's middle moves from 0 to the middle of [min, max] of (0.5 +- 0.5) / {1.5, 2.5}
        const double tanV = std::tan(base.verticalFov * 0.5), tanH = tanV * kWidth / kHeight;
        const double before = columnCentre(kHeight / 2);
        h.viewModelSetPose(id, translation(0.5f, 0, -2));
        render({ 5, 1, -1 }, 2.0f);
        double lo = 1e9, hi = -1e9;
        for (double x : { 0.0, 1.0 })
            for (double d : { 1.5, 2.5 }) { lo = std::min(lo, x / d); hi = std::max(hi, x / d); }
        const double after = columnCentre(kHeight / 2), predicted = 0.5 * (lo + hi) / tanH * kWidth * 0.5;
        logf("  pose 0.5 m right: centre column %.2f -> %.2f px (shift %.2f, predicted %.2f)\n", before, after, after - before, predicted);
        expect("a new pose moves the image by the projected amount (within 1 px)", before > 0 && std::abs((after - before) - predicted) < 1.0);

        // 3.
        h.viewModelRemove(id);
        render({ 0, 0.5f, 3 }, 0);
        const bool emptyAfter = albedo(kWidth / 2, kHeight / 2) == 0;
        logf("  after remove: centre %s\n", emptyAfter ? "sky" : "covered");
        expect("after the remove the box is back at its scene transform (centre sky)", emptyAfter);
        expect("D3D12 debug layer errors 0", h.debugErrors() == 0);
        logf(failures ? "HOST VIEW MODEL TEST FAILED (%u)\n" : "HOST VIEW MODEL TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST VIEW MODEL TEST ERROR: %s\n", e.what());
        return 2;
    }
}
