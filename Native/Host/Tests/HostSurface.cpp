// Surface state deltas through the host (A7 host path, INTERFACES v1.52; E's surface::SurfaceField): HostRenderer::
// surfaceDelta / setSurfaceHalfLives / setSurfaceTime queue with the next frame and reach the field on the render thread
// in call order, also when frames in between are skipped (an older ticket's batches go into the rendered one) or dropped
// from a full queue (they move into the next packet):
//   1. two bricks added, rendered: the field holds both, the half-lives and the time are the host's;
//   2. brick A removed and brick B replaced in one batch, then B changed again in a later batch, both queued behind
//      frames that are never rendered (skipped tickets), then a rendered frame: the field holds B with the last values;
//   3. more frames queued than the queue keeps (dropped packets): a brick added in the first of them is in the field
//      after the last one renders;
//   4. no D3D12 debug-layer errors.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"
#include "unx/decal/SurfaceState.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
surface::BrickInput brick(int x, int y, int z, double t0, float wet)
{
    surface::BrickInput b{};
    b.key[0] = x;
    b.key[1] = y;
    b.key[2] = z;
    b.t0 = t0;
    for (uint32_t v = 0; v < 64; ++v) b.value[v * surface::kChannels] = wet;
    return b;
}

// The quantized wet byte of voxel 0 of the brick with this key (E's record: 16 + 5 v + c), or -1 when absent.
int wetOf(surface::SurfaceField& f, int x, int y, int z)
{
    for (uint32_t slot : f.liveSlots())
    {
        const uint8_t* r = f.records().data() + (size_t)slot * surface::kBrickBytes;
        int32_t key[3];
        std::memcpy(key, r, sizeof key);
        if (key[0] == x && key[1] == y && key[2] == z) return r[16];
    }
    return -1;
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
        HostRenderer h(o);
        h.scene() = test::oneBox();
        h.commit();
        surface::SurfaceField& field = surface::surfaceField(h.trackStateForTest());
        uint64_t frame = 0;
        auto queue = [&] {
            FramePacket p;
            p.frameIndex = frame++;
            p.deltaTime = 1.0f / 60;
            p.width = 320;
            p.height = 180;
            p.camera = h.scene().cameras[0];
            return h.queueFrame(std::move(p));
        };

        // 1.
        const surface::BrickInput a = brick(0, 0, 0, 1.0, 1.0f), b = brick(1, 0, 0, 1.0, 0.5f);
        const surface::BrickInput both[2] = { a, b };
        h.surfaceDelta(both, 2, nullptr, 0);
        h.setSurfaceHalfLives({ 60, 0, 180, 0, 0, 0 });
        h.setSurfaceTime(12.5);
        h.renderStandalone(queue(), nullptr, 0);
        logf("  1: bricks %zu, wet A %d B %d, time %.2f, wet half-life %.0f\n", field.bricks(), wetOf(field, 0, 0, 0), wetOf(field, 1, 0, 0), field.now(), field.halfLives()[0]);
        expect("1: both bricks, the host's half-lives and time", field.bricks() == 2 && wetOf(field, 0, 0, 0) == 255 && wetOf(field, 1, 0, 0) == 128 &&
                                                                   field.now() == 12.5 && field.halfLives()[0] == 60 && field.halfLives()[2] == 180);

        // 2. batches behind skipped tickets
        const int32_t removeA[3] = { 0, 0, 0 };
        const surface::BrickInput b2 = brick(1, 0, 0, 2.0, 0.25f), b3 = brick(1, 0, 0, 3.0, 0.75f);
        h.surfaceDelta(&b2, 1, removeA, 1);
        queue();  // never rendered
        h.surfaceDelta(&b3, 1, nullptr, 0);
        h.setSurfaceTime(13.0);
        queue();  // never rendered
        h.renderStandalone(queue(), nullptr, 0);
        logf("  2: bricks %zu, wet A %d B %d (expected -1, %d), time %.2f\n", field.bricks(), wetOf(field, 0, 0, 0), wetOf(field, 1, 0, 0), (int)std::lround(0.75 * 255), field.now());
        expect("2: skipped frames' batches applied in order (A removed, B's last values)",
               field.bricks() == 1 && wetOf(field, 0, 0, 0) == -1 && wetOf(field, 1, 0, 0) == (int)std::lround(0.75 * 255) && field.now() == 13.0);

        // 3. dropped packets (more queued than framesInFlight + 2)
        const surface::BrickInput c = brick(-3, 2, 5, 4.0, 1.0f);
        h.surfaceDelta(&c, 1, nullptr, 0);
        uint64_t last = 0;
        for (int k = 0; k < 8; ++k) last = queue();
        h.renderStandalone(last, nullptr, 0);
        logf("  3: bricks %zu, wet C %d\n", field.bricks(), wetOf(field, -3, 2, 5));
        expect("3: a dropped packet's batch reaches the next rendered frame", field.bricks() == 2 && wetOf(field, -3, 2, 5) == 255);

        const uint32_t errors = h.debugErrors();
        expect("D3D12 debug layer errors 0", errors == 0);
        logf(failures ? "HOST SURFACE TEST FAILED (%u)\n" : "HOST SURFACE TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST SURFACE TEST ERROR: %s\n", e.what());
        return 2;
    }
}
