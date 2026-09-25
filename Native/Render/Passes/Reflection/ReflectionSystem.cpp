#include "unx/refl/ReflectionSystem.h"

#include "unx/rt/RayPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <array>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace unx::render::refl
{
namespace
{
uint32_t asU(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

constexpr uint32_t kDescStride = (uint32_t)((sizeof(D3D12_DISPATCH_RAYS_DESC) + 7) / 8 * 8);
constexpr uint32_t kPlanarMax = 4, kCandidatesMax = 64, kPlanarSlotBytes = 4096, kPlanarSlots = 4;  // ring slots > frames in flight
// Read-back slot: candidate pixel counts, timestamps (trace begin/end, then begin/end per view), job counters (args 0..16).
constexpr uint32_t kTicks = 2 + 2 * kPlanarMax, kTicksOffset = kCandidatesMax * 4, kJobsOffset = kTicksOffset + kTicks * 8, kReadbackStride = 512;
static_assert(kJobsOffset + 16 <= kReadbackStride);
struct PlanarGpu  // ReflectionInternal.hlsli: candidates [0, views) have a reflection camera
{
    uint32_t candidates, views, pad[2];
    struct
    {
        float4 plane;
        uint32_t rect[4];  // x, y, width, height in main-view pixels
    } planes[kCandidatesMax];
};
static_assert(sizeof(PlanarGpu) <= kPlanarSlotBytes);
static_assert(kPlanarMax == 4, "ReflectionSystem::m_viewRects and ReflectionClassify's mask constants hold 4 views");

float3 xform(const float4 rows[3], float3 p)
{
    return { rows[0].x * p.x + rows[0].y * p.y + rows[0].z * p.z + rows[0].w, rows[1].x * p.x + rows[1].y * p.y + rows[1].z * p.z + rows[1].w,
             rows[2].x * p.x + rows[2].y * p.y + rows[2].z * p.z + rows[2].w };
}
// Arguments buffer: counters, trace descriptions (SKY0, SKY1), shadow description, shade and combine Dispatch arguments.
constexpr uint32_t kShadowDescOffset = 16 + 2 * kDescStride, kShadeArgsOffset = kShadowDescOffset + kDescStride, kCombineArgsOffset = kShadeArgsOffset + 16;
constexpr uint32_t kArgumentsBytes = kCombineArgsOffset + 16;
const char* const kTraceLibrary[2] = { "Passes/Reflection/ReflectionTrace.SKY0", "Passes/Reflection/ReflectionTrace.SKY1" };
const char* const kShadeKernel[2] = { "Passes/Reflection/ReflectionShadeRays.SKY0", "Passes/Reflection/ReflectionShadeRays.SKY1" };
constexpr const char* kShadowLibrary = "Passes/Reflection/ReflectionShadow";
} // namespace

ReflectionSettings ReflectionSettings::fromQuality(const QualityConfig& q)
{
    ReflectionSettings s;
    s.kHalfAngle = (float)(q.number("reflection.cache_lobe_half_angle_min_deg") * 3.14159265358979 / 180.0);
    s.mirrorRoughness = (float)q.number("reflection.mirror_roughness_max");
    s.raysPerSample = (uint32_t)q.integer("reflection.g_rays_per_sample");
    const std::vector<double> spacing = q.numbers("reflection.g_sample_spacing_px");
    if (spacing.size() != 2 || spacing[0] != 1) fail("reflection.g_sample_spacing_px must be [1, max]");
    // Samples live on per-tile grids: spacings above 8 px are sampled at 8 (denser than the bound, never sparser).
    s.maxSpacing = (uint32_t)std::min(spacing[1], 8.0);
    if (s.raysPerSample == 0) fail("reflection.g_rays_per_sample must be > 0");
    s.planarViewsMax = (uint32_t)q.integer("reflection.planar_views_max");
    if (s.planarViewsMax > kPlanarMax) fail("reflection.planar_views_max must be <= %u", kPlanarMax);
    s.planarViewFixedMs = (float)q.number("reflection.planar_view_fixed_ms");
    s.experimentDisable = (uint32_t)q.integer("reflection.experiment_disable");
    s.planarViewNsPerPixel = (float)q.number("reflection.planar_view_ns_per_px");
    return s;
}

namespace
{
struct ReflectionSystemSlot
{
    std::unique_ptr<ReflectionSystem> system;
};
} // namespace

ReflectionSystem& ReflectionSystem::get(FramePassContext& fc)
{
    ReflectionSystemSlot& slot = fc.state<ReflectionSystemSlot>("R.reflection");
    if (!slot.system) slot.system = std::make_unique<ReflectionSystem>(fc.device, fc.shaders, fc.quality);
    return *slot.system;
}

ReflectionSystem* ReflectionSystem::find(TrackState& state) { return state.get<ReflectionSystemSlot>("R.reflection").system.get(); }

ReflectionSystem::ReflectionSystem(Device& device, ShaderLibrary& shaders, const QualityConfig& quality)
    : m_device(device), m_settings(ReflectionSettings::fromQuality(quality))
{
    // Indirect dispatch arguments: counter + one description per sky variant (shader tables fixed; Width per frame).
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, upload{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = kArgumentsBytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_arguments)),
          "reflection arguments");
    m_arguments->SetName(L"R reflection dispatch arguments");
    uint8_t image[kArgumentsBytes] = {};
    for (int v = 0; v < 2; ++v)
    {
        const D3D12_DISPATCH_RAYS_DESC desc = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline(kTraceLibrary[v], { "ReflectionTraceGen" })).dispatchDesc(0, 0, 1, 1);
        std::memcpy(image + 16 + v * kDescStride, &desc, sizeof desc);
    }
    {
        const D3D12_DISPATCH_RAYS_DESC desc = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline(kShadowLibrary, { "ReflectionShadowGen" })).dispatchDesc(0, 0, 1, 1);
        std::memcpy(image + kShadowDescOffset, &desc, sizeof desc);
    }
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sd{};
        sd.ByteStride = 16;
        sd.NumArgumentDescs = 1;
        sd.pArgumentDescs = &arg;
        check(device.d3d()->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&m_dispatchSignature)), "reflection dispatch signature");
    }
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource3(&upload, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "reflection arguments staging");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &mapped), "map arguments staging");
    std::memcpy(mapped, image, sizeof image);
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(m_arguments.Get(), 0, staging.Get(), 0, sizeof image);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));

    // Planar reflector parameters: CPU-written upload ring, read by classify/resolve through a raw SRV.
    d.Width = kPlanarSlots * kPlanarSlotBytes;
    check(device.d3d()->CreateCommittedResource3(&upload, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_planarRing)),
          "planar reflector ring");
    check(m_planarRing->Map(0, &none, reinterpret_cast<void**>(&m_planarMapped)), "map planar ring");
    std::memset(m_planarMapped, 0, kPlanarSlots * kPlanarSlotBytes);
    DescriptorHeaps& h = device.descriptors();
    m_planarSrv = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.Buffer.NumElements = kPlanarSlots * kPlanarSlotBytes / 4;
    sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device.d3d()->CreateShaderResourceView(m_planarRing.Get(), &sd, h.resourceCpu(m_planarSrv));

    // Per-candidate pixel counts (UAV) and their read-back ring.
    d.Width = kCandidatesMax * 4;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_planarCounts)),
          "planar counts");
    D3D12_HEAP_PROPERTIES readback{ D3D12_HEAP_TYPE_READBACK };
    d.Width = kPlanarSlots * kReadbackStride;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device.d3d()->CreateCommittedResource3(&readback, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_planarReadback)),
          "planar count readback");
    D3D12_RANGE all{ 0, (SIZE_T)d.Width };
    check(m_planarReadback->Map(0, &all, reinterpret_cast<void**>(const_cast<uint8_t**>(&m_readbackMapped))), "map planar readback");
    m_slotFrame.assign(kPlanarSlots, UINT64_MAX);

    // Own timestamps for the cost choice (the frame profiler is not visible to tracks).
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kPlanarSlots * kTicks;
    check(device.d3d()->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_timestamps)), "reflection timestamps");
    uint64_t frequency = 0;
    check(device.queue(QueueType::Graphics).get()->GetTimestampFrequency(&frequency), "timestamp frequency");
    m_tickMs = 1000.0 / (double)frequency;
    m_viewNsPerPixel = m_settings.planarViewNsPerPixel;
    m_viewFixedNs = m_settings.planarViewFixedMs * 1e6f;
    // The prior as two points of the fit (at 0 and at 1 M mirror pixels); measured views outweigh them after a few.
    addViewSample(0, m_viewFixedNs, 1);
    addViewSample(1e6, m_viewFixedNs + 1e6 * m_viewNsPerPixel, 1);
}

void ReflectionSystem::addViewSample(double pixels, double ns, double weight)
{
    double* f = m_viewFit;
    for (int i = 0; i < 5; ++i) f[i] *= 15.0 / 16.0;
    f[0] += weight;
    f[1] += weight * pixels;
    f[2] += weight * ns;
    f[3] += weight * pixels * pixels;
    f[4] += weight * pixels * ns;
    // y = a + b x by least squares; a, b >= 0 (a negative term refits the other alone).
    const double det = f[0] * f[3] - f[1] * f[1];
    double b = det > 1e-9 * f[0] * f[3] ? (f[0] * f[4] - f[1] * f[2]) / det : m_viewNsPerPixel;
    double a = (f[2] - b * f[1]) / f[0];
    if (b < 0) b = 0, a = f[2] / f[0];
    if (a < 0) a = 0, b = f[3] > 0 ? f[4] / f[3] : m_viewNsPerPixel;
    m_viewFixedNs = (float)a;
    m_viewNsPerPixel = (float)b;
}

// Planar reflector candidates: triangles on which a reflection camera is exact. The material (with instance overrides)
// is mirror-smooth (roughness <= reflection.mirror_roughness_max), not alpha-tested and has no normal map, and the
// triangle's three vertex normals are its face normal (cos >= 0.999): then the shading normal is the plane normal and
// the mirrored view is the reflection. Smooth-shaded curved or wavy surfaces fail that test and stay on rays, which
// are exact for them. Static instances only; triangles are clustered by world plane (normal quantised to 1e-3,
// offset to 1 mm; the side is the one the normals face), so a facade's window panes share one plane.
void ReflectionSystem::buildPlanes(const GpuScene& gpuScene)
{
    const auto start = std::chrono::steady_clock::now();
    m_planes.clear();
    m_planesRevision = gpuScene.revision();
    const scene::Scene* src = gpuScene.source();
    const auto& instances = gpuScene.instances();
    struct KeyHash
    {
        size_t operator()(const std::array<int64_t, 4>& k) const
        {
            uint64_t h = 1469598103934665603ull;
            for (int64_t x : k) h = (h ^ (uint64_t)x) * 1099511628211ull;
            return (size_t)h;
        }
    };
    std::unordered_map<std::array<int64_t, 4>, uint32_t, KeyHash> clusters;
    auto mirrorMaterial = [&](const scene::Instance& in, uint32_t k) -> bool {
        const scene::Mesh& m = src->meshes[in.mesh];
        const scene::Material& material = src->materials[k < in.materialOverrides.size() ? in.materialOverrides[k] : m.submeshes[k].material];
        return material.roughness <= m_settings.mirrorRoughness && material.alphaCutoff <= 0 && material.normalTexture == scene::kNone;
    };
    auto staticInstance = [](const scene::Instance& in) { return !(in.flags & (scene::InstanceDynamic | scene::InstanceSkinned | scene::InstanceWind)); };
    size_t candidateTriangles = 0;
    for (uint32_t i = 0; src && i < (uint32_t)src->instances.size(); ++i)
        if (staticInstance(src->instances[i]))
            for (uint32_t k = 0; k < (uint32_t)src->meshes[src->instances[i].mesh].submeshes.size(); ++k)
                if (mirrorMaterial(src->instances[i], k)) candidateTriangles += src->meshes[src->instances[i].mesh].submeshes[k].indexCount / 3;
    clusters.reserve(candidateTriangles);
    uint64_t mirrorTriangles = 0, curvedTriangles = 0;
    for (uint32_t i = 0; src && i < (uint32_t)src->instances.size(); ++i)
    {
        const scene::Instance& in = src->instances[i];
        if (!staticInstance(in)) continue;
        const scene::Mesh& m = src->meshes[in.mesh];
        const gpu::Instance& gi = instances[i];
        const float4* r = gi.objectToWorld;
        const float det = r[0].x * (r[1].y * r[2].z - r[1].z * r[2].y) - r[0].y * (r[1].x * r[2].z - r[1].z * r[2].x) + r[0].z * (r[1].x * r[2].y - r[1].y * r[2].x);
        for (uint32_t k = 0; k < (uint32_t)m.submeshes.size(); ++k)
        {
            if (!mirrorMaterial(in, k)) continue;
            const scene::Submesh& sm = m.submeshes[k];
            for (uint32_t t = sm.indexOffset; t + 2 < sm.indexOffset + sm.indexCount; t += 3)
            {
                const uint32_t ia = m.indices[t], ib = m.indices[t + 1], ic = m.indices[t + 2];
                const float3 objectCross = cross(m.positions[ib] - m.positions[ia], m.positions[ic] - m.positions[ia]);
                if (length(objectCross) <= 1e-20f) continue;
                const float3 objectNormal = normalize(objectCross);
                if (!m.normals.empty() && (dot(m.normals[ia], objectNormal) < 0.999f || dot(m.normals[ib], objectNormal) < 0.999f || dot(m.normals[ic], objectNormal) < 0.999f))
                {
                    ++curvedTriangles;
                    continue;
                }
                const float3 a = xform(gi.objectToWorld, m.positions[ia]), b = xform(gi.objectToWorld, m.positions[ib]), c = xform(gi.objectToWorld, m.positions[ic]);
                const float3 cr = cross(b - a, c - a);
                const float doubleArea = length(cr);
                if (doubleArea <= 1e-20f) continue;
                const float3 n = cr * ((det < 0 ? -1.0f : 1.0f) / doubleArea);  // a mirroring transform flips the winding, not the normals
                const float w = -dot(n, a);
                const std::array<int64_t, 4> key{ std::lround(n.x * 1000), std::lround(n.y * 1000), std::lround(n.z * 1000), std::llround(w * 1000) };
                auto [it, added] = clusters.try_emplace(key, (uint32_t)m_planes.size());
                if (added) m_planes.push_back({ { n.x, n.y, n.z, w }, { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f }, 0, 0 });
                PlanarReflector& p = m_planes[it->second];
                for (const float3& q : { a, b, c })
                {
                    p.lo = { std::min(p.lo.x, q.x), std::min(p.lo.y, q.y), std::min(p.lo.z, q.z) };
                    p.hi = { std::max(p.hi.x, q.x), std::max(p.hi.y, q.y), std::max(p.hi.z, q.z) };
                }
                p.area += 0.5f * doubleArea;
                ++p.triangles;
                ++mirrorTriangles;
            }
        }
    }
    m_planeOrder.resize(m_planes.size());
    for (uint32_t k = 0; k < (uint32_t)m_planes.size(); ++k) m_planeOrder[k] = k;
    m_planeNodes.clear();
    if (!m_planes.empty())
    {
        m_planeNodes.reserve(m_planes.size() / 2 + 2);
        buildPlaneNodes(0, (uint32_t)m_planes.size());
    }
    m_planePixels.assign(m_planes.size(), 0);
    m_planeViewMs.assign(m_planes.size(), 0);
    m_planeViewFrame.assign(m_planes.size(), UINT64_MAX);
    m_planeCameraFrame.assign(m_planes.size(), UINT64_MAX - 1);
    m_planeLastSeen.assign(m_planes.size(), UINT64_MAX - 1);
    m_planeRunStart.assign(m_planes.size(), UINT64_MAX);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    logf("R reflections: %zu planar reflector planes from %llu flat mirror triangles (%llu smooth-shaded mirror triangles stay on rays), %.1f ms\n",
         m_planes.size(), (unsigned long long)mirrorTriangles, (unsigned long long)curvedTriangles, ms);
}

// Node over m_planeOrder[begin, end) and its subtree, appended to m_planeNodes: leaves of up to 8 planes, split at the
// centroid median of the longest axis.
uint32_t ReflectionSystem::buildPlaneNodes(uint32_t begin, uint32_t end)
{
    const uint32_t index = (uint32_t)m_planeNodes.size();
    m_planeNodes.emplace_back();
    PlaneNode node;
    node.lo = { 1e30f, 1e30f, 1e30f };
    node.hi = { -1e30f, -1e30f, -1e30f };
    for (uint32_t k = begin; k < end; ++k)
    {
        const PlanarReflector& p = m_planes[m_planeOrder[k]];
        node.lo = { std::min(node.lo.x, p.lo.x), std::min(node.lo.y, p.lo.y), std::min(node.lo.z, p.lo.z) };
        node.hi = { std::max(node.hi.x, p.hi.x), std::max(node.hi.y, p.hi.y), std::max(node.hi.z, p.hi.z) };
        node.maxArea = std::max(node.maxArea, p.area);
    }
    if (end - begin <= 8)
    {
        node.first = begin;
        node.count = end - begin;
        m_planeNodes[index] = node;
        return index;
    }
    const float3 extent = node.hi - node.lo;
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
    auto centre = [&](uint32_t k) {
        const PlanarReflector& p = m_planes[k];
        return axis == 0 ? p.lo.x + p.hi.x : (axis == 1 ? p.lo.y + p.hi.y : p.lo.z + p.hi.z);
    };
    const uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(m_planeOrder.begin() + begin, m_planeOrder.begin() + mid, m_planeOrder.begin() + end, [&](uint32_t a, uint32_t b) { return centre(a) < centre(b); });
    node.first = buildPlaneNodes(begin, mid);
    node.second = buildPlaneNodes(mid, end);
    m_planeNodes[index] = node;
    return index;
}

ReflectionSystem::~ReflectionSystem()
{
    m_device.deferRelease(m_arguments);
    m_device.deferRelease(m_history);
    if (m_planarRing) m_planarRing->Unmap(0, nullptr);
    m_device.deferRelease(m_planarRing);
    if (m_planarReadback) m_planarReadback->Unmap(0, nullptr);
    m_device.deferRelease(m_planarReadback);
    m_device.deferRelease(m_planarCounts);
    m_device.deferRelease(m_timestamps);
    DescriptorHeaps* h = &m_device.descriptors();
    const uint32_t srv = m_planarSrv;
    if (srv != 0xFFFFFFFFu) m_device.deferCall([h, srv] { h->freeResource(srv); });
}

void ReflectionSystem::ensureHistory(uint32_t width, uint32_t height)
{
    if (m_history && m_historyWidth == width && m_historyHeight == height) return;
    if (m_history) m_device.deferRelease(m_history);
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_history)),
          "reflection distance history");
    m_history->SetName(L"R reflection distance history");
    m_historyWidth = width;
    m_historyHeight = height;
}

void ReflectionSystem::record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays)
{
    RenderGraph& g = fc.graph;
    const ReflectionSettings& s = m_settings;
    const uint32_t width = main.view.width, height = main.view.height;
    const uint32_t tilesX = (width + 7) / 8, tilesY = (height + 7) / 8;
    const bool fresh = !m_history || m_historyWidth != width || m_historyHeight != height;
    ensureHistory(width, height);

    main.reflection = g.createTexture({ "R reflection", width, height + tilesY, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef reflection = main.reflection, depth = main.depth, gbuffer = main.gbuffer, probes = main.screenProbes, lobes = main.reflectionLobeTiles;
    const TextureRef modes = g.createTexture({ "R reflection modes", width, height, 1, 1, DXGI_FORMAT_R32_UINT });
    m_modes = modes;
    const TextureRef history = g.importTexture(m_history.Get(), { "R reflection distance history", width, height, 1, 1, DXGI_FORMAT_R16_FLOAT },
                                               D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const BufferRef jobs = g.createBuffer({ "R reflection jobs", (uint64_t)width * height * 4, 4 });
    const BufferRef results = g.createBuffer({ "R reflection results", (uint64_t)width * height * 8, 8 });
    const BufferRef args = g.importBuffer(m_arguments.Get(), { "R reflection dispatch arguments", kArgumentsBytes, 0 });
    const BufferRef cache = fc.resources.giCache;
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = main.frameConstants;
    ShaderLibrary& shaders = fc.shaders;
    const float focal = height / (2.0f * std::tan(main.view.verticalFov * 0.5f));

    // Planar mirrors (design 2.6 cost formula, header comment): read back the per-plane pixel counts and the measured
    // costs of the frame that last used the slot framesInFlight frames ago; visible planes become candidates (counted
    // this frame), the ones whose rays cost more than their view get a reflection camera and their pixels take no rays.
    if (m_planesRevision != fc.scene.revision()) buildPlanes(fc.scene);
    const uint32_t ringSlot = (uint32_t)(fc.frame.frameIndex % kPlanarSlots);
    {
        // Frame f - framesInFlight is complete when frame f is recorded (FramePassContext::framesInFlight).
        if (fc.framesInFlight >= kPlanarSlots) fail("R reflections: %u frames in flight need more planar ring slots", fc.framesInFlight);
        const uint32_t oldSlot = (uint32_t)((fc.frame.frameIndex + kPlanarSlots - fc.framesInFlight) % kPlanarSlots);
        if (m_slotFrame[oldSlot] != UINT64_MAX && fc.frame.frameIndex >= m_slotFrame[oldSlot] + fc.framesInFlight)
        {
            const uint8_t* slot = m_readbackMapped + oldSlot * kReadbackStride;
            const uint32_t* counts = reinterpret_cast<const uint32_t*>(slot);
            for (size_t c = 0; c < m_slotPlanes[oldSlot].size(); ++c)
            {
                const uint32_t p = m_slotPlanes[oldSlot][c];
                // Only counts of the plane's current run of consecutive candidate frames (its view may have changed).
                if (p < m_planePixels.size() && m_planeRunStart[p] <= m_slotFrame[oldSlot]) m_planePixels[p] = counts[c];
            }
            // Costs: the trace per ray, each view per mirror pixel it drew (running averages over ~16 frames).
            const uint64_t* ticks = reinterpret_cast<const uint64_t*>(slot + kTicksOffset);
            const uint32_t* counters = reinterpret_cast<const uint32_t*>(slot + kJobsOffset);  // total jobs, M, G samples, G pixels
            const double traced = (double)counters[1] + (double)counters[2] * m_settings.raysPerSample;
            // Ray slots for the split passes: 1.5 x the traced rays once they pass 3/4 of the capacity (a frame beyond it
            // traces the overflowing jobs inline: the same values, slower). At most 2^25 slots (56 B each, 1.88 GB): a raw
            // view addresses 2^31 bytes, and slots past it lost their stores (a 64-ray reference lost its shadow rays).
            if (traced > 0.75 * m_rayCapacity)
                while (m_rayCapacity < 1.5 * traced && m_rayCapacity < (1u << 25)) m_rayCapacity *= 2;
            if (ticks[1] > ticks[0] && traced >= 4096)
            {
                const float sample = (float)((ticks[1] - ticks[0]) * m_tickMs * 1e6 / traced);
                m_rayNs = m_rayNs > 0 ? m_rayNs + (sample - m_rayNs) / 16 : sample;
            }
            m_lastViewMs = 0;
            for (size_t v = 0; v < m_slotViewPlanes[oldSlot].size(); ++v)
            {
                const uint64_t t0 = ticks[2 + 2 * v], t1 = ticks[3 + 2 * v];
                if (t1 <= t0) continue;  // not bracketed (the view's passes ran on another queue): no measurement
                const float ms = (float)((t1 - t0) * m_tickMs);
                m_lastViewMs += ms;
                const uint32_t p = m_slotViewPlanes[oldSlot][v], mirror = m_slotViewPixels[oldSlot][v];
                if (p < m_planeViewMs.size())
                {
                    m_planeViewMs[p] = ms;
                    m_planeViewFrame[p] = m_slotFrame[oldSlot];
                }
                addViewSample(mirror, ms * 1e6, 1);
            }
        }
    }
    PlanarGpu planar{};
    TextureRef planarColor[kPlanarMax];
    // Views chosen this frame: recorded after the classification, which writes their mirror masks (INTERFACES v1.22).
    struct PlanarView
    {
        ViewDesc desc;
        TextureRef mask, tileMask;
    } planarViews[kPlanarMax];
    m_lastPlanarViews = m_lastPlanarPixels = m_lastCandidates = m_lastRectPixels = 0;
    std::vector<uint32_t>& slotPlanes = m_slotPlanes[ringSlot];
    slotPlanes.clear();
    m_slotViewPlanes[ringSlot].clear();
    m_slotViewPixels[ringSlot].clear();
    // Exact threshold of the cost choice: a view costs at least a + b x pixels, rays c x pixels, so a plane can pay off
    // only when c > b and pixels > a / (c - b). Until the trace has been measured no plane is chosen.
    const float rayNs = m_rayNs, viewNs = m_viewNsPerPixel, viewFixedNs = m_viewFixedNs;
    const bool planarCanWin = fc.services.renderView && (m_planarForced || rayNs > viewNs);
    const double minPixels = m_planarForced ? 1.0 : planarCanWin ? viewFixedNs / (rayNs - viewNs) : 1e30;
    const auto selectStart = std::chrono::steady_clock::now();
    if (m_planarEnabled && !m_planes.empty() && planarCanWin)
    {
        struct Candidate
        {
            uint32_t plane, x, y, w, h;
            bool eligible;  // a current read-back count whose rays cost more than the plane's view
        };
        std::vector<Candidate> candidates;
        const float4x4& vp = main.view.viewProj;
        const float3 cam = main.view.position;
        const uint64_t frame = fc.frame.frameIndex;
        // Exact bound: a plane of area A whose bounds are at distance d covers a solid angle <= A / d^2, and a pixel
        // subtends >= cos^3(corner) / f^2 sr, so pixels <= A f^2 / (d^2 cos^3). Planes whose bound is below minPixels
        // can never pay for a camera and are not visited (BVH on bounds and max area).
        const float tanY = std::tan(main.view.verticalFov * 0.5f), tanX = tanY * width / height;
        const float cosCorner = 1.0f / std::sqrt(1 + tanX * tanX + tanY * tanY);
        const float reach = minPixels > 0 ? (float)(focal * focal / (minPixels * cosCorner * cosCorner * cosCorner)) : 1e30f;  // d^2 <= A * reach
        auto distance2 = [&](const float3& lo, const float3& hi) {
            const float dx = std::max({ lo.x - cam.x, 0.0f, cam.x - hi.x }), dy = std::max({ lo.y - cam.y, 0.0f, cam.y - hi.y }),
                        dz = std::max({ lo.z - cam.z, 0.0f, cam.z - hi.z });
            return dx * dx + dy * dy + dz * dz;
        };
        auto consider = [&](uint32_t k) {
            const PlanarReflector& r = m_planes[k];
            if (r.plane.x * cam.x + r.plane.y * cam.y + r.plane.z * cam.z + r.plane.w <= 0) return;
            if (distance2(r.lo, r.hi) > r.area * reach) return;
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            bool behind = false, visible = false;
            for (int c = 0; c < 8; ++c)
            {
                const float3 p{ (c & 1) ? r.hi.x : r.lo.x, (c & 2) ? r.hi.y : r.lo.y, (c & 4) ? r.hi.z : r.lo.z };
                const float cx = vp.m[0][0] * p.x + vp.m[0][1] * p.y + vp.m[0][2] * p.z + vp.m[0][3];
                const float cy = vp.m[1][0] * p.x + vp.m[1][1] * p.y + vp.m[1][2] * p.z + vp.m[1][3];
                const float cw = vp.m[3][0] * p.x + vp.m[3][1] * p.y + vp.m[3][2] * p.z + vp.m[3][3];
                if (cw <= main.view.nearPlane)
                {
                    behind = true;
                    continue;
                }
                visible = true;
                const float sx = (cx / cw * 0.5f + 0.5f) * width, sy = (0.5f - cy / cw * 0.5f) * height;
                x0 = std::min(x0, sx);
                x1 = std::max(x1, sx);
                y0 = std::min(y0, sy);
                y1 = std::max(y1, sy);
            }
            if (!visible && !behind) return;
            if (behind) x0 = y0 = 0, x1 = (float)width, y1 = (float)height;  // crosses the near plane: whole view
            // The origin on the 8 x 8 grid: a classification tile is one tile of the view's tile mask (ReflectionClassify).
            const uint32_t ix0 = (uint32_t)std::clamp(std::floor(x0) - 1, 0.0f, (float)width) & ~7u, iy0 = (uint32_t)std::clamp(std::floor(y0) - 1, 0.0f, (float)height) & ~7u;
            const uint32_t ix1 = (uint32_t)std::clamp(std::ceil(x1) + 1, 0.0f, (float)width), iy1 = (uint32_t)std::clamp(std::ceil(y1) + 1, 0.0f, (float)height);
            const uint64_t rect = (uint64_t)(ix1 - ix0) * (iy1 - iy0);
            if (ix1 <= ix0 || iy1 <= iy0 || (double)rect < minPixels) return;  // the rectangle bounds the count too
            const bool current = m_planeLastSeen[k] + 1 == frame && m_planeRunStart[k] + fc.framesInFlight <= frame;
            // The view's cost: measured within the last second, else the model a + b x mirror pixels (the rectangle, an
            // upper bound, until the plane has a current count; it is not eligible before).
            const bool measured = m_planeViewFrame[k] != UINT64_MAX && frame < m_planeViewFrame[k] + 60;
            const double viewCost = measured ? m_planeViewMs[k] * 1e6 : viewFixedNs + (double)viewNs * (current ? m_planePixels[k] : rect);
            const double rayCost = (double)rayNs * m_planePixels[k];
            const bool hadCamera = m_planeCameraFrame[k] + 1 == frame;
            const bool cheaper = hadCamera ? viewCost < rayCost * 1.1 : viewCost * 1.1 < rayCost;  // hysteresis
            candidates.push_back({ k, ix0, iy0, ix1 - ix0, iy1 - iy0, current && (m_planarForced ? m_planePixels[k] > 0 : cheaper) });
        };
        uint32_t stack[64], top = 0;
        stack[top++] = 0;
        while (top)
        {
            const PlaneNode& node = m_planeNodes[stack[--top]];
            if (distance2(node.lo, node.hi) > node.maxArea * reach) continue;
            if (node.count)
                for (uint32_t k = node.first; k < node.first + node.count; ++k) consider(m_planeOrder[k]);
            else
            {
                if (top + 2 > 64) fail("R reflections: plane hierarchy deeper than 64");
                stack[top++] = node.first;
                stack[top++] = node.second;
            }
        }
        // Eligible planes by count, then the others by rectangle (they get counted and may qualify framesInFlight later).
        std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
            if (a.eligible != b.eligible) return a.eligible;
            if (a.eligible && m_planePixels[a.plane] != m_planePixels[b.plane]) return m_planePixels[a.plane] > m_planePixels[b.plane];
            return (uint64_t)a.w * a.h > (uint64_t)b.w * b.h;
        });
        if (candidates.size() > kCandidatesMax) candidates.resize(kCandidatesMax);
        for (const Candidate& c : candidates)
        {
            const bool view = fc.services.renderView && c.eligible && planar.views == planar.candidates && planar.views < s.planarViewsMax;
            auto& slotData = planar.planes[planar.candidates];
            slotData.plane = m_planes[c.plane].plane;
            slotData.rect[0] = c.x;
            slotData.rect[1] = c.y;
            slotData.rect[2] = c.w;
            slotData.rect[3] = c.h;
            if (view)
            {
                PlanarView& pv = planarViews[planar.views];
                pv.desc = ViewDesc::planarReflection(main.view, m_planes[c.plane].plane, c.x, c.y, c.w, c.h);
                pv.mask = g.createTexture({ "R planar mirror mask", c.w, c.h, 1, 1, DXGI_FORMAT_R8_UINT });
                pv.tileMask = g.createTexture({ "R planar mirror tile mask", (c.w + 7) / 8, (c.h + 7) / 8, 1, 1, DXGI_FORMAT_R8_UINT });
                pv.desc.planarMask = pv.mask;
                pv.desc.planarTileMask = pv.tileMask;
                m_viewRects[planar.views] = { c.x, c.y, c.w, c.h };
                m_slotViewPlanes[ringSlot].push_back(c.plane);
                m_slotViewPixels[ringSlot].push_back(m_planePixels[c.plane]);
                m_planeCameraFrame[c.plane] = frame;
                m_lastPlanarPixels += m_planePixels[c.plane];
                m_lastRectPixels += c.w * c.h;
                ++planar.views;
            }
            if (m_planeLastSeen[c.plane] + 1 != frame)
            {
                m_planeRunStart[c.plane] = frame;
                m_planePixels[c.plane] = 0;
            }
            m_planeLastSeen[c.plane] = frame;
            slotPlanes.push_back(c.plane);
            ++planar.candidates;
        }
        m_lastSelectMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - selectStart).count();
        m_lastPlanarViews = planar.views;
        m_lastCandidates = planar.candidates;
    }
    m_slotFrame[ringSlot] = fc.frame.frameIndex;
    const uint32_t planarOffset = ringSlot * kPlanarSlotBytes;
    std::memcpy(m_planarMapped + planarOffset, &planar, sizeof planar);
    const BufferRef planarCounts = g.importBuffer(m_planarCounts.Get(), { "R planar counts", kCandidatesMax * 4, 0 });
    const BufferRef planarReadback = g.importBuffer(m_planarReadback.Get(), { "R planar readback", kPlanarSlots * kReadbackStride, 0 });
    const uint64_t readbackOffset = (uint64_t)ringSlot * kReadbackStride;
    const uint32_t planarSrv = m_planarSrv;

    if (fresh)
        g.addPass("r.refl.history.clear", QueueType::Compute, [&](PassBuilder& b) { b.use(history, Use::UavCompute); },
                  [&shaders, history, width, height](PassContext& c) {
                      const uint32_t k[4] = { c.uav(history), width, height, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionHistoryClear"));
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                  });
    g.addPass("r.refl.begin", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(reflection, Use::UavCompute);
                  b.use(args, Use::UavCompute);
                  b.use(planarCounts, Use::UavCompute);
              },
              [&shaders, reflection, args, planarCounts, tilesX, tilesY, height](PassContext& c) {
                  const uint32_t k[8] = { c.uav(reflection), c.uav(args), tilesX, tilesY, height, c.uav(planarCounts), 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionBegin"));
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((std::max(tilesX, kCandidatesMax) + 7) / 8, (tilesY + 7) / 8, 1);
              });
    uint32_t spacingLog2 = 0;  // reflection.g_sample_spacing_px bound (1, 2, 4 or 8)
    while ((2u << spacingLog2) <= s.maxSpacing && spacingLog2 < 3) ++spacingLog2;
    g.addPass("r.refl.classify", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  if (lobes.valid()) b.use(lobes, Use::SrvCompute);
                  b.use(history, Use::SrvCompute);
                  b.use(modes, Use::UavCompute);
                  b.use(jobs, Use::UavCompute);
                  b.use(args, Use::UavCompute);
                  b.use(reflection, Use::UavCompute);
                  b.use(planarCounts, Use::UavCompute);
                  for (uint32_t v = 0; v < planar.views; ++v)
                  {
                      b.use(planarViews[v].mask, Use::UavCompute);
                      b.use(planarViews[v].tileMask, Use::UavCompute);
                  }
              },
              [&shaders, depth, gbuffer, lobes, history, modes, jobs, args, reflection, s, focal, width, height, tilesX, tilesY, frameConstants, planarSrv,
               planarOffset, planarCounts, planarViews, viewCount = planar.views, spacingLog2](PassContext& c) {
                  uint32_t k[28] = { c.srv(depth), c.srv(gbuffer), lobes.valid() ? c.srv(lobes) : 0xFFFFFFFFu, c.srv(history),
                                     c.uav(modes), c.uav(jobs), c.uav(args), c.uav(reflection),
                                     asU(s.kHalfAngle), asU(s.mirrorRoughness), asU(focal), height,
                                     width, height, planarSrv, planarOffset, c.uav(planarCounts), spacingLog2, 0, 0 };
                  for (uint32_t v = 0; v < kPlanarMax; ++v)
                  {
                      k[20 + v] = v < viewCount ? c.uav(planarViews[v].mask) : 0xFFFFFFFFu;
                      k[24 + v] = v < viewCount ? c.uav(planarViews[v].tileMask) : 0xFFFFFFFFu;
                  }
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionClassify"));
                  c.computeConstants(k, 28);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(tilesX, tilesY, 1);
              });
    // Mask aprons (v1.28): 3 x 3 dilation of each view's mirror pixels as value 2, tile masks from the dilated masks.
    for (uint32_t v = 0; v < planar.views; ++v)
    {
        const TextureRef mask = planarViews[v].mask, tileMask = planarViews[v].tileMask;
        const uint32_t w = planarViews[v].desc.width, h = planarViews[v].desc.height;
        g.addPass("r.refl.planar.apron", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(mask, Use::UavCompute);
                      b.use(tileMask, Use::UavCompute);
                  },
                  [&shaders, mask, tileMask, w, h](PassContext& c) {
                      const uint32_t k[4] = { c.uav(mask), c.uav(tileMask), w, h };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionPlanarApron"));
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                  });
    }
    // The reflection cameras (their masks come from the classification and the aprons above).
    for (uint32_t v = 0; v < planar.views; ++v)
    {
        ID3D12QueryHeap* heap = m_timestamps.Get();
        const uint32_t tick = ringSlot * kTicks + 2 + 2 * v;
        g.addPass("r.refl.view.begin", QueueType::Graphics, [&](PassBuilder& b) { b.keep(); },
                  [heap, tick](PassContext& pc) { pc.cmd->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, tick); });
        planarColor[v] = fc.services.renderView(fc, planarViews[v].desc).color;
        g.addPass("r.refl.view.end", QueueType::Graphics, [&](PassBuilder& b) { b.keep(); },
                  [heap, tick](PassContext& pc) { pc.cmd->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, tick + 1); });
    }
    // Rays buffer of the split passes (ReflectionRay.hlsli): header, hit records, ray -> job, values, shadow rays.
    const uint32_t rayCapacity = (s.experimentDisable & 64) ? 0 : m_rayCapacity;
    static_assert(16 + (1ull << 25) * 56 <= (1ull << 31), "the rays buffer must fit one raw view");  // 64: every job inline (A/B of the split)
    const BufferRef raysBuffer = g.createBuffer({ "R reflection rays", 16 + (uint64_t)rayCapacity * 56, 0 });  // REFL_RAYS_SLOT_BYTES
    g.addPass("r.refl.args", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::UavCompute);
                  b.use(raysBuffer, Use::UavCompute);
              },
              [&shaders, args, raysBuffer, rayCapacity](PassContext& c) {
                  const uint32_t k[8] = { c.uav(args), 2, kDescStride, (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width), c.uav(raysBuffer), rayCapacity, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionArgs"));
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch(1, 1, 1);
              });

    uint32_t scene[8];
    rays.rootConstants(scene);
    const FrameResources& fr = fc.resources;
    const bool atmosphere = fr.transmittanceLut.valid() && fr.multiScatterLut.valid() && fr.skyViewLut.valid() && fr.aerialPerspective.valid();
    const TextureRef luts[4] = { fr.transmittanceLut, fr.multiScatterLut, fr.skyViewLut, fr.aerialPerspective };
    const int variant = atmosphere ? 0 : 1;
    rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(kTraceLibrary[variant], { "ReflectionTraceGen" }));
    const float3 sky = m_skyRadiance, sun = m_sunIlluminance;
    const float rayLength = (float)fc.quality.number("gi.ray_length_m");
    const uint32_t frame = (uint32_t)fc.frame.frameIndex;
    ID3D12Resource* argumentResource = m_arguments.Get();
    const uint32_t experiment = s.experimentDisable;
    const BufferRef exactCounts = rays.exactHitCounts();
    const TextureRef probeMaps = main.screenProbeMaps;
    const rt::RayScene::VsmRefs vsm = rt::RayScene::vsmRefs(fc.resources);  // S's shadowPages recorded before
    rt::RayScene* rayScene = &rays;
    const uint64_t frameIndex = fc.frame.frameIndex;
    ID3D12QueryHeap* timestamps = m_timestamps.Get();
    const uint32_t firstTick = ringSlot * kTicks;
    // Root constants shared by the trace, shade, shadow and combine passes (ReflectionRay.hlsli).
    auto constantsFor = [jobs, results, modes, probes, depth, gbuffer, cache, luts, atmosphere, sky, sun, rayLength, s, frame, scene, experiment, exactCounts,
                         probeMaps, vsm, rayScene, frameIndex, raysBuffer](PassContext& c, uint32_t k[32]) {
        k[0] = c.srv(jobs);
        k[1] = c.uav(results);
        k[2] = c.srv(modes);
        k[3] = c.srv(probes);
        k[4] = asU(sky.x);
        k[5] = asU(sky.y);
        k[6] = asU(sky.z);
        k[7] = asU(rayLength);
        for (int i = 0; i < 4; ++i) k[8 + i] = atmosphere ? c.srv(luts[i]) : 0xFFFFFFFFu;
        k[12] = asU(sun.x);
        k[13] = asU(sun.y);
        k[14] = asU(sun.z);
        k[15] = c.srv(probeMaps);
        k[16] = c.srv(depth);
        k[17] = c.srv(gbuffer);
        k[18] = c.uav(cache);
        k[19] = s.raysPerSample;
        k[20] = (frame & 0xFFFFFFu) | (experiment << 24);
        k[21] = c.uav(raysBuffer);
        k[22] = rayScene->vsmSrvs(c, vsm, frameIndex, 1);  // S's VSM for sun visibility at hits (UNX_NONE: rays)
        k[23] = exactCounts.valid() ? c.uav(exactCounts) : 0xFFFFFFFFu;
        std::memcpy(&k[24], scene, sizeof scene);
    };
    // Every resource constantsFor names, declared by each pass that binds it (all-shading uses cover DispatchRays and compute).
    auto declareShared = [&](PassBuilder& b) {
        b.use(raysBuffer, Use::UavGraphics);
        b.use(jobs, Use::SrvGraphics);
        b.use(modes, Use::SrvGraphics);
        b.use(probes, Use::SrvGraphics);
        b.use(probeMaps, Use::SrvGraphics);
        b.use(depth, Use::SrvGraphics);
        b.use(gbuffer, Use::SrvGraphics);
        b.use(cache, Use::UavGraphics);
        b.use(results, Use::UavGraphics);
        if (exactCounts.valid()) b.use(exactCounts, Use::UavGraphics);
        rt::RayScene::declareVsm(b, vsm);
        rays.declareTraversal(b);
        if (atmosphere)
            for (const TextureRef& t : luts) b.use(t, Use::SrvGraphics);
    };
    g.addPass("r.refl.trace", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b);
              },
              [&pipeline, constantsFor, frameConstants, argumentResource, variant, timestamps, firstTick](PassContext& c) {
                  uint32_t k[32] = {};
                  constantsFor(c, k);
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstTick);
                  pipeline.dispatchIndirect(c.cmd, argumentResource, 16 + variant * kDescStride);
              });
    // Split passes (ARCHITECTURE 2.6 revision 1): hit shading in compute, off-screen sun visibility, the jobs' values.
    auto rayArgs = [&](const char* name, uint32_t stage) {
        g.addPass(name, QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(args, Use::UavCompute);
                      b.use(raysBuffer, Use::UavCompute);
                  },
                  [&shaders, args, raysBuffer, stage](PassContext& c) {
                      const uint32_t k[8] = { c.uav(args), c.uav(raysBuffer), stage, 0, kShadeArgsOffset, kCombineArgsOffset,
                                              kShadowDescOffset + (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width), 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionRayArgs"));
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    };
    rayArgs("r.refl.rayargs", 0);
    ID3D12CommandSignature* dispatchSignature = m_dispatchSignature.Get();
    g.addPass("r.refl.shade", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b);
              },
              [&shaders, constantsFor, frameConstants, argumentResource, variant, dispatchSignature](PassContext& c) {
                  uint32_t k[32] = {};
                  constantsFor(c, k);
                  c.cmd->SetPipelineState(shaders.compute(kShadeKernel[variant]));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->ExecuteIndirect(dispatchSignature, 1, argumentResource, kShadeArgsOffset, nullptr, 0);
              });
    rayArgs("r.refl.shadowargs", 1);
    rt::RayPipeline& shadowPipeline = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(kShadowLibrary, { "ReflectionShadowGen" }));
    g.addPass("r.refl.shadow", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b);
              },
              [&shadowPipeline, constantsFor, frameConstants, argumentResource](PassContext& c) {
                  uint32_t k[32] = {};
                  constantsFor(c, k);
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  shadowPipeline.dispatchIndirect(c.cmd, argumentResource, kShadowDescOffset);
              });
    g.addPass("r.refl.combine", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b);
              },
              [&shaders, constantsFor, frameConstants, argumentResource, dispatchSignature, timestamps, firstTick](PassContext& c) {
                  uint32_t k[32] = {};
                  constantsFor(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionCombine"));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->ExecuteIndirect(dispatchSignature, 1, argumentResource, kCombineArgsOffset, nullptr, 0);
                  c.cmd->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstTick + 1);
              });
    rays.recordExactReadback(fc);  // after the trace: the counts pick next frames' exact set
    const uint32_t planarCount = planar.views;
    g.addPass("r.refl.resolve", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(modes, Use::SrvCompute);
                  b.use(results, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  for (uint32_t k = 0; k < planarCount; ++k) b.use(planarColor[k], Use::SrvCompute);
                  b.use(reflection, Use::UavCompute);
                  b.use(history, Use::UavCompute);
              },
              [&shaders, modes, results, depth, gbuffer, reflection, history, width, height, tilesX, tilesY, frameConstants, planarSrv, planarOffset, planarCount,
               planarColor](PassContext& c) {
                  uint32_t k[16] = { c.srv(modes), c.srv(results), c.srv(depth), c.srv(gbuffer), c.uav(reflection), c.uav(history), height, planarSrv,
                                     width, height, planarOffset, 0, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu };
                  for (uint32_t v = 0; v < planarCount; ++v) k[12 + v] = c.srv(planarColor[v]);
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionResolve"));
                  c.computeConstants(k, 16);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(tilesX, tilesY, 1);
              });
    // One copy into this frame's read-back slot (read framesInFlight later): candidate pixel counts, timestamps (trace,
    // views) and job counters. A single copy-destination use per frame: the read-back heap buffer takes no barrier.
    const uint32_t tickCount = 2 + 2 * planarCount;
    g.addPass("r.refl.readback", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(planarCounts, Use::CopySrc);
                  b.use(args, Use::CopySrc);
                  b.use(planarReadback, Use::CopyDst);
                  b.keep();
              },
              [timestamps, firstTick, tickCount, args, planarCounts, planarReadback, readbackOffset](PassContext& c) {
                  c.cmd->CopyBufferRegion(c.resource(planarReadback), readbackOffset, c.resource(planarCounts), 0, kCandidatesMax * 4);
                  c.cmd->ResolveQueryData(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstTick, tickCount, c.resource(planarReadback), readbackOffset + kTicksOffset);
                  c.cmd->CopyBufferRegion(c.resource(planarReadback), readbackOffset + kJobsOffset, c.resource(args), 0, 16);
              });
}
} // namespace unx::render::refl

namespace unx::render::refl
{
ReflectionSystem::Stats ReflectionSystem::readStats()
{
    m_device.waitIdle();
    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 16;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(m_device.d3d()->CreateCommittedResource3(&rb, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)),
          "reflection stats readback");
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(readback.Get(), 0, m_arguments.Get(), 0, 16);
    m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(cl));
    uint32_t v[4];
    void* mapped = nullptr;
    D3D12_RANGE all{ 0, 16 };
    check(readback->Map(0, &all, &mapped), "map reflection stats");
    std::memcpy(v, mapped, 16);
    D3D12_RANGE none{ 0, 0 };
    readback->Unmap(0, &none);
    const uint32_t largest = m_planePixels.empty() ? 0 : *std::max_element(m_planePixels.begin(), m_planePixels.end());
    return { v[0], v[1], v[2], v[3], m_lastPlanarViews, m_lastPlanarPixels, m_lastCandidates, largest, m_lastSelectMs, m_lastRectPixels, m_rayNs, m_viewNsPerPixel, m_viewFixedNs * 1e-6f,
             m_lastViewMs };
}
} // namespace unx::render::refl
