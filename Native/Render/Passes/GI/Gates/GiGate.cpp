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
// --dump FILE keeps the last frame's view.reflection (rows [0, H), RGBA16F) of the last resolution; --compare FILE compares
// it with an earlier dump of the same camera and resolution: per 8 x 8 tile the mean luminance of the pixels valid in both
// (a = 1), then over tiles whose reference mean is above 1e-3 of the image's mean the mean and P95 of |a - b| / b and the
// signed bias sum(a - b) / sum(b). Tile means average the per-pixel ray noise (e.g. reflection.experiment_disable=4, sun
// visibility at hits by shadow rays, against 0, by S's VSM). The copy runs in one extra frame after the timed ones.
//
//   GpuLock.ps1 -Track R -- unx_gate_gi_gigate --scene <file.unxscene> [--camera N] [--resolution 4K|1440p|both] [--frames N]
//                                                [--out DIR] [--integrated] [--set key=value ...] [--dump FILE] [--compare FILE]
//                                                [--planar-forced]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/gi/GiSystem.h"
#include "unx/refl/ReflectionSystem.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
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
float halfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
    float v = exponent == 0 ? std::ldexp((float)mantissa, -24) : exponent == 31 ? INFINITY : std::ldexp((float)(mantissa | 1024), (int)exponent - 25);
    return sign ? -v : v;
}

uint16_t floatToHalf(float v)
{
    uint32_t x;
    std::memcpy(&x, &v, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exponent = (int)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = x & 0x7FFFFFu;
    if (exponent <= 0) return (uint16_t)sign;  // flush tiny values to zero (diagnostic dumps)
    if (exponent >= 31) return (uint16_t)(sign | 0x7C00u);
    mantissa += 0x1000u;  // round to nearest
    if (mantissa & 0x800000u) return (uint16_t)(sign | ((exponent + 1) << 10));
    return (uint16_t)(sign | (exponent << 10) | (mantissa >> 13));
}

// Per 8 x 8 tile mean luminance of the valid pixels (a = 1) of a dumped reflection; tiles without any: -1.
std::vector<double> tileMeans(const std::vector<uint16_t>& texels, uint32_t width, uint32_t height, const std::vector<uint16_t>* other)
{
    const uint32_t tx = (width + 7) / 8, ty = (height + 7) / 8;
    std::vector<double> sum((size_t)tx * ty, 0), count((size_t)tx * ty, 0);
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t i = ((size_t)y * width + x) * 4;
            if (halfToFloat(texels[i + 3]) < 0.5f || (other && halfToFloat((*other)[i + 3]) < 0.5f)) continue;
            const size_t t = (size_t)(y / 8) * tx + x / 8;
            sum[t] += 0.2126 * halfToFloat(texels[i]) + 0.7152 * halfToFloat(texels[i + 1]) + 0.0722 * halfToFloat(texels[i + 2]);
            count[t] += 1;
        }
    for (size_t t = 0; t < sum.size(); ++t) sum[t] = count[t] > 0 ? sum[t] / count[t] : -1;
    return sum;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string scenePath, resolutions = "both", out, qualityPath = std::string(UNX_SOURCE_DIR) + "/Config/quality", dumpPath, comparePath;
        uint32_t frames = 600, cameraIndex = 0, averageFrames = 1, noiseFrames = 0;
        std::string noiseMapPath;
        bool integrated = false, planarForced = false;
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
            else if (a == "--planar-forced") planarForced = true;  // every counted plane gets a camera (view cost breakdown)
            else if (a == "--set") overrides.push_back(next());
            else if (a == "--dump") dumpPath = next();
            else if (a == "--average") averageFrames = std::max(1u, (uint32_t)std::stoul(next()));  // --dump/--compare over N frames
            else if (a == "--compare") comparePath = next();
            // --noise N (with --integrated): the displayed image (the tone-mapped output) of N consecutive untimed frames after
            // the harness; per pixel the temporal standard deviation of R, G, B (display units, 1 = full scale), i.e. what a
            // single displayed frame deviates by. --noise-map writes it as a PGM (255 = 8/255 or more).
            else if (a == "--noise") noiseFrames = (uint32_t)std::stoul(next());
            else if (a == "--noise-map") noiseMapPath = next();
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
            // A still camera: the previous view is this view (a zero previous matrix made every reprojection fail, so no
            // time integration could engage in the gate).
            const ViewDesc view = [&] {
                const float4x4 still = ViewDesc::fromCamera(s.cameras[cameraIndex], res.width, res.height, float4x4{}).viewProj;
                return ViewDesc::fromCamera(s.cameras[cameraIndex], res.width, res.height, still);
            }();
            gi::GiSystem* giSystem = nullptr;
            refl::ReflectionSystem* reflSystem = nullptr;
            rt::RayScene* rayScene = nullptr;
            std::unique_ptr<FrameRenderer> renderer;
            if (integrated) renderer = std::make_unique<FrameRenderer>(device, shaders, quality, gpuScene, opt.framesInFlight);
            // --dump / --compare: the reflection rows of every frame into one read-back buffer (the last frame's copy stays).
            const bool keepReflection = !dumpPath.empty() || !comparePath.empty();
            const uint32_t dumpPitch = (res.width * 8 + 255) & ~255u;
            ComPtr<ID3D12Resource> dumpBuffer;
            if (keepReflection)
            {
                D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC1 d{};
                d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                d.Width = (uint64_t)dumpPitch * res.height;
                d.Height = d.DepthOrArraySize = d.MipLevels = 1;
                d.SampleDesc.Count = 1;
                d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                check(device.d3d()->CreateCommittedResource3(&rb, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&dumpBuffer)),
                      "reflection dump");
            }
            const uint32_t noisePitch = (res.width * 4 + 255) & ~255u;
            ComPtr<ID3D12Resource> noiseBuffer;
            if (noiseFrames > 0)
            {
                if (!integrated) fail("--noise needs --integrated (the displayed image)");
                D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC1 d{};
                d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                d.Width = (uint64_t)noisePitch * res.height;
                d.Height = d.DepthOrArraySize = d.MipLevels = 1;
                d.SampleDesc.Count = 1;
                d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                check(device.d3d()->CreateCommittedResource3(&rb, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&noiseBuffer)),
                      "noise read-back");
            }
            bool noiseFrame = false;
            bool dumpFrame = false;  // the extra untimed frame after the harness
            uint64_t lastFrame = 0;
            auto copyReflection = [&](RenderGraph& graph, TextureRef reflection) {
                if (!dumpFrame || !reflection.valid()) return;
                const BufferRef dst = graph.importBuffer(dumpBuffer.Get(), { "gate reflection dump", (uint64_t)dumpPitch * res.height, 0 });
                graph.addPass("gate.dump", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  b.use(reflection, Use::CopySrc);
                                  b.use(dst, Use::CopyDst);
                                  b.keep();
                              },
                              [reflection, dst, dumpPitch, res](PassContext& c) {
                                  D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                                  to.pResource = c.resource(dst);
                                  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                  to.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16B16A16_FLOAT, res.width, res.height, 1, dumpPitch };
                                  from.pResource = c.resource(reflection);
                                  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                  const D3D12_BOX box{ 0, 0, 0, res.width, res.height, 1 };
                                  c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
                              });
            };
            auto frameBody = [&](RenderGraph& graph, const Resolution&, uint64_t f) {
                lastFrame = f;
                if (renderer)
                {
                    FrameContext frame;
                    frame.frameIndex = f;
                    frame.time = f / 165.0;
                    frame.deltaTime = 1 / 165.0f;
                    frame.mainView = view;
                    const TextureRef output = graph.createTexture({ "gate output", res.width, res.height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
                    if (planarForced)
                        if (refl::ReflectionSystem* rs = refl::ReflectionSystem::find(renderer->trackState())) rs->setPlanarForced(true);
                    const ViewResources mainView = renderer->record(graph, frame, output);
                    copyReflection(graph, mainView.reflection);
                    if (noiseFrame)
                    {
                        const BufferRef dst = graph.importBuffer(noiseBuffer.Get(), { "gate noise read-back", (uint64_t)noisePitch * res.height, 0 });
                        graph.addPass("gate.noise", QueueType::Graphics,
                                      [&](PassBuilder& b) {
                                          b.use(output, Use::CopySrc);
                                          b.use(dst, Use::CopyDst);
                                          b.keep();
                                      },
                                      [output, dst, noisePitch, res](PassContext& c) {
                                          D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                                          to.pResource = c.resource(dst);
                                          to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                          to.PlacedFootprint.Footprint = { DXGI_FORMAT_R10G10B10A2_UNORM, res.width, res.height, 1, noisePitch };
                                          from.pResource = c.resource(output);
                                          from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                          const D3D12_BOX box{ 0, 0, 0, res.width, res.height, 1 };
                                          c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
                                      });
                    }
                    // The output has no reader in a gate: keep its producers (shading) alive like a present would.
                    graph.addPass("gate.present", QueueType::Graphics, [&](PassBuilder& b) {
                        b.use(output, Use::SrvCompute);
                        b.keep();
                    }, [](PassContext&) {});
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
                rayScene = &rays;
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
                // The M-facing screen-probe lookups at every pixel, timed alone (ProbeLookupBench.hlsl).
                {
                    const TextureRef probesIn = main.screenProbes, mapsIn = main.screenProbeMaps;
                    const TextureRef benchOut = graph.createTexture({ "bench probe lookups", res.width, res.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
                    const D3D12_GPU_VIRTUAL_ADDRESS benchConstants = main.frameConstants;
                    for (uint32_t mode : { 0u, 1u, 2u, 3u, 4u, 8u })
                    {
                        static const char* const names[9] = { "bench.probe.none", "bench.probe.irradiance", "bench.probe.radiance", "bench.probe.both", "bench.probe.footprint", "", "", "", "bench.probe.gather" };
                        graph.addPass(names[mode], QueueType::Compute,
                                      [&](PassBuilder& b) {
                                          b.use(probesIn, Use::SrvCompute);
                                          b.use(mapsIn, Use::SrvCompute);
                                          b.use(depth, Use::SrvCompute);
                                          b.use(gbuffer, Use::SrvCompute);
                                          b.use(benchOut, Use::UavCompute);
                                          b.keep();
                                      },
                                      [&shaders, probesIn, mapsIn, depth, gbuffer, benchOut, benchConstants, mode, res](PassContext& c) {
                                          const uint32_t k[8] = { c.srv(probesIn), c.srv(depth), c.srv(gbuffer), c.uav(benchOut), mode, res.width, res.height, c.srv(mapsIn) };
                                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/Gates/ProbeLookupBench"));
                                          c.computeConstants(k, 8);
                                          c.bindFrameConstants(benchConstants);
                                          c.cmd->Dispatch((res.width + 7) / 8, (res.height + 7) / 8, 1);
                                      });
                    }
                }
                const TextureRef probes = main.screenProbes, reflection = main.reflection;
                copyReflection(graph, reflection);
                graph.addPass("standin.consume", QueueType::Compute, [&](PassBuilder& b) {
                    b.use(probes, Use::SrvCompute);
                    b.use(reflection, Use::SrvCompute);
                    b.keep();
                }, [](PassContext&) {});
            };
            HarnessResult r = harness.run(res, opt, frameBody);
            harness.printSummary(r);
            if (noiseFrames > 0)
            {
                device.waitIdle();
                const size_t pixels = (size_t)res.width * res.height;
                std::vector<double> sum(pixels * 3, 0.0), sum2(pixels * 3, 0.0);
                D3D12_RANGE all{ 0, (SIZE_T)noisePitch * res.height }, none{ 0, 0 };
                for (uint32_t k = 0; k < noiseFrames; ++k)
                {
                    noiseFrame = true;
                    RenderGraph noiseGraph(device);
                    frameBody(noiseGraph, res, lastFrame + 1);
                    noiseGraph.execute(nullptr);
                    device.waitIdle();
                    noiseFrame = false;
                    void* mapped = nullptr;
                    check(noiseBuffer->Map(0, &all, &mapped), "map noise read-back");
                    for (uint32_t y = 0; y < res.height; ++y)
                    {
                        const uint32_t* row = (const uint32_t*)((const uint8_t*)mapped + (size_t)y * noisePitch);
                        for (uint32_t x = 0; x < res.width; ++x)
                        {
                            const size_t i = (size_t)y * res.width + x;
                            for (int c = 0; c < 3; ++c)
                            {
                                const double v = ((row[x] >> (10 * c)) & 1023u) / 1023.0;
                                sum[3 * i + c] += v;
                                sum2[3 * i + c] += v * v;
                            }
                        }
                    }
                    noiseBuffer->Unmap(0, &none);
                }
                std::vector<float> sigma(pixels);
                double imageMean = 0, sigmaMean = 0;
                size_t over1 = 0, over4 = 0;
                for (size_t i = 0; i < pixels; ++i)
                {
                    double var = 0;
                    for (int c = 0; c < 3; ++c)
                    {
                        const double m = sum[3 * i + c] / noiseFrames;
                        var += std::max(0.0, sum2[3 * i + c] / noiseFrames - m * m) / 3;
                        imageMean += m / 3;
                    }
                    sigma[i] = (float)std::sqrt(var);
                    sigmaMean += sigma[i];
                    over1 += sigma[i] > 1.0 / 255;
                    over4 += sigma[i] > 4.0 / 255;
                }
                std::vector<float> sorted = sigma;
                std::sort(sorted.begin(), sorted.end());
                auto q = [&](double f) { return 255.0 * sorted[std::min(pixels - 1, (size_t)(f * pixels))]; };
                // 8 x 8 tile means of sigma: where the noise is (a tile P99 well above the pixel P99 = clustered noise).
                const uint32_t tx = (res.width + 7) / 8, ty = (res.height + 7) / 8;
                std::vector<double> tiles(tx * ty, 0.0);
                for (uint32_t y = 0; y < res.height; ++y)
                    for (uint32_t x = 0; x < res.width; ++x) tiles[(y / 8) * tx + x / 8] += sigma[(size_t)y * res.width + x] / 64.0;
                std::sort(tiles.begin(), tiles.end());
                logf("R %s: displayed-image noise over %u frames (temporal sigma per pixel, 1/255 units): mean %.3f, P50 %.3f, P95 %.3f, P99 %.3f, max %.2f; "
                     "pixels over 1/255 %.3f %%, over 4/255 %.4f %%; 8x8 tile mean P95 %.3f, P99 %.3f, max %.2f; image mean %.4f\n",
                     res.name.c_str(), noiseFrames, 255.0 * sigmaMean / pixels, q(0.5), q(0.95), q(0.99), 255.0 * sorted.back(), 100.0 * over1 / pixels,
                     100.0 * over4 / pixels, 255.0 * tiles[tiles.size() * 95 / 100], 255.0 * tiles[tiles.size() * 99 / 100], 255.0 * tiles.back(), imageMean / pixels);
                if (!noiseMapPath.empty())
                {
                    std::ofstream f(noiseMapPath, std::ios::binary);
                    const std::string header = "P5\n" + std::to_string(res.width) + " " + std::to_string(res.height) + "\n255\n";
                    f.write(header.data(), (std::streamsize)header.size());
                    std::vector<uint8_t> px(pixels);
                    for (size_t i = 0; i < pixels; ++i) px[i] = (uint8_t)std::min(255.0, sigma[i] * 255.0 * 255.0 / 8.0);
                    f.write((const char*)px.data(), (std::streamsize)px.size());
                    if (!f) fail("cannot write %s", noiseMapPath.c_str());
                    logf("R %s: noise map written to %s\n", res.name.c_str(), noiseMapPath.c_str());
                }
            }
            if (keepReflection)
            {
                // --average N: the per-pixel mean over N untimed frames (valid where valid in at least half of them), so an
                // estimator's per-frame noise averages out and its bias remains.
                device.waitIdle();
                const size_t pixels = (size_t)res.width * res.height;
                std::vector<float> sum(pixels * 3, 0.0f);
                std::vector<uint32_t> validCount(pixels, 0);
                std::vector<uint16_t> texels(pixels * 4);
                D3D12_RANGE all{ 0, (SIZE_T)dumpPitch * res.height }, none{ 0, 0 };
                for (uint32_t k = 0; k < averageFrames; ++k)
                {
                    dumpFrame = true;
                    RenderGraph dumpGraph(device);
                    frameBody(dumpGraph, res, lastFrame + 1 + k);
                    dumpGraph.execute(nullptr);
                    device.waitIdle();
                    dumpFrame = false;
                    void* mapped = nullptr;
                    check(dumpBuffer->Map(0, &all, &mapped), "map reflection dump");
                    for (uint32_t y = 0; y < res.height; ++y)
                    {
                        const uint16_t* row = (const uint16_t*)((const uint8_t*)mapped + (size_t)y * dumpPitch);
                        for (uint32_t x = 0; x < res.width; ++x)
                        {
                            const size_t i = (size_t)y * res.width + x;
                            if (halfToFloat(row[4 * x + 3]) < 0.5f) continue;
                            for (int c = 0; c < 3; ++c) sum[3 * i + c] += halfToFloat(row[4 * x + c]);
                            ++validCount[i];
                        }
                    }
                    dumpBuffer->Unmap(0, &none);
                }
                for (size_t i = 0; i < pixels; ++i)
                {
                    const bool valid = 2 * validCount[i] >= averageFrames && validCount[i] > 0;
                    for (int c = 0; c < 3; ++c) texels[4 * i + c] = floatToHalf(valid ? sum[3 * i + c] / validCount[i] : 0.0f);
                    texels[4 * i + 3] = floatToHalf(valid ? 1.0f : 0.0f);
                }
                if (averageFrames > 1) logf("R %s: reflection averaged over %u untimed frames\n", res.name.c_str(), averageFrames);
                if (!dumpPath.empty())
                {
                    std::ofstream f(dumpPath, std::ios::binary);
                    const uint32_t header[2] = { res.width, res.height };
                    f.write((const char*)header, sizeof header);
                    f.write((const char*)texels.data(), (std::streamsize)(texels.size() * 2));
                    if (!f) fail("cannot write %s", dumpPath.c_str());
                    logf("R %s: reflection dumped to %s\n", res.name.c_str(), dumpPath.c_str());
                }
                if (!comparePath.empty())
                {
                    std::ifstream f(comparePath, std::ios::binary);
                    uint32_t header[2] = {};
                    f.read((char*)header, sizeof header);
                    if (!f || header[0] != res.width || header[1] != res.height) fail("%s: not a %ux%u reflection dump", comparePath.c_str(), res.width, res.height);
                    std::vector<uint16_t> ref(texels.size());
                    f.read((char*)ref.data(), (std::streamsize)(ref.size() * 2));
                    if (!f) fail("%s: truncated", comparePath.c_str());
                    const std::vector<double> a = tileMeans(texels, res.width, res.height, &ref), b = tileMeans(ref, res.width, res.height, &texels);
                    double imageMean = 0, sumA = 0, sumB = 0;
                    size_t valid = 0;
                    for (size_t t = 0; t < b.size(); ++t)
                        if (b[t] >= 0) imageMean += b[t], ++valid;
                    imageMean /= std::max<size_t>(valid, 1);
                    std::vector<double> rel;
                    for (size_t t = 0; t < b.size(); ++t)
                    {
                        if (b[t] < 0 || a[t] < 0 || b[t] <= 1e-3 * imageMean) continue;
                        rel.push_back(std::fabs(a[t] - b[t]) / b[t]);
                        sumA += a[t];
                        sumB += b[t];
                    }
                    std::sort(rel.begin(), rel.end());
                    double mean = 0;
                    for (double v : rel) mean += v;
                    mean /= std::max<size_t>(rel.size(), 1);
                    logf("R %s: reflection vs %s: %zu tiles compared (of %zu with valid pixels), tile mean |a - b| / b %.4f, P95 %.4f, max %.4f, "
                         "signed bias %.4f\n", res.name.c_str(), comparePath.c_str(), rel.size(), valid, mean, rel.empty() ? 0.0 : rel[rel.size() * 95 / 100],
                         rel.empty() ? 0.0 : rel.back(), sumB > 0 ? (sumA - sumB) / sumB : 0.0);
                }
            }
            if (renderer)
            {
                giSystem = gi::GiSystem::find(renderer->trackState());
                reflSystem = refl::ReflectionSystem::find(renderer->trackState());
                rayScene = rt::RayScene::find(renderer->trackState());
            }
            const double reflMs = passSum(r, "r.refl."), traceReflMs = r.passMs.count("r.refl.trace") ? r.passMs.at("r.refl.trace").median : 0;
            logf("R %s: reflections %.3f ms (trace %.3f ms, classify %.3f, resolve %.3f)\n", res.name.c_str(), reflMs, traceReflMs,
                 r.passMs.count("r.refl.classify") ? r.passMs.at("r.refl.classify").median : 0, r.passMs.count("r.refl.resolve") ? r.passMs.at("r.refl.resolve").median : 0);
            const double giMs = passSum(r, "r.gi."), asMs = passSum(r, "r.as."), traceMs = r.passMs.count("r.gi.trace") ? r.passMs.at("r.gi.trace").median : 0;
            if (renderer) logf("R %s: integrated frame %.3f ms GPU (median)\n", res.name.c_str(), r.gpuFrameMs.median);
            logf("R %s: GI %.3f ms (trace %.3f ms = %.2f G rays/s incl. hit shading), acceleration structures %.3f ms; stand-in primary visibility %.3f ms (not R)\n",
                 res.name.c_str(), giMs, traceMs, traceMs > 0 ? gi::GiSettings::fromQuality(quality).updatesPerFrame * 64 / (traceMs * 1e-3) / 1e9 : 0, asMs,
                 r.passMs.count("standin.primary") ? r.passMs.at("standin.primary").median : 0);
            if (!renderer)
                logf("R %s: M-facing probe lookups at every pixel: irradiance %.3f ms, K radiance %.3f ms, both %.3f ms, footprint alone %.3f ms (lookup-free kernel %.3f ms), screenProbeGather %.3f ms\n", res.name.c_str(),
                     r.passMs.count("bench.probe.irradiance") ? r.passMs.at("bench.probe.irradiance").median : 0,
                     r.passMs.count("bench.probe.radiance") ? r.passMs.at("bench.probe.radiance").median : 0,
                     r.passMs.count("bench.probe.both") ? r.passMs.at("bench.probe.both").median : 0,
                     r.passMs.count("bench.probe.footprint") ? r.passMs.at("bench.probe.footprint").median : 0,
                     r.passMs.count("bench.probe.none") ? r.passMs.at("bench.probe.none").median : 0,
                     r.passMs.count("bench.probe.gather") ? r.passMs.at("bench.probe.gather").median : 0);
            if (rayScene && rayScene->stats().framesRecorded > 0)
            {
                const rt::RaySceneStats& as = rayScene->stats();
                const double recorded = (double)as.framesRecorded;
                logf("R %s: over %llu frames: exact set %.2f of %u slots occupied, %.3f slot builds / frame (%llu), %.0f exact vertices deformed / frame; "
                     "proxy cut switches %.3f / frame (%llu); per frame %.2f instances wanted, %.2f hit often but within their proxy's error bound\n",
                     res.name.c_str(), (unsigned long long)as.framesRecorded, as.exactOccupiedTotal / recorded, as.exactSlots, as.exactBuildsTotal / recorded,
                     (unsigned long long)as.exactBuildsTotal, as.exactVerticesTotal / recorded, as.proxySwitchesTotal / recorded, (unsigned long long)as.proxySwitchesTotal,
                     as.exactWantedTotal / recorded, as.exactWithinBoundTotal / recorded);
                const rt::DynamicTlasCensus& dc = as.dynamicCensus;
                logf("R %s: dynamic TLAS input: %u instances, %u distinct BLAS, %u non-finite transforms, %u mask 0, %u null BLAS, max scale %.3f, translation span %.1f x %.1f x %.1f m%s",
                     res.name.c_str(), dc.instances, dc.distinctBlas, dc.nonFinite, dc.maskZero, dc.nullBlas, dc.maxScale, dc.extent[0], dc.extent[1], dc.extent[2], "\n");
                logf("R %s: last frame: proxy cuts %llu triangles refit, %llu vertices deformed; finest cut's posed error bound up to %.1f x V's bind-pose error\n",
                     res.name.c_str(), (unsigned long long)as.deformedTriangles, (unsigned long long)as.deformedVertices, as.posedErrorOverBindMax);
            }
            if (reflSystem)
            {
                const refl::ReflectionSystem::Stats rs = reflSystem->readStats();
                logf("R %s: reflection jobs %u (M %u = rays %u; G samples %u = rays %u), G pixels %u\n", res.name.c_str(), rs.jobs, rs.mirrorJobs, rs.mirrorJobs, rs.glossyJobs,
                     rs.glossyJobs * reflSystem->settings().raysPerSample, rs.glossyPixels);
                logf("R %s: planar candidates %u visible, largest plane %u mirror pixels, %u views (%u px)%s, CPU %.3f ms\n", res.name.c_str(),
                     rs.planarCandidates, rs.planarLargestPixels, rs.planarViews, rs.planarPixels,
                     renderer ? "" : " (no renderView in this gate)", rs.planarSelectMs);
                logf("R %s: planar cost choice: rays %.3f ns/ray (measured), views %.3f ms + %.3f ns per mirror pixel (%s), last views %.3f ms over %u rectangle px\n",
                     res.name.c_str(), rs.rayNs, rs.viewFixedMs, rs.viewNsPerPixel, rs.planarViews || rs.viewMs > 0 ? "fit of measured views" : "prior", rs.viewMs,
                     rs.planarRectPixels);
            }
            if (giSystem)
            {
                const gi::GiStats st = giSystem->readStats();
                logf("R %s: cache %u live, %u requested, %u selected + %u background updates, %u hit entries, %u created, %u allocation failures, %u table overflows\n",
                     res.name.c_str(), st.live, st.requested, st.selected, st.background, st.hits, st.created, st.allocationFailures, st.tableFull);
                logf("R %s: reflection hits' cache lookups %u, with no data at any level searched %u (%.3f %%)\n", res.name.c_str(), st.hitLookups, st.hitMisses,
                     st.hitLookups ? 100.0 * st.hitMisses / st.hitLookups : 0.0);
                logf("R %s: reflection G samples %u, estimated by the ratio branch (beta < 1) %u (%.2f %%)\n", res.name.c_str(), st.gSamples, st.gRatio,
                     st.gSamples ? 100.0 * st.gRatio / st.gSamples : 0.0);
                logf("R %s: G samples by log2(mean L / mean g): <-3 %u, -3 %u, -2 %u, -1 %u, 0 %u, 1 %u, 2 %u, 3 %u, >=4 %u; mean g = 0 %u\n", res.name.c_str(),
                     st.gHistogram[0], st.gHistogram[1], st.gHistogram[2], st.gHistogram[3], st.gHistogram[4], st.gHistogram[5], st.gHistogram[6],
                     st.gHistogram[7], st.gHistogram[8], st.gZero);
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
