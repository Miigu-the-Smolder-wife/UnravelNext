// Output-size switches on one host renderer (I track), the Unity Player's path when the camera target changes size: a
// device hang (DXGI DEVICE_HUNG) happened at a 4K -> 1440p switch in the data World Player on 2026-09-25. A host-saved
// scene is committed on a standalone HostRenderer and rendered through 4K -> 1440p -> 4K -> 1440p phases with the D3D12
// debug layer and GPU-based validation (--no-gbv: debug layer only), then a phase in which every frame gets a newly
// created output of the same size (the old one released after the GPU is done): the allocator tends to give the new
// texture the old address, which is how a view cache keyed by resource pointer ends up using a destroyed texture (the
// suspected cause of the Player's DEVICE_HUNG). Each phase ends with a blocking readback, so a hang surfaces as a
// device-removed failure in that phase. Fails on any debug-layer error. Hardware GPU run: GpuLock -Track I while the
// temporary lock rule is in force (2026-09-25):
//   unx_test_host_hostswitch.exe --scene <file.unxscene> [--frames 16] [--no-gbv]
#include "Renderer/HostRenderer.h"

#include "unx/core/File.h"
#include "unx/scene/SceneData.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;

int main(int argc, char** argv)
{
    try
    {
        std::string scenePath;
        uint32_t frames = 16;
        bool gbv = true;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") scenePath = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--no-gbv") gbv = false;
            else fail("unknown argument %s", a.c_str());
        }
        if (scenePath.empty()) fail("--scene <file.unxscene> is required");
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;  // Unity's maxQueuedFrames in the data World Player
        o.debugLayer = true;
        o.gpuValidation = gbv;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        HostRenderer h(o);
        h.scene() = scene::load(scenePath);
        if (h.scene().cameras.empty()) fail("scene %s has no camera", scenePath.c_str());
        const scene::Camera camera = h.scene().cameras[0];
        h.commit();
        logf("host switch: %s, %u frames per phase, debug layer%s\n", scenePath.c_str(), frames, gbv ? " + GPU-based validation" : "");

        struct Phase
        {
            const char* name;
            uint32_t width, height;
        };
        const Phase phases[] = { { "4K", 3840, 2160 }, { "1440p", 2560, 1440 }, { "4K", 3840, 2160 }, { "1440p", 2560, 1440 } };
        uint64_t frame = 0;
        uint32_t errors = 0;
        std::vector<uint32_t> pixels;
        for (const Phase& ph : phases)
        {
            pixels.assign((size_t)ph.width * ph.height, 0);
            for (uint32_t f = 0; f < frames; ++f, ++frame)
            {
                FramePacket p;
                p.frameIndex = frame;
                p.time = frame / 60.0;
                p.deltaTime = 1.0f / 60;
                p.width = ph.width;
                p.height = ph.height;
                p.camera = camera;
                const uint64_t ticket = h.queueFrame(std::move(p));
                const bool last = f + 1 == frames;
                h.renderStandalone(ticket, last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
            }
            size_t lit = 0;
            for (uint32_t px : pixels) lit += (px & 0x3FFFFFFFu) != 0;
            const uint32_t e = h.debugErrors();
            logf("  %-5s %ux%u: %u frames done, %.1f %% non-black pixels, debug-layer errors so far %u\n", ph.name, ph.width, ph.height, frames,
                 100.0 * lit / pixels.size(), e);
            errors = e;
        }
        // Same-size recreation with address reuse.
        h.setRecreateStandaloneOutput(true);
        {
            const Phase ph{ "reuse", 2560, 1440 };
            pixels.assign((size_t)ph.width * ph.height, 0);
            for (uint32_t f = 0; f < frames; ++f, ++frame)
            {
                FramePacket p;
                p.frameIndex = frame;
                p.time = frame / 60.0;
                p.deltaTime = 1.0f / 60;
                p.width = ph.width;
                p.height = ph.height;
                p.camera = camera;
                const uint64_t ticket = h.queueFrame(std::move(p));
                const bool last = f + 1 == frames;
                h.renderStandalone(ticket, last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
            }
            h.setRecreateStandaloneOutput(false);
            size_t lit = 0;
            for (uint32_t px : pixels) lit += (px & 0x3FFFFFFFu) != 0;
            errors = h.debugErrors();
            logf("  reuse %ux%u: %u frames, each with a new output (address reused %u of %u), %.1f %% non-black pixels, debug-layer errors so far %u\n",
                 ph.width, ph.height, frames, h.outputAddressReuses(), h.outputRecreations(), 100.0 * lit / pixels.size(), errors);
            if (h.outputAddressReuses() == 0) logf("  note: no address reuse happened; the reuse case did not exercise pointer-keyed caches\n");
        }
        logf(errors ? "HOST SWITCH TEST FAILED (%u debug-layer errors)\n" : "HOST SWITCH TEST PASSED\n", errors);
        return errors ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
