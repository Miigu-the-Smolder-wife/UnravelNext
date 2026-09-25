// Frame flags through the host (I track, INTERFACES 5.5.2 and 6.3, v1.35): history discontinuity and GPU simulation bits
// reach the recorded frame, bits of frames that are never rendered carry into the next rendered one (OR), and a
// teleported instance has no motion in its frame (prevObjectToWorld = objectToWorld) while an ordinary move has motion.
// Correctness run on a one-box scene (standalone HostRenderer; hardware GPU, no lock needed for correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <cstdio>
#include <cstring>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
bool sameRows(const float4 a[3], const float4 b[3])
{
    return std::memcmp(a, b, sizeof(float4) * 3) == 0;
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
        h.scene().instances[0].flags |= scene::InstanceDynamic;
        const scene::Camera camera = h.scene().cameras[0];
        h.commit();
        uint64_t index = 0;
        auto queue = [&]() {
            FramePacket p;
            p.frameIndex = index++;
            p.deltaTime = 1.0f / 60;
            p.width = 2560;
            p.height = 1440;
            p.camera = camera;
            return h.queueFrame(std::move(p));
        };
        auto render = [&](uint64_t ticket) { h.renderStandalone(ticket, nullptr, 0); };
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-62s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };

        render(queue());
        expect("plain frame: no discontinuity, no GPU simulation", h.lastFrameFlags() == std::pair<uint32_t, uint32_t>{ 0, 0 });

        h.setDiscontinuity(kDiscontinuityCut);
        h.setSimulation(kGpuSimulationVfx);
        render(queue());
        expect("cut + VFX step reach the frame", h.lastFrameFlags() == std::pair<uint32_t, uint32_t>{ kDiscontinuityCut, kGpuSimulationVfx });
        render(queue());
        expect("the next frame is clean again", h.lastFrameFlags() == std::pair<uint32_t, uint32_t>{ 0, 0 });

        // A restore on a frame the host never renders: framesInFlight + 2 newer packets drop it, then the newest is
        // rendered (older queued packets fold into it too).
        h.setDiscontinuity(kDiscontinuityRestore);
        h.setSimulation(kGpuSimulationSoft);
        queue();
        uint64_t last = 0;
        for (int i = 0; i < 5; ++i) last = queue();
        render(last);
        expect("restore + soft step of a dropped frame carry into the rendered one",
               h.lastFrameFlags() == std::pair<uint32_t, uint32_t>{ kDiscontinuityRestore, kGpuSimulationSoft });

        // Motion: an ordinary move has prev != current in its frame; a teleport has prev == current.
        InstanceTransformUpdate move;
        move.instance = 0;
        move.objectToWorld.m[0][3] = 0.5f;
        h.setTransforms({ &move, 1 });
        render(queue());
        {
            const auto& g = h.gpuInstanceForTest(0);
            expect("ordinary move: previous transform differs (motion)", !sameRows(g.objectToWorld, g.prevObjectToWorld));
        }
        InstanceTransformUpdate jump = move;
        jump.objectToWorld.m[0][3] = 7.0f;
        jump.flags = render::kTransformTeleport;
        h.setTransforms({ &jump, 1 });
        render(queue());
        {
            const auto& g = h.gpuInstanceForTest(0);
            expect("teleport: previous transform equals the new one (no motion)", sameRows(g.objectToWorld, g.prevObjectToWorld) && g.objectToWorld[0].w == 7.0f);
        }
        logf(failures ? "HOST FRAME FLAGS TEST FAILED (%u)\n" : "HOST FRAME FLAGS TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
