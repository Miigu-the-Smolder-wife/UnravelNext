// V performance gate (ARCHITECTURE 2.1 cost formula, 7.2 P1 gate terms): renders one of C's procedural scenes
// through V only (other tracks are empty stubs in a core + V + C build, so the frame is V's passes) and compares each
// V term with the design formula evaluated on the measured quantities:
//   cull (instances, hierarchy, clusters) + HiZ     design: I x 32 B / 600 GB/s + C x 64 B / 600 GB/s x 1.5 + 0.02 ms
//   band A raster                                    design: M x 0.87 ns + T_A / 17 G/s
// Performance runs only under the GPU lock (INTERFACES 3.3):
//   powershell -File Tools/CI/GpuLock.ps1 -Track core -- build/<t>/bin/unx_gate_visibility_visibilitygate.exe
//       --scene city_block|forest_thin|... [--resolution 4K|1440p|both] [--frames 600] [--scale 1] [--moving] [--out DIR]
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/visibility/Visibility.h"
#if UNX_HAS_SCENEGEN
#include "unx/scenegen/SceneGen.h"
#endif

#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
scene::Camera cameraAt(const scene::Scene& s, bool moving, double time)
{
    scene::Camera c = s.cameras.at(0);
    if (!moving || s.paths.empty() || s.paths[0].keys.size() < 2) return c;
    const auto& keys = s.paths[0].keys;
    const double span = keys.back().time - keys.front().time;
    const float t = (float)(keys.front().time + std::fmod(time, span));
    size_t k = 0;
    while (k + 2 < keys.size() && keys[k + 1].time < t) ++k;
    const float u = std::clamp((t - keys[k].time) / std::max(keys[k + 1].time - keys[k].time, 1e-6f), 0.0f, 1.0f);
    c.position = keys[k].position + (keys[k + 1].position - keys[k].position) * u;
    c.forward = normalize(keys[k].forward + (keys[k + 1].forward - keys[k].forward) * u);
    c.up = normalize(keys[k].up + (keys[k + 1].up - keys[k].up) * u);
    return c;
}

double sumPasses(const HarnessResult& r, const std::function<bool(const std::string&)>& match)
{
    double ms = 0;
    for (const auto& [name, d] : r.passMs)
        if (match(name)) ms += d.median;
    return ms;
}

bool startsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string sceneName = "city_block", resolutionArg = "both", out;
        uint32_t frames = 600;
        float scale = 1.0f;
        bool moving = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") sceneName = next();
            else if (a == "--resolution") resolutionArg = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--scale") scale = std::stof(next());
            else if (a == "--moving") moving = true;
            else if (a == "--out") out = next();
            else fail("unknown argument %s", a.c_str());
        }
        requireGpuLock("unx_gate_visibility_visibilitygate");
#if !UNX_HAS_SCENEGEN
        fail("this build has no scene generator: build with track C enabled (Build.ps1 -Tracks \"V;C\" or -Track all)");
#else
        const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        scenegen::Request request;
        bool found = false;
        for (scenegen::SceneId id : scenegen::allScenes())
            if (sceneName == scenegen::sceneName(id))
            {
                request.id = id;
                found = true;
            }
        if (!found) fail("unknown scene %s", sceneName.c_str());
        request.scale = scale;
        auto t0 = std::chrono::steady_clock::now();
        const scene::Scene s = scenegen::generate(request);
        const double generateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        uint64_t sceneTriangles = 0;
        for (const auto& inst : s.instances) sceneTriangles += s.meshes[inst.mesh].indices.size() / 3;
        t0 = std::chrono::steady_clock::now();
        clusterbuilder::BuildStats buildStats;
        ClusterData clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality), &buildStats);
        const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        logf("scene %s (scale %.2f): %zu meshes, %zu instances, %llu instanced triangles, %zu clusters; generated in %.0f ms, clusters built in %.0f ms\n", sceneName.c_str(),
             scale, s.meshes.size(), s.instances.size(), (unsigned long long)sceneTriangles, clusters.clusters.size(), generateMs, buildMs);

        Device device({});
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene gpuScene(device);
        gpuScene.upload(s);
        gpuScene.setClusters(std::move(clusters));
        Harness harness(device, quality);
        std::vector<std::string> resolutions = resolutionArg == "both" ? std::vector<std::string>{ "4K", "1440p" } : std::vector<std::string>{ resolutionArg };
        int status = 0;
        for (const std::string& rs : resolutions)
        {
            const Resolution res = resolutionFromString(rs, quality);
            FrameRenderer renderer(device, shaders, quality, gpuScene, 2);
            HarnessOptions options;
            options.frames = frames;
            options.label = "V " + sceneName + (moving ? " moving" : " static");
            if (!out.empty()) options.outputDirectory = out;
            float4x4 prev = ViewDesc::fromCamera(cameraAt(s, moving, 0), res.width, res.height, {}).viewProj;
            const HarnessResult r = harness.run(res, options, [&](RenderGraph& g, const Resolution& rr, uint64_t frame) {
                FrameContext fc;
                fc.frameIndex = frame;
                fc.time = frame / 60.0;
                fc.deltaTime = 1.0f / 60;
                fc.mainView = ViewDesc::fromCamera(cameraAt(s, moving, fc.time), rr.width, rr.height, prev);
                prev = fc.mainView.viewProj;
                const TextureRef output = g.createTexture({ "gate output", rr.width, rr.height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
                renderer.record(g, fc, output);
            });
            harness.printSummary(r);
            const visibility::Stats st = visibility::latestStats(renderer.trackState());
            const double raster = sumPasses(r, [](const std::string& n) { return n.find(".raster.") != std::string::npos; });
            const double hiz = sumPasses(r, [](const std::string& n) { return startsWith(n, "v.hiz."); });
            const double cull = sumPasses(r, [](const std::string& n) { return startsWith(n, "v.cull.") && n.find(".raster.") == std::string::npos; });
            const uint32_t trianglesA = st.triangles[0];
            // Design formula on the measured quantities (ARCHITECTURE 2.1, 1.2 floors: 600 GB/s, 0.87 ns per meshlet,
            // 17 G triangles/s).
            const double designCull = st.instancesVisible * 32.0 / 600e9 * 1e3 + st.clustersTested * 64.0 / 600e9 * 1e3 * 1.5;
            const double designRaster = st.visibleClusters * 0.87e-6 + trianglesA / 17e9 * 1e3;
            logf("V %s %s: cull %.3f ms (design %.3f on %u instances, %u clusters tested) | HiZ %.3f ms (design 0.020) | band A raster %.3f ms "
                 "(design %.3f on %u clusters, %u triangles) | total V %.3f ms of frame %.3f ms\n",
                 sceneName.c_str(), rs.c_str(), cull, designCull, st.instancesVisible, st.clustersTested, hiz, raster, designRaster, st.visibleClusters, trianglesA,
                 cull + hiz + raster, r.gpuFrameMs.median);
            logf("  nodes tested %u, triangles by band A/B/C %u/%u/%u, deferred %u/%u/%u, overflow 0x%x (stats of frame %llu)\n", st.nodesTested, st.triangles[0],
                 st.triangles[1], st.triangles[2], st.deferredInstances, st.deferredNodes, st.deferredClusters, st.overflow, (unsigned long long)st.frameIndex);
            if (st.overflow) status = 1;
        }
        return status;
#endif
    }
    catch (const std::exception& e)
    {
        logf("unx_gate_visibility_visibilitygate: %s\n", e.what());
        return 1;
    }
}
