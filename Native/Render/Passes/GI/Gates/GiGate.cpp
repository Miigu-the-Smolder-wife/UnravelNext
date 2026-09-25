// R gate: GI, reflections and ray tracing structures at 4K and 1440p (ARCHITECTURE 2.5 GI 0.45-0.54 ms at 4K [expected], 2.12 dynamic
// TLAS 0.06 ms). Loads a generated scene (.unxscene from C's unx_scenegen), and per frame declares R's acceleration
// structure passes, a ray-traced primary visibility pass standing in for V/M (reported separately, not part of the R
// budget), and the GI passes; the harness times every pass (1.5 s warm-up, medians, P95).
//
// --integrated renders the whole frame through FrameRenderer instead (V, M, S and R of an integrated build,
// Tools/CI/Build.ps1 -Track all): R's passes then run on the real visibility, G-buffer, lobe tiles and planar
// reflection views (FrameServices::renderView). Same-named passes of secondary views are summed into one median (Harness),
// so a secondary view's cost is measured as a difference: e.g. --set reflection.planar_views_max=0.
//
//   GpuLock.ps1 -Track R -- unx_gate_gi_gigate --scene <file.unxscene> [--camera N] [--resolution 4K|1440p|both] [--frames N]
//                                                [--out DIR] [--integrated] [--set key=value ...]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/gi/GiSystem.h"
#include "unx/refl/ReflectionSystem.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <cmath>
#include <cstring>
#include <vector>
#if UNX_HAS_CLUSTERBUILDER
#include "unx/clusterbuilder/ClusterBuilder.h"
#endif

using namespace unx;
using namespace unx::render;

namespace
{
struct Constants
{
    ComPtr<ID3D12Resource> buffer;
    uint8_t* mapped = nullptr;
};

Constants createConstants(Device& device, uint32_t slots)
{
    Constants c;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = slots * 1024ull;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&c.buffer)), "constants");
    D3D12_RANGE none{ 0, 0 };
    check(c.buffer->Map(0, &none, reinterpret_cast<void**>(&c.mapped)), "map constants");
    return c;
}

double passSum(const HarnessResult& r, const char* prefix)
{
    double sum = 0;
    for (const auto& [name, d] : r.passMs)
        if (name.rfind(prefix, 0) == 0) sum += d.median;
    return sum;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string scenePath, resolutions = "both", out, qualityPath = std::string(UNX_SOURCE_DIR) + "/Config/quality";
        uint32_t frames = 600, cameraIndex = 0;
        bool integrated = false;
        std::vector<std::string> overrides;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--scene") scenePath = next();
            else if (a == "--camera") cameraIndex = (uint32_t)std::stoul(next());
            else if (a == "--resolution") resolutions = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--out") out = next();
            else if (a == "--quality") qualityPath = next();
            else if (a == "--integrated") integrated = true;
            else if (a == "--set") overrides.push_back(next());
            else fail("unknown argument %s", a.c_str());
        }
        if (scenePath.empty()) fail("--scene <file.unxscene> is required (Tools/SceneGen: unx_scenegen --scene <name> --out Cache/Scenes)");
        QualityConfig quality = QualityConfig::loadDirectory(qualityPath);
        for (const std::string& o : overrides) quality.applyOverride(o);
        const scene::Scene s = scene::load(scenePath);
        if (cameraIndex >= s.cameras.size()) fail("scene has %zu cameras", s.cameras.size());
        if (out.empty()) out = std::string(UNX_SOURCE_DIR) + "/Results/R/GiGate";

        Device device(DeviceOptions{});
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene gpuScene(device);
        gpuScene.upload(s);
        if (integrated)
        {
#if UNX_HAS_CLUSTERBUILDER
            gpuScene.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality)));
#else
            fail("--integrated needs the integrated build (Tools/CI/Build.ps1 -Track all): V's cluster builder is not in this one");
#endif
        }
        Harness harness(device, quality);
        logf("scene %s (%s), camera '%s', %zu instances, quality %s\n", s.name.c_str(), scene::contentHash(s).substr(0, 16).c_str(), s.cameras[cameraIndex].name.c_str(),
             s.instances.size(), quality.shortHash().c_str());

        const std::vector<std::string> list = resolutions == "both" ? std::vector<std::string>{ "4K", "1440p" } : std::vector<std::string>{ resolutions };
        for (const std::string& name : list)
        {
            const Resolution res = resolutionFromString(name, quality);
            TrackState state;
            HarnessOptions opt;
            opt.frames = frames;
            opt.outputDirectory = out;
            opt.label = s.name + "_" + s.cameras[cameraIndex].name + "_" + res.name + (integrated ? "_integrated" : "");
            Constants constants = createConstants(device, opt.framesInFlight);
            const ViewDesc view = ViewDesc::fromCamera(s.cameras[cameraIndex], res.width, res.height, float4x4{});
            gi::GiSystem* giSystem = nullptr;
            refl::ReflectionSystem* reflSystem = nullptr;
            std::unique_ptr<FrameRenderer> renderer;
            if (integrated) renderer = std::make_unique<FrameRenderer>(device, shaders, quality, gpuScene, opt.framesInFlight);
            HarnessResult r = harness.run(res, opt, [&](RenderGraph& graph, const Resolution&, uint64_t f) {
                if (renderer)
                {
                    FrameContext frame;
                    frame.frameIndex = f;
                    frame.time = f / 165.0;
                    frame.deltaTime = 1 / 165.0f;
                    frame.mainView = view;
                    renderer->record(graph, frame, graph.createTexture({ "gate output", res.width, res.height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM }));
                    return;
                }
                FrameContext frame;
                frame.frameIndex = f;
                frame.time = f / 165.0;
                frame.deltaTime = 1 / 165.0f;
                frame.mainView = view;
                FrameResources resources;
                FrameServices services;
                const uint64_t slot = f % opt.framesInFlight;
                auto frameConstantsFor = [&](const ViewDesc& v) {
                    gpu::FrameConstants c{};
                    c.viewProj = v.viewProj;
                    c.prevViewProj = v.prevViewProj;
                    c.invViewProj = v.invViewProj;
                    c.view = v.view;
                    c.proj = v.proj;
                    c.cameraPosition = v.position;
                    c.nearPlane = v.nearPlane;
                    c.viewWidth = v.width;
                    c.viewHeight = v.height;
                    c.frameIndex = (uint32_t)f;
                    c.time = (float)frame.time;
                    c.deltaTime = frame.deltaTime;
                    c.exposure = 1.0f / (1.2f * std::exp2(v.ev100));
                    c.tanHalfFovY = std::tan(v.verticalFov * 0.5f);
                    c.sunDirection = s.sun.direction;
                    c.sunIlluminance = s.sun.illuminance;
                    c.sunColor = s.sun.color;
                    c.sunAngularRadius = s.sun.angularRadius;
                    c.windDirection = s.windDirection;
                    c.windSpeed = s.windSpeed;
                    gpuScene.fill(c);
                    std::memcpy(constants.mapped + slot * 1024, &c, sizeof c);
                    return constants.buffer->GetGPUVirtualAddress() + slot * 1024;
                };
                FramePassContext fc{ device, graph, shaders, quality, gpuScene, frame, resources, services, frameConstantsFor, &state };
                ViewResources main;
                main.view = view;
                main.frameConstants = frameConstantsFor(view);
                main.depth = graph.createTexture({ "stand-in depth", res.width, res.height, 1, 1, DXGI_FORMAT_R32_FLOAT });
                main.gbuffer = graph.createTexture({ "stand-in gbuffer", res.width, res.height, 1, 1, DXGI_FORMAT_R32G32_UINT });
                rt::RayScene& rays = rt::RayScene::get(fc);
                rays.record(fc);
                uint32_t sceneConstants[8];
                rays.rootConstants(sceneConstants);
                rt::RayPipeline& primary = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline("Passes/GI/Tests/GiTestPrimary", { "GiTestPrimaryGen" }));
                const TextureRef depth = main.depth, gbuffer = main.gbuffer;
                const D3D12_GPU_VIRTUAL_ADDRESS fcAddress = main.frameConstants;
                graph.addPass("standin.primary", QueueType::Compute,
                              [&](PassBuilder& b) {
                                  b.use(depth, Use::UavGraphics);
                                  b.use(gbuffer, Use::UavGraphics);
                                  rays.declareTraversal(b);
                              },
                              [&primary, depth, gbuffer, sceneConstants, fcAddress, res](PassContext& c) {
                                  uint32_t k[32] = {};
                                  k[0] = c.uav(depth);
                                  k[1] = c.uav(gbuffer);
                                  std::memcpy(&k[24], sceneConstants, sizeof sceneConstants);
                                  c.computeConstants(k, 32);
                                  c.bindFrameConstants(fcAddress);
                                  primary.dispatch(c.cmd, 0, res.width, res.height, 1);
                              });
                gi::GiSystem& gi = gi::GiSystem::get(fc);
                giSystem = &gi;
                gi.record(fc, main, rays);
                reflSystem = &refl::ReflectionSystem::get(fc);
                reflSystem->record(fc, main, rays);
                const TextureRef probes = main.screenProbes, reflection = main.reflection;
                graph.addPass("standin.consume", QueueType::Compute, [&](PassBuilder& b) {
                    b.use(probes, Use::SrvCompute);
                    b.use(reflection, Use::SrvCompute);
                    b.keep();
                }, [](PassContext&) {});
            });
            harness.printSummary(r);
            if (renderer)
            {
                giSystem = gi::GiSystem::find(renderer->trackState());
                reflSystem = refl::ReflectionSystem::find(renderer->trackState());
            }
            const double reflMs = passSum(r, "r.refl."), traceReflMs = r.passMs.count("r.refl.trace") ? r.passMs.at("r.refl.trace").median : 0;
            logf("R %s: reflections %.3f ms (trace %.3f ms, classify %.3f, resolve %.3f)\n", res.name.c_str(), reflMs, traceReflMs,
                 r.passMs.count("r.refl.classify") ? r.passMs.at("r.refl.classify").median : 0, r.passMs.count("r.refl.resolve") ? r.passMs.at("r.refl.resolve").median : 0);
            const double giMs = passSum(r, "r.gi."), asMs = passSum(r, "r.as."), traceMs = r.passMs.count("r.gi.trace") ? r.passMs.at("r.gi.trace").median : 0;
            if (renderer) logf("R %s: integrated frame %.3f ms GPU (median)\n", res.name.c_str(), r.gpuFrameMs.median);
            logf("R %s: GI %.3f ms (trace %.3f ms = %.2f G rays/s incl. hit shading), acceleration structures %.3f ms; stand-in primary visibility %.3f ms (not R)\n",
                 res.name.c_str(), giMs, traceMs, traceMs > 0 ? gi::GiSettings::fromQuality(quality).updatesPerFrame * 64 / (traceMs * 1e-3) / 1e9 : 0, asMs,
                 r.passMs.count("standin.primary") ? r.passMs.at("standin.primary").median : 0);
            if (reflSystem)
            {
                const refl::ReflectionSystem::Stats rs = reflSystem->readStats();
                logf("R %s: reflection jobs %u (M %u = rays %u; G samples %u = rays %u), G pixels %u\n", res.name.c_str(), rs.jobs, rs.mirrorJobs, rs.mirrorJobs, rs.glossyJobs,
                     rs.glossyJobs * reflSystem->settings().raysPerSample, rs.glossyPixels);
                logf("R %s: planar candidates %u visible, largest plane %u mirror pixels (camera from %u), %u views (%u px)%s, CPU %.3f ms\n", res.name.c_str(),
                     rs.planarCandidates, rs.planarLargestPixels, reflSystem->settings().planarMinPixels, rs.planarViews, rs.planarPixels,
                     renderer ? "" : " (no renderView in this gate)", rs.planarSelectMs);
            }
            if (giSystem)
            {
                const gi::GiStats st = giSystem->readStats();
                logf("R %s: cache %u live, %u requested, %u selected + %u background updates, %u hit entries, %u created, %u allocation failures, %u table overflows\n",
                     res.name.c_str(), st.live, st.requested, st.selected, st.background, st.hits, st.created, st.allocationFailures, st.tableFull);
            }
        }
        rt::RayPipeline::releaseDevice(device);
        rt::RayScene::releaseDevice(device);
        device.waitIdle();
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
