// S performance gate through the real renderer (FrameRenderer: V's cluster pipeline and depth raster service, M's
// material resolve, S; other tracks as built): per-pass GPU time of every S pass at 4K / 1440p on a C-track scene, with
// a static or a moving camera (the scene's first camera path), and the quantities the design's cost formulas use:
// dirty pages and the triangles V rasterised into them (T_sun), page requests and pool use, visibility paths.
// Needs a build with tracks V, M, S and C (Build.ps1 -Track S -Tracks "V;M;S;C", or -Track all). GPU lock required:
//   powershell -File Tools/CI/GpuLock.ps1 -Track S -- build/S/bin/unx_gate_shadow_renderergate.exe
//       --scene city_block|forest_thin|... [--resolution 4K|1440p|both] [--frames 600] [--moving] [--sun-deg-per-s R]
//       [--wind-gust-period-s T] [--capture FILE.pfm] [--out DIR] [--set k=v]
// --capture: the main view's linear scene radiance (FrameContext::outputLinearHdr, no exposure) of the last frame as
// a PFM for unx_reference compare (one resolution; the frames still render as measured, plus one copy each).
#if __has_include("unx/clusterbuilder/ClusterBuilder.h") && defined(UNX_HAS_SCENEGEN)
#define S_RENDERER_GATE 1
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/scenegen/SceneGen.h"
#include "unx/visibility/Visibility.h"
#endif
#include "FroxelSystem.h"
#include "VsmSystem.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

#if S_RENDERER_GATE
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
} // namespace
#endif

int main(int argc, char** argv)
{
    int gateFailures = 0;
    try
    {
#if !S_RENDERER_GATE
        (void)argc;
        (void)gateFailures;
        (void)argv;
        fail("this build lacks V's cluster builder or C's scene generator: Build.ps1 -Track S -Tracks \"V;M;S;C\" (or -Track all)");
#else
        std::string sceneName = "city_block", resolutionArg = "both", out;
        uint32_t frames = 600;
        bool moving = false;
        float sunDegPerS = 0;  // moving sun (time of day): the sun turns about the horizontal axis normal to it
        std::string capturePath;  // --capture: last frame's linear radiance as PFM
        float gustPeriodS = 0;  // wind change after commit (v1.23): every gustPeriodS the source scene's wind alternates
                                // between the scene's and +30 % speed / +20 degrees (no reload; the host's path)
        std::vector<std::string> overrides;
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
            else if (a == "--moving") moving = true;
            else if (a == "--sun-deg-per-s") sunDegPerS = std::stof(next());
            else if (a == "--wind-gust-period-s") gustPeriodS = std::stof(next());
            else if (a == "--capture") capturePath = next();
            else if (a == "--out") out = next();
            else if (a == "--set") overrides.push_back(next());
            else fail("unknown argument %s", a.c_str());
        }
        requireGpuLock("unx_gate_shadow_renderergate");
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
        scene::Scene s = scenegen::generate(request);  // not const: --sun-deg-per-s turns its sun (GpuScene keeps &s)
        const float3 sun0 = normalize(s.sun.direction);
        const float3 sunAxis = normalize(cross(sun0, float3{ 0, 1, 0 }));
        const float wind0 = s.windSpeed;
        const float3 windDir0 = s.windDirection;
        ClusterData clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality));
        logf("scene %s (%s), %zu instances, %zu clusters, camera %s\n", sceneName.c_str(), scene::contentHash(s).substr(0, 16).c_str(), s.instances.size(),
             clusters.clusters.size(), moving ? "path 0 (moving)" : "0 (static)");
        Device device({});
        ComPtr<ID3D12Resource> captureBuffer;  // --capture: readback of the gate output (last frame wins)
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFootprint{};
        float captureEv100 = 0;  // the captured frame's exposure (the output is radiance x exposure)
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
            shadow::setKeepFroxels(renderer.trackState(), true);  // no consumer of the volume yet (M): measure it anyway
            HarnessOptions options;
            options.frames = frames;
            options.label = "S " + sceneName + (moving ? " moving " : " static ") + (sunDegPerS != 0 ? "sun " + std::to_string(sunDegPerS) + " deg/s " : "") + (gustPeriodS > 0 ? "gusts " : "") + rs;
            if (!out.empty()) options.outputDirectory = out;
            float4x4 prev = ViewDesc::fromCamera(cameraAt(s, moving, 0), res.width, res.height, {}).viewProj;
            // Dirty pages and T_sun averaged over the measured frames (the counters lag the frame by two).
            double dirtySum = 0, trianglesSum = 0, requestedSum = 0;
            uint32_t samples = 0, exhausted = 0, requestedMax = 0, overTiles = 0, overflowWordsMax = 0, overflowLightsMax = 0;
            uint64_t lastStatsFrame = 0;
            const HarnessResult r = harness.run(res, options, [&](RenderGraph& g, const Resolution& rr, uint64_t frame) {
                FrameContext fc;
                fc.frameIndex = frame;
                fc.time = frame / 60.0;
                fc.deltaTime = 1.0f / 60;
                fc.mainView = ViewDesc::fromCamera(cameraAt(s, moving, fc.time), rr.width, rr.height, prev);
                if (gustPeriodS > 0)
                {
                    const bool gust = ((uint64_t)(fc.time / gustPeriodS) & 1) != 0;
                    const float a = gust ? 20.0f * 0.01745329f : 0.0f;
                    s.windSpeed = gust ? wind0 * 1.3f : wind0;
                    s.windDirection = float3{ windDir0.x * std::cos(a) - windDir0.z * std::sin(a), windDir0.y, windDir0.x * std::sin(a) + windDir0.z * std::cos(a) };
                }
                if (sunDegPerS != 0)
                {
                    // Rodrigues rotation of the initial sun direction (the axis is normal to it).
                    const float a = sunDegPerS * 0.01745329252f * (float)fc.time, ca = std::cos(a), sa = std::sin(a);
                    s.sun.direction = normalize(sun0 * ca + cross(sunAxis, sun0) * sa);
                }
                prev = fc.mainView.viewProj;
                fc.outputLinearHdr = !capturePath.empty();
                captureEv100 = fc.mainView.ev100;
                const TextureRef output = g.createTexture({ "gate output", rr.width, rr.height, 1, 1,
                                                            capturePath.empty() ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R32G32B32A32_FLOAT });
                renderer.record(g, fc, output);
                if (!capturePath.empty())
                {
                    if (!captureBuffer)
                    {
                        D3D12_RESOURCE_DESC td{};
                        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                        td.Width = rr.width;
                        td.Height = rr.height;
                        td.DepthOrArraySize = 1;
                        td.MipLevels = 1;
                        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                        td.SampleDesc.Count = 1;
                        UINT rows;
                        UINT64 rowBytes, total;
                        device.d3d()->GetCopyableFootprints(&td, 0, 1, 0, &captureFootprint, &rows, &rowBytes, &total);
                        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                        D3D12_RESOURCE_DESC bd{};
                        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                        bd.Width = total;
                        bd.Height = 1;
                        bd.DepthOrArraySize = 1;
                        bd.MipLevels = 1;
                        bd.SampleDesc.Count = 1;
                        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                        check(device.d3d()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                    IID_PPV_ARGS(&captureBuffer)),
                              "capture readback");
                    }
                    ID3D12Resource* rb = captureBuffer.Get();
                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = captureFootprint;
                    g.addPass("s.gate.capture", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  b.use(output, Use::CopySrc);
                                  b.keep();
                              },
                              [=](PassContext& ctx) {
                                  D3D12_TEXTURE_COPY_LOCATION dst{ rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                                  dst.PlacedFootprint = fp;
                                  D3D12_TEXTURE_COPY_LOCATION src{ ctx.resource(output), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                                  src.SubresourceIndex = 0;
                                  ctx.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                              });
                }
                const shadow::VsmStats& st = shadow::stats(renderer.trackState());
                if (frame > 8 && st.frame != lastStatsFrame)
                {
                    lastStatsFrame = st.frame;
                    const visibility::Stats vs = visibility::latestStats(renderer.trackState(), "s.vsm.raster");
                    dirtySum += st.dirty;
                    trianglesSum += (double)vs.triangles[0] + vs.triangles[1] + vs.triangles[2];
                    requestedSum += st.requested;
                    requestedMax = std::max(requestedMax, st.requested);
                    exhausted += st.exhausted;
                    overTiles += st.overflowOverTiles;
                    overflowWordsMax = std::max(overflowWordsMax, st.overflowWords);
                    overflowLightsMax = std::max(overflowLightsMax, st.overflowLights);
                    ++samples;
                }
            });
            harness.printSummary(r);
            if (!capturePath.empty() && captureBuffer)
            {
                // The harness waits for the GPU at its end: the readback holds the last frame. PFM: "PF", width height,
                // -1 (little endian), rows bottom to top, RGB float.
                void* mapped = nullptr;
                check(captureBuffer->Map(0, nullptr, &mapped), "map capture");
                std::string header = "PF\n" + std::to_string(res.width) + " " + std::to_string(res.height) + "\n-1.0\n";
                std::vector<float> rgb((size_t)res.width * res.height * 3);
                // The linear-HDR output is scene radiance already (measured against C's reference: the exposure is not in
                // it, although Frame.h's comment says 'x exposure'); written as it is.
                const float toRadiance = 1.0f;
                (void)captureEv100;
                for (uint32_t y = 0; y < res.height; ++y)
                {
                    const float* row = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped) + captureFootprint.Offset + (size_t)y * captureFootprint.Footprint.RowPitch);
                    float* dstRow = &rgb[(size_t)(res.height - 1 - y) * res.width * 3];
                    for (uint32_t x = 0; x < res.width; ++x)
                        for (int c = 0; c < 3; ++c) dstRow[x * 3 + c] = row[x * 4 + c] * toRadiance;
                }
                D3D12_RANGE none{ 0, 0 };
                captureBuffer->Unmap(0, &none);
                std::ofstream file(capturePath, std::ios::binary);
                if (!file) fail("cannot write %s", capturePath.c_str());
                file.write(header.data(), (std::streamsize)header.size());
                file.write(reinterpret_cast<const char*>(rgb.data()), (std::streamsize)(rgb.size() * sizeof(float)));
                logf("captured %ux%u linear radiance -> %s\n", res.width, res.height, capturePath.c_str());
                captureBuffer.Reset();
            }
            const shadow::VsmStats& st = shadow::stats(renderer.trackState());
            double sPasses = 0, raster = 0;
            for (const auto& [name, d] : r.passMs)
                if (name.rfind("s.", 0) == 0)
                {
                    logf("  %-32s median %.4f ms  P95 %.4f ms\n", name.c_str(), d.median, d.p95);
                    sPasses += d.median;
                    if (name.rfind("s.vsm.raster", 0) == 0) raster += d.median;
                }
            const double n = std::max(samples, 1u);
            logf("%s: S passes %.4f ms (page raster %.4f ms) of GPU frame %.4f ms | pages requested mean %.0f max %u, dirty mean %.1f, T_sun mean %.2f M, pool exhausted %u\n",
                 rs.c_str(), sPasses, raster, r.gpuFrameMs.median, requestedSum / n, requestedMax, dirtySum / n, trianglesSum / n / 1e6, exhausted);
            const shadow::FroxelStats& fs = shadow::froxelStats(renderer.trackState());
            logf("  froxels: %u light entries (%.2f per froxel), %u truncated lists, %u lights dropped, max %u per froxel\n", fs.indexCount,
                 (double)fs.indexCount / std::max(1.0, (double)shadow::froxelGridFor(quality, res.width, res.height).gridX *
                                                       shadow::froxelGridFor(quality, res.width, res.height).gridY *
                                                       shadow::froxelGridFor(quality, res.width, res.height).slices),
                 fs.overflowLists, fs.droppedLights, fs.maxCount);
            if (quality.integer("atmosphere.froxels.walk_stats") != 0)
                logf("  air shadow walk (last frame): %u slices walked, %u with a mixed page (%.1f %%), block loads 32: %u, 8: %u, texel loads %u (%.1f per mixed slice)\n",
                     st.airSlices, st.airSlicesMixed, 100.0 * st.airSlicesMixed / std::max(st.airSlices, 1u), st.airBlocks32, st.airBlocks8, st.airTexels,
                     (double)st.airTexels / std::max(st.airSlicesMixed, 1u));
            {
                std::string pagesLine;
                for (uint32_t k = 0; k < shadow::kLevels; ++k) pagesLine += format(" L%u:%u", k, st.levelPages[k]);
                logf("  requested sun pages by level (last frame):%s\n", pagesLine.c_str());
                if (quality.integer("shadow.vsm.subtile_stats") != 0)
                    logf("  sampled 32^2 sub-tiles %u of %u in pixel-requested pages (%.1f %%)\n", st.sampledSubtiles, st.pixelRequested * 16,
                         100.0 * st.sampledSubtiles / std::max(st.pixelRequested * 16, 1u));
            }
            // Overflow list (INTERFACES 7.3): the gate requires no tile over the capacity in the measured frames.
            logf("  shadow overflow: lights past the third max %u, words needed max %u, tiles over capacity %u %s\n", overflowLightsMax, overflowWordsMax, overTiles,
                 overTiles ? "FAIL" : "ok");
            if (overTiles) ++gateFailures;
            const double px = st.pathNoCaster + st.pathRegionLit + st.pathRegionUmbra + st.pathSearchLit + st.pathFiltered + st.pathDiskLit + st.pathDiskUmbra;
            logf("  visibility paths (%% of %.2f M pixels): no caster %.1f, reach lit %.1f, reach umbra %.1f, search lit %.1f, disk lit %.1f, disk umbra %.1f, filtered %.1f\n",
                 px / 1e6, 100 * st.pathNoCaster / px, 100 * st.pathRegionLit / px, 100 * st.pathRegionUmbra / px, 100 * st.pathSearchLit / px, 100 * st.pathDiskLit / px,
                 100 * st.pathDiskUmbra / px, 100 * st.pathFiltered / px);
        }
        return gateFailures ? 1 : 0;
#endif
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
