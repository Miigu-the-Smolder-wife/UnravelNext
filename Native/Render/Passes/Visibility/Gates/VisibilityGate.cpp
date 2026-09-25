// V performance gate (ARCHITECTURE 2.1 cost formula, 7.2 P1 gate terms): renders one of C's procedural scenes
// through V only (other tracks are empty stubs in a core + V + C build, so the frame is V's passes) and compares each
// V term with the design formula evaluated on the measured quantities:
//   cull (instances, hierarchy, clusters) + HiZ     design: I x 32 B / 600 GB/s + C x 64 B / 600 GB/s x 1.5 + 0.02 ms
//   band A raster                                    design: M x 0.87 ns + T_A / 17 G/s
// --service adds the depth raster service in a VSM page-raster setting (S's clipmap: 12 orthographic sun views of
// 16384^2, level k texel 2^(k-10) m, centred on the camera; the camera's page dirty in levels 0-5; pixel kernel
// ServicePagePixel), drawn per frame as whole-view raster, tile-local raster (DepthRasterRequest::tileLocal), or depth
// only into a tile atlas (DepthRasterRequest::atlasSlots, D16 or D32, 64 or 128 slots of 128 px per row) -- --service takes a
// comma-separated list of whole, local, atlas16, atlas32 (or both = whole,local); each is its own request, so one
// request's tail stays out of the next one's first pass. --service-pages picks the requested pages: camera (the
// camera's page in levels 0-5, S's static-sun measurement) or ring (synthetic receiver-driven request of a moving sun:
// in level k the pages whose ground point lies in the camera frustum at distance [d_k, 2 d_k), d_k = texel / pixel
// angle; level 0 from the camera, level 11 to its window edge; about S's 3,744 pages at 4K).
// With --set visibility.coverage_layer=true the coverage layer's passes are reported against the design formula
// (ARCHITECTURE 2.1: F_cov x 0.05 ns raster + sort F_cov x 16 B x 2 / 600 GB/s) with the fragments-per-pixel
// histogram of one extra frame. --cluster-stats FILE writes every cluster's width, size and LOD errors (CSV) and a
// per-mesh summary of the band distances at the resolution (band C design input).
// Performance runs only under the GPU lock (INTERFACES 3.3):
//   powershell -File Tools/CI/GpuLock.ps1 -Track core -- build/<t>/bin/unx_gate_visibility_visibilitygate.exe
//       --scene city_block|forest_thin|...|deep_tile [--camera NAME|INDEX] [--deep-tile-cards N] [--resolution 4K|1440p|both] [--frames 600] [--scale 1] [--moving]
//       [--service whole,local,atlas16,atlas32] [--service-pages camera|ring] [--service-levels 12] [--service-spacing 2]
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
#include <bit>
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

// Coverage layer growth curve (INTERFACES 3.6: diagnose superlinear work at sub-TDR sizes): 'cards' horizontal cards,
// each about 11 x 0.5 px at 4K, stacked in depth over one 8 x 8 tile in the middle of the view (camera at the origin
// looking along +z), so one tile receives about 12 fragments per card (two triangles over 6 pixels).
scene::Scene deepTileScene(uint32_t cards)
{
    scene::Scene s;
    s.name = "deep_tile";
    s.materials.resize(1);
    scene::Mesh m;
    m.name = "card stack";
    const float focal = 1080.0f / std::tan(0.5235988f);  // 4K height 2160, 60 degree vertical fov: pixels per unit at z = 1
    for (uint32_t k = 0; k < cards; ++k)
    {
        const float z = 2.0f + 0.0004f * k, w = 6.0f / focal * z, h = 0.3f / focal * z;
        const float x = 3.0f / focal * z, y = 0.5f / focal * z;  // inside the tile right of and above the centre
        const uint32_t b = (uint32_t)m.positions.size();
        const float3 p[4] = { { x - 0.5f * w, y - 0.5f * h, z }, { x + 0.5f * w, y - 0.5f * h, z }, { x + 0.5f * w, y + 0.5f * h, z }, { x - 0.5f * w, y + 0.5f * h, z } };
        for (int v = 0; v < 4; ++v)
        {
            m.positions.push_back(p[v]);
            m.normals.push_back({ 0, 0, -1 });
            m.uv0.push_back({ (v == 1 || v == 2) ? 1.0f : 0.0f, v >= 2 ? 1.0f : 0.0f });
        }
        m.indices.insert(m.indices.end(), { b, b + 2, b + 1, b, b + 3, b + 2 });
    }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    s.meshes.push_back(m);
    scene::Instance inst;
    inst.mesh = 0;
    s.instances.push_back(inst);
    scene::Camera c;
    c.name = "main";
    c.forward = { 0, 0, 1 };
    c.nearPlane = 0.05f;
    s.cameras.push_back(c);
    scene::validate(s);
    return s;
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
constexpr uint32_t kServiceVirtual = 16384, kServicePage = 128, kServiceTable = 128;
// Levels of the service clipmap (--service-levels, --service-spacing): level k texel 2^-10 m x spacing^k. S's clipmap is
// 2 x spacing (12 levels to 2 m here); the design's fractional levels (11.4 (7)) are sqrt(2) spacing, twice the levels.
uint32_t g_serviceLevels = 12;
float g_serviceSpacing = 2.0f;
float serviceTexel(uint32_t k) { return std::pow(g_serviceSpacing, (float)k) / 1024.0f; }

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
    for (uint32_t k = 0; k < g_serviceLevels; ++k)
    {
        const float t = serviceTexel(k), pageSize = t * kServicePage;
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

// Requested pages of the ring pattern (see the header): one bit per page, 512 words per level.
std::vector<uint32_t> ringPages(const GpuScene& scene, float3 sun, const ViewDesc& camera, uint32_t height)
{
    const std::vector<RasterView> views = serviceViews(scene, sun, camera.position);
    const float3 z = normalize(sun);
    const float3 up = std::abs(z.y) < 0.999f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    const float3 x = normalize(cross(up, z)), y = cross(z, x);
    const float pixelAngle = 2.0f / (camera.proj.m[1][1] * (float)height);
    std::vector<uint32_t> words(g_serviceLevels * kServiceTable * kServiceTable / 32, 0);
    for (uint32_t k = 0; k < g_serviceLevels; ++k)
    {
        const float t = serviceTexel(k), d = t / pixelAngle;
        const float nearD = k == 0 ? 0.0f : d, farD = k + 1 == g_serviceLevels ? 1e30f : g_serviceSpacing * d;
        // Window origin in texels (serviceViews: texel u = dot(p, x) / t - ox).
        const float ox = (1 + views[k].viewProj.m[0][3]) * kServiceVirtual / 2 * -1;
        const float oy = (views[k].viewProj.m[1][3] - 1) * kServiceVirtual / 2;
        for (uint32_t py = 0; py < kServiceTable; ++py)
            for (uint32_t px = 0; px < kServiceTable; ++px)
            {
                const float lx = ((px + 0.5f) * kServicePage + ox) * t, ly = ((py + 0.5f) * kServicePage + oy) * t;
                if (std::abs(z.y) < 1e-3f) continue;
                const float lambda = -(lx * x.y + ly * y.y) / z.y;  // ground (y = 0) along the sun direction
                const float3 g = x * lx + y * ly + z * lambda;
                const float4x4& m = camera.viewProj;  // row-major: clip = M (g, 1)
                const float cx = m.m[0][0] * g.x + m.m[0][1] * g.y + m.m[0][2] * g.z + m.m[0][3];
                const float cy = m.m[1][0] * g.x + m.m[1][1] * g.y + m.m[1][2] * g.z + m.m[1][3];
                const float cw = m.m[3][0] * g.x + m.m[3][1] * g.y + m.m[3][2] * g.z + m.m[3][3];
                if (cw <= 0 || std::abs(cx) > cw || std::abs(cy) > cw) continue;
                const float dist = length(g - camera.position);
                if (dist < nearD || dist >= farD) continue;
                const uint32_t bit = py * kServiceTable + px;
                words[k * (kServiceTable * kServiceTable / 32) + bit / 32] |= 1u << (bit & 31);
            }
    }
    return words;
}

// Persistent upload ring (one slot per frame in flight) for per-frame CPU data copied into a default-heap buffer.
struct UploadRing
{
    ComPtr<ID3D12Resource> resource;
    uint8_t* mapped = nullptr;
    uint64_t slotBytes = 0;
};

UploadRing uploadRing(Device& device, uint64_t slotBytes, uint32_t slots)
{
    UploadRing u;
    u.slotBytes = slotBytes;
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = slotBytes * slots;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&u.resource)),
          "gate upload ring");
    check(u.resource->Map(0, nullptr, reinterpret_cast<void**>(&u.mapped)), "map upload ring");
    return u;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string sceneName = "city_block", resolutionArg = "both", out, clusterStats, cameraName;
        uint32_t deepTileCards = 1000;
        std::vector<std::string> overrides;
        uint32_t frames = 600;
        float scale = 1.0f;
        bool moving = false, service = false, ringPattern = false;
        std::vector<std::string> serviceModes;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") sceneName = next();
            else if (a == "--camera") cameraName = next();
            else if (a == "--deep-tile-cards") deepTileCards = (uint32_t)std::stoul(next());
            else if (a == "--resolution") resolutionArg = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--scale") scale = std::stof(next());
            else if (a == "--moving") moving = true;
            else if (a == "--service")
            {
                std::string list = next() + ",";
                for (size_t b = 0, e; (e = list.find(',', b)) != std::string::npos; b = e + 1)
                {
                    const std::string m = list.substr(b, e - b);
                    if (m == "both")
                    {
                        serviceModes.push_back("whole");
                        serviceModes.push_back("local");
                    }
                    else if (m == "whole" || m == "local" || m == "atlas16" || m == "atlas32")
                        serviceModes.push_back(m);
                    else
                        fail("--service: a comma-separated list of whole, local, atlas16, atlas32 (or both)");
                }
                service = true;
            }
            else if (a == "--service-levels")
            {
                g_serviceLevels = (uint32_t)std::stoul(next());
                if (g_serviceLevels < 6 || g_serviceLevels > 64) fail("--service-levels 6..64");
            }
            else if (a == "--service-spacing") g_serviceSpacing = std::stof(next());
            else if (a == "--service-pages")
            {
                const std::string m = next();
                if (m != "camera" && m != "ring") fail("--service-pages camera|ring");
                ringPattern = m == "ring";
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
        bool found = sceneName == "deep_tile";
        for (scenegen::SceneId id : scenegen::allScenes())
            if (sceneName == scenegen::sceneName(id))
            {
                request.id = id;
                found = true;
            }
        if (!found) fail("unknown scene %s", sceneName.c_str());
        request.scale = scale;
        auto t0 = std::chrono::steady_clock::now();
        scene::Scene s = sceneName == "deep_tile" ? deepTileScene(deepTileCards) : scenegen::generate(request);
        const double generateMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!cameraName.empty())  // the named (or numbered) scene camera becomes camera 0
        {
            size_t index = s.cameras.size();
            for (size_t c = 0; c < s.cameras.size(); ++c)
                if (s.cameras[c].name == cameraName) index = c;
            if (index == s.cameras.size() && !cameraName.empty() && std::isdigit((unsigned char)cameraName[0])) index = std::stoul(cameraName);
            if (index >= s.cameras.size()) fail("--camera %s: the scene has no such camera", cameraName.c_str());
            std::swap(s.cameras[0], s.cameras[index]);
            logf("camera '%s'\n", s.cameras[0].name.c_str());
        }
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
        const RawBuffer serviceMask = rawBuffer(device, (uint64_t)g_serviceLevels * kServiceTable * kServiceTable / 8, false, L"gate service mask");
        const RawBuffer servicePage = rawBuffer(device, (uint64_t)kServicePage * kServicePage * 4, true, L"gate service page");
        // Ring pattern: the mask and the atlas slots (one word per tile) come from the CPU each frame.
        const uint64_t maskBytes = serviceMask.bytes, slotBytes = (uint64_t)g_serviceLevels * kServiceTable * kServiceTable * 4;
        const RawBuffer serviceSlots = rawBuffer(device, slotBytes, false, L"gate service slots");
        const UploadRing serviceUpload = uploadRing(device, maskBytes + slotBytes, 3);
        uint64_t requestedPages = 0, requestedFrames = 0;
        uint32_t maxPages = 0;
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
                const BufferRef mask = g.importBuffer(serviceMask.resource.Get(), { "gate.service.mask", serviceMask.bytes, 0 });
                const BufferRef page = g.importBuffer(servicePage.resource.Get(), { "gate.service.page", servicePage.bytes, 0 });
                const BufferRef slots = g.importBuffer(serviceSlots.resource.Get(), { "gate.service.slots", serviceSlots.bytes, 0 });
                const float3 sun = s.sun.direction;
                uint32_t pages = 0;
                if (!ringPattern)
                {
                    // The camera's page (tile 64, 64) dirty in levels 0-5: six 128^2 pages, as S measured (5.5 per frame).
                    g.addPass("gate.service.mask", QueueType::Graphics, [&](PassBuilder& b) { b.use(mask, Use::CopyDst); },
                              [=](PassContext& c) {
                                  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER words[6];
                                  const uint32_t bit = 64 * kServiceTable + 64;
                                  for (uint32_t k = 0; k < 6; ++k)
                                      words[k] = { c.address(mask) + 4 * (k * (kServiceTable * kServiceTable / 32) + bit / 32), 1u << (bit & 31) };
                                  c.cmd->WriteBufferImmediate(6, words, nullptr);
                              });
                    pages = 6;
                }
                else
                {
                    const std::vector<uint32_t> words = ringPages(gpuScene, sun, fc.mainView, rr.height);
                    uint8_t* dst = serviceUpload.mapped + (frame % 3) * serviceUpload.slotBytes;
                    std::memcpy(dst, words.data(), maskBytes);
                    uint32_t* slotWords = reinterpret_cast<uint32_t*>(dst + maskBytes);
                    for (uint32_t i = 0; i < g_serviceLevels * kServiceTable * kServiceTable; ++i)
                        slotWords[i] = (words[i / 32] >> (i & 31)) & 1 ? pages++ : UINT32_MAX;
                    ID3D12Resource* upload = serviceUpload.resource.Get();
                    const uint64_t offset = (frame % 3) * serviceUpload.slotBytes;
                    g.addPass("gate.service.upload", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  b.use(mask, Use::CopyDst);
                                  b.use(slots, Use::CopyDst);
                              },
                              [=](PassContext& c) {
                                  c.cmd->CopyBufferRegion(c.resource(mask), 0, upload, offset, maskBytes);
                                  c.cmd->CopyBufferRegion(c.resource(slots), 0, upload, offset + maskBytes, slotBytes);
                              });
                }
                requestedPages += pages;
                ++requestedFrames;
                maxPages = std::max(maxPages, pages);
                FramePassContext sc{ device, g, shaders, quality, gpuScene, fc, serviceResources, serviceServices,
                                     [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { fail("gate: no frame constants for service views"); }, &renderer.trackState(), 2 };
                DepthRasterRequest req;
                req.views = serviceViews(gpuScene, sun, fc.mainView.position);
                req.pixelKernel = "Passes/Visibility/Gates/ServicePagePixel";
                req.bufferUses = { { mask, Use::SrvGraphics }, { page, Use::UavGraphics } };
                req.pixelConstants[0] = serviceMask.descriptor;
                req.pixelConstants[1] = servicePage.descriptor;
                req.cullMask = mask;
                req.cullTilePx = kServicePage;
                // Atlas: 64 slots of 128 px per row (8192 wide), or 128 (16384 wide) past 8192 pages; rows for this frame's
                // pages (at least one). Past 16384 pages one atlas cannot hold them (S draws such a frame in several atlases).
                const uint32_t slotsPerRow = pages > 8192 ? 128 : 64, atlasRows = std::max(1u, (pages + slotsPerRow - 1) / slotsPerRow);
                if (atlasRows * kServicePage > 16384) fail("%u requested pages exceed one 16384^2 atlas of 128 px slots", pages);
                for (const std::string& mode : serviceModes)
                {
                    DepthRasterRequest one = req;
                    one.name = "svc." + mode;
                    one.tileLocal = mode != "whole";
                    if (mode == "atlas16" || mode == "atlas32")
                    {
                        if (!ringPattern) fail("--service atlas16|atlas32 needs --service-pages ring (the camera pattern has no slots)");
                        const TextureRef atlas = g.createTexture({ "gate.service.atlas", slotsPerRow * kServicePage, atlasRows * kServicePage, 1, 1,
                                                                   mode == "atlas16" ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT });
                        g.addPass(one.name + ".clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(atlas, Use::DepthWrite); },
                                  [=](PassContext& c) { c.cmd->ClearDepthStencilView(c.dsv(atlas), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); });
                        one.pixelKernel.clear();
                        one.bufferUses.clear();
                        one.depthTarget = atlas;
                        one.atlasSlots = slots;
                        one.atlasTilesPerRow = slotsPerRow;
                        tracks::rasterizeDepth(sc, one);
                        // S's lookups read the atlas; here nothing does, so a kept reader keeps the raster from being culled.
                        g.addPass(one.name + ".keep", QueueType::Graphics,
                                  [&](PassBuilder& b) {
                                      b.use(atlas, Use::DepthRead);
                                      b.keep();
                                  },
                                  [](PassContext&) {});
                        continue;
                    }
                    tracks::rasterizeDepth(sc, one);
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
            logf("  visible clusters by band A/B/C %u/%u/%u (of B: %u mixed sheet clusters split per triangle, %u triangles)\n", st.bandClusters[0], st.bandClusters[1],
                 st.bandClusters[2], st.mixedClusters, st.mixedTriangles);
            logf("  nodes tested %u, triangles by band A/B/C %u/%u/%u, deferred %u/%u/%u, overflow 0x%x (stats of frame %llu)\n", st.nodesTested, st.triangles[0],
                 st.triangles[1], st.triangles[2], st.deferredInstances, st.deferredNodes, st.deferredClusters, st.overflow, (unsigned long long)st.frameIndex);
            if (st.overflow) status = 1;
            if (coverage)
            {
                const double covRaster = sumPasses(r, [](const std::string& n) { return n == "v.coverage.raster"; });
                const double covTiles = sumPasses(r, [](const std::string& n) { return n == "v.coverage.tiles"; });
                const double covOther = sumPasses(r, [](const std::string& n) { return startsWith(n, "v.coverage.") && n != "v.coverage.raster" && n != "v.coverage.tiles"; });
                const double F = st.coverageFragments;
                if (st.coverageInvocations > 0)
                    logf("  coverage pixel kernel invocations %u (%.2f per fragment; measurement stage)\n", st.coverageInvocations,
                         st.coverageFragments ? (double)st.coverageInvocations / st.coverageFragments : 0.0);
                logf("  coverage layer: %u fragments in %u tiles (%.2f per pixel of a listed tile), %u chunks of pool %u (%u lost to races), %u heavy tiles | raster "
                     "%.3f ms (%.3f ns/fragment; design F x 0.05 ns = %.3f ms) | clear/prepare/args %.3f ms, tiles (opaqueCovered, heavy) %.3f ms\n",
                     st.coverageFragments, st.coverageTiles, st.coverageTiles ? F / (64.0 * st.coverageTiles) : 0.0, st.coverageChunks, st.coveragePoolChunks,
                     st.coverageChunksLost, st.coverageHeavyTiles, covRaster, F > 0 ? covRaster * 1e6 / F : 0.0, F * 0.05e-6, covOther, covTiles);
                // Fragments-per-tile histogram of one more frame (tile header word 0).
                RenderGraph g(device);
                FrameContext fc;
                fc.frameIndex = frames + 1000;
                fc.time = fc.frameIndex / 60.0;
                fc.deltaTime = 1.0f / 60;
                fc.mainView = ViewDesc::fromCamera(cameraAt(s, moving, fc.time), res.width, res.height, prev);
                const TextureRef output = g.createTexture({ "gate output", res.width, res.height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
                const ViewResources main = renderer.record(g, fc, output);
                const uint32_t tilesX = (res.width + 7) / 8, tileCount = tilesX * ((res.height + 7) / 8);
                const uint64_t slots = (uint64_t)quality.integer("visibility.coverage_table_slots");
                const uint64_t tileBytes = (uint64_t)tileCount * 32, tableBytes = (uint64_t)tileCount * slots * 4, listBytes = (16 + 2ull * tileCount) * 4;
                const uint64_t recordBytes = g.desc(main.coverageChunks).size;
                const uint64_t visibleBytes = g.desc(main.visibleClusters).size;
                auto readback = [&](uint64_t bytes) {
                    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                    D3D12_RESOURCE_DESC1 rd{};
                    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                    rd.Width = bytes;
                    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
                    rd.SampleDesc.Count = 1;
                    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                    ComPtr<ID3D12Resource> buffer;
                    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&buffer)),
                          "gate coverage readback");
                    return buffer;
                };
                const uint32_t visPitch = (res.width * 4 + 255) / 256 * 256;
                ComPtr<ID3D12Resource> rbTiles = readback(tileBytes), rbTable = readback(tableBytes), rbList = readback(listBytes), rbRecords = readback(recordBytes),
                                       rbVis = readback((uint64_t)visPitch * res.height), rbVisible = readback(visibleBytes);
                ID3D12Resource *dTiles = rbTiles.Get(), *dTable = rbTable.Get(), *dList = rbList.Get(), *dRecords = rbRecords.Get(), *dVis = rbVis.Get(),
                               *dVisible = rbVisible.Get();
                const uint32_t visW = res.width, visH = res.height;
                g.addPass("gate.coverage.readback", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(main.coverageTiles, Use::CopySrc);
                              b.use(main.coverageChunkTable, Use::CopySrc);
                              b.use(main.coverageTileList, Use::CopySrc);
                              b.use(main.coverageChunks, Use::CopySrc);
                              b.use(main.visId, Use::CopySrc);
                              b.use(main.visibleClusters, Use::CopySrc);
                              b.keep();
                          },
                          [=](PassContext& c) {
                              D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                              to.pResource = dVis;
                              to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                              to.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, visW, visH, 1, visPitch };
                              from.pResource = c.resource(main.visId);
                              from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                              c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                              c.cmd->CopyBufferRegion(dTiles, 0, c.resource(main.coverageTiles), 0, tileBytes);
                              c.cmd->CopyBufferRegion(dTable, 0, c.resource(main.coverageChunkTable), 0, tableBytes);
                              c.cmd->CopyBufferRegion(dList, 0, c.resource(main.coverageTileList), 0, listBytes);
                              c.cmd->CopyBufferRegion(dRecords, 0, c.resource(main.coverageChunks), 0, recordBytes);
                              c.cmd->CopyBufferRegion(dVisible, 0, c.resource(main.visibleClusters), 0, visibleBytes);
                          });
                g.execute(nullptr);
                device.waitIdle();
                const uint32_t *p = nullptr, *table = nullptr, *list = nullptr, *records = nullptr, *visible = nullptr;
                check(rbVisible->Map(0, nullptr, (void**)&visible), "map visible clusters");
                check(rbTiles->Map(0, nullptr, (void**)&p), "map tiles");
                check(rbTable->Map(0, nullptr, (void**)&table), "map chunk table");
                check(rbList->Map(0, nullptr, (void**)&list), "map tile list");
                check(rbRecords->Map(0, nullptr, (void**)&records), "map records");
                // Bins by fragments per tile (per pixel = / 64); the heavy threshold of M's block sort is 1024.
                const uint32_t edges[] = { 1, 64, 128, 256, 512, 1024, 2048, 4096, 16384 };
                uint64_t bins[std::size(edges)] = {}, tiles = 0, fragments = 0, heavyFragments = 0, opaquePixels = 0;
                uint32_t deepest = 0;
                for (uint32_t t = 0; t < tileCount; ++t)
                {
                    const uint32_t n = p[8 * t];
                    opaquePixels += std::popcount(p[8 * t + 3]) + std::popcount(p[8 * t + 4]);
                    if (n == 0) continue;
                    ++tiles;
                    fragments += n;
                    deepest = std::max(deepest, n);
                    if (n > 1024) heavyFragments += n;
                    size_t k = 0;
                    while (k + 1 < std::size(edges) && n >= edges[k + 1]) ++k;
                    ++bins[k];
                }
                // Hidden fragments: per pixel in depth order (nearest first), those after the union of the opaque records'
                // masks became full have weight zero under the composite's mask-union occlusion (what an exact cut behind
                // opaque band B fragments could drop), and the pixels whose opaque union fills at all.
                const uint64_t poolChunks = recordBytes / 1024;
                uint64_t hidden = 0, stored = 0, fullPixels = 0, seeThrough = 0, coveragePixels = 0;
                struct Rec
                {
                    uint32_t depth, mask, vis;  // depth word (see-through flag in the sign bit), mask, vis id
                };
                std::vector<std::vector<Rec>> byPixel(64);
                // Distance bands (design 4.5, edge/vista request): view distance = near / depth (infinite reversed Z).
                // Tree records: the record's instance draws a mesh named "tree..." (the forest scenes' crowns and trunks).
                const float nearPlane = s.cameras.at(0).nearPlane;
                const double bandEdges[] = { 62.0, 187.0, 374.0 };
                auto bandOf = [&](uint32_t depthBits) {
                    float d;
                    const uint32_t bits = depthBits & 0x7FFFFFFFu;
                    std::memcpy(&d, &bits, 4);
                    const double z = d > 0 ? nearPlane / d : 1e30;
                    uint32_t b = 0;
                    while (b < 3 && z >= bandEdges[b]) ++b;
                    return b;
                };
                std::vector<uint8_t> treeInstance(s.instances.size(), 0);
                for (size_t i = 0; i < s.instances.size(); ++i) treeInstance[i] = s.meshes[s.instances[i].mesh].name.rfind("tree", 0) == 0;
                const uint64_t visibleEntries = visibleBytes / 8;
                auto isTree = [&](uint32_t vis) {
                    const uint64_t entry = (vis - 1) >> 7;
                    if (vis == 0 || entry >= visibleEntries) return false;
                    const uint32_t instance = visible[2 * entry];
                    return instance < treeInstance.size() && treeInstance[instance] != 0;
                };
                uint64_t recAll[4] = {}, recTree[4] = {}, keptAll[4] = {}, keptTree[4] = {}, pixAll[4] = {}, pixTree[4] = {};
                std::vector<uint32_t> perPixel, untilFull;  // records per coverage pixel; records up to (and with) the one filling its union
                for (uint32_t k = 0; k < std::min(list[3], tileCount); ++k)
                {
                    const uint32_t tile = list[16 + k], n = p[8 * tile];
                    for (auto& v : byPixel) v.clear();
                    for (uint32_t c = 0; c * 64 < n && c < poolChunks; ++c)
                    {
                        uint32_t chunk = 0;
                        if (c < slots) chunk = table[(size_t)tile * slots + c];
                        else
                        {
                            // Extension tree (CoverageTiles.hlsli): root words 0..253 direct, 254 a 2-level subtree, 255 a
                            // 3-level subtree.
                            uint64_t e = c - slots;
                            uint32_t rootWord = (uint32_t)e, digits = 0;
                            if (e >= 254)
                            {
                                e -= 254;
                                rootWord = 254;
                                digits = 2;
                                if (e >= 65536)
                                {
                                    e -= 65536;
                                    rootWord = 255;
                                    digits = 3;
                                }
                            }
                            chunk = p[8 * tile + 5];
                            if (chunk != 0 && chunk <= poolChunks) chunk = records[(size_t)(chunk - 1) * 256 + rootWord];
                            for (uint32_t digit = digits; digit > 0 && chunk != 0 && chunk <= poolChunks; --digit)
                                chunk = records[(size_t)(chunk - 1) * 256 + ((e >> (8 * (digit - 1))) & 0xFF)];
                        }
                        if (chunk == 0 || chunk > poolChunks) continue;
                        for (uint32_t ri = 0; ri < 64 && c * 64 + ri < n; ++ri)
                        {
                            const uint32_t* rec = &records[((size_t)(chunk - 1) * 64 + ri) * 4];
                            byPixel[rec[3] >> 26].push_back({ rec[1], rec[2], rec[0] });
                            ++stored;
                        }
                    }
                    for (auto& v : byPixel)
                    {
                        std::sort(v.begin(), v.end(), [](const Rec& x, const Rec& y) { return (x.depth & 0x7FFFFFFFu) > (y.depth & 0x7FFFFFFFu); });
                        uint32_t u = 0;
                        bool full = false;
                        for (const Rec& rec : v)
                        {
                            const uint32_t band = bandOf(rec.depth);
                            const bool tree = isTree(rec.vis);
                            ++recAll[band];
                            recTree[band] += tree;
                            if (full) ++hidden;
                            else
                            {
                                ++keptAll[band];
                                keptTree[band] += tree;
                            }
                            if (rec.depth >> 31) ++seeThrough;
                            else u |= rec.mask;
                            full = full || u == 0xFFFFFFFFu;
                        }
                        if (!v.empty())
                        {
                            perPixel.push_back((uint32_t)v.size());
                            {
                                uint32_t u2 = 0, needed = 0;
                                for (const Rec& rec : v)
                                {
                                    ++needed;
                                    if (!(rec.depth >> 31)) u2 |= rec.mask;
                                    if (u2 == 0xFFFFFFFFu) break;
                                }
                                untilFull.push_back(needed);
                            }
                            const uint32_t band = bandOf(v.front().depth);
                            ++pixAll[band];
                            pixTree[band] += isTree(v.front().vis);
                        }
                        fullPixels += full;
                        coveragePixels += !v.empty();
                    }
                }
                // Band A pixels: a vis id in the vis buffer (sky and pixels covered only by the coverage layer have none).
                uint64_t bandAPixels = 0;
                {
                    const uint8_t* vis = nullptr;
                    check(rbVis->Map(0, nullptr, (void**)&vis), "map vis ids");
                    for (uint32_t y = 0; y < visH; ++y)
                    {
                        const uint32_t* row = reinterpret_cast<const uint32_t*>(vis + (size_t)y * visPitch);
                        for (uint32_t x = 0; x < visW; ++x) bandAPixels += row[x] != 0;
                    }
                    rbVis->Unmap(0, nullptr);
                }
                rbTiles->Unmap(0, nullptr);
                rbTable->Unmap(0, nullptr);
                rbList->Unmap(0, nullptr);
                rbRecords->Unmap(0, nullptr);
                rbVisible->Unmap(0, nullptr);
                std::string hist;
                for (size_t k = 0; k < std::size(edges); ++k)
                {
                    char b[64];
                    if (k + 1 < std::size(edges)) snprintf(b, sizeof b, "%s%u-%u: %.2f%%", k ? ", " : "", edges[k], edges[k + 1] - 1, tiles ? 100.0 * bins[k] / tiles : 0.0);
                    else snprintf(b, sizeof b, "%s%u+: %.2f%%", k ? ", " : "", edges[k], tiles ? 100.0 * bins[k] / tiles : 0.0);
                    hist += b;
                }
                logf("  fragments per tile (one frame, %llu tiles, %llu fragments, deepest %u): %s | tiles over 1024 hold %.1f%% of the fragments\n",
                     (unsigned long long)tiles, (unsigned long long)fragments, deepest, hist.c_str(), fragments ? 100.0 * heavyFragments / fragments : 0.0);
                logf("  pixels: P_A %.3f M (vis id), P_B+C %.3f M (coverage records), F_B+C %.3f M stored fragments (%.2f per coverage pixel); T_A %u band A "
                     "triangles of visible clusters\n",
                     bandAPixels / 1e6, coveragePixels / 1e6, stored / 1e6, coveragePixels ? (double)stored / coveragePixels : 0.0, st.triangles[0]);
                logf("  opaque union: %llu pixels fill it, %llu opaqueCovered (band A behind the farthest opaque record) | %llu of %llu stored fragments (%.1f%%) lie "
                     "behind a filled union (weight zero) | %llu see-through records\n",
                     (unsigned long long)fullPixels, (unsigned long long)opaquePixels, (unsigned long long)hidden, (unsigned long long)stored,
                     stored ? 100.0 * hidden / stored : 0.0, (unsigned long long)seeThrough);
                {
                    auto pct = [](std::vector<uint32_t>& v, double q) -> uint32_t {
                        if (v.empty()) return 0;
                        const size_t k = std::min(v.size() - 1, (size_t)(q * v.size()));
                        std::nth_element(v.begin(), v.begin() + k, v.end());
                        return v[k];
                    };
                    logf("  records per coverage pixel P50/P99/max %u/%u/%u | records up to the one filling the opaque union (all when it never fills) P50/P99/max %u/%u/%u\n",
                         pct(perPixel, 0.5), pct(perPixel, 0.99), perPixel.empty() ? 0u : *std::max_element(perPixel.begin(), perPixel.end()), pct(untilFull, 0.5),
                         pct(untilFull, 0.99), untilFull.empty() ? 0u : *std::max_element(untilFull.begin(), untilFull.end()));
                }
                {
                    const char* names[4] = { "< 62 m", "62-187 m", "187-374 m", "> 374 m" };
                    std::string line;
                    for (uint32_t b = 0; b < 4; ++b)
                    {
                        char buf[256];
                        snprintf(buf, sizeof buf, "%s%s: pixels %.3f M (tree %.3f M), records %.3f M (tree %.3f M), in front of a full union %.3f M (tree %.3f M)", b ? " | " : "",
                                 names[b], pixAll[b] / 1e6, pixTree[b] / 1e6, recAll[b] / 1e6, recTree[b] / 1e6, keptAll[b] / 1e6, keptTree[b] / 1e6);
                        line += buf;
                    }
                    logf("  by distance (nearest record per pixel; record's own distance): %s\n", line.c_str());
                }
                for (ComPtr<ID3D12Resource>* buffer : { std::addressof(rbTiles), std::addressof(rbTable), std::addressof(rbList), std::addressof(rbRecords), std::addressof(rbVis),
                                                        std::addressof(rbVisible) })
                    device.deferRelease(*buffer);
            }
            if (service)
            {
                const double pagesPerFrame = requestedFrames ? (double)requestedPages / requestedFrames : 0;
                logf("  service pages (%s): %.1f requested per frame (max %u) = %.1f M texels\n", ringPattern ? "ring" : "camera", pagesPerFrame, maxPages,
                     pagesPerFrame * kServicePage * kServicePage / 1e6);
                for (const std::string& mode : serviceModes)
                {
                    const std::string run = "svc." + mode, p = run + ".";
                    const double rasterMs = sumPasses(r, [&](const std::string& n) { return n == p + "raster"; });
                    const double clearMs = sumPasses(r, [&](const std::string& n) { return n == p + "clear"; });
                    const double cullMs = sumPasses(r, [&](const std::string& n) {
                        return startsWith(n, p.c_str()) && n != p + "raster" && n != p + "stats" && n != p + "clear" && n != p + "keep";
                    });
                    const visibility::Stats ss = visibility::latestStats(renderer.trackState(), run);
                    logf("  service %s: raster %.3f ms (%.4f ns per requested texel), cull %.3f ms, clear %.3f ms | %u clusters visible, %u triangles, %u tile pairs, "
                         "overflow 0x%x\n",
                         run.c_str(), rasterMs, pagesPerFrame > 0 ? rasterMs * 1e6 / (pagesPerFrame * kServicePage * kServicePage) : 0.0, cullMs, clearMs,
                         ss.visibleClusters, ss.triangles[0] + ss.triangles[1] + ss.triangles[2], ss.tilePairs, ss.overflow);
                    if (ss.overflow) status = 1;
                }
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
