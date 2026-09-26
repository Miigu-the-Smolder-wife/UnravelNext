// W water surface timing (FEATURES_GAME 1.9; the stage 3 ray jobs' cost under the rule that a change of ray counts is
// timed before it is committed): one of C's procedural scenes through the whole frame (FrameRenderer: every track of the
// build, R's FrameServices::traceRefractions included) with a closed basin of water in front of the camera (W2's pool
// stream, engine 2), reporting W's surface passes, R's refraction and reflection ray passes they call, the water sample
// counts (WaterSurfaceStats: interior samples, fallbacks, jobs, bands) and the frame.
// Performance runs only under the GPU lock, in the integrated build:
//   powershell -File Tools/CI/GpuLock.ps1 -Track W -Kind timing -- build/all/bin/unx_gate_water_watergate.exe
//       [--scene interior] [--camera NAME] [--resolution 4K|1440p] [--frames 300] [--pool 3.0] [--ahead 2.5] [--below 1.2]
//       [--out DIR] [--set key=value ...] [--planar auto|on|off]
//   --planar: calm water's reflection camera (A14): by the cost rule (default), forced, or never (reflection rays).
//   --pool: the basin's side (m); --ahead / --below: its centre along the camera's horizontal forward and under the eye.
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/water/WaterSurface.h"
#if UNX_M_HAS_SCENEGEN && UNX_M_HAS_CLUSTERBUILDER
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/scenegen/SceneGen.h"
#endif

#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
double sumPasses(const HarnessResult& r, const std::function<bool(const std::string&)>& match)
{
    double ms = 0;
    for (const auto& [name, d] : r.passMs)
        if (match(name)) ms += d.median;
    return ms;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string sceneName = "interior", cameraName, resolutionArg = "4K", out;
        uint32_t frames = 300;
        float side = 3.0f, ahead = 2.5f, below = 1.2f;
        std::vector<std::string> overrides;
        int planar = -1;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") sceneName = next();
            else if (a == "--camera") cameraName = next();
            else if (a == "--resolution") resolutionArg = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--pool") side = std::stof(next());
            else if (a == "--ahead") ahead = std::stof(next());
            else if (a == "--below") below = std::stof(next());
            else if (a == "--out") out = next();
            else if (a == "--set") overrides.push_back(next());
            else if (a == "--planar")
            {
                const std::string v = next();
                planar = v == "on" ? 1 : v == "off" ? 0 : v == "auto" ? -1 : (fail("--planar takes auto, on or off"), -1);
            }
            else fail("unknown argument %s", a.c_str());
        }
        requireGpuLock("unx_gate_water_watergate");
#if !(UNX_M_HAS_SCENEGEN && UNX_M_HAS_CLUSTERBUILDER)
        fail("this build has no scene generator or cluster builder: use the integrated build (Build.ps1 -Track all)");
#else
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const std::string& o : overrides) quality.applyOverride(o);
        scenegen::Request request;
        bool found = false;
        for (scenegen::SceneId id : scenegen::allScenes())
            if (sceneName == scenegen::sceneName(id))
            {
                request.id = id;
                found = true;
            }
        if (!found) fail("unknown scene %s", sceneName.c_str());
        scene::Scene s = scenegen::generate(request);
        // The basin's water: pure water (baseColor = the medium's 1 m transmittance, Pope & Fry), as D0 and W2 author it.
        scene::Material water;
        water.name = "gate water";
        water.cls = scene::MaterialClass::Water;
        water.baseColor = { 0.712f, 0.945f, 0.991f };
        water.roughness = 0.02f;
        water.ior = 1.333f;
        const uint32_t waterMaterial = (uint32_t)s.materials.size();
        s.materials.push_back(water);
        scene::Camera cam = s.cameras.at(0);
        for (const scene::Camera& k : s.cameras)
            if (k.name == cameraName) cam = k;
        ClusterData clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality));

        Device device({});
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene gpuScene(device);
        gpuScene.upload(s);
        gpuScene.setClusters(std::move(clusters));
        Harness harness(device, quality);
        const Resolution res = resolutionFromString(resolutionArg, quality);
        FrameRenderer renderer(device, shaders, quality, gpuScene, 2);
        renderer.trackState().get<water::WaterSurfaceDebug>("W.surface.debug").planar = planar;
        ComPtr<ID3D12Resource> outputTexture;
        {
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC1 d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = res.width;
            d.Height = res.height;
            d.DepthOrArraySize = d.MipLevels = 1;
            d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&outputTexture)),
                  "W gate output");
        }
        // The basin: centred ahead of the eye along its horizontal forward, its still surface below the eye.
        const float3 f{ cam.forward.x, 0, cam.forward.z };
        const float fl = std::sqrt(f.x * f.x + f.z * f.z);
        PoolFrame pool;
        pool.id = 1;
        pool.material = waterMaterial;
        pool.sizeX = pool.sizeZ = side;
        pool.depth = 1.0f;
        pool.centre[0] = cam.position.x + (fl > 0 ? f.x / fl : 0) * ahead;
        pool.centre[1] = cam.position.y - below;
        pool.centre[2] = cam.position.z + (fl > 0 ? f.z / fl : 1) * ahead;
        HarnessOptions options;
        options.frames = frames;
        options.label = "W " + sceneName + " basin";
        if (!out.empty()) options.outputDirectory = out;
        float4x4 prev = ViewDesc::fromCamera(cam, res.width, res.height, {}).viewProj;
        const HarnessResult r = harness.run(res, options, [&](RenderGraph& g, const Resolution& rr, uint64_t frame) {
            FrameContext fc;
            fc.frameIndex = frame;
            fc.time = frame / 60.0;
            fc.deltaTime = 1.0f / 60;
            fc.mainView = ViewDesc::fromCamera(cam, rr.width, rr.height, prev);
            prev = fc.mainView.viewProj;
            fc.pools = &pool;
            fc.poolCount = 1;
            const TextureRef output = g.importTexture(outputTexture.Get(), { "display output", rr.width, rr.height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM }, D3D12_BARRIER_LAYOUT_COMMON);
            renderer.record(g, fc, output);
        });
        harness.printSummary(r);
        const water::WaterSurfaceStats st = water::latestWaterSurfaceStats(renderer.trackState());
        const water::WaterSurfaceDebug& dbg = renderer.trackState().get<water::WaterSurfaceDebug>("W.surface.debug");
        const double surface = sumPasses(r, [](const std::string& n) { return n.rfind("w.surface", 0) == 0; });
        const double rays = sumPasses(r, [](const std::string& n) { return n.rfind("w.surface.ray", 0) == 0; });
        const double traced = sumPasses(r, [](const std::string& n) { return n.find("refraction") != std::string::npos || n.find("refract") != std::string::npos; });
        const double pool0 = sumPasses(r, [](const std::string& n) { return n.rfind("w.pool", 0) == 0 || n.rfind("pool", 0) == 0; });
        const uint32_t samples = st.shaded + st.offscreen + st.exited + st.occluded + st.steps;
        logf("W %s %s: water samples %u (%.1f %% of the view; fallbacks %u), jobs %u reflection + %u refraction, traced %u, overflow %u; %u bands, %u record rounds\n",
             sceneName.c_str(), resolutionArg.c_str(), samples, 100.0 * samples / ((double)res.width * res.height), st.fallbacks(), st.reflectJobs, st.refractJobs, st.traced,
             st.rayOverflow, dbg.rayBands, dbg.rayRounds);
        logf("W %s %s: calm water (--planar %s): %u reflection cameras, %u samples read them, mask pixels %u\n", sceneName.c_str(), resolutionArg.c_str(),
             planar == 1 ? "on" : planar == 0 ? "off" : "auto", st.planarViews, st.planar, st.planarMask[0]);
        logf("W %s %s: surface passes %.3f ms (of which list clear/args/apply %.3f) | R ray passes (names with 'refract') %.3f ms | pool stream %.3f ms | frame %.3f ms [measured, median of %u]\n",
             sceneName.c_str(), resolutionArg.c_str(), surface, rays, traced, pool0, r.gpuFrameMs.median, frames);
        for (const std::string& n : r.passOrder)
            if (n.rfind("w.", 0) == 0 || n.find("refract") != std::string::npos || n.rfind("pool", 0) == 0 || n.find("planar") != std::string::npos || n.find("secondary") != std::string::npos)
                logf("  %-40s %.4f ms\n", n.c_str(), r.passMs.at(n).median);
        return 0;
#endif
    }
    catch (const std::exception& e)
    {
        logf("unx_gate_water_watergate: %s\n", e.what());
        return 1;
    }
}
