// V performance gate (ARCHITECTURE 2.1 cost formula, 7.2 P1 gate terms): renders one of C's procedural scenes
// through V only (other tracks are empty stubs in a core + V + C build, so the frame is V's passes) and compares each
// V term with the design formula evaluated on the measured quantities:
//   cull (instances, hierarchy, clusters) + HiZ     design: I x 32 B / 600 GB/s + C x 64 B / 600 GB/s x 1.5 + 0.02 ms
//   band A raster                                    design: M x 0.87 ns + T_A / 17 G/s
// --service adds the depth raster service in a VSM page-raster setting (S's clipmap: 12 orthographic sun views of
// 16384^2, level k texel 2^(k-10) m, centred on the camera; the camera's page dirty in levels 0-5; pixel kernel
// ServicePagePixel), drawn per frame as whole-view raster, tile-local raster (DepthRasterRequest::tileLocal) or both
// (--service whole|local|both; separate runs keep one request's tail out of the other's first pass).
// With --set visibility.coverage_layer=true the coverage layer's passes are reported against the design formula
// (ARCHITECTURE 2.1: F_cov x 0.05 ns raster + sort F_cov x 16 B x 2 / 600 GB/s) with the fragments-per-pixel
// histogram of one extra frame. --cluster-stats FILE writes every cluster's width, size and LOD errors (CSV) and a
// per-mesh summary of the band distances at the resolution (band C design input).
// Performance runs only under the GPU lock (INTERFACES 3.3):
//   powershell -File Tools/CI/GpuLock.ps1 -Track core -- build/<t>/bin/unx_gate_visibility_visibilitygate.exe
//       --scene city_block|forest_thin|... [--resolution 4K|1440p|both] [--frames 600] [--scale 1] [--moving] [--service whole|local|both]
//       [--out DIR] [--set key=value ...] [--cluster-stats FILE]
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/render/Tracks.h"
#include "unx/visibility/Visibility.h"
#if UNX_HAS_SCENEGEN
#include "unx/scenegen/SceneGen.h"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
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

// Persistent raw buffer with its own bindless descriptor (the service's pixel constants are fixed at request time).
struct RawBuffer
{
    ComPtr<ID3D12Resource> resource;
    uint32_t descriptor = gpu::kNone;
    uint64_t bytes = 0;
};

RawBuffer rawBuffer(Device& device, uint64_t bytes, bool uav, const wchar_t* name)
{
    RawBuffer b;
    b.bytes = bytes;
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&b.resource)),
          "gate buffer");
    b.resource->SetName(name);
    b.descriptor = device.descriptors().allocateResource();
    if (uav)
    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = (UINT)(bytes / 4);
        ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device.d3d()->CreateUnorderedAccessView(b.resource.Get(), nullptr, &ud, device.descriptors().resourceCpu(b.descriptor));
    }
    else
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = (UINT)(bytes / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device.d3d()->CreateShaderResourceView(b.resource.Get(), &sd, device.descriptors().resourceCpu(b.descriptor));
    }
    return b;
}

// Every cluster's band quantities (CSV) and a per-mesh summary: minimum feature width, bounding radius, LOD errors, and
// at focal length 'focal' (pixels at distance 1) the distances where the width projects to 1.5 px (band A limit) and
// 0.25 px (band C limit), and the cluster's projected diameter there (the brick resolution the cluster would need).
void writeClusterStats(const scene::Scene& s, const ClusterData& cd, float focal, const std::string& path)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "w") != 0 || !f) fail("cannot write %s", path.c_str());
    fprintf(f, "mesh,cluster,triangles,min_feature_width_m,sheet,radius_m,lod_error_m,parent_lod_error_m,d_band_a_m,d_band_c_m,diameter_px_at_band_c\n");
    std::vector<uint32_t> instancesOf(s.meshes.size(), 0);
    for (const auto& inst : s.instances) ++instancesOf[inst.mesh];
    struct MeshSum
    {
        uint32_t clusters = 0, source = 0;
        std::vector<float> width, diameterC;
    };
    std::vector<MeshSum> sums(s.meshes.size());
    for (uint32_t m = 0; m < cd.meshes.size() && m < s.meshes.size(); ++m)
    {
        const auto& range = cd.meshes[m];
        for (uint32_t c = range.clusterOffset; c < range.clusterOffset + range.clusterCount; ++c)
        {
            const gpu::Cluster& cl = cd.clusters[c];
            const float w = std::fabs(cl.minFeatureWidth), r = cl.boundsSphere.w;
            const float dA = w > 0 ? focal * w / 1.5f : 0, dC = w > 0 ? focal * w / 0.25f : 0;
            const float diamC = dC > 0 ? 2 * r * focal / dC : 0;
            fprintf(f, "%u,%u,%u,%.6g,%d,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g\n", m, c, (cl.counts >> 8) & 0xFFu, w, cl.minFeatureWidth < 0 ? 1 : 0, r, cl.lodError,
                    cl.parentLodError > 1e30f ? -1.0f : cl.parentLodError, dA, dC, diamC);
            MeshSum& ms = sums[m];
            ++ms.clusters;
            if (cl.lodError == 0) ++ms.source;
            if (w > 0)
            {
                ms.width.push_back(w);
                ms.diameterC.push_back(diamC);
            }
        }
    }
    fclose(f);
    auto pct = [](std::vector<float> v, float q) {
        if (v.empty()) return 0.0f;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, (size_t)(q * (v.size() - 1) + 0.5f))];
    };
    logf("cluster statistics -> %s (focal %.0f px): per mesh: instances, clusters (source), min feature width median / P10 (m), band A / C distance "
         "at the median width (m), cluster diameter at the band C distance median / P90 (px)\n",
         path.c_str(), focal);
    for (uint32_t m = 0; m < sums.size(); ++m)
    {
        const MeshSum& ms = sums[m];
        if (ms.clusters == 0) continue;
        const float wMed = pct(ms.width, 0.5f);
        logf("  mesh %u '%s': %u instances, %u clusters (%u source), width %.4g / %.4g, d_A %.1f, d_C %.1f, diameter %.1f / %.1f\n", m, s.meshes[m].name.c_str(),
             instancesOf[m], ms.clusters, ms.source, wMed, pct(ms.width, 0.1f), focal * wMed / 1.5f, focal * wMed / 0.25f, pct(ms.diameterC, 0.5f),
             pct(ms.diameterC, 0.9f));
    }
}

// S's clipmap geometry (VsmSystem.cpp): light basis z towards the sun, x horizontal; level k texel 2^(k-10) m over
// 16384 texels, window origin in 128-texel pages with the camera's page at tile (64, 64); height range = casters'
// bounding spheres along z +- 500 m.
constexpr uint32_t kServiceLevels = 12, kServiceVirtual = 16384, kServicePage = 128, kServiceTable = 128;

std::vector<RasterView> serviceViews(const GpuScene& scene, float3 sun, float3 camera)
{
    const float3 z = normalize(sun);
    const float3 up = std::abs(z.y) < 0.999f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    const float3 x = normalize(cross(up, z)), y = cross(z, x);
    float lo = 1e30f, hi = -1e30f;
    for (const gpu::Instance& i : scene.instances())
    {
        if ((i.flags & scene::InstanceCastShadow) == 0) continue;
        const gpu::Mesh& m = scene.meshes()[i.mesh];
        const float3 c{ m.boundsSphere.x, m.boundsSphere.y, m.boundsSphere.z };
        const float3 w{ i.objectToWorld[0].x * c.x + i.objectToWorld[0].y * c.y + i.objectToWorld[0].z * c.z + i.objectToWorld[0].w,
                        i.objectToWorld[1].x * c.x + i.objectToWorld[1].y * c.y + i.objectToWorld[1].z * c.z + i.objectToWorld[1].w,
                        i.objectToWorld[2].x * c.x + i.objectToWorld[2].y * c.y + i.objectToWorld[2].z * c.z + i.objectToWorld[2].w };
        const float r = m.boundsSphere.w * length(float3{ i.objectToWorld[0].x, i.objectToWorld[0].y, i.objectToWorld[0].z });
        lo = std::min(lo, dot(w, z) - r);
        hi = std::max(hi, dot(w, z) + r);
    }
    const float hMin = lo - 500, hMax = hi + 500, range = hMax - hMin, V = (float)kServiceVirtual;
    std::vector<RasterView> views;
    for (uint32_t k = 0; k < kServiceLevels; ++k)
    {
        const float t = std::ldexp(1.0f, (int)k - 10), pageSize = t * kServicePage;
        const float ox = (float)((int32_t)std::floor(dot(camera, x) / pageSize) - (int32_t)kServiceTable / 2) * kServicePage;
        const float oy = (float)((int32_t)std::floor(dot(camera, y) / pageSize) - (int32_t)kServiceTable / 2) * kServicePage;
        RasterView v;
        float4x4& m = v.viewProj;
        m.m[0][0] = 2 * x.x / (t * V); m.m[0][1] = 2 * x.y / (t * V); m.m[0][2] = 2 * x.z / (t * V); m.m[0][3] = -2 * ox / V - 1;
        m.m[1][0] = -2 * y.x / (t * V); m.m[1][1] = -2 * y.y / (t * V); m.m[1][2] = -2 * y.z / (t * V); m.m[1][3] = 1 + 2 * oy / V;
        m.m[2][0] = -z.x / range; m.m[2][1] = -z.y / range; m.m[2][2] = -z.z / range; m.m[2][3] = hMax / range;
        m.m[3][0] = 0; m.m[3][1] = 0; m.m[3][2] = 0; m.m[3][3] = 1;
        v.viewportWidth = v.viewportHeight = kServiceVirtual;
        v.lodPixelsPerMetre = 1.0f / t;
        v.userData = k;
        v.cullMaskOffset = k * (kServiceTable * kServiceTable / 32);
        views.push_back(v);
    }
    return views;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string sceneName = "city_block", resolutionArg = "both", out, clusterStats;
        std::vector<std::string> overrides;
        uint32_t frames = 600;
        float scale = 1.0f;
        bool moving = false, service = false;
        std::string serviceMode;
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
            else if (a == "--service")
            {
                serviceMode = next();
                service = true;
                if (serviceMode != "whole" && serviceMode != "local" && serviceMode != "both") fail("--service whole|local|both");
            }
            else if (a == "--out") out = next();
            else if (a == "--set")
            {
                // key=value in TOML syntax; a bare word (no quotes survive nested shells) is taken as a string.
                std::string o = next();
                const size_t eq = o.find('=');
                if (eq != std::string::npos && eq + 1 < o.size() && std::isalpha((unsigned char)o[eq + 1]) && o.substr(eq + 1) != "true" && o.substr(eq + 1) != "false")
                    o = o.substr(0, eq + 1) + "\"" + o.substr(eq + 1) + "\"";
                overrides.push_back(o);
            }
            else if (a == "--cluster-stats") clusterStats = next();
            else fail("unknown argument %s", a.c_str());
        }
        requireGpuLock("unx_gate_visibility_visibilitygate");
#if !UNX_HAS_SCENEGEN
        fail("this build has no scene generator: build with track C enabled (Build.ps1 -Tracks \"V;C\" or -Track all)");
#else
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const std::string& o : overrides) quality.applyOverride(o);
        const bool coverage = quality.boolean("visibility.coverage_layer");
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
        if (!clusterStats.empty())
        {
            const Resolution first = resolutionFromString(resolutionArg == "both" ? "4K" : resolutionArg, quality);
            const scene::Camera cam = cameraAt(s, false, 0);
            writeClusterStats(s, clusters, 0.5f * first.height / std::tan(0.5f * cam.verticalFov), clusterStats);
        }

        Device device({});
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene gpuScene(device);
        gpuScene.upload(s);
        gpuScene.setClusters(std::move(clusters));
        Harness harness(device, quality);
        // Service measurement resources: tile mask (12 views x 128^2 tiles; committed buffers start zeroed) and one page.
        const RawBuffer serviceMask = rawBuffer(device, (uint64_t)kServiceLevels * kServiceTable * kServiceTable / 8, false, L"gate service mask");
        const RawBuffer servicePage = rawBuffer(device, (uint64_t)kServicePage * kServicePage * 4, true, L"gate service page");
        FrameResources serviceResources;
        FrameServices serviceServices;
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
                if (!service) return;
                // The camera's page (tile 64, 64) dirty in levels 0-5: six 128^2 pages, as S measured (5.5 per frame).
                const BufferRef mask = g.importBuffer(serviceMask.resource.Get(), { "gate.service.mask", serviceMask.bytes, 0 });
                const BufferRef page = g.importBuffer(servicePage.resource.Get(), { "gate.service.page", servicePage.bytes, 0 });
                g.addPass("gate.service.mask", QueueType::Graphics, [&](PassBuilder& b) { b.use(mask, Use::CopyDst); },
                          [=](PassContext& c) {
                              D3D12_WRITEBUFFERIMMEDIATE_PARAMETER words[6];
                              const uint32_t bit = 64 * kServiceTable + 64;
                              for (uint32_t k = 0; k < 6; ++k)
                                  words[k] = { c.address(mask) + 4 * (k * (kServiceTable * kServiceTable / 32) + bit / 32), 1u << (bit & 31) };
                              c.cmd->WriteBufferImmediate(6, words, nullptr);
                          });
                FramePassContext sc{ device, g, shaders, quality, gpuScene, fc, serviceResources, serviceServices,
                                     [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { fail("gate: no frame constants for service views"); }, &renderer.trackState(), 2 };
                const float3 sun = s.sun.direction;
                DepthRasterRequest req;
                req.views = serviceViews(gpuScene, sun, fc.mainView.position);
                req.pixelKernel = "Passes/Visibility/Gates/ServicePagePixel";
                req.bufferUses = { { mask, Use::SrvGraphics }, { page, Use::UavGraphics } };
                req.pixelConstants[0] = serviceMask.descriptor;
                req.pixelConstants[1] = servicePage.descriptor;
                req.cullMask = mask;
                req.cullTilePx = kServicePage;
                for (const bool local : { false, true })
                {
                    if (serviceMode != "both" && local != (serviceMode == "local")) continue;
                    req.name = local ? "svc.local" : "svc.whole";
                    req.tileLocal = local;
                    tracks::rasterizeDepth(sc, req);
                }
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
            if (coverage)
            {
                const double covRaster = sumPasses(r, [](const std::string& n) { return n == "v.coverage.raster"; });
                const double covBuild = sumPasses(r, [](const std::string& n) { return n == "v.coverage.build"; });
                const double covOther = sumPasses(r, [](const std::string& n) { return startsWith(n, "v.coverage.") && n != "v.coverage.raster" && n != "v.coverage.build"; });
                const double F = st.coverageFragments;
                logf("  coverage layer: %u fragments in %u pixels (%.2f per pixel) | raster %.3f ms (%.3f ns/fragment; design F x 0.05 ns = %.3f ms) | build (walk, "
                     "sort, write) %.3f ms (design F x 16 B x 2 / 600 GB/s = %.3f ms) | clear/prepare/args %.3f ms\n",
                     st.coverageFragments, st.coveragePixels, st.coveragePixels ? F / st.coveragePixels : 0.0, covRaster, F > 0 ? covRaster * 1e6 / F : 0.0,
                     F * 0.05e-6, covBuild, F * 32.0 / 600e9 * 1e3, covOther);
                // Fragments-per-pixel histogram of one more frame (heads = first | count << 24).
                RenderGraph g(device);
                FrameContext fc;
                fc.frameIndex = frames + 1000;
                fc.time = fc.frameIndex / 60.0;
                fc.deltaTime = 1.0f / 60;
                fc.mainView = ViewDesc::fromCamera(cameraAt(s, moving, fc.time), res.width, res.height, prev);
                const TextureRef output = g.createTexture({ "gate output", res.width, res.height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
                const ViewResources main = renderer.record(g, fc, output);
                const uint32_t pitch = (res.width * 4 + 255) / 256 * 256;
                ComPtr<ID3D12Resource> rb;
                {
                    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                    D3D12_RESOURCE_DESC1 rd{};
                    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    rd.Width = (uint64_t)pitch * res.height;
                    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
                    rd.SampleDesc.Count = 1;
                    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)),
                          "gate heads readback");
                }
                ID3D12Resource* dst = rb.Get();
                const uint32_t w = res.width, h = res.height;
                g.addPass("gate.heads.readback", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(main.coverageHeads, Use::CopySrc);
                              b.keep();
                          },
                          [=](PassContext& c) {
                              D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                              to.pResource = dst;
                              to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                              to.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, w, h, 1, pitch };
                              from.pResource = c.resource(main.coverageHeads);
                              from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                              c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                          });
                g.execute(nullptr);
                device.waitIdle();
                const uint8_t* p = nullptr;
                check(rb->Map(0, nullptr, (void**)&p), "map heads");
                const uint32_t edges[] = { 1, 2, 3, 4, 6, 8, 12, 16, 32, 64, 128, 256 };
                uint64_t bins[std::size(edges)] = {}, pixels = 0, fragments = 0, over8 = 0, fragmentsOver8 = 0;
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                    {
                        uint32_t v;
                        std::memcpy(&v, p + (size_t)y * pitch + 4 * x, 4);
                        const uint32_t n = v >> 24;
                        if (n == 0) continue;
                        ++pixels;
                        fragments += n;
                        if (n > 8)
                        {
                            ++over8;
                            fragmentsOver8 += n;
                        }
                        size_t k = 0;
                        while (k + 1 < std::size(edges) && n >= edges[k + 1]) ++k;
                        ++bins[k];
                    }
                rb->Unmap(0, nullptr);
                std::string hist;
                for (size_t k = 0; k < std::size(edges); ++k)
                {
                    char b[64];
                    const uint32_t lo = edges[k], hi = k + 1 < std::size(edges) ? edges[k + 1] - 1 : 255;
                    if (lo == hi) snprintf(b, sizeof b, "%s%u: %.2f%%", k ? ", " : "", lo, pixels ? 100.0 * bins[k] / pixels : 0.0);
                    else snprintf(b, sizeof b, "%s%u-%u: %.2f%%", k ? ", " : "", lo, hi, pixels ? 100.0 * bins[k] / pixels : 0.0);
                    hist += b;
                }
                logf("  fragments per pixel (one frame, %llu pixels, %llu fragments): %s | pixels over 8: %.2f%% holding %.1f%% of the fragments\n",
                     (unsigned long long)pixels, (unsigned long long)fragments, hist.c_str(), pixels ? 100.0 * over8 / pixels : 0.0,
                     fragments ? 100.0 * fragmentsOver8 / fragments : 0.0);
                device.deferRelease(rb);
            }
            if (service)
                for (const char* run : { "svc.whole", "svc.local" })
                {
                    if (serviceMode != "both" && serviceMode != std::string(run).substr(4)) continue;
                    const std::string p = std::string(run) + ".";
                    const double rasterMs = sumPasses(r, [&](const std::string& n) { return n == p + "raster"; });
                    const double cullMs = sumPasses(r, [&](const std::string& n) { return startsWith(n, p.c_str()) && n != p + "raster" && n != p + "stats"; });
                    const visibility::Stats ss = visibility::latestStats(renderer.trackState(), run);
                    logf("  service %s: raster %.3f ms, cull %.3f ms | %u clusters visible, %u triangles, %u tile pairs, overflow 0x%x\n", run, rasterMs, cullMs,
                         ss.visibleClusters, ss.triangles[0] + ss.triangles[1] + ss.triangles[2], ss.tilePairs, ss.overflow);
                    if (ss.overflow) status = 1;
                }
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
