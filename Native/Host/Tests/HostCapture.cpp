// Host-scene capture (I track): renders a host-saved scene on a standalone HostRenderer (the Unity Player's renderer
// path without Unity) from its camera 0 for N frames and writes the last frame's RGB10A2 pixels (rows top to bottom,
// width * height * 4 bytes, plus a .txt with the size) for image comparisons outside Unity: quality variants for
// diagnosis (--quality <folder>), before/after comparisons across renderer commits. Not a measurement; a hardware GPU
// run (GpuLock -Track I -Kind correctness while the temporary lock rule holds):
//   unx_test_host_hostcapture.exe --scene <file.unxscene> --out <file.rgb10> [--resolution 4K|1440p|WxH] [--frames 600]
//       [--quality <folder>] [--set key=value ...] [--walk m/s] [--turn rad/s] [--ppm <file.ppm>]
// --walk / --turn move camera 0 during the run like a first-person player (forward along its horizontal heading, yaw
// about +y), so the last frame shows what a moving camera sees (history, disocclusion); --ppm also writes the last
// frame as 8-bit PPM (the output's display encoding, top 8 of 10 bits) for viewing.
#include "Renderer/HostRenderer.h"

#include "unx/core/File.h"
#include "unx/scene/SceneData.h"

#include <cmath>
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
        std::string scenePath, outPath, resolution = "4K", quality, ppmPath;
        uint32_t frames = 600;
        std::vector<std::string> overrides;
        float walk = 0, turn = 0;
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
            else if (a == "--set") overrides.push_back(next());
            else if (a == "--walk") walk = std::stof(next());
            else if (a == "--turn") turn = std::stof(next());
            else if (a == "--ppm") ppmPath = next();
            else fail("unknown argument %s", a.c_str());
        }
        if (scenePath.empty() || outPath.empty()) fail("--scene <file.unxscene> and --out <file.rgb10> are required");
        uint32_t w = 0, h = 0;
        if (resolution == "4K") w = 3840, h = 2160;
        else if (resolution == "1440p") w = 2560, h = 1440;
        else
        {
            const size_t x = resolution.find('x');
            if (x == std::string::npos) fail("resolution is 4K, 1440p or WxH");
            w = (uint32_t)std::stoul(resolution.substr(0, x));
            h = (uint32_t)std::stoul(resolution.substr(x + 1));
            if (!w || !h) fail("resolution is 4K, 1440p or WxH");
        }
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = quality.empty() ? std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality" : std::filesystem::path(quality);
        o.qualityOverrides = overrides;
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
            if (walk != 0 || turn != 0)
            {
                // the last frame at camera 0: earlier frames behind it on the path
                const float t = (float)((double)f - (double)(frames - 1)) / 60.0f, yaw = turn * t;
                const float3 fw = camera.forward;
                const float c = std::cos(yaw), s = std::sin(yaw);
                p.camera.forward = normalize(float3{ c * fw.x + s * fw.z, fw.y, -s * fw.x + c * fw.z });
                const float3 heading = normalize(float3{ fw.x, 0, fw.z });
                p.camera.position = camera.position + heading * (walk * t);
            }
            const bool last = f + 1 == frames;
            r.renderStandalone(r.queueFrame(std::move(p)), last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
        }
        std::ofstream out(outPath, std::ios::binary);
        out.write(reinterpret_cast<const char*>(pixels.data()), (std::streamsize)(pixels.size() * 4));
        if (!out) fail("could not write %s", outPath.c_str());
        writeTextFile(outPath + ".txt", format("%u %u\n", w, h));
        if (!ppmPath.empty())
        {
            std::ofstream ppm(ppmPath, std::ios::binary);
            ppm << "P6\n" << w << ' ' << h << "\n255\n";
            for (uint32_t px : pixels)
                for (int c = 0; c < 3; ++c) ppm.put((char)(((px >> (10 * c)) & 1023u) >> 2));
            if (!ppm) fail("could not write %s", ppmPath.c_str());
        }
        logf("captured %s at %ux%u after %u frames -> %s (quality %s)\n", scenePath.c_str(), w, h, frames, outPath.c_str(), o.qualityDirectory.string().c_str());
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
