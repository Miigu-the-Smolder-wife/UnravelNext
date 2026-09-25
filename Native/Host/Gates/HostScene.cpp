// Host scene gate (I track): renders a .unxscene saved by a host (UnxSceneSave, e.g. the old engine's data World from
// Unity) through FrameRenderer with the core harness, from its camera 0, at 4K and 1440p: the same scene outside Unity,
// so the Unity-hosted frame time can be compared with the renderer alone. GPU lock required:
//   GpuLock.ps1 -Track I -- build/I/bin/unx_gate_host_hostscene.exe --scene <file.unxscene> [--resolution 4K|1440p|both]
//       [--frames 600] [--out DIR]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"

#if __has_include("unx/clusterbuilder/ClusterBuilder.h")
#include "unx/clusterbuilder/ClusterBuilder.h"
#define HOST_SCENE_GATE 1
#endif

#include <cstdio>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

int main(int argc, char** argv)
{
    try
    {
#if !HOST_SCENE_GATE
        (void)argc;
        (void)argv;
        fail("this build lacks V's cluster builder: Tools/CI/Build.ps1 -Track I");
#else
        std::string scenePath, resolutionArg = "both", out;
        uint32_t frames = 600;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") scenePath = next();
            else if (a == "--resolution") resolutionArg = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--out") out = next();
            else fail("unknown argument %s", a.c_str());
        }
        if (scenePath.empty()) fail("--scene <file.unxscene> is required");
        requireGpuLock("unx_gate_host_hostscene");
        const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        const scene::Scene s = scene::load(scenePath);
        scene::validate(s);
        if (s.cameras.empty()) fail("scene %s has no camera", scenePath.c_str());
        ClusterData clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality));
        logf("scene %s '%s' (%s): %zu meshes, %zu instances, %zu clusters, camera '%s'\n", scenePath.c_str(), s.name.c_str(), scene::contentHash(s).substr(0, 16).c_str(),
             s.meshes.size(), s.instances.size(), clusters.clusters.size(), s.cameras[0].name.c_str());
        Device device({});
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene gpuScene(device);
        gpuScene.upload(s);
        gpuScene.setClusters(std::move(clusters));
        Harness harness(device, quality);
        const std::vector<std::string> resolutions = resolutionArg == "both" ? std::vector<std::string>{ "4K", "1440p" } : std::vector<std::string>{ resolutionArg };
        for (const std::string& rs : resolutions)
        {
            const Resolution res = resolutionFromString(rs, quality);
            FrameRenderer renderer(device, shaders, quality, gpuScene, 2);
            HarnessOptions options;
            options.frames = frames;
            options.label = "I host scene " + (s.name.empty() ? std::string("unnamed") : s.name) + " " + rs;
            options.outputDirectory = out.empty() ? std::filesystem::path(UNX_SOURCE_DIR) / "Results/I/HostScene" : std::filesystem::path(out);
            options.writePassCsv = false;
            float4x4 prev = ViewDesc::fromCamera(s.cameras[0], res.width, res.height, {}).viewProj;
            const HarnessResult r = harness.run(res, options, [&](RenderGraph& g, const Resolution& rr, uint64_t frame) {
                FrameContext fc;
                fc.frameIndex = frame;
                fc.time = frame / 60.0;
                fc.deltaTime = 1.0f / 60;
                fc.mainView = ViewDesc::fromCamera(s.cameras[0], rr.width, rr.height, prev);
                prev = fc.mainView.viewProj;
                const TextureRef output = g.createTexture({ "host scene output", rr.width, rr.height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
                renderer.record(g, fc, output);
            });
            harness.printSummary(r);
        }
        return 0;
#endif
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
