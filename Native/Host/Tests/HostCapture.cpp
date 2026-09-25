// Host-scene capture (I track): renders a host-saved scene on a standalone HostRenderer (the Unity Player's renderer
// path without Unity) from its camera 0 for N frames and writes the last frame's RGB10A2 pixels (rows top to bottom,
// width * height * 4 bytes, plus a .txt with the size) for image comparisons outside Unity: quality variants for
// diagnosis (--quality <folder>), before/after comparisons across renderer commits. Not a measurement; a hardware GPU
// run (GpuLock -Track I -Kind correctness while the temporary lock rule holds):
//   unx_test_host_hostcapture.exe --scene <file.unxscene> --out <file.rgb10> [--resolution 4K|1440p] [--frames 600]
//       [--quality <folder>]
#include "Renderer/HostRenderer.h"

#include "unx/core/File.h"
#include "unx/scene/SceneData.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;

int main(int argc, char** argv)
{
    try
    {
        std::string scenePath, outPath, resolution = "4K", quality;
        uint32_t frames = 600;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") scenePath = next();
            else if (a == "--out") outPath = next();
            else if (a == "--resolution") resolution = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--quality") quality = next();
            else fail("unknown argument %s", a.c_str());
        }
        if (scenePath.empty() || outPath.empty()) fail("--scene <file.unxscene> and --out <file.rgb10> are required");
        if (resolution != "4K" && resolution != "1440p") fail("resolution is 4K or 1440p");
        const uint32_t w = resolution == "4K" ? 3840 : 2560, h = resolution == "4K" ? 2160 : 1440;
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = quality.empty() ? std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality" : std::filesystem::path(quality);
        HostRenderer r(o);
        r.scene() = scene::load(scenePath);
        if (r.scene().cameras.empty()) fail("scene %s has no camera", scenePath.c_str());
        const scene::Camera camera = r.scene().cameras[0];
        r.commit();
        std::vector<uint32_t> pixels((size_t)w * h);
        for (uint32_t f = 0; f < frames; ++f)
        {
            FramePacket p;
            p.frameIndex = f;
            p.time = f / 60.0;
            p.deltaTime = 1.0f / 60;
            p.width = w;
            p.height = h;
            p.camera = camera;
            const bool last = f + 1 == frames;
            r.renderStandalone(r.queueFrame(std::move(p)), last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
        }
        std::ofstream out(outPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(pixels.data()), (std::streamsize)(pixels.size() * 4));
        if (!out) fail("could not write %s", outPath.c_str());
        writeTextFile(outPath + ".txt", format("%u %u\n", w, h));
        logf("captured %s at %ux%u after %u frames -> %s (quality %s)\n", scenePath.c_str(), w, h, frames, outPath.c_str(), o.qualityDirectory.string().c_str());
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
