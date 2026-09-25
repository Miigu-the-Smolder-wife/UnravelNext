// Track entry points of V (visibility) (INTERFACES_KO.md 5.2, 5.3; ARCHITECTURE 2.1).
//
// A cull run (one view, or every view of a raster-service request) walks instances -> per-depth hierarchy roots ->
// hierarchy nodes (one pass per tree level, indirect) -> hierarchy leaves (groups) -> clusters, with the DAG cut test
// (a leaf is reached while its group error projects above the threshold; a cluster is drawn when its own error
// projects at or below it), frustum, clip plane, normal cone and, for the main view, two-phase HiZ occlusion: phase 1
// tests against the previous frame's HiZ with previous transforms and defers what it rejects; after the phase-1
// raster the HiZ is rebuilt and phase 2 retests the deferred instances, nodes and clusters against it. Only geometry
// that the current frame's own depth hides is ever dropped. Visible clusters are classified into bands A/B/C and
// pipeline lists; the band A lists are drawn by mesh shaders (vis id + depth). With visibility.coverage_layer the main
// view draws bands B and C into the coverage layer instead (CoverageLayer.hlsli): conservative raster with exact pixel
// areas into per-pixel lists, then per pixel a sorted fragment range (INTERFACES 7.1).
#include "unx/render/Tracks.h"

#include "VisibilityInternal.h"
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/clusterbuilder/ClusterHierarchy.h"
#include "unx/core/Log.h"
#include "unx/render/GpuScene.h"
#include "unx/visibility/Visibility.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace unx::render::tracks
{
namespace
{
using namespace unx::visibility::detail;
namespace hier = unx::clusterbuilder::gpu;

constexpr uint32_t kNone = gpu::kNone;
constexpr uint32_t kViewsPerSlot = 4096;  // cull views uploaded per frame (all runs)
constexpr uint32_t kReadbackBytes = 256;

struct Settings
{
    uint32_t capVisible = 0, capNodes = 0, capGroups = 0, capDeferred = 0, capCoverageFragments = 0;
    float lodErrorPx = 0, bandAMinPx = 0, bandCMaxPx = 0;
    bool occlusion = true, coverageLayer = false, coverageBandC = true;

    static Settings load(const QualityConfig& q)
    {
        Settings s;
        s.capVisible = (uint32_t)q.integer("visibility.max_visible_clusters");
        s.capNodes = (uint32_t)q.integer("visibility.max_node_items");
        s.capGroups = (uint32_t)q.integer("visibility.max_group_items");
        s.capDeferred = (uint32_t)q.integer("visibility.max_deferred_items");
        s.lodErrorPx = (float)q.number("visibility.lod_error_px");
        s.bandAMinPx = (float)q.number("visibility.band_a_min_width_px");
        s.bandCMaxPx = (float)q.number("visibility.band_c_max_width_px");
        s.occlusion = q.boolean("visibility.occlusion_culling");
        s.coverageLayer = q.boolean("visibility.coverage_layer");
        s.coverageBandC = q.boolean("visibility.coverage_band_c");
        const int64_t coverage = q.integer("visibility.max_coverage_fragments");
        if (coverage < 1 || coverage >= (1 << 24)) fail("visibility.max_coverage_fragments = %lld: 1 .. 2^24 - 1 (24-bit first-fragment field of the heads)", (long long)coverage);
        s.capCoverageFragments = (uint32_t)coverage;
        return s;
    }
};

// Persistent HiZ of the main view (history for phase 1, product for consumers).
struct Hiz
{
    ComPtr<ID3D12Resource> texture;
    uint32_t width = 0, height = 0, mips = 0;  // valid mip 0 size (half resolution, rounded up); mip m: rounded up again
    uint32_t allocWidth = 0, allocHeight = 0;  // powers of two: D3D mip sizes round down, so the rounded-up chain fits
    uint32_t srv = kNone;
    std::vector<uint32_t> uavs;                // one per mip
    bool history = false;                      // holds a complete HiZ of the previous frame
};

// Persistent coverage layer resources of the main view (CoverageLayer.hlsli): heads and the pixel buffer (header + list).
struct Coverage
{
    ComPtr<ID3D12Resource> heads, pixels;
    uint32_t width = 0, height = 0;
    bool fresh = true;  // heads and header still to be zeroed (CoverageBuild MODE 4)
};

struct State
{
    Device* device = nullptr;
    ComPtr<ID3D12CommandSignature> dispatchSignature, meshSignature;
    ComPtr<ID3D12Resource> upload;
    uint8_t* uploadMapped = nullptr;
    uint32_t slots = 0;
    uint64_t frame = UINT64_MAX;
    uint32_t viewsUsed = 0, runsUsed = 0;
    std::vector<std::vector<uint32_t>> runSrvs;  // per slot: view SRVs of the runs of that frame
    struct StatsRun  // cull state readback of a named run ("main" or a raster request), one slot per frame in flight
    {
        ComPtr<ID3D12Resource> readback;
        uint8_t* mapped = nullptr;
        std::vector<uint64_t> frames;
        visibility::Stats latest;
        bool overflowReported = false;
    };
    std::map<std::string, StatsRun> stats;
    Hiz mainHiz;
    Coverage mainCoverage;
    D3D12_GPU_VIRTUAL_ADDRESS mainFrameConstants = 0;  // this frame's main view constants (scene indices, time, wind)
    uint64_t mainFrameConstantsFrame = UINT64_MAX;
    uint32_t sceneRevision = UINT32_MAX;
    uint32_t traversalLevels = 0;  // deepest per-depth tree of any mesh (node passes per phase)

    ~State()
    {
        if (!device) return;
        DescriptorHeaps& h = device->descriptors();
        for (auto& slot : runSrvs)
            for (uint32_t i : slot) h.freeResource(i);
        if (mainHiz.srv != kNone) h.freeResource(mainHiz.srv);
        for (uint32_t i : mainHiz.uavs) h.freeResource(i);
        if (upload) upload->Unmap(0, nullptr);
        for (auto& [name, run] : stats)
            if (run.readback) run.readback->Unmap(0, nullptr);
    }
};

// IEEE half bits of a positive normal float (round to nearest even): band thresholds share one root constant.
uint32_t halfBits(float f)
{
    if (!(f >= 6.2e-5f && f <= 65504.0f)) fail("V: band threshold %g outside the half-float normal range", f);
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t exponent = ((u >> 23) & 0xFF) - 127 + 15, mantissa = u & 0x7FFFFF;
    uint32_t h = exponent << 10 | mantissa >> 13;
    const uint32_t rest = mantissa & 0x1FFF;
    if (rest > 0x1000 || (rest == 0x1000 && (h & 1))) ++h;
    return h;
}

ComPtr<ID3D12Resource> createBuffer(Device& d, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{ type };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(d.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "V buffer");
    r->SetName(name);
    return r;
}

ComPtr<ID3D12CommandSignature> signature(Device& d, D3D12_INDIRECT_ARGUMENT_TYPE type)
{
    D3D12_INDIRECT_ARGUMENT_DESC a{};
    a.Type = type;
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    sd.ByteStride = 12;
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &a;
    ComPtr<ID3D12CommandSignature> s;
    check(d.d3d()->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&s)), "V command signature");
    return s;
}

State& state(FramePassContext& fc)
{
    State& s = fc.state<State>("v.state");
    if (s.device) return s;
    s.device = &fc.device;
    s.slots = std::max(fc.framesInFlight, 1u);
    s.dispatchSignature = signature(fc.device, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
    s.meshSignature = signature(fc.device, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH);
    s.upload = createBuffer(fc.device, (uint64_t)s.slots * kViewsPerSlot * sizeof(CullView), D3D12_HEAP_TYPE_UPLOAD, L"V cull views");
    D3D12_RANGE none{ 0, 0 };
    check(s.upload->Map(0, &none, reinterpret_cast<void**>(&s.uploadMapped)), "map V cull views");
    s.runSrvs.resize(s.slots);
    return s;
}

// Deepest per-depth tree over all meshes (the node passes each phase needs), and the 24-bit packing limits.
void refreshScene(State& s, FramePassContext& fc)
{
    if (s.sceneRevision == fc.scene.revision()) return;
    const ClusterData& d = fc.scene.clusters();
    const ClusterData::Named* nodesNamed = nullptr;
    const ClusterData::Named* rootsNamed = nullptr;
    for (const auto& n : d.named)
    {
        if (n.name == clusterbuilder::kClusterNodes) nodesNamed = &n;
        if (n.name == clusterbuilder::kMeshClusterRoots) rootsNamed = &n;
    }
    if (!nodesNamed || !rootsNamed) fail("V: the scene has no cluster hierarchy (install clusterbuilder::build output with GpuScene::setClusters)");
    std::vector<hier::ClusterNode> nodes(nodesNamed->bytes.size() / sizeof(hier::ClusterNode));
    std::vector<hier::MeshClusterRoots> roots(rootsNamed->bytes.size() / sizeof(hier::MeshClusterRoots));
    if (!nodes.empty()) std::memcpy(nodes.data(), nodesNamed->bytes.data(), nodesNamed->bytes.size());
    if (!roots.empty()) std::memcpy(roots.data(), rootsNamed->bytes.data(), rootsNamed->bytes.size());
    if (nodes.size() >= (1u << 24) || d.clusters.size() >= (1u << 24) || fc.scene.instances().size() >= (1u << 24))
        fail("V: %zu nodes / %zu clusters / %zu instances exceed the 24-bit work-item fields", nodes.size(), d.clusters.size(), fc.scene.instances().size());
    uint32_t deepest = 0;
    std::vector<std::pair<uint32_t, uint32_t>> stack;
    for (const auto& r : roots)
        for (uint32_t k = 0; k < r.rootCount; ++k) stack.push_back({ r.nodeOffset + k, 1 });
    while (!stack.empty())
    {
        const auto [node, depth] = stack.back();
        stack.pop_back();
        deepest = std::max(deepest, depth);
        if (!nodes[node].leaf)
            for (uint32_t c = 0; c < nodes[node].count; ++c) stack.push_back({ nodes[node].first + c, depth + 1 });
    }
    // Alpha-tested materials are cut by their baseColor texture on the GPU (AlphaTest.hlsli). A texture the GPU scene
    // does not carry (M's prepareScene not run: a build without M, or a context built without FrameRenderer) leaves the
    // material covering its whole triangles; said once, so results carry it.
    if (const scene::Scene* src = fc.scene.source())
    {
        uint32_t untextured = 0;
        const auto& gpuMaterials = fc.scene.materials();
        for (size_t i = 0; i < src->materials.size() && i < gpuMaterials.size(); ++i)
            if (src->materials[i].alphaCutoff > 0 && src->materials[i].baseColorTexture != UINT32_MAX && gpuMaterials[i].baseColorTexture == gpu::kNone) ++untextured;
        if (untextured) logf("V: %u alpha-tested materials have a baseColor texture that is not in the GPU scene (M.prepareScene not run): drawn uncut\n", untextured);
    }
    s.traversalLevels = deepest;
    s.sceneRevision = fc.scene.revision();
    s.mainHiz.history = false;
}

float4 normalisedPlane(float4 p)
{
    const float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    if (len < 1e-20f) return { 0, 0, 0, 1 };  // degenerate (e.g. z >= 0 of an infinite reversed-Z projection): always kept
    return { p.x / len, p.y / len, p.z / len, p.w / len };
}

float4 row(const float4x4& m, int r) { return { m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3] }; }
float4 add(float4 a, float4 b, float s) { return { a.x + s * b.x, a.y + s * b.y, a.z + s * b.z, a.w + s * b.w }; }

bool isOrthographic(const float4x4& m) { return m.m[3][0] == 0 && m.m[3][1] == 0 && m.m[3][2] == 0 && m.m[3][3] == 1; }

// Planes of -w <= x, y <= w and 0 <= z <= w (clip = viewProj * p), normalised.
void setPlanes(CullView& v)
{
    const float4 r0 = row(v.viewProj, 0), r1 = row(v.viewProj, 1), r2 = row(v.viewProj, 2), r3 = row(v.viewProj, 3);
    v.planes[0] = normalisedPlane(add(r3, r0, 1));
    v.planes[1] = normalisedPlane(add(r3, r0, -1));
    v.planes[2] = normalisedPlane(add(r3, r1, 1));
    v.planes[3] = normalisedPlane(add(r3, r1, -1));
    v.planes[4] = normalisedPlane(add(r3, r2, -1));  // z <= w
    v.planes[5] = normalisedPlane(r2);               // z >= 0
}

// Perspective eye: the point where rows 0, 1 and 3 of viewProj vanish.
float3 eyeOf(const float4x4& m)
{
    const float a[3][4] = { { m.m[0][0], m.m[0][1], m.m[0][2], -m.m[0][3] }, { m.m[1][0], m.m[1][1], m.m[1][2], -m.m[1][3] }, { m.m[3][0], m.m[3][1], m.m[3][2], -m.m[3][3] } };
    auto det3 = [](float a00, float a01, float a02, float a10, float a11, float a12, float a20, float a21, float a22) {
        return a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
    };
    const float d = det3(a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2]);
    if (std::fabs(d) < 1e-30f) fail("V: raster view projection has no eye point");
    const float x = det3(a[0][3], a[0][1], a[0][2], a[1][3], a[1][1], a[1][2], a[2][3], a[2][1], a[2][2]) / d;
    const float y = det3(a[0][0], a[0][3], a[0][2], a[1][0], a[1][3], a[1][2], a[2][0], a[2][3], a[2][2]) / d;
    const float z = det3(a[0][0], a[0][1], a[0][3], a[1][0], a[1][1], a[1][3], a[2][0], a[2][1], a[2][3]) / d;
    return { x, y, z };
}

// Planar reflection views' tile cull mask (PlanarMask.hlsl). A performance filter: mirror pixels stay exact through the
// depth fill. 64 px tiles keep the cull kernels' coarse-cell walk short (8 px tiles at 4K walked up to ~2000 cells per
// instance, node and cluster test: 3 ms per view [R, measured]).
constexpr uint32_t kPlanarTilePx = 64;

CullView viewOf(const ViewDesc& view, const Settings& cfg, bool occlusion)
{
    CullView v{};
    v.viewProj = view.viewProj;
    v.prevViewProj = view.prevViewProj;
    setPlanes(v);
    v.clipPlane = view.clipPlane;
    v.position = view.position;
    v.lodScale = view.proj.m[1][1] * 0.5f * (float)view.height;  // pixels per metre at distance 1
    v.lodThreshold = cfg.lodErrorPx;
    v.orthographic = 0;
    v.nearPlane = view.nearPlane;
    v.flags = kViewCullBack | (occlusion ? kViewOcclusion : 0);
    v.viewportSize = { (float)view.width, (float)view.height };
    v.cullMaskOffset = kNone;
    if (view.planarMask.valid())  // PlanarMask.hlsl: the view's tile cull mask
    {
        v.cullMaskOffset = 0;
        v.tilePx = kPlanarTilePx;
        v.tilesX = (view.width + kPlanarTilePx - 1) / kPlanarTilePx;
    }
    return v;
}

CullView viewOf(const RasterView& r, const DepthRasterRequest& req, const Settings& cfg)
{
    CullView v{};
    v.viewProj = r.viewProj;
    v.prevViewProj = r.viewProj;
    setPlanes(v);
    v.orthographic = isOrthographic(r.viewProj) ? 1 : 0;
    if (v.orthographic)
    {
        const float3 a{ r.viewProj.m[0][0], r.viewProj.m[0][1], r.viewProj.m[0][2] }, b{ r.viewProj.m[1][0], r.viewProj.m[1][1], r.viewProj.m[1][2] };
        const float3 dir = normalize(cross(a, b));
        v.viewDirection = { dir.x, dir.y, dir.z, 0 };
    }
    else
        v.position = eyeOf(r.viewProj);
    v.lodScale = r.lodPixelsPerMetre;
    v.lodThreshold = cfg.lodErrorPx;
    v.nearPlane = 1e-4f;
    // Back-face cone culling needs to know which side the viewer is on: perspective views only (orthographic views
    // cull back faces in the rasteriser when requested).
    v.flags = (req.cull == D3D12_CULL_MODE_BACK && !v.orthographic) ? kViewCullBack : 0;
    v.viewportSize = { (float)r.viewportWidth, (float)r.viewportHeight };
    v.viewportOffset = { (float)r.viewportX, (float)r.viewportY };
    v.cullMaskOffset = req.cullMask.valid() ? r.cullMaskOffset : kNone;
    if (req.atlasSlots.valid()) v.flags |= kViewTileSingle;
    v.userData = r.userData;
    v.tilePx = std::max(req.cullTilePx, 1u);
    v.tilesX = (r.viewportWidth + v.tilePx - 1) / v.tilePx;
    return v;
}

uint32_t uploadViews(State& s, FramePassContext& fc, const std::vector<CullView>& views)
{
    if (s.frame != fc.frame.frameIndex)
    {
        s.frame = fc.frame.frameIndex;
        s.viewsUsed = 0;
        s.runsUsed = 0;
    }
    if (s.viewsUsed + views.size() > kViewsPerSlot) fail("V: more than %u cull views in one frame", kViewsPerSlot);
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % s.slots);
    const uint32_t first = slot * kViewsPerSlot + s.viewsUsed;
    std::memcpy(s.uploadMapped + (uint64_t)first * sizeof(CullView), views.data(), views.size() * sizeof(CullView));
    std::vector<uint32_t>& srvs = s.runSrvs[slot];
    if (s.runsUsed == srvs.size()) srvs.push_back(fc.device.descriptors().allocateResource());
    const uint32_t srv = srvs[s.runsUsed++];
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.Buffer.FirstElement = first;
    sd.Buffer.NumElements = (UINT)views.size();
    sd.Buffer.StructureByteStride = sizeof(CullView);
    fc.device.d3d()->CreateShaderResourceView(s.upload.Get(), &sd, fc.device.descriptors().resourceCpu(srv));
    s.viewsUsed += (uint32_t)views.size();
    return srv;
}

void ensureHiz(Device& device, Hiz& h, uint32_t width, uint32_t height)
{
    const uint32_t w = (width + 1) / 2, hh = (height + 1) / 2;
    if (h.texture && h.width == w && h.height == hh) return;
    DescriptorHeaps& heaps = device.descriptors();
    if (h.texture) device.deferRelease(h.texture);
    if (h.srv != kNone) heaps.freeResource(h.srv);
    for (uint32_t u : h.uavs) heaps.freeResource(u);
    h.uavs.clear();
    h.width = w;
    h.height = hh;
    h.mips = 1;
    for (uint32_t m = std::max(w, hh); m > 1; m = (m + 1) / 2) ++h.mips;
    auto pow2 = [](uint32_t x) { uint32_t p = 1; while (p < x) p <<= 1; return p; };
    h.allocWidth = pow2(w);
    h.allocHeight = pow2(hh);
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = h.allocWidth;
    rd.Height = h.allocHeight;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = (UINT16)h.mips;
    rd.Format = DXGI_FORMAT_R32_FLOAT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&h.texture)),
          "V HiZ");
    h.texture->SetName(L"V HiZ (main view)");
    h.srv = heaps.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_FLOAT;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = h.mips;
    device.d3d()->CreateShaderResourceView(h.texture.Get(), &sd, heaps.resourceCpu(h.srv));
    for (uint32_t m = 0; m < h.mips; ++m)
    {
        const uint32_t u = heaps.allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ud.Texture2D.MipSlice = m;
        device.d3d()->CreateUnorderedAccessView(h.texture.Get(), nullptr, &ud, heaps.resourceCpu(u));
        h.uavs.push_back(u);
    }
    h.history = false;
}

// Transient buffers of one cull run and everything its kernels need in root constants.
struct Run
{
    BufferRef state, args, nodeItems, groupItems, visible, lists, deferInstances, deferNodes, deferClusters, tileMask;
    BufferRef tilePairs;  // tile-local raster runs: uint3 (visible index, tile rectangle) per list entry
    BufferRef tileCoarse;  // runs with a tile mask: bit per 8 x 8 tiles (TileMaskCoarse.hlsl)
    uint32_t tileCoarseWords = 0;  // per view
    TextureRef hiz;
    uint32_t hizSrv = kNone, hizMips = 0, hizWidth = 0, hizHeight = 0;
    uint32_t viewsSrv = kNone, viewCount = 0, instanceMask = 0;
    uint32_t nodesSrv = kNone, rootsSrv = kNone, spheresSrv = kNone, sheetsSrv = kNone;
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;  // scene indices, time and wind for the kernels (b1)
    uint32_t bandMode = kBandModeA;
    Settings cfg;
    std::string prefix;
};

Run createRun(FramePassContext& fc, const Settings& cfg, const std::string& prefix, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    RenderGraph& g = fc.graph;
    Run r;
    r.cfg = cfg;
    r.prefix = prefix;
    r.frameConstants = frameConstants;
    r.state = g.createBuffer({ "v.cull.state", kStateWords * 4, 0 });
    r.args = g.createBuffer({ "v.cull.args", kArgWords * 4, 0 });
    r.nodeItems = g.createBuffer({ "v.cull.nodes", (uint64_t)cfg.capNodes * 8, 8 });
    r.groupItems = g.createBuffer({ "v.cull.groups", (uint64_t)cfg.capGroups * 8, 8 });
    r.visible = g.createBuffer({ "v.visibleClusters", (uint64_t)cfg.capVisible * 8, 8 });
    r.lists = g.createBuffer({ "v.lists", (uint64_t)cfg.capVisible * 4 * kLists, 0 });
    r.deferInstances = g.createBuffer({ "v.cull.deferredInstances", (uint64_t)cfg.capDeferred * 4, 4 });
    r.deferNodes = g.createBuffer({ "v.cull.deferredNodes", (uint64_t)cfg.capDeferred * 8, 8 });
    r.deferClusters = g.createBuffer({ "v.cull.deferredClusters", (uint64_t)cfg.capDeferred * 8, 8 });
    r.nodesSrv = fc.scene.srv(clusterbuilder::kClusterNodes);
    r.rootsSrv = fc.scene.srv(clusterbuilder::kMeshClusterRoots);
    r.spheresSrv = fc.scene.srv(clusterbuilder::kClusterLodSpheres);
    r.sheetsSrv = fc.scene.srv(clusterbuilder::kClusterSheets);
    return r;
}

void declareCull(PassBuilder& b, const Run& r, Use argsUse)
{
    for (BufferRef x : { r.state, r.nodeItems, r.groupItems, r.visible, r.lists, r.deferInstances, r.deferNodes, r.deferClusters }) b.use(x, Use::UavCompute);
    b.use(r.args, argsUse);
    if (r.hiz.valid()) b.use(r.hiz, Use::SrvCompute);
    if (r.tileMask.valid()) b.use(r.tileMask, Use::SrvCompute);
    if (r.tilePairs.valid()) b.use(r.tilePairs, Use::UavCompute);
    if (r.tileCoarse.valid()) b.use(r.tileCoarse, Use::SrvCompute);
}

void cullConstants(const PassContext& c, const Run& r, uint32_t phase, uint32_t k[32], uint32_t instanceCount)
{
    std::memset(k, 0, 32 * 4);
    k[0] = r.viewsSrv;
    k[1] = c.uav(r.state);
    k[2] = c.uav(r.args);
    k[3] = phase;
    k[4] = c.uav(r.nodeItems);
    k[5] = c.uav(r.groupItems);
    k[6] = c.uav(r.visible);
    k[7] = c.uav(r.lists);
    k[8] = c.uav(r.deferInstances);
    k[9] = c.uav(r.deferNodes);
    k[10] = c.uav(r.deferClusters);
    k[11] = r.hizSrv;
    k[12] = r.hizMips;
    k[13] = r.hizWidth;
    k[14] = r.hizHeight;
    k[15] = r.instanceMask;
    k[16] = r.nodesSrv;
    k[17] = r.rootsSrv;
    k[18] = r.spheresSrv;
    k[19] = r.tileMask.valid() ? c.srv(r.tileMask) : kNone;
    k[20] = r.cfg.capNodes;
    k[21] = r.cfg.capGroups;
    k[22] = r.cfg.capVisible;
    k[23] = r.cfg.capDeferred;
    k[24] = r.viewCount;
    k[25] = instanceCount;
    k[26] = r.bandMode;
    k[27] = r.tilePairs.valid() ? c.uav(r.tilePairs) : kNone;
    k[28] = halfBits(r.cfg.bandAMinPx) | halfBits(r.cfg.bandCMaxPx) << 16;
    k[29] = r.sheetsSrv;
    k[30] = r.tileCoarse.valid() ? c.srv(r.tileCoarse) : kNone;
    k[31] = r.tileCoarseWords;
}

// A cull kernel pass: direct dispatch (groupsX > 0) or indirect through the run's args at 'argWord'. Prepare and
// direct passes write the args (UAV); indirect passes consume them.
void cullPass(FramePassContext& fc, State& s, const Run& r, const std::string& name, const std::string& kernel, uint32_t phase, uint32_t groupsX, uint32_t groupsY,
              uint32_t argWord)
{
    ID3D12PipelineState* pso = fc.shaders.compute(kernel);
    const uint32_t instanceCount = (uint32_t)fc.scene.instances().size();
    ID3D12CommandSignature* sig = s.dispatchSignature.Get();
    fc.graph.addPass(r.prefix + name, QueueType::Graphics, [&](PassBuilder& b) { declareCull(b, r, groupsX > 0 ? Use::UavCompute : Use::IndirectArgs); },
                     [=](PassContext& c) {
                         uint32_t k[32];
                         cullConstants(c, r, phase, k, instanceCount);
                         c.cmd->SetPipelineState(pso);
                         c.bindFrameConstants(r.frameConstants);
                         c.computeConstants(k, 32);
                         if (groupsX > 0) c.cmd->Dispatch(groupsX, groupsY, 1);
                         else c.cmd->ExecuteIndirect(sig, 1, c.resource(r.args), argWord * 4, nullptr, 0);
                     });
}

// Coarse summary of a run's tile mask (8 x 8 tiles per bit, 64 cells = two words per group): the cull kernels visit
// only the set cells under a bounding rectangle.
void tileCoarsePass(FramePassContext& fc, Run& r, const std::vector<CullView>& views)
{
    uint32_t cells = 1;
    for (const CullView& v : views)
    {
        const uint32_t tilesY = ((uint32_t)v.viewportSize.y + v.tilePx - 1) / v.tilePx;
        cells = std::max(cells, ((v.tilesX + 7) / 8) * ((tilesY + 7) / 8));
    }
    const uint32_t groups = (cells + 63) / 64;
    r.tileCoarseWords = groups * 2;
    r.tileCoarse = fc.graph.createBuffer({ "v.cull.tileCoarse", (uint64_t)r.tileCoarseWords * r.viewCount * 4, 0 });
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Visibility/TileMaskCoarse");
    const BufferRef mask = r.tileMask, coarse = r.tileCoarse;
    const uint32_t viewsSrv = r.viewsSrv, words = r.tileCoarseWords, viewCount = r.viewCount;
    fc.graph.addPass(r.prefix + "tilemask", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(mask, Use::SrvCompute);
                         b.use(coarse, Use::UavComputeDisjoint);
                     },
                     [=](PassContext& c) {
                         const uint32_t k[4] = { viewsSrv, c.srv(mask), c.uav(coarse), words };
                         c.cmd->SetPipelineState(pso);
                         c.computeConstants(k, 4);
                         c.cmd->Dispatch(groups, viewCount, 1);
                     });
}

// Planar reflection view mask (ViewDesc::planarMask) -> the run's tile cull mask (kPlanarTilePx tiles; PlanarMask.hlsl).

void planarTileMask(FramePassContext& fc, Run& r, const ViewDesc& view)
{
    const uint32_t tilesX = (view.width + kPlanarTilePx - 1) / kPlanarTilePx, tilesY = (view.height + kPlanarTilePx - 1) / kPlanarTilePx;
    const uint32_t words = (tilesX * tilesY + 31) / 32;
    r.tileMask = fc.graph.createBuffer({ "v.cull.planarTiles", (uint64_t)words * 4, 0 });
    const BufferRef bits = r.tileMask;
    const TextureRef mask = view.planarMask;
    const uint32_t width = view.width, height = view.height;
    for (uint32_t mode = 0; mode < 2; ++mode)
    {
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Visibility/PlanarMask.MODE" + std::to_string(mode));
        fc.graph.addPass(r.prefix + (mode == 0 ? "planar.clear" : "planar.tiles"), QueueType::Graphics,
                         [&](PassBuilder& b) {
                             if (mode == 1) b.use(mask, Use::SrvCompute);
                             b.use(bits, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             const uint32_t k[8] = { mode == 1 ? c.srv(mask) : kNone, c.uav(bits), width, height, tilesX, words, kPlanarTilePx, 0 };
                             c.cmd->SetPipelineState(pso);
                             c.computeConstants(k, 8);
                             if (mode == 0) c.cmd->Dispatch((words + 63) / 64, 1, 1);
                             else c.cmd->Dispatch(tilesX, tilesY, 1);
                         });
    }
}

// One cull phase: instances (phase 1: all x views; phase 2: deferred), the traversal levels, the cluster pass(es) and
// the draw arguments of the lists for this phase.
void cullPhase(FramePassContext& fc, State& s, const Run& r, uint32_t phase)
{
    const uint32_t instances = (uint32_t)fc.scene.instances().size();
    const std::string p = std::to_string(phase);
    if (phase == 1)
    {
        cullPass(fc, s, r, "reset", "Passes/Visibility/CullReset", phase, 1, 1, 0);
        cullPass(fc, s, r, "instances.p1", "Passes/Visibility/CullInstances.PHASE1", phase, std::max((instances + 63) / 64, 1u), r.viewCount, 0);
    }
    else
    {
        cullPass(fc, s, r, "prepare.p2", "Passes/Visibility/CullPrepare.MODE2", phase, 1, 1, 0);
        cullPass(fc, s, r, "instances.p2", "Passes/Visibility/CullInstances.PHASE2", phase, 0, 0, kArgDeferredInstances);
        cullPass(fc, s, r, "seed.p2", "Passes/Visibility/CullSeed", phase, 0, 0, kArgSeedNodes);
    }
    for (uint32_t level = 0; level < s.traversalLevels; ++level)
    {
        cullPass(fc, s, r, "prepare.nodes.p" + p + "." + std::to_string(level), "Passes/Visibility/CullPrepare.MODE0", phase, 1, 1, 0);
        cullPass(fc, s, r, "nodes.p" + p + "." + std::to_string(level), "Passes/Visibility/CullNodes.PHASE" + p, phase, 0, 0, kArgNodes);
    }
    cullPass(fc, s, r, "prepare.groups.p" + p, "Passes/Visibility/CullPrepare.MODE1", phase, 1, 1, 0);
    cullPass(fc, s, r, "clusters.p" + p, "Passes/Visibility/CullClusters.MODE0", phase, 0, 0, kArgGroups);
    if (phase == 2) cullPass(fc, s, r, "clusters.deferred.p2", "Passes/Visibility/CullClusters.MODE1", phase, 0, 0, kArgDeferredClusters);
    cullPass(fc, s, r, "prepare.draw.p" + p, "Passes/Visibility/CullPrepare.MODE3", phase, 1, 1, 0);
}

ID3D12PipelineState* visPipeline(FramePassContext& fc, uint32_t list, bool mirrored)
{
    const bool back = list == kListABack || list == kListAAlphaBack;
    const bool alpha = list == kListAAlphaBack || list == kListAAlphaNone;
    MeshPipelineDesc d;
    d.meshShader = alpha ? "Passes/Visibility/VisRaster.ms.ALPHA1" : "Passes/Visibility/VisRaster.ms.ALPHA0";
    d.pixelShader = alpha ? "Passes/Visibility/VisRaster.ps.ALPHA1" : "Passes/Visibility/VisRaster.ps.ALPHA0";
    d.renderTargets = { DXGI_FORMAT_R32_UINT };
    d.depthFormat = DXGI_FORMAT_D32_FLOAT;
    d.cull = back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.frontCounterClockwise = !mirrored;
    return fc.shaders.mesh(std::string("v.vis|") + (back ? "back" : "none") + (alpha ? "|alpha" : "") + (mirrored ? "|mirrored" : ""), d);
}

void rasterPass(FramePassContext& fc, State& s, const Run& r, ViewResources& view, uint32_t phase)
{
    ID3D12PipelineState* pso[kAListCount];
    for (uint32_t l = 0; l < kAListCount; ++l) pso[l] = visPipeline(fc, l, view.view.mirrored);
    ID3D12CommandSignature* sig = s.meshSignature.Get();
    const uint32_t width = view.view.width, height = view.view.height;
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = view.frameConstants;
    const TextureRef depth = view.depth, visId = view.visId;
    // Planar reflection views: pixels that are not mirror pixels get the nearest depth first (PlanarFill), so every
    // fragment there fails the early depth test and they stay VIS_NONE.
    const TextureRef planarMask = phase == 1 ? view.view.planarMask : TextureRef{};
    ID3D12PipelineState* fillPso = nullptr;
    if (planarMask.valid())
    {
        MeshPipelineDesc d;
        d.meshShader = "Passes/Visibility/PlanarFill.ms";
        d.pixelShader = "Passes/Visibility/PlanarFill.ps";
        d.renderTargets = { DXGI_FORMAT_R32_UINT };
        d.depthFormat = DXGI_FORMAT_D32_FLOAT;
        d.depthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        d.cull = D3D12_CULL_MODE_NONE;
        fillPso = fc.shaders.mesh("v.planarfill", d);
    }
    fc.graph.addPass(r.prefix + "raster.p" + std::to_string(phase), QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(r.args, Use::IndirectArgs);
                         b.use(r.visible, Use::SrvGraphics);
                         b.use(r.lists, Use::SrvGraphics);
                         b.use(r.state, Use::SrvGraphics);
                         b.use(depth, Use::DepthWrite);
                         b.use(visId, Use::RenderTarget);
                         if (planarMask.valid()) b.use(planarMask, Use::SrvGraphics);
                     },
                     [=](PassContext& c) {
                         const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(visId), dsv = c.dsv(depth);
                         if (phase == 1)
                         {
                             const float none[4] = { 0, 0, 0, 0 };  // VIS_NONE (VisBuffer.hlsli)
                             c.cmd->ClearRenderTargetView(rtv, none, 0, nullptr);
                             c.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);  // reversed Z: far = 0
                         }
                         c.cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                         const D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
                         const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };
                         c.cmd->RSSetViewports(1, &vp);
                         c.cmd->RSSetScissorRects(1, &sc);
                         if (fillPso)
                         {
                             const uint32_t k[4] = { c.srv(planarMask), 0, 0, 0 };
                             c.cmd->SetPipelineState(fillPso);
                             c.graphicsConstants(k, 4);
                             c.cmd->DispatchMesh(1, 1, 1);
                         }
                         c.bindFrameConstants(frameConstants);
                         for (uint32_t l = 0; l < kAListCount; ++l)  // bands B and C: empty lists or the coverage layer
                         {
                             const uint32_t k[8] = { c.srv(r.visible), c.srv(r.lists), c.srv(r.state), l, phase, r.cfg.capVisible, r.viewsSrv, 0 };
                             c.cmd->SetPipelineState(pso[l]);
                             c.graphicsConstants(k, 8);
                             c.cmd->ExecuteIndirect(sig, 1, c.resource(r.args), (kArgMesh + 3 * l) * 4, nullptr, 0);
                         }
                     });
}

// HiZ from the depth buffer: five levels per pass (HiZ.hlsl), each pass after the previous one's writes.
void hizPasses(FramePassContext& fc, const Hiz& h, TextureRef hizRef, TextureRef depth, uint32_t width, uint32_t height, const std::string& tag)
{
    auto mipSize = [](uint32_t m, uint32_t base) { return (base + (1u << m) - 1) >> m; };
    for (uint32_t first = 0; first < h.mips; first += 5)
    {
        const bool fromDepth = first == 0;
        ID3D12PipelineState* pso = fc.shaders.compute(fromDepth ? "Passes/Visibility/HiZ.FIRST1" : "Passes/Visibility/HiZ.FIRST0");
        const uint32_t levels = std::min(5u, h.mips - first);
        const uint32_t w0 = mipSize(first, h.width), h0 = mipSize(first, h.height);
        const uint32_t srcW = fromDepth ? width : mipSize(first - 1, h.width), srcH = fromDepth ? height : mipSize(first - 1, h.height);
        const std::vector<uint32_t> uavs = h.uavs;
        fc.graph.addPass("v.hiz." + tag + "." + std::to_string(first), QueueType::Graphics,
                         [&](PassBuilder& b) {
                             if (fromDepth) b.use(depth, Use::SrvCompute);
                             b.use(hizRef, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             uint32_t k[12] = {};
                             k[0] = fromDepth ? c.srv(depth) : uavs[first - 1];
                             k[1] = srcW;
                             k[2] = srcH;
                             k[3] = levels;
                             for (uint32_t l = 0; l < 5; ++l)
                             {
                                 const uint32_t u = l < levels ? uavs[first + l] : uavs[first];
                                 if (l < 4) k[4 + l] = u;
                                 else k[8] = u;
                             }
                             k[9] = w0;
                             k[10] = h0;
                             c.cmd->SetPipelineState(pso);
                             c.computeConstants(k, 12);
                             c.cmd->Dispatch((w0 + 15) / 16, (h0 + 15) / 16, 1);
                         });
    }
}

void ensureCoverage(Device& device, Coverage& cv, uint32_t width, uint32_t height)
{
    if (cv.heads && cv.width == width && cv.height == height) return;
    if (cv.heads) device.deferRelease(cv.heads);
    if (cv.pixels) device.deferRelease(cv.pixels);
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32_UINT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&cv.heads)),
          "V coverage heads");
    cv.heads->SetName(L"V coverage heads (main view)");
    rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (uint64_t)(kCovPixelList + (uint64_t)width * height) * 4;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&cv.pixels)),
          "V coverage pixels");
    cv.pixels->SetName(L"V coverage pixels (main view)");
    cv.width = width;
    cv.height = height;
    cv.fresh = true;
}

// Coverage layer of the main view (bands B and C until the bricks exist; CoverageLayer.hlsli), after the final HiZ:
// clear last frame's heads, conservative raster with exact pixel areas into per-pixel linked lists, then per pixel a
// sorted range. Products: ViewResources::coverageHeads, coverageFragments, coveragePixels (INTERFACES 7.1).
void coveragePasses(FramePassContext& fc, State& s, const Run& r, ViewResources& view)
{
    const uint32_t width = view.view.width, height = view.view.height;
    Coverage& cv = s.mainCoverage;
    ensureCoverage(fc.device, cv, width, height);
    RenderGraph& g = fc.graph;
    const uint32_t capFragments = r.cfg.capCoverageFragments, capPixels = width * height;
    const TextureRef heads = g.importTexture(cv.heads.Get(), { "v.coverage.heads", width, height, 1, 1, DXGI_FORMAT_R32_UINT }, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    const BufferRef pixels = g.importBuffer(cv.pixels.Get(), { "v.coverage.pixels", (uint64_t)(kCovPixelList + (uint64_t)capPixels) * 4, 0 });
    const BufferRef raw = g.createBuffer({ "v.coverage.raw", (uint64_t)capFragments * kCoverageRawBytes, kCoverageRawBytes });
    const BufferRef sorted = g.createBuffer({ "v.coverage.fragments", (uint64_t)capFragments * kCoverageFragmentBytes, kCoverageFragmentBytes });
    view.coverageHeads = heads;
    view.coverageFragments = sorted;
    view.coveragePixels = pixels;
    const uint32_t hizSrv = s.mainHiz.srv, hizSize = s.mainHiz.width | s.mainHiz.height << 16;
    const float frontSign = view.view.mirrored ? -1.0f : 1.0f;
    const Run run = r;
    // Root constants (CoverageLayer.hlsli); resources a pass does not declare are kNone.
    auto constants = [=](const PassContext& c, uint32_t k[20], bool rawUsed, bool sortedUsed, bool raster) {
        std::memset(k, 0, 20 * 4);
        k[0] = c.uav(run.state);
        k[1] = raster ? kNone : c.uav(run.args);
        k[2] = rawUsed ? c.uav(raw) : kNone;
        k[3] = sortedUsed ? c.uav(sorted) : kNone;
        k[4] = c.uav(heads);
        k[5] = c.uav(pixels);
        k[6] = capFragments;
        k[7] = capPixels;
        k[8] = hizSrv;
        k[9] = width;
        k[10] = height;
        k[11] = hizSize;
        k[12] = raster ? c.srv(run.visible) : kNone;
        k[13] = raster ? c.srv(run.lists) : kNone;
        k[14] = run.cfg.capVisible;
        k[15] = run.viewsSrv;
        std::memcpy(&k[16], &frontSign, 4);
    };
    ID3D12CommandSignature* dispatchSig = s.dispatchSignature.Get();
    ID3D12CommandSignature* meshSig = s.meshSignature.Get();
    // Compute pass of CoverageBuild: direct (groupsX > 0) or indirect at argWord.
    auto build = [&](const char* name, uint32_t mode, uint32_t groupsX, uint32_t groupsY, uint32_t argWord, bool rawUsed, bool sortedUsed) {
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Visibility/CoverageBuild.MODE" + std::to_string(mode));
        const bool indirect = groupsX == 0;
        g.addPass(std::string("v.coverage.") + name, QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(run.state, Use::UavCompute);
                      b.use(run.args, indirect ? Use::IndirectArgs : Use::UavCompute);
                      b.use(heads, Use::UavCompute);
                      b.use(pixels, Use::UavCompute);
                      if (rawUsed) b.use(raw, Use::UavCompute);
                      if (sortedUsed) b.use(sorted, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      uint32_t k[20];
                      constants(c, k, rawUsed, sortedUsed, false);
                      if (indirect) k[1] = kNone;  // the args are read as indirect arguments here
                      c.cmd->SetPipelineState(pso);
                      c.computeConstants(k, 20);
                      if (indirect) c.cmd->ExecuteIndirect(dispatchSig, 1, c.resource(run.args), argWord * 4, nullptr, 0);
                      else c.cmd->Dispatch(groupsX, groupsY, 1);
                  });
    };
    if (cv.fresh)
    {
        build("init", 4, (width + 7) / 8, (height + 7) / 8, 0, false, false);
        cv.fresh = false;
    }
    build("prepare", 0, 1, 1, 0, false, false);
    build("clear", 1, 0, 0, kArgCovReset, false, false);

    MeshPipelineDesc d;
    d.meshShader = "Passes/Visibility/CoverageRaster.ms";
    d.pixelShader = "Passes/Visibility/CoverageRaster.ps";
    d.depthFormat = DXGI_FORMAT_UNKNOWN;
    d.depthWrite = false;
    d.cull = D3D12_CULL_MODE_NONE;  // one-sided back faces are culled by the mesh kernel (with the near clip)
    d.conservative = true;
    ID3D12PipelineState* rasterPso = fc.shaders.mesh("v.coverage", d);
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = view.frameConstants;
    const TextureRef hiz = r.hiz;
    g.addPass("v.coverage.raster", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(run.args, Use::IndirectArgs);
                  b.use(run.visible, Use::SrvGraphics);
                  b.use(run.lists, Use::SrvGraphics);
                  b.use(run.state, Use::UavGraphics);
                  b.use(heads, Use::UavGraphics);
                  b.use(pixels, Use::UavGraphics);
                  b.use(raw, Use::UavGraphics);
                  if (hiz.valid()) b.use(hiz, Use::SrvGraphics);
              },
              [=](PassContext& c) {
                  uint32_t k[20];
                  constants(c, k, true, false, true);
                  if (!hiz.valid()) k[8] = kNone;
                  c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                  const D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
                  const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };
                  c.cmd->RSSetViewports(1, &vp);
                  c.cmd->RSSetScissorRects(1, &sc);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->SetPipelineState(rasterPso);
                  c.graphicsConstants(k, 20);
                  c.cmd->ExecuteIndirect(meshSig, 1, c.resource(run.args), kArgCovMesh * 4, nullptr, 0);
              });
    build("args", 2, 1, 1, 0, false, false);
    build("build", 3, 0, 0, kArgCovPixels, true, true);
}

// Reads the named run's statistics of the frame that last used this frame's slot (complete: the caller waited for
// that slot before recording, FrameRenderer contract), then records this frame's copy into the slot.
void recordStats(FramePassContext& fc, State& s, const Run& r, const std::string& name)
{
    State::StatsRun& run = s.stats[name];
    if (!run.readback)
    {
        run.readback = createBuffer(fc.device, (uint64_t)s.slots * kReadbackBytes, D3D12_HEAP_TYPE_READBACK, L"V stats readback");
        check(run.readback->Map(0, nullptr, reinterpret_cast<void**>(&run.mapped)), "map V stats");
        run.frames.assign(s.slots, UINT64_MAX);
    }
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % s.slots);
    if (run.frames[slot] != UINT64_MAX)
    {
        uint32_t w[kStateWords];
        std::memcpy(w, run.mapped + (uint64_t)slot * kReadbackBytes, sizeof w);
        visibility::Stats st;
        st.frameIndex = run.frames[slot];
        st.instancesVisible = w[kStateStatInstances];
        st.nodesTested = w[kStateStatNodes];
        st.clustersTested = w[kStateStatClusters];
        st.visibleClusters = w[kStateVisible];
        for (uint32_t b = 0; b < 3; ++b) st.triangles[b] = w[kStateStatTriangles + b];
        for (uint32_t b = 0; b < 3; ++b) st.bandClusters[b] = w[kStateStatBandClusters + b];
        for (uint32_t l = 0; l < kLists; ++l) st.listEntries[l] = w[kStateListCount + l];
        st.tilePairs = w[kStateTilePairs];
        st.deferredInstances = w[kStateDeferInstances];
        st.deferredNodes = w[kStateDeferNodes];
        st.deferredClusters = w[kStateDeferClusters];
        st.coverageFragments = w[kStateCovFragments];
        st.coveragePixels = w[kStateCovPixels];
        st.overflow = w[kStateOverflow];
        if (st.overflow && !run.overflowReported)
        {
            logf("V: capacity exceeded in '%s', frame %llu (bits 0x%x): raise visibility.max_* (Stats::overflow; 0x200: a pixel with more than 255 coverage "
                 "fragments)\n",
                 name.c_str(), (unsigned long long)st.frameIndex, st.overflow);
            run.overflowReported = true;
        }
        run.latest = st;
    }
    ID3D12Resource* readback = run.readback.Get();
    fc.graph.addPass(r.prefix + "stats", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(r.state, Use::CopySrc);
                         b.keep();
                     },
                     [=](PassContext& c) { c.cmd->CopyBufferRegion(readback, (uint64_t)slot * kReadbackBytes, c.resource(r.state), 0, kStateWords * 4); });
    run.frames[slot] = fc.frame.frameIndex;
}
} // namespace

void visibility(FramePassContext& fc, ViewResources& view)
{
    State& s = state(fc);
    refreshScene(s, fc);
    const Settings cfg = Settings::load(fc.quality);
    const bool main = view.view.kind == gpu::ViewKind::Main;
    const uint32_t width = view.view.width, height = view.view.height;
    RenderGraph& g = fc.graph;
    if (main)
    {
        s.mainFrameConstants = view.frameConstants;
        s.mainFrameConstantsFrame = fc.frame.frameIndex;
    }

    view.depth = g.createTexture({ "v.depth", width, height, 1, 1, DXGI_FORMAT_D32_FLOAT });
    view.visId = g.createTexture({ "v.visId", width, height, 1, 1, DXGI_FORMAT_R32_UINT });
    Run r = createRun(fc, cfg, main ? "v.cull." : "v.cull.secondary.", view.frameConstants);
    view.visibleClusters = r.visible;
    r.viewCount = 1;
    // Secondary views (planar reflections) still draw every band in the vis buffer: their coverage layer needs its own
    // persistent heads and M's composite in that view (V status).
    r.bandMode = main && cfg.coverageLayer ? (cfg.coverageBandC ? kBandModeCoverage : kBandModeFull) : kBandModeA;

    // Main view: two-phase occlusion against its persistent HiZ once a previous frame produced one. Secondary views
    // (planar reflections) have no history: one phase without occlusion, no HiZ.
    bool occlusion = false;
    if (main)
    {
        ensureHiz(fc.device, s.mainHiz, width, height);
        occlusion = cfg.occlusion && s.mainHiz.history;
        r.hiz = g.importTexture(s.mainHiz.texture.Get(), { "v.hiz", s.mainHiz.allocWidth, s.mainHiz.allocHeight, 1, (uint16_t)s.mainHiz.mips, DXGI_FORMAT_R32_FLOAT },
                                D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
        view.hiz = r.hiz;
        r.hizSrv = s.mainHiz.srv;
        r.hizMips = s.mainHiz.mips;
        r.hizWidth = s.mainHiz.width;
        r.hizHeight = s.mainHiz.height;
    }
    const CullView cullView = viewOf(view.view, cfg, occlusion);
    r.viewsSrv = uploadViews(s, fc, { cullView });
    if (view.view.planarMask.valid())
    {
        if (main) fail("V: ViewDesc::planarMask is for planar reflection views");
        planarTileMask(fc, r, view.view);
        tileCoarsePass(fc, r, { cullView });
    }

    cullPhase(fc, s, r, 1);
    rasterPass(fc, s, r, view, 1);
    if (!main)
    {
        recordStats(fc, s, r, "secondary");  // the frame's last secondary view (R's planar reflection costs)
        return;
    }
    hizPasses(fc, s.mainHiz, r.hiz, view.depth, width, height, occlusion ? "p1" : "final");
    if (occlusion)
    {
        cullPhase(fc, s, r, 2);
        rasterPass(fc, s, r, view, 2);
        hizPasses(fc, s.mainHiz, r.hiz, view.depth, width, height, "final");
    }
    if (r.bandMode != kBandModeA) coveragePasses(fc, s, r, view);
    recordStats(fc, s, r, "main");
    s.mainHiz.history = true;  // complete for the next frame once this frame's passes run
}

void rasterizeDepth(FramePassContext& fc, const DepthRasterRequest& request)
{
    State& s = state(fc);
    refreshScene(s, fc);
    const Settings cfg = Settings::load(fc.quality);
    if (request.views.empty()) return;
    if (!request.depthTarget.valid() && request.pixelKernel.empty()) fail("rasterizeDepth '%s': neither a depth target nor a pixel kernel", request.name.c_str());
    if (s.mainFrameConstantsFrame != fc.frame.frameIndex)
        fail("rasterizeDepth '%s': called before V's main view of this frame (it reads the frame's scene indices and time)", request.name.c_str());
    if (request.views.size() >= 256) fail("rasterizeDepth '%s': %zu views (limit 255, 8-bit view field)", request.name.c_str(), request.views.size());
    if (request.coverage || request.bands != 7)
        fail("rasterizeDepth '%s': coverage mode and band selection (v1.26) are not implemented yet (V)", request.name.c_str());
    if (request.tileLocal && (!request.cullMask.valid() || request.cullTilePx == 0))
        fail("rasterizeDepth '%s': tileLocal needs a tile mask (cullMask, cullTilePx)", request.name.c_str());
    const DXGI_FORMAT depthFormat = request.depthTarget.valid() ? fc.graph.desc(request.depthTarget).format : DXGI_FORMAT_UNKNOWN;
    if (request.depthTarget.valid() && depthFormat != DXGI_FORMAT_D32_FLOAT && depthFormat != DXGI_FORMAT_D16_UNORM)
        fail("rasterizeDepth '%s': depth target format %u (D32_FLOAT or D16_UNORM)", request.name.c_str(), (unsigned)depthFormat);
    const bool atlas = request.atlasSlots.valid();
    uint32_t atlasWidth = 0, atlasHeight = 0;
    if (atlas)
    {
        if (!request.tileLocal || !request.depthTarget.valid() || request.atlasTilesPerRow == 0)
            fail("rasterizeDepth '%s': the tile atlas needs tileLocal, a tile mask, a depth target and atlasTilesPerRow", request.name.c_str());
        atlasWidth = fc.graph.desc(request.depthTarget).width;
        atlasHeight = fc.graph.desc(request.depthTarget).height;
        if (request.atlasTilesPerRow * request.cullTilePx > atlasWidth || atlasWidth > 0xFFFF || atlasHeight > 0xFFFF)
            fail("rasterizeDepth '%s': atlas %ux%u for %u tiles of %u px per row", request.name.c_str(), atlasWidth, atlasHeight, request.atlasTilesPerRow, request.cullTilePx);
    }

    // One viewport for all views when they agree; else SV_ViewportArrayIndex (at most 16). The atlas is one viewport.
    bool sameViewport = true;
    for (const RasterView& v : request.views)
        sameViewport = sameViewport && v.viewportX == request.views[0].viewportX && v.viewportY == request.views[0].viewportY &&
                       v.viewportWidth == request.views[0].viewportWidth && v.viewportHeight == request.views[0].viewportHeight;
    if (atlas) sameViewport = true;
    if (!sameViewport && request.views.size() > D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE)
        fail("rasterizeDepth '%s': %zu views with different viewports (limit %u)", request.name.c_str(), request.views.size(),
             (unsigned)D3D12_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE);

    Run r = createRun(fc, cfg, request.name + ".", s.mainFrameConstants);
    r.tileMask = request.cullMask;
    if (request.tileLocal) r.tilePairs = fc.graph.createBuffer({ "v.cull.tilePairs", (uint64_t)cfg.capVisible * 12, 12 });
    r.instanceMask = request.instanceMask;
    std::vector<CullView> views;
    for (const RasterView& v : request.views) views.push_back(viewOf(v, request, cfg));
    r.viewCount = (uint32_t)views.size();
    r.viewsSrv = uploadViews(s, fc, views);
    if (r.tileMask.valid()) tileCoarsePass(fc, r, views);
    cullPhase(fc, s, r, 1);

    // Back-face lists exist only when BACK was requested; shadow casters need every band (band mode A puts all visible
    // clusters in band A lists).
    const bool depthOut = request.depthTarget.valid();
    ID3D12PipelineState* pso[kLists];
    for (uint32_t l = 0; l < kLists; ++l)
    {
        const bool back = request.cull == D3D12_CULL_MODE_BACK && (l == kListABack || l == kListAAlphaBack);
        MeshPipelineDesc d;
        d.meshShader = atlas ? "Passes/Visibility/DepthRaster.ms.TILE2"
                             : request.tileLocal ? "Passes/Visibility/DepthRaster.ms.TILE1" : "Passes/Visibility/DepthRaster.ms.TILE0";
        d.pixelShader = request.pixelKernel;
        d.depthFormat = depthOut ? depthFormat : DXGI_FORMAT_UNKNOWN;
        d.depthWrite = depthOut;
        d.cull = back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        d.conservative = request.conservative;
        pso[l] = fc.shaders.mesh("v.depth|" + request.pixelKernel + (back ? "|back" : "|none") +
                                     (depthOut ? (depthFormat == DXGI_FORMAT_D16_UNORM ? "|d16" : "|d32") : "|uav") + (request.conservative ? "|cons" : "") +
                                     (atlas ? "|atlas" : request.tileLocal ? "|tile" : ""), d);
    }
    std::vector<D3D12_VIEWPORT> viewports;
    std::vector<D3D12_RECT> scissors;
    for (size_t i = 0; i < (sameViewport ? 1 : request.views.size()); ++i)
    {
        const RasterView& v = request.views[i];
        if (atlas)
        {
            viewports.push_back({ 0, 0, (float)atlasWidth, (float)atlasHeight, 0, 1 });
            scissors.push_back({ 0, 0, (LONG)atlasWidth, (LONG)atlasHeight });
            continue;
        }
        viewports.push_back({ (float)v.viewportX, (float)v.viewportY, (float)v.viewportWidth, (float)v.viewportHeight, 0, 1 });
        scissors.push_back({ (LONG)v.viewportX, (LONG)v.viewportY, (LONG)(v.viewportX + v.viewportWidth), (LONG)(v.viewportY + v.viewportHeight) });
    }
    ID3D12CommandSignature* sig = s.meshSignature.Get();
    const DepthRasterRequest req = request;
    fc.graph.addPass(request.name + ".raster", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(r.args, Use::IndirectArgs);
                         b.use(r.visible, Use::SrvGraphics);
                         b.use(r.lists, Use::SrvGraphics);
                         b.use(r.state, Use::SrvGraphics);
                         if (r.tilePairs.valid()) b.use(r.tilePairs, Use::SrvGraphics);
                         if (req.atlasSlots.valid()) b.use(req.atlasSlots, Use::SrvGraphics);
                         if (req.depthTarget.valid()) b.use(req.depthTarget, Use::DepthWrite);
                         for (const auto& [t, u] : req.textureUses) b.use(t, u);
                         for (const auto& [bu, u] : req.bufferUses) b.use(bu, u);
                     },
                     [=](PassContext& c) {
                         if (req.depthTarget.valid())
                         {
                             const D3D12_CPU_DESCRIPTOR_HANDLE dsv = c.dsv(req.depthTarget);
                             c.cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                         }
                         else
                             c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                         c.cmd->RSSetViewports((UINT)viewports.size(), viewports.data());
                         c.cmd->RSSetScissorRects((UINT)scissors.size(), scissors.data());
                         c.bindFrameConstants(r.frameConstants);
                         for (uint32_t l = 0; l < kLists; ++l)
                         {
                             uint32_t k[32] = { c.srv(r.visible), c.srv(r.lists), c.srv(r.state), l, 1, r.cfg.capVisible, r.viewsSrv, sameViewport ? 0u : 1u,
                                                r.tilePairs.valid() ? c.srv(r.tilePairs) : kNone, req.atlasSlots.valid() ? c.srv(req.atlasSlots) : kNone,
                                                req.atlasTilesPerRow, atlasWidth | atlasHeight << 16 };
                             std::memcpy(&k[16], req.pixelConstants, sizeof req.pixelConstants);
                             c.cmd->SetPipelineState(pso[l]);
                             c.graphicsConstants(k, 32);
                             c.cmd->ExecuteIndirect(sig, 1, c.resource(r.args), (kArgMesh + 3 * l) * 4, nullptr, 0);
                         }
                     });
    recordStats(fc, s, r, request.name);
}
} // namespace unx::render::tracks

namespace unx::visibility
{
Stats latestStats(render::TrackState& trackState, const std::string& run)
{
    auto& runs = trackState.get<render::tracks::State>("v.state").stats;
    const auto it = runs.find(run);
    return it == runs.end() ? Stats{} : it->second.latest;
}
} // namespace unx::visibility
