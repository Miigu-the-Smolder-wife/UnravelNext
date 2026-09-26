// Runtime geometry and origin rebase through the host (render C, C2b and C9; INTERFACES v1.47, v1.49): a runtime mesh and
// instance added after commit are drawn in the next rendered frame, move with runtime transforms, vanish when removed
// (the image returns to the scene without them), and their slots are reused; a runtime mesh still used by an instance
// cannot be removed. An origin shift with the camera moved by the same amount leaves the image unchanged and moves the
// GPU scene's instances. The same state drawn twice in a row must match (no pixel off by more than 0.1 in a channel).
// Across a change and back, R's world-space GI cache keeps entries of different ages whose estimates differ (measured:
// up to 0.14 in ~220 pixels, unchanged after 256 frames), and its origin-shift re-indexing is B's (pending); so those
// comparisons require the same geometry - no pixel off by more than 0.25 (a hole or a misplaced surface differs by far
// more: the box against the background 0.58) - and log the pixels above 0.1. The GPU scene's records are checked
// exactly, and V's depth-level test (unx_test_visibility_runtimepooltests) proves re-added geometry bit-identical. Correctness run on a one-box scene
// (standalone HostRenderer; a hardware GPU run under GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 1280, kHeight = 720;

// Pixels whose RGB differs by more than 102/1023 in some channel, and the mean column of those pixels.
struct Difference
{
    uint32_t count = 0;
    double meanColumn = 0;
    uint32_t worst = 0;  // largest channel difference (of 1023)
};
Difference difference(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
    Difference d;
    double columns = 0;
    for (size_t i = 0; i < a.size(); ++i)
        for (uint32_t c = 0; c < 3; ++c)
        {
            const int x = (int)((a[i] >> (10 * c)) & 1023), y = (int)((b[i] >> (10 * c)) & 1023);
            d.worst = std::max(d.worst, (uint32_t)std::abs(x - y));
            if (std::abs(x - y) > 102)
            {
                ++d.count;
                columns += (double)(i % kWidth);
                break;
            }
        }
    d.meanColumn = d.count ? columns / d.count : 0;
    return d;
}

float3x4 translation(float x, float y, float z)
{
    float3x4 t;
    t.m[0][3] = x, t.m[1][3] = y, t.m[2][3] = z;
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
        scene::Camera camera = h.scene().cameras[0];
        RuntimeCapacity capacity;
        capacity.meshes = 4, capacity.submeshes = 8, capacity.vertices = 1024, capacity.indices = 4096;
        capacity.clusters = 64, capacity.clusterVertexIndices = 4096, capacity.clusterTriangles = 4096, capacity.nodes = 64;
        capacity.instances = 16;
        h.reserveRuntime(capacity);
        h.commit();
        const uint32_t staticInstances = h.instanceCount();

        uint64_t index = 0;
        std::vector<uint32_t> image(kWidth * kHeight);
        // Renders 'frames' frames (the first after a history cut) and reads the last one back.
        auto render = [&](uint32_t frames) {
            h.setDiscontinuity(kDiscontinuityCut);
            for (uint32_t f = 0; f < frames; ++f)
            {
                FramePacket p;
                p.frameIndex = index;
                p.time = index / 60.0;
                ++index;
                p.deltaTime = 1.0f / 60;
                p.width = kWidth;
                p.height = kHeight;
                p.camera = camera;
                const bool last = f + 1 == frames;
                h.renderStandalone(h.queueFrame(std::move(p)), last ? image.data() : nullptr, last ? image.size() * 4 : 0);
            }
            return image;
        };
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-72s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        // 64 frames: R's GI cache (200,000 entries, ~7,800 refreshed per frame) converges after a change; images compare converged states.
        const uint32_t settle = 64;

        const std::vector<uint32_t> baseline = render(settle);
        {
            const Difference d = difference(render(settle), baseline);
            logf("  (noise between two renders of the same scene: %u pixels)\n", d.count);
            expect("same scene twice: no pixel differs by more than 0.1", d.count == 0);
        }

        // A runtime box (half size) to the right of the scene box.
        scene::Mesh box = test::oneBox().meshes[0];
        box.name = "runtime box";
        for (float3& p : box.positions) p = p * 0.5f;
        const uint32_t mesh = h.addRuntimeMesh(box);
        const uint32_t instance = h.addRuntimeInstance(mesh, translation(1.2f, 0, 0), scene::InstanceCastShadow);
        const std::vector<uint32_t> present = render(settle);
        const Difference added = difference(present, baseline);
        logf("  (runtime box: %u pixels, mean column %.0f of %u)\n", added.count, added.meanColumn, kWidth);
        expect("runtime box drawn to the right of the scene box", added.count > 2000 && added.meanColumn > kWidth / 2);
        expect("runtime instance takes the first slot after the scene's", h.gpuInstanceForTest(staticInstances).mesh != gpu::kNone &&
                                                                              h.gpuInstanceForTest(staticInstances).objectToWorld[0].w == 1.2f);

        h.setRuntimeTransform(instance, translation(-1.2f, 0, 0));
        const Difference moved = difference(render(settle), baseline);
        logf("  (moved runtime box: %u pixels, mean column %.0f)\n", moved.count, moved.meanColumn);
        expect("runtime transform moves the box to the left", moved.count > 2000 && moved.meanColumn < kWidth / 2);

        bool refused = false;
        try
        {
            h.removeRuntimeMesh(mesh);
        }
        catch (const std::exception&)
        {
            refused = true;
        }
        expect("a runtime mesh with a live instance cannot be removed", refused);

        h.removeRuntimeInstance(instance);
        h.removeRuntimeMesh(mesh);
        {
            const Difference d = difference(render(settle), baseline);
            logf("  (removed: %u pixels above 0.1, worst channel %u/1023)\n", d.count, d.worst);
            expect("removed: the image returns to the scene without it (worst <= 0.25)", d.worst <= 256);
        }

        // Slots freed framesInFlight + 1 frames ago are reused (8 frames rendered since).
        const uint32_t mesh2 = h.addRuntimeMesh(box);
        const uint32_t instance2 = h.addRuntimeInstance(mesh2, translation(1.2f, 0, 0), scene::InstanceCastShadow);
        {
            const Difference d = difference(render(settle), present);
            logf("  (re-added: %u pixels above 0.1, mean column %.0f, worst channel %u/1023)\n", d.count, d.meanColumn, d.worst);
            expect("re-added box in reused slots: same geometry as the first add (worst <= 0.25)", d.worst <= 256);
            expect("re-added instance reuses the freed slot", h.gpuInstanceForTest(staticInstances).objectToWorld[0].w == 1.2f);
        }

        // Origin shift by one grid cell with the camera moved along: nothing on screen changes.
        const float x0 = h.gpuInstanceForTest(0).objectToWorld[0].w;
        h.setOriginShift({ kOriginGrid, 0, 0 });
        camera.position.x -= kOriginGrid;
        {
            const Difference d = difference(render(settle), present);
            logf("  (origin shift: %u pixels above 0.1, mean column %.0f, worst channel %u/1023)\n", d.count, d.meanColumn, d.worst);
            expect("origin shift with the camera: same geometry (worst <= 0.25)", d.worst <= 256);
            expect("origin shift moves the scene instance", h.gpuInstanceForTest(0).objectToWorld[0].w == x0 - kOriginGrid);
            expect("origin shift moves the runtime instance", h.gpuInstanceForTest(staticInstances).objectToWorld[0].w == 1.2f - kOriginGrid);
        }
        // A runtime move queued before the shift is in the old coordinates and follows it.
        h.setRuntimeTransform(instance2, translation(1.2f - kOriginGrid, 0, 0));
        h.setOriginShift({ -kOriginGrid, 0, 0 });
        camera.position.x += kOriginGrid;
        {
            const Difference d = difference(render(settle), present);
            logf("  (shift back: %u pixels above 0.1, mean column %.0f, worst channel %u/1023)\n", d.count, d.meanColumn, d.worst);
            expect("move queued before a shift back: same geometry as before (worst <= 0.25)", d.worst <= 256);
            expect("the runtime instance is back at x = 1.2 (float rounding of the round trip)", std::fabs(h.gpuInstanceForTest(staticInstances).objectToWorld[0].w - 1.2f) < 1e-3f);
        }
        h.removeRuntimeInstance(instance2);
        h.removeRuntimeMesh(mesh2);
        render(2);

        logf(failures ? "HOST RUNTIME TEST FAILED (%u)\n" : "HOST RUNTIME TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
