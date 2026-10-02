// Track entry points of V (visibility) (INTERFACES_KO.md 5.2, 5.3; ARCHITECTURE 2.1).
//
// A cull run (one view, or every view of a raster-service request) walks instances -> per-depth hierarchy roots ->
// hierarchy nodes (one dispatch over the node work queue, CullNodes QUEUE=1; visibility.traversal_work_queue false: one
// pass per tree level, indirect) -> hierarchy leaves (groups) -> clusters, with the DAG cut test
// (a leaf is reached while its group error projects above the threshold; a cluster is drawn when its own error
// projects at or below it), frustum, clip plane, normal cone and, for the main view, two-phase HiZ occlusion: phase 1
// tests against the previous frame's HiZ with previous transforms and defers what it rejects; after the phase-1
// raster the HiZ is rebuilt and phase 2 retests the deferred instances, nodes and clusters against it. Only geometry
// that the current frame's own depth hides is ever dropped. Visible clusters are classified into bands A/B/C and
// pipeline lists; the band A lists are drawn by mesh shaders (vis id + depth). With visibility.coverage_layer the main
// view draws bands B and C into the coverage layer instead (CoverageLayer.hlsli): conservative raster with exact pixel
// areas appended to a stream, sorted into each tile's pixel-major range (INTERFACES 7.1 v1.41).
//
// Instance hierarchy (C3, ARCHITECTURE 2.1 "64 m cell BVH"): static instances (neither dynamic nor skinned) are grouped
// by 64 m cell and cut into chunks of at most 256 (Morton order inside a cell); a chunk is culled first (CullChunks) and
// only the members of visible chunks are tested one by one; dynamic and skinned instances form the flat list. Chunk
// spheres come from the GPU (ChunkBounds, each scene revision) with the same worldSphere as the instance test, plus the
// wind term at the frame's wind speed. Skinned instances are culled by per-frame palette bounds (SkinBounds).
#include "unx/render/Tracks.h"

#include "VisibilityInternal.h"
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/clusterbuilder/ClusterHierarchy.h"
#include "unx/core/Log.h"
#include "unx/render/GpuScene.h"
#include "unx/render/PassChain.h"
#include "unx/visibility/Visibility.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <map>
#include <tuple>

namespace unx::render::tracks
{
namespace
{
using namespace unx::visibility::detail;
namespace hier = unx::clusterbuilder::gpu;

constexpr uint32_t kNone = gpu::kNone;
constexpr uint32_t kViewsPerSlot = kViewsPerRun;  // cull views of one upload chunk per frame in flight (more chunks as a frame needs)
static_assert(kViewsPerRun <= kDepthRasterMaxViews && kDepthRasterMaxViews <= 65536);
constexpr uint32_t kReadbackBytes = 320;  // >= kStateWords x 4 (a slot holds one copy of the cull state)
static_assert(kReadbackBytes >= kStateWords * 4);

struct Settings
{
    uint32_t capVisible = 0, capNodes = 0, capGroups = 0, capDeferred = 0;
    uint32_t coverageDebugStage = 0;
    bool coverageHair = false;  // visibility.coverage_hair (B10 strands in the coverage layer)
    bool rasterAmplification = true;  // visibility.raster_amplification (tile-local raster runs: no stored pairs)
    bool workQueue = true;            // visibility.traversal_work_queue (the node traversal in one dispatch)
    bool passMerge = true;            // visibility.cull_pass_merge (no argument passes; dispatches that share a pass)
    bool foldSmall = true;            // visibility.fold_small_passes (HiZ levels, mask and layer clears: PassChain.h)
    uint32_t workerGroups = 0;        // visibility.traversal_worker_groups (its dispatch: groups of 64 threads)
    double coveragePoolMinPerPixel = 0;
    uint32_t oceanEdgesMin = 0;  // ocean edge pixel list capacity floor (entries)
    uint32_t coverageSpecialMin = 0;  // special record list capacity floor (entries)
    float lodErrorPx = 0, bandAMinPx = 0, bandCMaxPx = 0, bandAHysteresisPx = 0;
    bool occlusion = true, coverageLayer = false, coverageBandC = true, coverageBandCVisible = false;
    // The coverage raster's depth buckets and what goes with them (CoverageBuckets.hlsli): buckets the band B list is
    // drawn in, nearer first (1: drawn whole, as before); the fewest list entries of the latest completed frame that turn
    // them on; the mesh kernel's triangle tests; the compute rasteriser of small triangles; the fragment counters.
    uint32_t coverageDepthBuckets = 1, coverageBucketsMinClusters = 0;
    bool coverageTriangleCull = false, coverageComputeRaster = false, coverageStatistics = false;
    bool coverageDropWeightless = false;  // fragments without weight (no area step, no subsample) are not stored

    static Settings load(const QualityConfig& q)
    {
        Settings s;
        s.capVisible = (uint32_t)q.integer("visibility.max_visible_clusters");
        // Cluster vis ids ((index << 7 | triangle) + 1) must stay below 2^30: the coverage records' top two bits name
        // their kind (CoverageTiles.hlsli COV_PRESHADE_ID, COV_HAIR_ID, COV_STREAM_ID).
        if (s.capVisible > (1u << 23)) fail("visibility.max_visible_clusters = %u: at most 2^23 (coverage record kinds use vis id bits 30, 31)", s.capVisible);
        s.capNodes = (uint32_t)q.integer("visibility.max_node_items");
        s.capGroups = (uint32_t)q.integer("visibility.max_group_items");
        s.capDeferred = (uint32_t)q.integer("visibility.max_deferred_items");
        s.lodErrorPx = (float)q.number("visibility.lod_error_px");
        s.bandAMinPx = (float)q.number("visibility.band_a_min_width_px");
        s.bandCMaxPx = (float)q.number("visibility.band_c_max_width_px");
        s.bandAHysteresisPx = (float)q.number("visibility.band_a_hysteresis_px");
        if (!(s.bandAHysteresisPx >= s.bandAMinPx)) fail("visibility.band_a_hysteresis_px = %g: at least band_a_min_width_px (%g)", s.bandAHysteresisPx, s.bandAMinPx);
        s.occlusion = q.boolean("visibility.occlusion_culling");
        s.coverageLayer = q.boolean("visibility.coverage_layer");
        s.coverageBandC = q.boolean("visibility.coverage_band_c");
        s.coverageBandCVisible = q.has("visibility.coverage_band_c_visbuffer") && q.boolean("visibility.coverage_band_c_visbuffer");
        s.coverageHair = q.boolean("visibility.coverage_hair");
        s.rasterAmplification = q.has("visibility.raster_amplification") ? q.boolean("visibility.raster_amplification") : true;
        s.workQueue = q.has("visibility.traversal_work_queue") ? q.boolean("visibility.traversal_work_queue") : true;
        s.passMerge = q.has("visibility.cull_pass_merge") ? q.boolean("visibility.cull_pass_merge") : true;
        s.foldSmall = q.has("visibility.fold_small_passes") ? q.boolean("visibility.fold_small_passes") : true;
        const int64_t workers = q.has("visibility.traversal_worker_groups") ? q.integer("visibility.traversal_worker_groups") : 1024;
        if (workers < 1 || workers > 65535) fail("visibility.traversal_worker_groups = %lld: 1 .. 65535 (one dispatch row)", (long long)workers);
        s.workerGroups = (uint32_t)workers;
        const int64_t stage = q.integer("visibility.coverage_debug_stage");
        if (stage < 0 || stage > 4) fail("visibility.coverage_debug_stage = %lld: 0 (the layer), 1 .. 4 (measurement variants)", (long long)stage);
        s.coverageDebugStage = (uint32_t)stage;
        s.coveragePoolMinPerPixel = q.number("visibility.coverage_pool_min_fragments_per_pixel");
        s.coverageSpecialMin = (uint32_t)q.integer("visibility.coverage_special_min");
        s.oceanEdgesMin = (uint32_t)q.integer("visibility.ocean_edges_min");
        if (!(s.coveragePoolMinPerPixel > 0)) fail("visibility.coverage_pool_min_fragments_per_pixel = %g: must be positive", s.coveragePoolMinPerPixel);
        // (keys newer than some quality files in use: absent = the layer as it was)
        if (q.has("visibility.coverage_depth_buckets"))
        {
            const int64_t buckets = q.integer("visibility.coverage_depth_buckets");
            if (buckets < 1 || buckets > (int64_t)kCovBucketsMax) fail("visibility.coverage_depth_buckets = %lld: 1 (the list drawn whole) .. %u", (long long)buckets, kCovBucketsMax);
            s.coverageDepthBuckets = (uint32_t)buckets;
        }
        if (q.has("visibility.coverage_depth_buckets_min_clusters"))
            s.coverageBucketsMinClusters = (uint32_t)std::max<int64_t>(q.integer("visibility.coverage_depth_buckets_min_clusters"), 0);
        s.coverageTriangleCull = q.has("visibility.coverage_triangle_cull") && q.boolean("visibility.coverage_triangle_cull");
        s.coverageComputeRaster = q.has("visibility.coverage_compute_raster") && q.boolean("visibility.coverage_compute_raster");
        s.coverageStatistics = q.has("visibility.coverage_statistics") && q.boolean("visibility.coverage_statistics");
        s.coverageDropWeightless = q.has("visibility.coverage_drop_weightless") && q.boolean("visibility.coverage_drop_weightless");
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

// Persistent coverage layer resources of the main view (CoverageTiles.hlsli): tile headers, pixel counters, the tile list
// and the depth range; the stream, keys, records, pixel starts and scratch are graph buffers sized per frame.
struct Coverage
{
    ComPtr<ID3D12Resource> headers, counters, list, depthRange;
    ComPtr<ID3D12Resource> cover;  // depth buckets: per pixel { opaque union, ~farthest } of the buckets drawn (CoverageBuckets.hlsli)
    uint32_t width = 0, height = 0, tilesX = 0, tiles = 0;
    uint32_t capacity = 0;  // records
    uint32_t specialCapacity = 0;  // special record list entries (v1.73)
    uint32_t oceanEdgeCapacity = 0;  // ocean edge pixel list entries (v1.73)
    bool fresh = true;      // every tile still to be emptied (CoverageBuild MODE 4)
};

// A14: what a full view (main, render texture, mirror, portal, split) keeps between frames, per ViewResources::viewId.
struct ViewState
{
    Hiz hiz;
    Coverage coverage;
    float3 prevPosition{};  // the view's camera position of the previous frame (band hysteresis)
    bool hasPrevPosition = false;
    uint64_t lastFrame = UINT64_MAX;  // frame index of its latest use (views unseen for framesInFlight + 1 frames are freed)
};

struct State
{
    Device* device = nullptr;
    ComPtr<ID3D12CommandSignature> dispatchSignature, meshSignature;
    // Cull view uploads: chunks of slots x kViewsPerSlot views; a frame takes the chunks it needs in order (a run's views stay
    // in one chunk). A frame with more views than one chunk holds - S's 42 views per shadowed local light past ~97 lights -
    // used to fail ("more than 4096 cull views"); the chunks stay allocated for later frames.
    struct ViewChunk
    {
        ComPtr<ID3D12Resource> buffer;
        uint8_t* mapped = nullptr;
    };
    std::vector<ViewChunk> viewChunks;
    uint32_t slots = 0;
    uint64_t frame = UINT64_MAX, record = UINT64_MAX;
    uint32_t chunkUsed = 0, viewsUsed = 0, runsUsed = 0;
    std::vector<std::vector<uint32_t>> runSrvs;  // per slot: view SRVs of the runs of that frame
    struct StatsRun  // cull state readback of a named run ("main" or a raster request), one slot per frame in flight
    {
        ComPtr<ID3D12Resource> readback;
        uint8_t* mapped = nullptr;
        std::vector<uint64_t> frames;
        visibility::Stats latest;
        uint32_t overflowSeen = 0;  // every read frame's overflow bits
        bool overflowReported = false;
    };
    std::map<std::string, StatsRun> stats;
    std::map<uint32_t, ViewState> views;  // A14: full views by id (0 = main)
    D3D12_GPU_VIRTUAL_ADDRESS mainFrameConstants = 0;  // this frame's main view constants (scene indices, time, wind)
    uint64_t mainFrameConstantsFrame = UINT64_MAX;
    uint32_t sceneRevision = UINT32_MAX;
    uint64_t viewModelRevision = UINT64_MAX;  // GpuScene::viewModelRevision the instance hierarchy was built with
    uint32_t traversalLevels = 0;  // deepest per-depth tree of any mesh (node passes per phase without the work queue)

    // C3: instance chunks, flat list, skinned bounds (persistent; rebuilt at each scene revision).
    struct VBuffer
    {
        ComPtr<ID3D12Resource> resource;
        uint32_t srv = kNone, uav = kNone;
        uint64_t bytes = 0;
    };
    VBuffer cullScene, chunks, chunkInstances, flat, skinList, skinSlots, jointSpheres, skinBounds;
    uint32_t chunkCount = 0, flatCount = 0, skinCount = 0;
    bool chunkBoundsPending = false;
    std::vector<uint8_t> chunked;  // per instance: a member of a chunk (its moves invalidate the chunk spheres)
    uint64_t preparedFrame = UINT64_MAX;  // frame whose graph has the imports and the bounds passes below
    BufferRef chunksRef, skinBoundsRef;

    ~State()
    {
        if (!device) return;
        DescriptorHeaps& h = device->descriptors();
        for (auto& slot : runSrvs)
            for (uint32_t i : slot) h.freeResource(i);
        for (auto& [id, v] : views)
        {
            if (v.hiz.srv != kNone) h.freeResource(v.hiz.srv);
            for (uint32_t i : v.hiz.uavs) h.freeResource(i);
        }
        for (ViewChunk& c : viewChunks)
            if (c.buffer) c.buffer->Unmap(0, nullptr);
        for (auto& [name, run] : stats)
            if (run.readback) run.readback->Unmap(0, nullptr);
        for (VBuffer* b : { &cullScene, &chunks, &chunkInstances, &flat, &skinList, &skinSlots, &jointSpheres, &skinBounds })
        {
            if (b->srv != kNone) h.freeResource(b->srv);
            if (b->uav != kNone) h.freeResource(b->uav);
        }
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
    s.runSrvs.resize(s.slots);
    return s;
}

void releaseBuffer(Device& d, State::VBuffer& b)
{
    if (b.resource) d.deferRelease(b.resource);
    DescriptorHeaps* h = &d.descriptors();
    for (uint32_t i : { b.srv, b.uav })
        if (i != kNone) d.deferCall([h, i] { h->freeResource(i); });
    b = {};
}

// A default-heap structured buffer with 'count' elements of 'stride' bytes (at least one, zeroed), filled from 'data'
// (blocking upload: scene revisions only), with an SRV and optionally a UAV.
State::VBuffer structuredBuffer(Device& d, const void* data, uint32_t stride, uint32_t count, bool uav, const wchar_t* name)
{
    std::vector<uint8_t> zero;
    if (count == 0 || !data)
    {
        zero.assign((size_t)stride * std::max(count, 1u), 0);
        data = zero.data();
        count = std::max(count, 1u);
    }
    State::VBuffer b;
    b.bytes = (uint64_t)stride * count;
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = b.bytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(d.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&b.resource)), "V buffer");
    b.resource->SetName(name);
    ComPtr<ID3D12Resource> staging = createBuffer(d, b.bytes, D3D12_HEAP_TYPE_UPLOAD, L"V staging");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &mapped), "map V staging");
    std::memcpy(mapped, data, (size_t)b.bytes);
    staging->Unmap(0, nullptr);
    CommandList cl = d.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(b.resource.Get(), 0, staging.Get(), 0, b.bytes);
    d.queue(QueueType::Graphics).waitCpu(d.submit(cl));
    DescriptorHeaps& h = d.descriptors();
    b.srv = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.Buffer.NumElements = count;
    sd.Buffer.StructureByteStride = stride;
    d.d3d()->CreateShaderResourceView(b.resource.Get(), &sd, h.resourceCpu(b.srv));
    if (uav)
    {
        b.uav = h.allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Format = DXGI_FORMAT_UNKNOWN;
        ud.Buffer.NumElements = count;
        ud.Buffer.StructureByteStride = stride;
        d.d3d()->CreateUnorderedAccessView(b.resource.Get(), nullptr, &ud, h.resourceCpu(b.uav));
    }
    return b;
}

// Interleaves the low 6 bits of x, y, z (Morton order of 1 m sub-cells inside a 64 m cell).
uint32_t morton6(uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t m = 0;
    for (uint32_t b = 0; b < 6; ++b) m |= ((x >> b) & 1) << (3 * b) | ((y >> b) & 1) << (3 * b + 1) | ((z >> b) & 1) << (3 * b + 2);
    return m;
}

// C3 instance hierarchy and skinned-bounds inputs of the current scene revision (VisibilityCommon.hlsli CullScene).
void buildCullScene(State& s, FramePassContext& fc)
{
    Device& d = fc.device;
    for (State::VBuffer* b : { &s.cullScene, &s.chunks, &s.chunkInstances, &s.flat, &s.skinList, &s.skinSlots, &s.jointSpheres, &s.skinBounds }) releaseBuffer(d, *b);
    const std::vector<gpu::Instance>& instances = fc.scene.instances();
    const std::vector<gpu::Mesh>& meshes = fc.scene.meshes();
    const scene::Scene* src = fc.scene.source();
    struct Item
    {
        int32_t cx, cy, cz;
        uint32_t fine, index;
    };
    std::vector<Item> items;
    std::vector<uint32_t> flat, skinned;
    for (uint32_t i = 0; i < fc.scene.staticInstanceCount(); ++i)  // runtime instances (C2b) follow via the views
    {
        const gpu::Instance& in = instances[i];
        const bool skin = (in.flags & scene::InstanceSkinned) != 0;
        if (skin && in.bonePalette != kNone && meshes[in.mesh].skinOffset != kNone) skinned.push_back(i);
        // Dynamic, skinned and view-model instances (A12: moved with the camera every frame, after V's preparation) are
        // tested one by one.
        if ((in.flags & (scene::InstanceDynamic | scene::InstanceSkinned | gpu::kInstanceViewModel)) != 0)
        {
            flat.push_back(i);
            continue;
        }
        const float4& c = meshes[in.mesh].boundsSphere;
        const float3 w{ in.objectToWorld[0].x * c.x + in.objectToWorld[0].y * c.y + in.objectToWorld[0].z * c.z + in.objectToWorld[0].w,
                        in.objectToWorld[1].x * c.x + in.objectToWorld[1].y * c.y + in.objectToWorld[1].z * c.z + in.objectToWorld[1].w,
                        in.objectToWorld[2].x * c.x + in.objectToWorld[2].y * c.y + in.objectToWorld[2].z * c.z + in.objectToWorld[2].w };
        const float fx = std::floor(w.x / kChunkCell), fy = std::floor(w.y / kChunkCell), fz = std::floor(w.z / kChunkCell);
        auto sub = [](float v, float cell) { return (uint32_t)std::clamp((int)std::floor((v - cell * kChunkCell) / (kChunkCell / 64)), 0, 63); };
        items.push_back({ (int32_t)std::clamp(fx, -1e8f, 1e8f), (int32_t)std::clamp(fy, -1e8f, 1e8f), (int32_t)std::clamp(fz, -1e8f, 1e8f),
                          morton6(sub(w.x, fx), sub(w.y, fy), sub(w.z, fz)), i });
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return std::tie(a.cx, a.cy, a.cz, a.fine, a.index) < std::tie(b.cx, b.cy, b.cz, b.fine, b.index);
    });
    std::vector<CullChunk> chunks;
    std::vector<uint32_t> members;
    for (size_t k = 0; k < items.size(); ++k)
    {
        const bool newCell = k == 0 || items[k].cx != items[k - 1].cx || items[k].cy != items[k - 1].cy || items[k].cz != items[k - 1].cz;
        if (newCell || chunks.back().count == kChunkInstances) chunks.push_back({ float4{ 0, 0, 0, -1 }, (uint32_t)members.size(), 0, 0, 0 });
        members.push_back(items[k].index);
        ++chunks.back().count;
    }
    s.chunked.assign(instances.size(), 0);
    for (uint32_t m : members) s.chunked[m] = 1;
    // Skinned instances: per mesh, the bind-space sphere of the vertices each joint influences (weight > 0), and one
    // origin point when a vertex has no weight at all (skinning leaves it at the object origin).
    std::vector<uint32_t> slots;  // uint4 per skinned instance: instance, first sphere, sphere count, 0
    std::vector<SkinJointSphere> spheres;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> meshSpheres;
    for (uint32_t i : skinned)
    {
        const uint32_t m = instances[i].mesh;
        auto it = meshSpheres.find(m);
        if (it == meshSpheres.end())
        {
            const uint32_t first = (uint32_t)spheres.size();
            if (src && m < src->meshes.size())
            {
                const scene::Mesh& mesh = src->meshes[m];
                const scene::SkinStream& sk = mesh.skin;
                const size_t vertices = mesh.positions.size();
                std::map<uint32_t, std::pair<float3, float3>> box;  // joint -> AABB
                bool origin = false;
                for (size_t v = 0; v < vertices && 4 * v + 3 < sk.joints.size() && 4 * v + 3 < sk.weights.size(); ++v)
                {
                    float total = 0;
                    for (int k = 0; k < 4; ++k)
                    {
                        const float wgt = sk.weights[4 * v + k];
                        if (!(wgt > 0)) continue;
                        total += wgt;
                        const float3 p = mesh.positions[v];
                        auto [b, inserted] = box.try_emplace(sk.joints[4 * v + k], std::pair<float3, float3>{ p, p });
                        if (!inserted)
                        {
                            b->second.first = { std::min(b->second.first.x, p.x), std::min(b->second.first.y, p.y), std::min(b->second.first.z, p.z) };
                            b->second.second = { std::max(b->second.second.x, p.x), std::max(b->second.second.y, p.y), std::max(b->second.second.z, p.z) };
                        }
                    }
                    if (!(total > 0)) origin = true;
                }
                std::map<uint32_t, float> radius;
                for (size_t v = 0; v < vertices && 4 * v + 3 < sk.joints.size() && 4 * v + 3 < sk.weights.size(); ++v)
                    for (int k = 0; k < 4; ++k)
                    {
                        if (!(sk.weights[4 * v + k] > 0)) continue;
                        const auto& b = box[sk.joints[4 * v + k]];
                        const float3 c = (b.first + b.second) * 0.5f, q = mesh.positions[v] - c;
                        float& r = radius[sk.joints[4 * v + k]];
                        r = std::max(r, std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z));
                    }
                for (const auto& [joint, b] : box)
                {
                    const float3 c = (b.first + b.second) * 0.5f;
                    spheres.push_back({ float4{ c.x, c.y, c.z, radius[joint] * (1 + 1e-6f) + 1e-6f }, joint, { 0, 0, 0 } });
                }
                if (origin) spheres.push_back({ float4{ 0, 0, 0, 0 }, kSkinJointOrigin, { 0, 0, 0 } });
            }
            it = meshSpheres.emplace(m, std::make_pair(first, (uint32_t)spheres.size() - first)).first;
        }
        slots.insert(slots.end(), { i, it->second.first, it->second.second, 0 });
    }
    s.chunkCount = (uint32_t)chunks.size();
    s.flatCount = (uint32_t)flat.size();
    s.skinCount = (uint32_t)skinned.size();
    s.chunks = structuredBuffer(d, chunks.data(), sizeof(CullChunk), s.chunkCount, true, L"V instance chunks");
    s.chunkInstances = structuredBuffer(d, members.data(), 4, (uint32_t)members.size(), false, L"V chunk instances");
    s.flat = structuredBuffer(d, flat.data(), 4, s.flatCount, false, L"V flat instances");
    s.skinList = structuredBuffer(d, skinned.data(), 4, s.skinCount, false, L"V skinned instances");
    s.skinSlots = structuredBuffer(d, slots.data(), 16, s.skinCount, false, L"V skin slots");
    s.jointSpheres = structuredBuffer(d, spheres.data(), sizeof(SkinJointSphere), (uint32_t)spheres.size(), false, L"V joint spheres");
    s.skinBounds = structuredBuffer(d, nullptr, 16, 2 * std::max(s.skinCount, 1u), true, L"V skinned bounds");
    const CullScene cs{ s.chunks.srv, s.chunkInstances.srv, s.chunkCount, s.flat.srv, s.flatCount, s.skinBounds.srv, s.skinList.srv, s.skinCount };
    s.cullScene = structuredBuffer(d, &cs, sizeof(CullScene), 1, false, L"V cull scene");
    s.chunkBoundsPending = s.chunkCount > 0;
    s.preparedFrame = UINT64_MAX;
    logf("V: instance hierarchy: %u static instances in %u chunks (64 m cells, <= %u each), %u flat instances, %u skinned (%zu joint spheres)\n",
         (uint32_t)members.size(), s.chunkCount, kChunkInstances, s.flatCount, s.skinCount, spheres.size());
}

// Once per frame, before the first cull run: imports the persistent C3 buffers into the frame's graph, computes the
// chunk bounds after a scene revision and the skinned bounds of this frame (current and previous palettes).
void prepareCullScene(FramePassContext& fc, State& s, D3D12_GPU_VIRTUAL_ADDRESS frameConstants)
{
    if (s.preparedFrame == fc.frame.frameIndex) return;
    s.preparedFrame = fc.frame.frameIndex;
    // C9: an origin shift moved every instance; the chunk spheres (world space) follow from the shifted table.
    if (fc.frame.originShift.x != 0 || fc.frame.originShift.y != 0 || fc.frame.originShift.z != 0) s.chunkBoundsPending = s.chunkCount > 0;
    // A chunked (static) instance moved by a transform update in this frame (no scene revision: an editor move, a view
    // model, E's A12 pose): the chunk spheres are recomputed from the moved table (one ChunkBounds pass, all chunks).
    for (uint32_t i : fc.scene.movedInstances())
        if (i < s.chunked.size() && s.chunked[i])
        {
            s.chunkBoundsPending = s.chunkCount > 0;
            break;
        }
    RenderGraph& g = fc.graph;
    s.chunksRef = g.importBuffer(s.chunks.resource.Get(), { "v.cull.chunks", s.chunks.bytes, (uint32_t)sizeof(CullChunk) });
    s.skinBoundsRef = g.importBuffer(s.skinBounds.resource.Get(), { "v.cull.skinBounds", s.skinBounds.bytes, 16 });
    if (s.skinCount > 0)
    {
        // (S's page cache: the skinned casters' shadow footprint, FrameResources::skinBounds)
        fc.resources.skinBounds = s.skinBoundsRef;
        fc.resources.skinInstancesSrv = s.skinList.srv;
        fc.resources.skinCount = s.skinCount;
    }
    if (s.chunkBoundsPending)
    {
        s.chunkBoundsPending = false;
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Visibility/ChunkBounds");
        const BufferRef chunks = s.chunksRef;
        const uint32_t k[4] = { s.chunks.uav, s.chunkInstances.srv, s.chunkCount, 0 };
        const uint32_t groups = (s.chunkCount + 63) / 64;
        g.addPass("v.cull.chunkBounds", QueueType::Graphics, [&](PassBuilder& b) { b.use(chunks, Use::UavCompute); },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(pso);
                      c.bindFrameConstants(frameConstants);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(groups, 1, 1);
                  });
    }
    if (s.skinCount > 0)
    {
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Visibility/SkinBounds");
        const BufferRef bounds = s.skinBoundsRef;
        const uint32_t k[4] = { s.skinSlots.srv, s.jointSpheres.srv, s.skinBounds.uav, s.skinCount };
        const uint32_t groups = s.skinCount;
        g.addPass("v.cull.skinBounds", QueueType::Graphics, [&](PassBuilder& b) { b.use(bounds, Use::UavCompute); },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(pso);
                      c.bindFrameConstants(frameConstants);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(groups, 1, 1);
                  });
    }
}

// Deepest per-depth tree over all meshes (the node passes each phase needs), and the 24-bit packing limits.
void refreshScene(State& s, FramePassContext& fc)
{
    if (s.sceneRevision == fc.scene.revision())
    {
        // A view model was marked or unmarked (no scene revision): only the instance hierarchy changes.
        if (s.viewModelRevision != fc.scene.viewModelRevision())
        {
            s.viewModelRevision = fc.scene.viewModelRevision();
            buildCullScene(s, fc);
        }
        return;
    }
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
    // C2b: runtime meshes arrive without a revision; their hierarchies are at most kRuntimeMaxDepth deep.
    s.traversalLevels = fc.scene.runtimeCapacity().meshes > 0 ? std::max(deepest, kRuntimeMaxDepth) : deepest;
    s.sceneRevision = fc.scene.revision();
    s.viewModelRevision = fc.scene.viewModelRevision();
    for (auto& [id, v] : s.views) v.hiz.history = false;
    buildCullScene(s, fc);
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
    v.prevPosition = view.position;  // the main view's caller sets the previous frame's
    v.bandAMinPx = cfg.bandAMinPx;
    v.bandAHysteresisPx = cfg.bandAHysteresisPx;
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
    v.prevPosition = v.position;
    v.bandAMinPx = cfg.bandAMinPx;
    v.bandAHysteresisPx = cfg.bandAHysteresisPx;
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
    v.slotOffset = r.atlasSlotOffset != UINT32_MAX ? r.atlasSlotOffset : (v.cullMaskOffset != kNone ? v.cullMaskOffset * 32 : 0);
    v.userData = r.userData;
    v.tilePx = std::max(req.cullTilePx, 1u);
    v.tilesX = (r.viewportWidth + v.tilePx - 1) / v.tilePx;
    v.instanceFirst = r.instanceFirst;
    v.instanceEnd = r.instanceEnd;
    v.minInstancePx = r.minInstanceTexels;
    v.instanceSet = r.instanceSet;
    if (req.proxies && r.minInstanceTexels > 0) v.flags |= kViewProxies;
    if (r.tileOccluders && req.tileOccluders.valid() && req.atlasSlots.valid())
    {
        if (req.cullTilePx != 128 || req.tileOccludersSrv == UINT32_MAX || req.atlasSlotsSrv == UINT32_MAX)
            fail("rasterizeDepth '%s': tile occluders need 128 px tiles and the persistent SRVs of the occluders and the atlas slots", req.name.c_str());
        v.occluderSrv = req.tileOccludersSrv;
        v.occluderSlotsSrv = req.atlasSlotsSrv;
        if (r.tileTwoPhase)
        {
            if (!req.tileGuess.valid() || req.tileGuessSrv == UINT32_MAX || !req.buildTileOccluders)
                fail("rasterizeDepth '%s': tile occluders in two phases need the tiles' guesses and the occluders' rebuild", req.name.c_str());
            v.flags |= kViewTileTwoPhase;
            v.guessSrv = req.tileGuessSrv;
        }
        else
            v.flags |= kViewTileOccluders;
    }
    return v;
}

uint32_t uploadViews(State& s, FramePassContext& fc, const std::vector<CullView>& views)
{
    const uint64_t record = fc.trackState ? fc.trackState->recordSerial() : 0;
    if (s.frame != fc.frame.frameIndex || s.record != record)
    {
        s.frame = fc.frame.frameIndex;
        s.record = record;
        s.chunkUsed = 0;
        s.viewsUsed = 0;
        s.runsUsed = 0;
    }
    if (views.size() > kViewsPerSlot) fail("V: a raster run of %zu views (at most %u)", views.size(), kViewsPerSlot);
    if (s.viewsUsed + views.size() > kViewsPerSlot)
    {
        ++s.chunkUsed;
        s.viewsUsed = 0;
    }
    if (s.chunkUsed == s.viewChunks.size())
    {
        State::ViewChunk c;
        c.buffer = createBuffer(fc.device, (uint64_t)s.slots * kViewsPerSlot * sizeof(CullView), D3D12_HEAP_TYPE_UPLOAD, L"V cull views");
        D3D12_RANGE none{ 0, 0 };
        check(c.buffer->Map(0, &none, reinterpret_cast<void**>(&c.mapped)), "map V cull views");
        s.viewChunks.push_back(c);
    }
    const State::ViewChunk& chunk = s.viewChunks[s.chunkUsed];
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % s.slots);
    const uint32_t first = slot * kViewsPerSlot + s.viewsUsed;
    std::memcpy(chunk.mapped + (uint64_t)first * sizeof(CullView), views.data(), views.size() * sizeof(CullView));
    // Every view of a run culls the same scene (C3) and the same runtime instances (C2b).
    const GpuScene::GpuInstanceRange gpuRange = fc.scene.gpuInstanceRange();
    const uint32_t runtime[5] = { s.cullScene.srv, fc.scene.staticInstanceCount(),
                                  (uint32_t)fc.scene.instances().size() > fc.scene.staticInstanceCount() ? (uint32_t)fc.scene.instances().size() - fc.scene.staticInstanceCount() : 0u,
                                  gpuRange.first, gpuRange.capacity };
    for (size_t v = 0; v < views.size(); ++v) std::memcpy(chunk.mapped + (uint64_t)(first + v) * sizeof(CullView) + offsetof(CullView, cullSceneSrv), runtime, sizeof runtime);
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
    fc.device.d3d()->CreateShaderResourceView(chunk.buffer.Get(), &sd, fc.device.descriptors().resourceCpu(srv));
    s.viewsUsed += (uint32_t)views.size();
    return srv;
}

void ensureHiz(Device& device, Hiz& h, uint32_t width, uint32_t height)
{
    const uint32_t w = (width + 1) / 2, hh = (height + 1) / 2;
    if (h.texture && h.width == w && h.height == hh) return;
    h.history = false;  // a new texture (first use or a size change) holds no previous frame
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
    BufferRef tilePairs;  // tile-local raster runs: uint2 tile rectangle per visible entry (DepthRaster.as expands the pairs)
    BufferRef tileCoarse;  // runs with a tile mask: bit per 8 x 8 tiles (TileMaskCoarse.hlsl)
    BufferRef chunkWork;   // C3: visible chunk items [0, capDeferred), deferred chunks [capDeferred, 2 capDeferred)
    BufferRef chunks, skinBounds;  // C3 persistent buffers imported for this frame (read by the cull kernels)
    BufferRef tileOccluders, occluderSlots;  // raster service: the request's tile occluders and atlas slots (tilesOcclude)
    BufferRef tileGuess;                     // ... and the tiles' guesses of a run with two-phase views
    uint32_t chunkCount = 0, flatCount = 0;
    uint32_t tileCoarseWords = 0;  // per view
    TextureRef hiz;
    uint32_t hizSrv = kNone, hizMips = 0, hizWidth = 0, hizHeight = 0;
    uint32_t viewsSrv = kNone, viewCount = 0, instanceMask = 0;
    uint32_t nodesSrv = kNone, rootsSrv = kNone, spheresSrv = kNone, sheetsSrv = kNone;
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;  // scene indices, time and wind for the kernels (b1)
    uint32_t bandMode = kBandModeA;
    bool storedPairs = false;  // tile-local run with the stored pair list (visibility.raster_amplification false, A/B)
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
    r.deferInstances = g.createBuffer({ "v.cull.deferredInstances", (uint64_t)cfg.capDeferred * 8, 8 });
    r.deferNodes = g.createBuffer({ "v.cull.deferredNodes", (uint64_t)cfg.capDeferred * 8, 8 });
    r.deferClusters = g.createBuffer({ "v.cull.deferredClusters", (uint64_t)cfg.capDeferred * 8, 8 });
    r.chunkWork = g.createBuffer({ "v.cull.chunkWork", (uint64_t)cfg.capDeferred * 16, 8 });
    r.nodesSrv = fc.scene.srv(clusterbuilder::kClusterNodes);
    r.rootsSrv = fc.scene.srv(clusterbuilder::kMeshClusterRoots);
    r.spheresSrv = fc.scene.srv(clusterbuilder::kClusterLodSpheres);
    r.sheetsSrv = fc.scene.srv(clusterbuilder::kClusterSheets);
    const State& st = fc.state<State>("v.state");
    r.chunks = st.chunksRef;
    r.skinBounds = st.skinBoundsRef;
    r.chunkCount = st.chunkCount;
    r.flatCount = st.flatCount;
    return r;
}

void declareCull(PassBuilder& b, const Run& r, Use argsUse)
{
    for (BufferRef x : { r.state, r.nodeItems, r.groupItems, r.visible, r.lists, r.deferInstances, r.deferNodes, r.deferClusters, r.chunkWork }) b.use(x, Use::UavCompute);
    for (BufferRef x : { r.chunks, r.skinBounds })
        if (x.valid()) b.use(x, Use::SrvCompute);
    b.use(r.args, argsUse);
    if (r.hiz.valid()) b.use(r.hiz, Use::SrvCompute);
    if (r.tileMask.valid()) b.use(r.tileMask, Use::SrvCompute);
    if (r.tilePairs.valid()) b.use(r.tilePairs, Use::UavCompute);
    if (r.tileCoarse.valid()) b.use(r.tileCoarse, Use::SrvCompute);
    if (r.tileOccluders.valid())
    {
        b.use(r.tileOccluders, Use::SrvCompute);
        b.use(r.occluderSlots, Use::SrvCompute);
    }
    if (r.tileGuess.valid()) b.use(r.tileGuess, Use::SrvCompute);
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
    if (r.hizMips > 31) fail("V: %u HiZ mips do not fit the 5-bit field", r.hizMips);
    k[12] = r.hizMips | c.uav(r.chunkWork) << 5;
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
    k[26] = r.bandMode | (r.storedPairs ? 256u : 0u) | (r.cfg.workQueue ? 512u : 0u);  // CullShared.hlsli BAND_MODE, TILE_STORED_PAIRS, NODE_WORK_QUEUE
    k[27] = r.tilePairs.valid() ? c.uav(r.tilePairs) : kNone;
    k[28] = halfBits(r.cfg.bandAMinPx) | halfBits(r.cfg.bandCMaxPx) << 16;
    k[29] = r.sheetsSrv;
    k[30] = r.tileCoarse.valid() ? c.srv(r.tileCoarse) : kNone;
    k[31] = r.tileCoarseWords;
}

// One dispatch of a cull pass: direct (groupsX > 0) or indirect through the run's args at 'argWord'. 'after': it reads
// what the pass's earlier dispatches wrote (a barrier stands before it).
struct CullStep
{
    std::string kernel;
    uint32_t groupsX = 0, groupsY = 0, argWord = 0;
    bool after = false;
};

// A cull pass of one or more dispatches: all direct (prepare and direct dispatches write the args: UAV) or all indirect
// (the args are their arguments). Dispatches of one pass without 'after' between them do not read each other's
// results: they write disjoint items and share the state's counters through atomics only (visibility.cull_pass_merge).
void cullPass(FramePassContext& fc, State& s, const Run& r, const std::string& name, uint32_t phase, const std::vector<CullStep>& steps)
{
    const bool direct = steps.front().groupsX > 0;
    std::vector<ID3D12PipelineState*> pso;
    for (const CullStep& st : steps)
    {
        if ((st.groupsX > 0) != direct) fail("V: cull pass '%s' mixes direct and indirect dispatches", name.c_str());
        pso.push_back(fc.shaders.compute(st.kernel));
    }
    const uint32_t instanceCount = (uint32_t)fc.scene.instances().size();
    ID3D12CommandSignature* sig = s.dispatchSignature.Get();
    fc.graph.addPass(r.prefix + name, QueueType::Graphics, [&](PassBuilder& b) { declareCull(b, r, direct ? Use::UavCompute : Use::IndirectArgs); },
                     [=](PassContext& c) {
                         uint32_t k[32];
                         cullConstants(c, r, phase, k, instanceCount);
                         for (size_t i = 0; i < steps.size(); ++i)
                         {
                             if (steps[i].after)
                             {
                                 D3D12_GLOBAL_BARRIER gb{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                                          D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
                                 D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
                                 group.pGlobalBarriers = &gb;
                                 c.cmd->Barrier(1, &group);
                             }
                             c.cmd->SetPipelineState(pso[i]);
                             if (i == 0)
                             {
                                 c.bindFrameConstants(r.frameConstants);
                                 c.computeConstants(k, 32);
                             }
                             if (direct) c.cmd->Dispatch(steps[i].groupsX, steps[i].groupsY, 1);
                             else c.cmd->ExecuteIndirect(sig, 1, c.resource(r.args), steps[i].argWord * 4, nullptr, 0);
                         }
                     });
}

void cullPass(FramePassContext& fc, State& s, const Run& r, const std::string& name, const std::string& kernel, uint32_t phase, uint32_t groupsX, uint32_t groupsY,
              uint32_t argWord)
{
    cullPass(fc, s, r, name, phase, { CullStep{ kernel, groupsX, groupsY, argWord, false } });
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
    PassChain chain(fc.graph, QueueType::Graphics, r.cfg.foldSmall);  // (the clear, then the tiles: one pass)
    for (uint32_t mode = 0; mode < 2; ++mode)
    {
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Visibility/PlanarMask.MODE" + std::to_string(mode));
        chain.add(r.prefix + (mode == 0 ? "planar.clear" : "planar.tiles"),
                  [=](PassBuilder& b) {
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
    chain.flush(r.prefix + "planar.tiles");
}

// One cull phase: instances (phase 1: all x views; phase 2: deferred), the traversal (one dispatch over the node work
// queue, or the levels), the cluster pass(es) and the draw arguments of the lists for this phase.
// visibility.cull_pass_merge: the kernels that append a pass's items keep its arguments current (raiseDispatch) and
// phase 1's draw-argument pass leaves phase 2's first arguments, so the argument passes between them go
// (prepare.chunks.p1, prepare.chunks.p2, prepare.groups with the work queue), and dispatches that do not read each
// other's results share a pass: the reset with the chunk and flat instance dispatches after it (seed.p1), the indirect
// instance dispatches (instances.indirect.p1), phase 2's deferred instances and nodes (seed.p2) and its two cluster
// dispatches (clusters.p2). The kernels and what each tests are those of the separate passes.
void cullPhase(FramePassContext& fc, State& s, const Run& r, uint32_t phase)
{
    const std::string p = std::to_string(phase);
    const bool merge = r.cfg.passMerge;
    if (phase == 1)
    {
        const uint32_t runtime = (uint32_t)fc.scene.instances().size() - fc.scene.staticInstanceCount();  // C2b
        const uint32_t gpuCapacity = fc.scene.gpuInstanceRange().capacity;  // A3 mesh particles (count on the GPU)
        const CullStep reset{ "Passes/Visibility/CullReset", 1, 1, 0, false };
        const CullStep chunks{ "Passes/Visibility/CullChunks.PHASE1", (r.chunkCount + 63) / 64, r.viewCount, 0, false };
        const CullStep flat{ "Passes/Visibility/CullInstances.PHASE1.SOURCE0", std::max((r.flatCount + runtime + 63) / 64, 1u), r.viewCount, 0, false };
        // (the GPU-written instances' live count sizes their dispatch: CullReset's VA_GPU_INSTANCES)
        const CullStep gpu{ "Passes/Visibility/CullInstances.PHASE1.SOURCE2", 0, 0, kArgGpuInstances, false };
        const CullStep members{ "Passes/Visibility/CullInstances.PHASE1.SOURCE1", 0, 0, kArgChunkItems, false };
        if (merge)
        {
            std::vector<CullStep> seed{ reset };
            if (r.chunkCount > 0) seed.push_back(chunks);
            seed.push_back(flat);
            seed[1].after = true;  // every dispatch after the reset starts from the cleared state
            cullPass(fc, s, r, "seed.p1", phase, seed);
            std::vector<CullStep> indirect;
            if (gpuCapacity > 0) indirect.push_back(gpu);
            if (r.chunkCount > 0) indirect.push_back(members);
            if (!indirect.empty()) cullPass(fc, s, r, "instances.indirect.p1", phase, indirect);
        }
        else
        {
            cullPass(fc, s, r, "reset", phase, { reset });
            if (r.chunkCount > 0) cullPass(fc, s, r, "chunks.p1", phase, { chunks });
            cullPass(fc, s, r, "instances.p1", phase, { flat });
            if (gpuCapacity > 0) cullPass(fc, s, r, "instances.gpu.p1", phase, { gpu });
            if (r.chunkCount > 0)
            {
                cullPass(fc, s, r, "prepare.chunks.p1", "Passes/Visibility/CullPrepare.MODE4", phase, 1, 1, 0);
                cullPass(fc, s, r, "instances.chunks.p1", phase, { members });
            }
        }
    }
    else
    {
        if (r.chunkCount > 0)
        {
            if (!merge) cullPass(fc, s, r, "prepare.chunks.p2", "Passes/Visibility/CullPrepare.MODE5", phase, 1, 1, 0);
            cullPass(fc, s, r, "chunks.p2", "Passes/Visibility/CullChunks.PHASE2", phase, 0, 0, kArgDeferredChunks);
        }
        cullPass(fc, s, r, "prepare.p2", "Passes/Visibility/CullPrepare.MODE2", phase, 1, 1, 0);
        const CullStep deferred{ "Passes/Visibility/CullInstances.PHASE2.SOURCE0", 0, 0, kArgDeferredInstances, false };
        const CullStep nodes{ "Passes/Visibility/CullSeed", 0, 0, kArgSeedNodes, false };
        if (merge) cullPass(fc, s, r, "seed.p2", phase, { deferred, nodes });
        else
        {
            cullPass(fc, s, r, "instances.p2", phase, { deferred });
            cullPass(fc, s, r, "seed.p2", phase, { nodes });
        }
    }
    if (r.cfg.workQueue)
        cullPass(fc, s, r, "nodes.p" + p, "Passes/Visibility/CullNodes.PHASE" + p + ".QUEUE1", phase, r.cfg.workerGroups, 1, 0);
    else
        for (uint32_t level = 0; level < s.traversalLevels; ++level)
        {
            cullPass(fc, s, r, "prepare.nodes.p" + p + "." + std::to_string(level), "Passes/Visibility/CullPrepare.MODE0", phase, 1, 1, 0);
            cullPass(fc, s, r, "nodes.p" + p + "." + std::to_string(level), "Passes/Visibility/CullNodes.PHASE" + p + ".QUEUE0", phase, 0, 0, kArgNodes);
        }
    // (the level passes' dispatch reads the args, so only the work queue's kernel can raise the cluster pass's)
    if (!(merge && r.cfg.workQueue)) cullPass(fc, s, r, "prepare.groups.p" + p, "Passes/Visibility/CullPrepare.MODE1", phase, 1, 1, 0);
    const CullStep groups{ "Passes/Visibility/CullClusters.MODE0", 0, 0, kArgGroups, false };
    const CullStep deferredClusters{ "Passes/Visibility/CullClusters.MODE1", 0, 0, kArgDeferredClusters, false };
    if (merge && phase == 2) cullPass(fc, s, r, "clusters.p2", phase, { groups, deferredClusters });
    else
    {
        cullPass(fc, s, r, "clusters.p" + p, phase, { groups });
        if (phase == 2) cullPass(fc, s, r, "clusters.deferred.p2", phase, { deferredClusters });
    }
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

// Deterministic mode's immutable-depth replay: choose the lowest stable
// (instance, cluster, triangle), then publish its transient visibility id.
void resolveDepthTies(FramePassContext& fc, State& s, const Run& r, const ViewResources& view,
                      TextureRef depth, TextureRef vis, uint32_t firstList, uint32_t endList)
{
    if (!fc.quality.has("debug.deterministic") || !fc.quality.boolean("debug.deterministic")) return;
    const uint32_t width = view.view.width, height = view.view.height;
    RenderGraph& graph = fc.graph;
    ShaderLibrary& shaders = fc.shaders;
    const auto winners = graph.createBuffer({ "v.depth tie winners", (uint64_t)width * height * 8, 0 });
    const auto args = graph.createBuffer({ "v.depth tie arguments", kLists * 12ull, 0 });
    const Run run = r;
    graph.addPass("v.depth tie init", QueueType::Compute,
                  [&](PassBuilder& b) { b.use(winners, Use::UavCompute); b.use(args, Use::UavCompute); b.use(run.state, Use::SrvCompute); },
                  [=, &shaders](PassContext& c) {
                      const uint32_t k[8] = { c.uav(winners), c.uav(args), c.srv(run.state), run.cfg.capVisible, width, height, 0, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Visibility/DepthTieInit")); c.computeConstants(k, 8);
                      c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                  });
    ID3D12CommandSignature* signature = s.meshSignature.Get();
    const auto frameConstants = view.frameConstants;
    for (uint32_t mode = 0; mode < 2; ++mode)
    {
        std::vector<ID3D12PipelineState*> pipelines;
        for (uint32_t list = firstList; list < endList; ++list)
        {
            const bool alpha = list == kListAAlphaBack || list == kListAAlphaNone || list >= kListTBack;
            const bool back = list == kListABack || list == kListAAlphaBack || list == kListTBack;
            MeshPipelineDesc d;
            d.meshShader = alpha ? "Passes/Visibility/VisRaster.ms.ALPHA1" : "Passes/Visibility/VisRaster.ms.ALPHA0";
            d.pixelShader = "Passes/Visibility/DepthTie.ps.MODE" + std::to_string(mode) + ".ALPHA" + (alpha ? "1" : "0");
            if (mode == 1) d.renderTargets = { DXGI_FORMAT_R32_UINT };
            d.depthWrite = false;
            d.cull = back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            d.frontCounterClockwise = !view.view.mirrored;
            pipelines.push_back(shaders.mesh("v.tie|" + d.pixelShader + (back ? "|back" : "|none") +
                                             (view.view.mirrored ? "|mirrored" : ""), d));
        }
        graph.addPass(mode == 0 ? "v.depth tie select" : "v.depth tie publish", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(winners, mode == 0 ? Use::UavGraphics : Use::SrvGraphics);
                          b.use(vis, mode == 0 ? Use::SrvGraphics : Use::RenderTarget);
                          b.use(depth, Use::SrvGraphics); b.use(args, Use::IndirectArgs);
                          b.use(run.visible, Use::SrvGraphics); b.use(run.lists, Use::SrvGraphics); b.use(run.state, Use::SrvGraphics);
                      },
                      [=](PassContext& c) {
                          const D3D12_VIEWPORT viewport{ 0, 0, (float)width, (float)height, 0, 1 };
                          const D3D12_RECT scissor{ 0, 0, (LONG)width, (LONG)height };
                          c.cmd->RSSetViewports(1, &viewport); c.cmd->RSSetScissorRects(1, &scissor);
                          if (mode == 1) { const auto target = c.rtv(vis); c.cmd->OMSetRenderTargets(1, &target, FALSE, nullptr); }
                          else c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                          c.bindFrameConstants(frameConstants);
                          for (uint32_t list = firstList; list < endList; ++list)
                          {
                              const uint32_t k[16] = { c.srv(run.visible), c.srv(run.lists), c.srv(run.state), list,
                                  1, run.cfg.capVisible, run.viewsSrv, 0,
                                  mode == 0 ? c.uav(winners) : c.srv(winners), c.srv(depth), width, 0,
                                  mode == 0 ? c.srv(vis) : kNone, 0, 0, 0 };
                              c.cmd->SetPipelineState(pipelines[list - firstList]); c.graphicsConstants(k, 16);
                              c.cmd->ExecuteIndirect(signature, 1, c.resource(args), list * 12ull, nullptr, 0);
                          }
                      });
    }
}

// HiZ from the depth buffer: five levels per dispatch (HiZ.hlsl), each after the previous one's writes.
// fold (visibility.fold_small_passes): the dispatches are one pass "v.hiz.<tag>" (they write mips of one texture as UAVs,
// each after a barrier); else a pass each.
void hizPasses(FramePassContext& fc, const Hiz& h, TextureRef hizRef, TextureRef depth, uint32_t width, uint32_t height, const std::string& tag, bool fold)
{
    auto mipSize = [](uint32_t m, uint32_t base) { return (base + (1u << m) - 1) >> m; };
    PassChain chain(fc.graph, QueueType::Graphics, fold);
    for (uint32_t first = 0; first < h.mips; first += 5)
    {
        const bool fromDepth = first == 0;
        ID3D12PipelineState* pso = fc.shaders.compute(fromDepth ? "Passes/Visibility/HiZ.FIRST1" : "Passes/Visibility/HiZ.FIRST0");
        const uint32_t levels = std::min(5u, h.mips - first);
        const uint32_t w0 = mipSize(first, h.width), h0 = mipSize(first, h.height);
        const uint32_t srcW = fromDepth ? width : mipSize(first - 1, h.width), srcH = fromDepth ? height : mipSize(first - 1, h.height);
        const std::vector<uint32_t> uavs = h.uavs;
        chain.add("v.hiz." + tag + "." + std::to_string(first),
                  [=](PassBuilder& b) {
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
    chain.flush("v.hiz." + tag);
}

ComPtr<ID3D12Resource> createCoverageBuffer(Device& device, uint64_t bytes, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "V coverage buffer");
    r->SetName(name);
    return r;
}

void ensureCoverage(Device& device, Coverage& cv, uint32_t width, uint32_t height, bool cover)
{
    // The depth buckets' cover (8 B per pixel of the tile grid) exists while they are configured; made later than the
    // rest (the setting changed), every tile is emptied again with it.
    if (cv.headers && cv.width == width && cv.height == height)
    {
        if (cover && !cv.cover)
        {
            cv.cover = createCoverageBuffer(device, (uint64_t)cv.tiles * kCovTilePixels * 8, L"V coverage cover (main view)");
            cv.fresh = true;
        }
        return;
    }
    for (ComPtr<ID3D12Resource>* r : { std::addressof(cv.headers), std::addressof(cv.counters), std::addressof(cv.list), std::addressof(cv.depthRange), std::addressof(cv.cover) })
        if (*r) device.deferRelease(*r);
    cv.cover.Reset();
    cv.tilesX = (width + kCovTilePx - 1) / kCovTilePx;
    cv.tiles = cv.tilesX * ((height + kCovTilePx - 1) / kCovTilePx);
    if (cover) cv.cover = createCoverageBuffer(device, (uint64_t)cv.tiles * kCovTilePixels * 8, L"V coverage cover (main view)");
    cv.headers = createCoverageBuffer(device, (uint64_t)cv.tiles * kCovTileWords * 4, L"V coverage tile headers (main view)");
    cv.counters = createCoverageBuffer(device, (uint64_t)cv.tiles * kCovTilePixels * 4, L"V coverage pixel counters (main view)");
    cv.list = createCoverageBuffer(device, (uint64_t)(kCovListInfo + 4ull * cv.tiles) * 4, L"V coverage tile list (main view)");
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = width;
    rd.Height = height;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R32G32_UINT;
    rd.SampleDesc.Count = 1;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&cv.depthRange)),
          "V coverage depth range");
    cv.depthRange->SetName(L"V coverage depth range (main view)");
    cv.width = width;
    cv.height = height;
    cv.fresh = true;
}

void readStats(FramePassContext& fc, State& s, const std::string& name);

// Coverage layer of the main view (bands B and C until the bricks exist; CoverageLayer.hlsli, CoverageTiles.hlsli), after
// the final HiZ: empty last frame's tiles, conservative raster with exact pixel areas appending to a stream, then count,
// scan, offsets and scatter into the tiles' pixel-major ranges, and the per-pixel products (opaqueCovered, depth range) in
// blocks of 1,024 records. Products: ViewResources::coverageTiles, coverageRecords, coverageTileList, coverageTilePixels,
// coverageDepthRange (INTERFACES 7.1 v1.41).
void coveragePasses(FramePassContext& fc, State& s, const Run& r, ViewResources& view)
{
    const uint32_t width = view.view.width, height = view.view.height;
    ViewState& vs = s.views[view.viewId];
    Coverage& cv = vs.coverage;
    const std::string statsName = view.viewId == 0 ? std::string("main") : "view" + std::to_string(view.viewId);
    const bool bucketsConfigured = r.cfg.coverageDepthBuckets >= 2 && r.cfg.coverageDebugStage == 0;
    ensureCoverage(fc.device, cv, width, height, bucketsConfigured);
    // Record capacity: 1.5 x the latest measured need (fragments appended in the latest completed frame, read here before
    // the frame's own passes), at least the floor, in
    // steps of 64 K records (1 MB); it grows at once and shrinks only below half, so the graph keeps its placed buffers
    // between changes. A frame whose need jumps past the capacity before the statistics catch up loses fragments and
    // reports OVERFLOW_COVERAGE.
    readStats(fc, s, statsName);
    {
        uint64_t want = (uint64_t)std::ceil(r.cfg.coveragePoolMinPerPixel * width * height);
        const auto it = s.stats.find(statsName);
        if (it != s.stats.end() && it->second.latest.frameIndex != UINT64_MAX) want = std::max<uint64_t>(want, (it->second.latest.coverageFragments * 3ull + 1) / 2);
        constexpr uint64_t kStep = 1u << 16;
        want = std::min<uint64_t>((want + kStep - 1) / kStep * kStep, kCovPoolMaxRecords);
        if (want > cv.capacity || want * 2 < cv.capacity) cv.capacity = (uint32_t)want;
        // Special list: the same rule over its own need (the latest frame's special records).
        uint64_t special = r.cfg.coverageSpecialMin;
        if (it != s.stats.end() && it->second.latest.frameIndex != UINT64_MAX) special = std::max<uint64_t>(special, (it->second.latest.coverageSpecial * 3ull + 1) / 2);
        special = std::min<uint64_t>((special + kStep - 1) / kStep * kStep, kCovPoolMaxRecords);
        if (special > cv.specialCapacity || special * 2 < cv.specialCapacity) cv.specialCapacity = (uint32_t)special;
    }
    // Depth buckets (CoverageBuckets.hlsli): the band B list drawn nearer first in buckets, each after the first cut by
    // the cover of those before. Their passes (bins, cover) are recorded when the latest completed frame's band B list
    // held at least the configured entries: a frame without thin geometry keeps the one raster pass. Either way the
    // records of the fragments with weight are the same, so the frames between a change and its statistics only cost what
    // the other path costs.
    uint32_t buckets = 1;
    if (bucketsConfigured)
    {
        const auto it = s.stats.find(statsName);
        if (it != s.stats.end() && it->second.latest.frameIndex != UINT64_MAX && it->second.latest.listEntries[kListB] >= std::max(r.cfg.coverageBucketsMinClusters, 1u))
            buckets = r.cfg.coverageDepthBuckets;
    }
    const bool binned = buckets >= 2, computeRaster = binned && r.cfg.coverageComputeRaster, triangleCull = r.cfg.coverageTriangleCull;
    RenderGraph& g = fc.graph;
    const uint32_t capacity = cv.capacity, tiles = cv.tiles, tilesX = cv.tilesX, slots = coverageScratchSlots(capacity);
    const BufferRef headers = g.importBuffer(cv.headers.Get(), { "v.coverage.tiles", (uint64_t)tiles * kCovTileWords * 4, 0 });
    const BufferRef counters = g.importBuffer(cv.counters.Get(), { "v.coverage.counters", (uint64_t)tiles * kCovTilePixels * 4, 0 });
    const BufferRef list = g.importBuffer(cv.list.Get(), { "v.coverage.tileList", (uint64_t)(kCovListInfo + 4ull * tiles) * 4, 0 });
    const TextureRef depthRange = g.importTexture(cv.depthRange.Get(), { "v.coverage.depthRange", width, height, 1, 1, DXGI_FORMAT_R32G32_UINT },
                                                  D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    const BufferRef stream = g.createBuffer({ "v.coverage.stream", (uint64_t)capacity * 16, 16 });
    const BufferRef keys = g.createBuffer({ "v.coverage.keys", (uint64_t)capacity * 4, 0 });
    const BufferRef records = g.createBuffer({ "v.coverage.records", (uint64_t)capacity * 16, 16 });
    const BufferRef starts = g.createBuffer({ "v.coverage.tilePixels", (uint64_t)tiles * kCovTilePixels * 4, 0 });
    const BufferRef scratch = g.createBuffer({ "v.coverage.scratch", (uint64_t)slots * (kCovScratchWords + 1) * 4, 0 });
    const uint32_t specialCapacity = cv.specialCapacity;
    const BufferRef special = g.createBuffer({ "v.coverage.special", 16 + (uint64_t)specialCapacity * 8, 0 });
    // Depth buckets: the persistent cover (emptied with the tiles), and the frame's binned list (per list entry its fine
    // bin and its sorted place; with the compute rasteriser 4 words of taken triangles) and bucket arguments.
    const BufferRef cover = cv.cover ? g.importBuffer(cv.cover.Get(), { "v.coverage.cover", (uint64_t)tiles * kCovTilePixels * 8, 0 }) : BufferRef{};
    const BufferRef bins = binned ? g.createBuffer({ "v.coverage.bins", ((uint64_t)kCovBinHeaderWords + (uint64_t)(computeRaster ? kCovBinEntryWords : 2u) * r.cfg.capVisible) * 4, 0 })
                                  : BufferRef{};
    const BufferRef binArgs = binned ? g.createBuffer({ "v.coverage.binArgs", kCovArgWords * 4, 0 }) : BufferRef{};
    view.coverageSpecial = special;
    view.coverageTiles = headers;
    view.coverageRecords = records;
    view.coverageTileList = list;
    view.coverageTilePixels = starts;
    view.coverageDepthRange = depthRange;
    view.coverageChunkTable = {};  // v1.40 names (Frame.h): off until M's composite reads the ranges
    view.coverageChunks = records;
    const uint32_t hizSrv = vs.hiz.srv, hizSize = vs.hiz.width | vs.hiz.height << 16;
    const float frontSign = view.view.mirrored ? -1.0f : 1.0f;
    const Run run = r;
    const TextureRef depthA = view.depth;
    // What a pass touches (the cull state always). The list is read-only (SRV) in the passes it drives indirectly.
    enum : uint32_t
    {
        kUseArgs = 1, kUseList = 2, kUseListRead = 4, kUseTiles = 8, kUseCounters = 16, kUseStream = 32, kUseRecords = 64, kUseRecordsRead = 128,
        kUseStarts = 256, kUseRange = 512, kUseScratch = 1024, kUseDepthA = 2048, kUseSpecial = 4096,
        kUseCover = 8192  // the tile clears: the depth buckets' cover in the records' slot (P[0].z)
    };
    auto constants = [=](const PassContext& c, uint32_t k[28], uint32_t uses, bool raster) {
        std::memset(k, 0, 28 * 4);
        k[0] = c.uav(run.state);
        k[1] = (uses & kUseArgs) ? c.uav(run.args) : kNone;
        k[2] = (uses & kUseRecords) ? c.uav(records) : ((uses & kUseRecordsRead) ? c.srv(records) : ((uses & kUseCover) ? c.uav(cover) : kNone));
        k[3] = (uses & kUseStream) ? c.uav(stream) : kNone;
        k[4] = (uses & kUseStream) ? c.uav(keys) : kNone;
        k[5] = (uses & kUseList) ? c.uav(list) : ((uses & kUseListRead) ? c.srv(list) : kNone);
        k[6] = capacity;
        k[7] = (uses & kUseCounters) ? c.uav(counters) : kNone;
        k[8] = hizSrv;
        k[9] = width;
        k[10] = height;
        k[11] = hizSize;
        k[12] = raster ? c.srv(run.visible) : ((uses & kUseDepthA) ? c.srv(depthA) : kNone);
        k[13] = raster ? c.srv(run.lists) : kNone;
        k[14] = run.cfg.capVisible;
        k[15] = run.viewsSrv;
        std::memcpy(&k[16], &frontSign, 4);
        k[17] = (uses & kUseTiles) ? c.uav(headers) : kNone;
        k[18] = tilesX;
        k[19] = tiles;
        k[20] = (uses & kUseStarts) ? c.uav(starts) : kNone;
        k[21] = (uses & kUseRange) ? c.uav(depthRange) : kNone;
        k[22] = (uses & kUseScratch) ? c.uav(scratch) : kNone;
        k[23] = slots;
        k[24] = (uses & kUseSpecial) ? c.uav(special) : kNone;
        k[25] = specialCapacity;
    };
    ID3D12CommandSignature* dispatchSig = s.dispatchSignature.Get();
    ID3D12CommandSignature* meshSig = s.meshSignature.Get();
    // visibility.fold_small_passes: the layer's single-group steps are timed with the pass they prepare (PassChain.h):
    // prepare with the clear, the bins' header and prefix with the classify and scatter around them, the cover's
    // arguments with the cover, the count's arguments with the count, the scan with the offsets, the special list's
    // header with the scatter, and the heavy tiles with the blocks.
    PassChain chain(g, QueueType::Graphics, r.cfg.foldSmall);
    // Compute pass of CoverageBuild: direct (groupsX > 0), or indirect at argWord of the cull args (fromList: of the tile
    // list, which the pass then reads as an SRV).
    auto build = [&](const char* name, uint32_t mode, uint32_t groupsX, uint32_t groupsY, uint32_t argWord, uint32_t uses, bool fromList = false) {
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Visibility/CoverageBuild.MODE" + std::to_string(mode));
        const bool indirect = groupsX == 0;
        chain.add(std::string("v.coverage.") + name,
                  [&](PassBuilder& b) {
                      b.use(run.state, Use::UavCompute);
                      if (indirect && !fromList) b.use(run.args, Use::IndirectArgs);
                      else if (uses & kUseArgs) b.use(run.args, Use::UavCompute);
                      if (fromList) b.use(list, Use::IndirectArgs);
                      if (uses & kUseList) b.use(list, Use::UavCompute);
                      if (uses & kUseListRead) b.use(list, Use::SrvCompute);
                      if (uses & kUseTiles) b.use(headers, Use::UavCompute);
                      if (uses & kUseCounters) b.use(counters, Use::UavCompute);
                      if (uses & kUseStream)
                      {
                          b.use(stream, Use::UavCompute);
                          b.use(keys, Use::UavCompute);
                      }
                      if (uses & kUseRecords) b.use(records, Use::UavCompute);
                      if (uses & kUseRecordsRead) b.use(records, Use::SrvCompute);
                      if (uses & kUseStarts) b.use(starts, Use::UavCompute);
                      if (uses & kUseRange) b.use(depthRange, Use::UavCompute);
                      if (uses & kUseScratch) b.use(scratch, Use::UavCompute);
                      if (uses & kUseDepthA) b.use(depthA, Use::SrvCompute);
                      if (uses & kUseSpecial) b.use(special, Use::UavCompute);
                      if (uses & kUseCover) b.use(cover, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      uint32_t k[28];
                      constants(c, k, (indirect && !fromList) ? uses & ~kUseArgs : uses, false);  // indirect: the args are read as arguments here
                      c.cmd->SetPipelineState(pso);
                      c.computeConstants(k, 28);
                      if (indirect) c.cmd->ExecuteIndirect(dispatchSig, 1, c.resource(fromList ? list : run.args), argWord * 4, nullptr, 0);
                      else c.cmd->Dispatch(groupsX, groupsY, 1);
                  });
    };
    const uint32_t coverUse = cover.valid() ? (uint32_t)kUseCover : 0u;
    if (cv.fresh)
    {
        build("init", 4, tilesX, tiles / tilesX, 0, kUseList | kUseTiles | kUseCounters | kUseRange | coverUse);
        cv.fresh = false;
    }
    build("prepare", 0, 1, 1, 0, kUseArgs | kUseListRead);
    build("clear", 1, 0, 0, kArgCovClear, kUseListRead | kUseTiles | kUseCounters | kUseRange | coverUse);
    chain.flush("v.coverage.clear");

    // The pixel kernel's variant: the measurement stage, and the fragment counters (visibility.coverage_statistics).
    const std::string psVariant = ".STAGE" + std::to_string(r.cfg.coverageDebugStage) + (r.cfg.coverageStatistics ? ".STATS1" : ".STATS0");
    const std::string psoVariant = "stage" + std::to_string(r.cfg.coverageDebugStage) + (r.cfg.coverageStatistics ? ".stats" : "");
    MeshPipelineDesc d;
    d.meshShader = "Passes/Visibility/CoverageRaster.ms";
    d.pixelShader = "Passes/Visibility/CoverageRaster.ps" + psVariant;
    d.depthFormat = DXGI_FORMAT_UNKNOWN;
    d.depthWrite = false;
    d.cull = D3D12_CULL_MODE_NONE;  // one-sided back faces are culled by the mesh kernel (with the near clip)
    d.conservative = true;
    ID3D12PipelineState* rasterPso = fc.shaders.mesh("v.coverage." + psoVariant, d);
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = view.frameConstants;
    const TextureRef hiz = r.hiz;
    // P[1].w of the cluster raster kernels (CoverageBuckets.hlsli COV_RASTER_*): the bucket, the triangle tests, the
    // compute raster, the drop of fragments without weight. The hair and stream passes take 0: their records as before.
    const uint32_t rasterFlags = (triangleCull ? 0x100u : 0u) | (computeRaster ? 0x200u : 0u) | (r.cfg.coverageDropWeightless ? 0x400u : 0u);
    // The band B list's raster: the whole list (bucket < 0), or one depth bucket of the binned list - after the first
    // with the cover (fragments) and the tile headers (clusters, triangles) the buckets before it left.
    auto addClusterRaster = [&](int bucket) {
        const bool fromBins = bucket >= 0, covered = bucket > 0;
        g.addPass("v.coverage.raster", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(fromBins ? binArgs : run.args, Use::IndirectArgs);
                      b.use(run.visible, Use::SrvGraphics);
                      b.use(run.lists, Use::SrvGraphics);
                      b.use(run.state, Use::UavGraphics);
                      b.use(stream, Use::UavGraphics);
                      b.use(keys, Use::UavGraphics);
                      if (hiz.valid()) b.use(hiz, Use::SrvGraphics);
                      if (fromBins) b.use(bins, Use::SrvGraphics);
                      if (covered)
                      {
                          b.use(cover, Use::SrvGraphics);
                          b.use(headers, Use::SrvGraphics);
                      }
                  },
                  [=](PassContext& c) {
                      uint32_t k[32];
                      constants(c, k, 0, true);
                      k[1] = fromBins ? c.srv(bins) : kNone;     // COV_BINS
                      k[2] = covered ? c.srv(cover) : kNone;     // COV_COVER
                      k[3] = c.uav(stream);
                      k[4] = c.uav(keys);
                      k[5] = covered ? c.srv(headers) : kNone;   // COV_TILE_HIZ
                      k[7] = (fromBins ? (uint32_t)bucket : 0u) | (fromBins ? rasterFlags : rasterFlags & ~0x200u);
                      if (!hiz.valid()) k[8] = kNone;
                      std::memset(&k[24], 0, 8 * 4);
                      k[24] = kListB;  // COV_RASTER_LIST; P[6].y 0: band B
                      k[30] = kNone;
                      c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                      const D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
                      const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };
                      c.cmd->RSSetViewports(1, &vp);
                      c.cmd->RSSetScissorRects(1, &sc);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->SetPipelineState(rasterPso);
                      c.graphicsConstants(k, 32);
                      if (fromBins) c.cmd->ExecuteIndirect(meshSig, 1, c.resource(binArgs), (kCovArgDraw + 3 * (uint32_t)bucket) * 4, nullptr, 0);
                      else c.cmd->ExecuteIndirect(meshSig, 1, c.resource(run.args), kArgCovMesh * 4, nullptr, 0);
                  });
    };
    if (!binned) addClusterRaster(-1);
    else
    {
        // CoverageBins.hlsl: direct (groupsX > 0) or indirect at argWord of the bin arguments.
        enum : uint32_t { kBinArgs = 1, kBinLists = 2, kBinVisible = 4, kBinHiz = 8, kBinStream = 16, kBinCover = 32, kBinRead = 64 };
        auto bin = [&](const char* name, uint32_t mode, uint32_t groupsX, uint32_t argWord, uint32_t uses, uint32_t bucket) {
            ID3D12PipelineState* pso = fc.shaders.compute("Passes/Visibility/CoverageBins.MODE" + std::to_string(mode));
            const bool indirect = groupsX == 0;
            chain.add(std::string("v.coverage.") + name,
                      [&](PassBuilder& b) {
                          b.use(run.state, Use::UavCompute);
                          b.use(bins, (uses & kBinRead) ? Use::SrvCompute : Use::UavCompute);
                          if (indirect) b.use(binArgs, Use::IndirectArgs);
                          else if (uses & kBinArgs) b.use(binArgs, Use::UavCompute);
                          if (uses & kBinLists) b.use(run.lists, Use::SrvCompute);
                          if (uses & kBinVisible) b.use(run.visible, Use::SrvCompute);
                          if ((uses & kBinHiz) && hiz.valid()) b.use(hiz, Use::SrvCompute);
                          if (uses & kBinStream)
                          {
                              b.use(stream, Use::UavCompute);
                              b.use(keys, Use::UavCompute);
                          }
                          if (uses & kBinCover)
                          {
                              b.use(cover, Use::UavCompute);
                              b.use(headers, Use::UavCompute);
                          }
                      },
                      [=](PassContext& c) {
                          uint32_t k[28];
                          std::memset(k, 0, sizeof k);
                          k[0] = c.uav(run.state);
                          k[1] = (uses & kBinRead) ? c.srv(bins) : c.uav(bins);                       // COV_BINS
                          k[2] = (uses & kBinCover) ? c.uav(cover) : kNone;                           // COV_COVER
                          k[3] = (uses & kBinStream) ? c.uav(stream) : kNone;
                          k[4] = (uses & kBinStream) ? c.uav(keys) : kNone;
                          k[5] = (!indirect && (uses & kBinArgs)) ? c.uav(binArgs) : kNone;            // COV_BIN_ARGS
                          k[6] = capacity;
                          k[7] = bucket;
                          k[8] = (uses & kBinHiz) && hiz.valid() ? hizSrv : kNone;
                          k[9] = width;
                          k[10] = height;
                          k[11] = hizSize;
                          k[12] = (uses & kBinVisible) ? c.srv(run.visible) : kNone;
                          k[13] = (uses & kBinLists) ? c.srv(run.lists) : kNone;
                          k[14] = run.cfg.capVisible;
                          k[15] = run.viewsSrv;
                          std::memcpy(&k[16], &frontSign, 4);
                          k[17] = (uses & kBinCover) ? c.uav(headers) : kNone;
                          k[18] = tilesX;
                          k[19] = tiles;
                          k[24] = buckets;                                                            // COV_BUCKETS
                          c.cmd->SetPipelineState(pso);
                          c.bindFrameConstants(frameConstants);
                          c.computeConstants(k, 28);
                          if (indirect) c.cmd->ExecuteIndirect(dispatchSig, 1, c.resource(binArgs), argWord * 4, nullptr, 0);
                          else c.cmd->Dispatch(groupsX, 1, 1);
                      });
        };
        bin("bins.begin", 0, (kCovBinHeaderWords + 63) / 64, 0, kBinArgs, 0);
        bin("bins.classify", 1, 0, kCovArgEntries, kBinLists | kBinVisible | kBinHiz, 0);
        bin("bins.prefix", 2, 1, 0, kBinArgs, 0);
        bin("bins.scatter", 3, 0, kCovArgEntries, kBinLists, 0);
        chain.flush("v.coverage.bins");
        // The compute rasteriser of a bucket's small triangles, before its mesh raster (which skips what this took).
        ID3D12PipelineState* swPso = computeRaster ? fc.shaders.compute(std::string("Passes/Visibility/CoverageRasterSw.STATS") + (r.cfg.coverageStatistics ? "1" : "0")) : nullptr;
        auto addComputeRaster = [&](uint32_t bucket) {
            const bool covered = bucket > 0;
            g.addPass("v.coverage.sw", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(binArgs, Use::IndirectArgs);
                          b.use(run.visible, Use::SrvCompute);
                          b.use(run.lists, Use::SrvCompute);
                          b.use(run.state, Use::UavCompute);
                          b.use(stream, Use::UavCompute);
                          b.use(keys, Use::UavCompute);
                          b.use(bins, Use::UavCompute);
                          if (hiz.valid()) b.use(hiz, Use::SrvCompute);
                          if (covered)
                          {
                              b.use(cover, Use::SrvCompute);
                              b.use(headers, Use::SrvCompute);
                          }
                      },
                      [=](PassContext& c) {
                          uint32_t k[32];
                          constants(c, k, 0, true);
                          k[1] = c.uav(bins);                        // COV_BINS (the taken triangles are written)
                          k[2] = covered ? c.srv(cover) : kNone;     // COV_COVER
                          k[3] = c.uav(stream);
                          k[4] = c.uav(keys);
                          k[5] = covered ? c.srv(headers) : kNone;   // COV_TILE_HIZ
                          k[7] = bucket | rasterFlags;
                          if (!hiz.valid()) k[8] = kNone;
                          std::memset(&k[24], 0, 8 * 4);
                          k[24] = kListB;
                          k[30] = kNone;
                          c.cmd->SetPipelineState(swPso);
                          c.bindFrameConstants(frameConstants);
                          c.computeConstants(k, 32);
                          c.cmd->ExecuteIndirect(dispatchSig, 1, c.resource(binArgs), (kCovArgDraw + 3 * bucket) * 4, nullptr, 0);
                      });
        };
        for (uint32_t bucket = 0; bucket < buckets; ++bucket)
        {
            if (computeRaster) addComputeRaster(bucket);
            addClusterRaster((int)bucket);
            if (bucket + 1 < buckets)  // what the bucket stored enters the cover the next ones read
            {
                bin("cover.args", 4, 1, 0, kBinArgs, bucket);
                bin("cover", 5, 0, kCovArgCover, kBinRead | kBinStream | kBinCover, bucket);
                chain.flush("v.coverage.cover");
            }
        }
    }
    // A6: the translucent lists' see-through records in the pixels of translucent class 2 (edges, seams, overlaps).
    const TextureRef translucentClass = view.translucentClass;
    if (translucentClass.valid())
        g.addPass("v.coverage.translucent", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(run.args, Use::IndirectArgs);
                      b.use(run.visible, Use::SrvGraphics);
                      b.use(run.lists, Use::SrvGraphics);
                      b.use(run.state, Use::UavGraphics);
                      b.use(stream, Use::UavGraphics);
                      b.use(keys, Use::UavGraphics);
                      b.use(translucentClass, Use::SrvGraphics);
                      if (hiz.valid()) b.use(hiz, Use::SrvGraphics);
                  },
                  [=](PassContext& c) {
                      uint32_t k[32];
                      constants(c, k, 0, true);
                      k[3] = c.uav(stream);
                      k[4] = c.uav(keys);
                      if (!hiz.valid()) k[8] = kNone;
                      k[7] = rasterFlags & ~0x200u;  // (no bucket: the translucent lists are drawn whole)
                      std::memset(&k[24], 0, 8 * 4);
                      k[25] = 1;  // COV_RASTER_TRANSLUCENT
                      k[30] = c.srv(translucentClass);
                      c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                      const D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
                      const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };
                      c.cmd->RSSetViewports(1, &vp);
                      c.cmd->RSSetScissorRects(1, &sc);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->SetPipelineState(rasterPso);
                      for (uint32_t l = kListTBack; l <= kListTNone; ++l)
                      {
                          k[24] = l;  // COV_RASTER_LIST
                          c.graphicsConstants(k, 32);
                          c.cmd->ExecuteIndirect(meshSig, 1, c.resource(run.args), (kArgCovTMesh + 3 * (l - kListTBack)) * 4, nullptr, 0);
                      }
                  });
    // B10 strand hair (E's FrameResources::hairSegments / hairBodies): one mesh group per 32 segments, the same pixel
    // kernel and stream (HairRaster.ms). Its group count is known here (E sizes the buffer per frame).
    const BufferRef hairSegments = fc.resources.hairSegments, hairBodies = fc.resources.hairBodies;
    if (r.cfg.coverageHair && hairSegments.valid() && hairBodies.valid())
    {
        const uint32_t segments = (uint32_t)(g.desc(hairSegments).size / 32);
        const uint32_t groups = (segments + 31) / 32;
        MeshPipelineDesc hd = d;
        hd.meshShader = "Passes/Visibility/HairRaster.ms";
        ID3D12PipelineState* hairPso = fc.shaders.mesh("v.coverage.hair." + psoVariant, hd);
        g.addPass("v.coverage.hair", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(hairSegments, Use::SrvGraphics);
                      b.use(hairBodies, Use::SrvGraphics);
                      b.use(run.state, Use::UavGraphics);
                      b.use(stream, Use::UavGraphics);
                      b.use(keys, Use::UavGraphics);
                      if (hiz.valid()) b.use(hiz, Use::SrvGraphics);
                  },
                  [=](PassContext& c) {
                      uint32_t k[28];
                      constants(c, k, 0, false);  // not the cluster lists: the hair inputs take P[3].xyz
                      k[3] = c.uav(stream);
                      k[4] = c.uav(keys);
                      k[7] = 0;  // (the pixel kernel's switches, COV_RASTER_*: none)
                      if (!hiz.valid()) k[8] = kNone;
                      k[12] = c.srv(hairSegments);
                      k[13] = c.srv(hairBodies);
                      k[14] = segments;
                      c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                      const D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
                      const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };
                      c.cmd->RSSetViewports(1, &vp);
                      c.cmd->RSSetScissorRects(1, &sc);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->SetPipelineState(hairPso);
                      c.graphicsConstants(k, 24);
                      if (groups > 0) c.cmd->DispatchMesh(std::min(groups, 65535u), (groups + 65534) / 65535, 1);
                  });
    }
    // GPU triangle streams (W's water and fluid surfaces, FrameResources::triangleStreams, v1.60): see-through records, one
    // mesh group per 32 triangles of each stream's capacity (the live count comes from its draw arguments on the GPU).
    const std::vector<TriangleStream>& streams = fc.resources.triangleStreams;
    if (streams.size() > kMaxTriangleStreams) fail("V: %zu triangle streams (at most %u)", streams.size(), kMaxTriangleStreams);
    for (uint32_t slot = 0; slot < (uint32_t)streams.size(); ++slot)
    {
        const TriangleStream st = streams[slot];
        if (!st.vertices.valid() || !st.drawArgs.valid() || st.maxTriangles == 0) continue;
        // v1.64: a water-layer stream gives records only in the layer's edge pixels (its interior is the layer's sample).
        const bool edgeOnly = st.layer == 1;
        const TextureRef waterVis = view.waterVis, waterDepth = view.waterDepth, bandADepth = view.depth;
        if (edgeOnly && !waterVis.valid()) continue;
        if (st.maxTriangles > (1u << 24)) fail("V: triangle stream %u holds %u triangles (at most 2^24)", slot, st.maxTriangles);
        if (g.desc(st.vertices).size < (uint64_t)st.maxTriangles * 96) fail("V: triangle stream %u: vertex buffer below %u triangles", slot, st.maxTriangles);
        const uint32_t groups = (st.maxTriangles + 31) / 32;
        MeshPipelineDesc sd = d;
        sd.meshShader = "Passes/Visibility/StreamRaster.ms";
        ID3D12PipelineState* streamPso = fc.shaders.mesh("v.coverage.stream." + psoVariant, sd);
        g.addPass("v.coverage.stream", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(st.vertices, Use::SrvGraphics);
                      b.use(st.drawArgs, Use::SrvGraphics);
                      if (edgeOnly)
                      {
                          b.use(waterVis, Use::SrvGraphics);
                          b.use(waterDepth, Use::SrvGraphics);
                          b.use(bandADepth, Use::SrvGraphics);
                      }
                      b.use(run.state, Use::UavGraphics);
                      b.use(stream, Use::UavGraphics);
                      b.use(keys, Use::UavGraphics);
                      if (hiz.valid()) b.use(hiz, Use::SrvGraphics);
                  },
                  [=](PassContext& c) {
                      uint32_t k[30];
                      constants(c, k, 0, false);  // the stream inputs take P[3].xyz
                      k[3] = c.uav(stream);
                      k[4] = c.uav(keys);
                      k[7] = 0;  // (the pixel kernel's switches, COV_RASTER_*: none)
                      if (!hiz.valid()) k[8] = kNone;
                      k[12] = c.srv(st.vertices);
                      k[13] = c.srv(st.drawArgs);
                      k[14] = st.maxTriangles;
                      k[24] = slot;
                      k[25] = st.material;
                      k[26] = edgeOnly ? c.srv(waterVis) : kNone;
                      k[27] = edgeOnly ? c.srv(waterDepth) : kNone;
                      k[28] = edgeOnly ? c.srv(bandADepth) : kNone;
                      k[29] = slot;
                      c.cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
                      const D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
                      const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };
                      c.cmd->RSSetViewports(1, &vp);
                      c.cmd->RSSetScissorRects(1, &sc);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->SetPipelineState(streamPso);
                      c.graphicsConstants(k, 30);
                      c.cmd->DispatchMesh(std::min(groups, 65535u), (groups + 65534) / 65535, 1);
                  });
    }
    // v1.73: other tracks append coverage records here (W's ocean edges), before the count.
    view.coverageState = run.state;
    view.coverageStream = stream;
    view.coverageKeys = keys;
    view.coverageCapacity = capacity;
    view.coverageTilesX = tilesX;
    if (fc.services.coverageAppend) fc.services.coverageAppend(fc, view);
    build("args", 2, 1, 1, 0, kUseArgs);
    build("count", 6, 0, 0, kArgCovRecords, kUseList | kUseTiles | kUseCounters | kUseStream);
    chain.flush("v.coverage.count");
    build("scan", 7, 1, 1, 0, kUseList | kUseTiles | kUseScratch);
    build("offsets", 9, 0, 0, kCovListArgs, kUseListRead | kUseTiles | kUseCounters | kUseStarts | kUseScratch, true);
    chain.flush("v.coverage.offsets");
    build("scatter", 8, 0, 0, kArgCovRecords, kUseCounters | kUseStream | kUseRecords | kUseSpecial);
    build("special", 10, 1, 1, 0, kUseSpecial);
    chain.flush("v.coverage.scatter");
    build("blocks", 3, 0, 0, kCovListBlockArgs, kUseListRead | kUseTiles | kUseRecordsRead | kUseRange | kUseScratch | kUseDepthA, true);
    build("heavy", 5, 0, 0, kCovListHeavyArgs, kUseListRead | kUseTiles | kUseRange | kUseScratch | kUseDepthA, true);
    chain.flush("v.coverage.blocks");
}

// Reads the named run's statistics of the frame that last used this frame's slot (complete: the caller waited for
// that slot before recording, FrameRenderer contract) into StatsRun::latest, once per frame.
void readStats(FramePassContext& fc, State& s, const std::string& name)
{
    State::StatsRun& run = s.stats[name];
    if (!run.readback)
    {
        run.readback = createBuffer(fc.device, (uint64_t)s.slots * kReadbackBytes, D3D12_HEAP_TYPE_READBACK, L"V stats readback");
        check(run.readback->Map(0, nullptr, reinterpret_cast<void**>(&run.mapped)), "map V stats");
        run.frames.assign(s.slots, UINT64_MAX);
    }
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % s.slots);
    if (run.frames[slot] != UINT64_MAX && run.frames[slot] != run.latest.frameIndex)
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
        st.coverageTiles = w[kStateCovTiles];
        st.coverageBlocks = w[kStateCovBlocks];
        st.coverageHeavyTiles = w[kStateCovHeavy];
        st.coveragePoolRecords = w[kStateCovPool];
        st.coverageSpecial = w[kStateCovSpecial];
        st.oceanEdges = w[kStateOceanEdges];
        st.coverageMeasured = w[kStateCovMeasured];
        st.coverageInvocations = w[kStateCovInvocations];
        st.coverageClustersBehindBandA = w[kStateCovClustersHiz];
        st.coverageClustersBehindCover = w[kStateCovClustersTile];
        st.coverageTriangles = w[kStateCovTriangles];
        st.coverageTrianglesBehindBandA = w[kStateCovTrianglesHiz];
        st.coverageTrianglesBehindCover = w[kStateCovTrianglesTile];
        st.coverageTrianglesCompute = w[kStateCovTrianglesSw];
        st.coverageFragmentsCompute = w[kStateCovFragmentsSw];
        st.coverageEvaluated = w[kStateCovEvaluated];
        st.coverageCutBandA = w[kStateCovCutBandA];
        st.coverageCutCover = w[kStateCovCutCover];
        st.coverageCutAlpha = w[kStateCovCutAlpha];
        st.coverageCutWeight = w[kStateCovCutWeight];
        st.mixedClusters = w[kStateStatMixedClusters];
        st.mixedTriangles = w[kStateStatMixedTriangles];
        st.chunkItems = w[kStateChunkItems];
        st.deferredChunks = w[kStateDeferChunks];
        st.overflow = w[kStateOverflow];
        run.overflowSeen |= st.overflow;
        st.overflowSeen = run.overflowSeen;
        if (st.overflow && !run.overflowReported)
        {
            logf("V: capacity exceeded in '%s', frame %llu (bits 0x%x): raise visibility.max_* (Stats::overflow; 0x100: the coverage record pool ran "
                 "out, it grows from the next completed frame; 0x400: a shader loop reached its iteration bound or a coverage consistency check failed, "
                 "a defect)\n",
                 name.c_str(), (unsigned long long)st.frameIndex, st.overflow);
            // the need (the counters count past the capacity): visible entries, tile pairs, deferred items
            logf("V: '%s' frame %llu needed %u visible entries, %u tile pairs, %u deferred instances, %u deferred nodes, %u deferred clusters\n",
                 name.c_str(), (unsigned long long)st.frameIndex, st.visibleClusters, st.tilePairs, st.deferredInstances, st.deferredNodes, st.deferredClusters);
            run.overflowReported = true;
        }
        run.latest = st;
        if (st.overflow && name != "main")
        {
            // a depth raster run dropped geometry: its requester redraws what it kept (DepthRaster.h DepthRasterOverflows)
            DepthRasterOverflow& o = fc.state<DepthRasterOverflows>(kDepthRasterOverflowKey)[name];
            if (o.frame == UINT64_MAX || st.frameIndex > o.frame) o = { st.frameIndex, st.overflow };
        }
    }
}

// Where this frame's copy of a run's cull state goes (the run's readback slot of this frame).
struct StatsCopy
{
    ID3D12Resource* readback = nullptr;
    uint64_t offset = 0;
};

// Reads the run's statistics of the frame that last used this frame's slot (if not read yet this frame) and gives the
// slot to this frame: the caller copies the run's final state there (in a pass that is kept).
StatsCopy statsCopy(FramePassContext& fc, State& s, const std::string& name)
{
    readStats(fc, s, name);
    State::StatsRun& run = s.stats[name];
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % s.slots);
    run.frames[slot] = fc.frame.frameIndex;
    return { run.readback.Get(), (uint64_t)slot * kReadbackBytes };
}

// This frame's copy of the run's statistics, in a pass of its own.
void recordStats(FramePassContext& fc, State& s, const Run& r, const std::string& name)
{
    const StatsCopy copy = statsCopy(fc, s, name);
    fc.graph.addPass(r.prefix + "stats", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(r.state, Use::CopySrc);
                         b.keep();
                     },
                     [=](PassContext& c) { c.cmd->CopyBufferRegion(copy.readback, copy.offset, c.resource(r.state), 0, kStateWords * 4); });
}
} // namespace

void waterLayer(FramePassContext& fc, State& s, const Run& r, ViewResources& view);  // below
void translucentLayer(FramePassContext& fc, State& s, const Run& r, ViewResources& view);

void visibility(FramePassContext& fc, ViewResources& view)
{
    State& s = state(fc);
    refreshScene(s, fc);
    const Settings cfg = Settings::load(fc.quality);
    const bool main = view.view.kind == gpu::ViewKind::Main;
    // A14: every view but a planar reflection is a full view: two-phase occlusion against its own HiZ, the coverage,
    // water and translucent layers, its own histories (ViewState under view.viewId).
    const bool full = view.view.kind != gpu::ViewKind::PlanarReflection;
    if (main && view.viewId != 0) fail("V: the main view has view id 0");
    if (!main && full && view.viewId == 0) fail("V: an auxiliary view needs a nonzero view id");
    const uint32_t width = view.view.width, height = view.view.height;
    RenderGraph& g = fc.graph;
    if (main)
    {
        s.mainFrameConstants = view.frameConstants;
        s.mainFrameConstantsFrame = fc.frame.frameIndex;
        // Views not drawn for framesInFlight + 1 frames give their persistent resources back.
        for (auto it = s.views.begin(); it != s.views.end();)
        {
            if (it->first != 0 && it->second.lastFrame != UINT64_MAX && it->second.lastFrame + fc.framesInFlight + 1 < fc.frame.frameIndex)
            {
                DescriptorHeaps& h = fc.device.descriptors();
                if (it->second.hiz.texture) fc.device.deferRelease(it->second.hiz.texture);
                if (it->second.hiz.srv != kNone) h.freeResource(it->second.hiz.srv);
                for (uint32_t i : it->second.hiz.uavs) h.freeResource(i);
                for (ComPtr<ID3D12Resource>* b : { std::addressof(it->second.coverage.headers), std::addressof(it->second.coverage.counters), std::addressof(it->second.coverage.list), std::addressof(it->second.coverage.depthRange), std::addressof(it->second.coverage.cover) })
                    if (*b) fc.device.deferRelease(*b);
                it = s.views.erase(it);
            }
            else
                ++it;
        }
    }
    ViewState* vs = full ? &s.views[view.viewId] : nullptr;
    if (vs) vs->lastFrame = fc.frame.frameIndex;
    const std::string statsName = main ? std::string("main") : (full ? "view" + std::to_string(view.viewId) : std::string("secondary"));

    view.depth = g.createTexture({ "v.depth", width, height, 1, 1, DXGI_FORMAT_D32_FLOAT });
    view.visId = g.createTexture({ "v.visId", width, height, 1, 1, DXGI_FORMAT_R32_UINT });
    prepareCullScene(fc, s, view.frameConstants);
    Run r = createRun(fc, cfg, main ? "v.cull." : (full ? "v.cull.view" + std::to_string(view.viewId) + "." : "v.cull.secondary."), view.frameConstants);
    view.visibleClusters = r.visible;
    r.viewCount = 1;
    // Secondary views (planar reflections) still draw every band in the vis buffer: their coverage layer needs its own
    // persistent heads and M's composite in that view (V status).
    r.bandMode = full && cfg.coverageLayer ? (cfg.coverageBandCVisible ? kBandModeCVisible : (cfg.coverageBandC ? kBandModeCoverage : kBandModeFull)) : kBandModeA;

    // Full views: two-phase occlusion against their persistent HiZ once a previous frame produced one. Planar
    // reflection views have no history: one phase without occlusion, no HiZ.
    bool occlusion = false;
    if (vs)
    {
        ensureHiz(fc.device, vs->hiz, width, height);
        occlusion = cfg.occlusion && vs->hiz.history;
        r.hiz = g.importTexture(vs->hiz.texture.Get(), { main ? "v.hiz" : "v.hiz.view", vs->hiz.allocWidth, vs->hiz.allocHeight, 1, (uint16_t)vs->hiz.mips, DXGI_FORMAT_R32_FLOAT },
                                D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
        view.hiz = r.hiz;
        r.hizSrv = vs->hiz.srv;
        r.hizMips = vs->hiz.mips;
        r.hizWidth = vs->hiz.width;
        r.hizHeight = vs->hiz.height;
    }
    CullView cullView = viewOf(view.view, cfg, occlusion);
    if (vs)
    {
        // Band hysteresis (a) re-evaluates the band from the previous frame's camera; a history discontinuity (cut,
        // restore) starts over from this frame's.
        // C9: the previous camera position in this frame's coordinates.
        if (vs->hasPrevPosition) vs->prevPosition = vs->prevPosition - fc.frame.originShift;
        if (vs->hasPrevPosition && fc.frame.discontinuity == 0) cullView.prevPosition = vs->prevPosition;
        vs->prevPosition = view.view.position;
        vs->hasPrevPosition = true;
    }
    r.viewsSrv = uploadViews(s, fc, { cullView });
    if (view.view.planarMask.valid())
    {
        if (main) fail("V: ViewDesc::planarMask is for planar reflection views");
        planarTileMask(fc, r, view.view);
        tileCoarsePass(fc, r, { cullView });
    }

    cullPhase(fc, s, r, 1);
    rasterPass(fc, s, r, view, 1);
    if (!vs)
    {
        resolveDepthTies(fc, s, r, view, view.depth, view.visId, 0, kAListCount);
        recordStats(fc, s, r, statsName);  // the frame's last planar reflection view (R's planar reflection costs)
        return;
    }
    hizPasses(fc, vs->hiz, r.hiz, view.depth, width, height, occlusion ? "p1" : "final", cfg.foldSmall);
    if (occlusion)
    {
        cullPhase(fc, s, r, 2);
        rasterPass(fc, s, r, view, 2);
        hizPasses(fc, vs->hiz, r.hiz, view.depth, width, height, "final", cfg.foldSmall);
    }
    resolveDepthTies(fc, s, r, view, view.depth, view.visId, 0, kAListCount);
    waterLayer(fc, s, r, view);
    if (r.bandMode != kBandModeA) translucentLayer(fc, s, r, view);
    if (r.bandMode != kBandModeA) coveragePasses(fc, s, r, view);
    recordStats(fc, s, r, statsName);
    vs->hiz.history = true;  // complete for the next frame once this frame's passes run
}

// A6 translucent layer (v1.67; A's decision): band A width glass and water clusters of the main view (LIST_T_BACK,
// LIST_T_NONE: every entry of both cull phases), drawn over a copy of band A's depth with the material's alpha test
// (VisRaster.ms.ALPHA1, TranslucentLayer.ps):
//   clear    the count (TranslucentClass MODE=0);
//   count    depth test only (in front of band A): the number of translucent surfaces at each pixel centre;
//   nearest  depth test and write: the nearest one's vis id and linear view depth (ViewResources::translucentVis, Depth);
//   class    per pixel (TranslucentClass.hlsl): 0 none, 1 the sample alone covers the pixel, 2 records (count >= 2, or an
//            edge: a 3 x 3 neighbour with another count or surface, or a depth bend). CoverageRaster then draws the
//            translucent lists into the coverage layer, keeping class 2 pixels only (coveragePasses).
// Skipped when the scene has no glass or water material (the layer's full-screen work: a depth copy, the count clear, the
// target clears and the class pass).
void translucentLayer(FramePassContext& fc, State& s, const Run& r, ViewResources& view)
{
    bool any = false;
    for (const gpu::Material& m : fc.scene.materials())
        any = any || (m.classFlags & 0xFFu) == 3u || (m.classFlags & 0xFFu) == 4u;  // scene::MaterialClass Water, Glass
    if (!any) return;
    RenderGraph& g = fc.graph;
    const uint32_t width = view.view.width, height = view.view.height;
    const TextureRef depthA = view.depth;
    const TextureRef depth = g.createTexture({ "v.translucent.depth.test", width, height, 1, 1, DXGI_FORMAT_D32_FLOAT });
    const TextureRef count = g.createTexture({ "v.translucent.count", width, height, 1, 1, DXGI_FORMAT_R32_UINT });
    const TextureRef vis = g.createTexture({ "v.translucent.vis", width, height, 1, 1, DXGI_FORMAT_R32_UINT });
    const TextureRef linear = g.createTexture({ "v.translucent.depth", width, height, 1, 1, DXGI_FORMAT_R32_FLOAT });
    const TextureRef cls = g.createTexture({ "v.translucent.class", width, height, 1, 1, DXGI_FORMAT_R8_UINT });
    // (visibility.fold_small_passes: the depth copy and the count's clear are one pass - they touch different textures)
    PassChain chain(g, QueueType::Graphics, r.cfg.foldSmall);
    chain.add("v.translucent.copy",
              [&](PassBuilder& b) {
                  b.use(depthA, Use::CopySrc);
                  b.use(depth, Use::CopyDst);
              },
              [=](PassContext& c) { c.cmd->CopyResource(c.resource(depth), c.resource(depthA)); });
    ID3D12PipelineState* pso[2][2];  // [count, nearest][back, none]
    for (uint32_t mode = 0; mode < 2; ++mode)
        for (uint32_t back = 0; back < 2; ++back)
        {
            MeshPipelineDesc d;
            d.meshShader = "Passes/Visibility/VisRaster.ms.ALPHA1";
            d.pixelShader = "Passes/Visibility/TranslucentLayer.ps.MODE" + std::to_string(mode);
            if (mode == 1) d.renderTargets = { DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_FLOAT };
            d.depthFormat = DXGI_FORMAT_D32_FLOAT;
            d.depthFunc = D3D12_COMPARISON_FUNC_GREATER;  // reversed Z: in front of band A (and, nearest: of nearer ones)
            d.depthWrite = mode == 1;
            d.cull = back == 0 ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            d.frontCounterClockwise = !view.view.mirrored;
            pso[mode][back] = fc.shaders.mesh(std::string("v.translucent|") + (mode == 0 ? "count" : "nearest") + (back == 0 ? "|back" : "|none") +
                                                  (view.view.mirrored ? "|mirrored" : ""),
                                              d);
        }
    ID3D12CommandSignature* sig = s.meshSignature.Get();
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = view.frameConstants;
    const Run run = r;
    auto draw = [=](PassContext& c, uint32_t mode, const uint32_t* extra) {
        const D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
        const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };
        c.cmd->RSSetViewports(1, &vp);
        c.cmd->RSSetScissorRects(1, &sc);
        c.bindFrameConstants(frameConstants);
        for (uint32_t l = kListTBack; l <= kListTNone; ++l)
        {
            // VisRaster.ms: P[1].x = 1 draws the list from entry 0; the translucent lists' args cover every entry.
            const uint32_t k[12] = { c.srv(run.visible), c.srv(run.lists), c.srv(run.state), l, 1, run.cfg.capVisible, run.viewsSrv, 0, extra[0], 0, 0, 0 };
            c.cmd->SetPipelineState(pso[mode][l - kListTBack]);
            c.graphicsConstants(k, 12);
            c.cmd->ExecuteIndirect(sig, 1, c.resource(run.args), (kArgMesh + 3 * l) * 4, nullptr, 0);
        }
    };
    ID3D12PipelineState* clearPso = fc.shaders.compute("Passes/Visibility/TranslucentClass.MODE0");
    chain.add("v.translucent.clear", [&](PassBuilder& b) { b.use(count, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[8] = { kNone, kNone, kNone, c.uav(count), kNone, width, height, 0 };
                  c.cmd->SetPipelineState(clearPso);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
              });
    chain.flush("v.translucent.clear");
    g.addPass("v.translucent.count", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(run.args, Use::IndirectArgs);
                  b.use(run.visible, Use::SrvGraphics);
                  b.use(run.lists, Use::SrvGraphics);
                  b.use(run.state, Use::SrvGraphics);
                  b.use(depth, Use::DepthWrite);
                  b.use(count, Use::UavGraphics);
              },
              [=](PassContext& c) {
                  const D3D12_CPU_DESCRIPTOR_HANDLE dsv = c.dsv(depth);
                  c.cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                  const uint32_t extra[1] = { c.uav(count) };
                  draw(c, 0, extra);
              });
    g.addPass("v.translucent.nearest", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(run.args, Use::IndirectArgs);
                  b.use(run.visible, Use::SrvGraphics);
                  b.use(run.lists, Use::SrvGraphics);
                  b.use(run.state, Use::SrvGraphics);
                  b.use(depth, Use::DepthWrite);
                  b.use(vis, Use::RenderTarget);
                  b.use(linear, Use::RenderTarget);
              },
              [=](PassContext& c) {
                  const D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = { c.rtv(vis), c.rtv(linear) }, dsv = c.dsv(depth);
                  const float none[4] = { 0, 0, 0, 0 }, infinite[4] = { INFINITY, INFINITY, INFINITY, INFINITY };
                  c.cmd->ClearRenderTargetView(rtv[0], none, 0, nullptr);
                  c.cmd->ClearRenderTargetView(rtv[1], infinite, 0, nullptr);
                  c.cmd->OMSetRenderTargets(2, rtv, FALSE, &dsv);
                  const uint32_t extra[1] = { kNone };
                  draw(c, 1, extra);
              });
    resolveDepthTies(fc, s, r, view, depth, vis, kListTBack, kListTNone + 1);
    ID3D12PipelineState* classPso = fc.shaders.compute("Passes/Visibility/TranslucentClass.MODE1");
    g.addPass("v.translucent.class", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(count, Use::SrvCompute);
                  b.use(vis, Use::SrvCompute);
                  b.use(linear, Use::SrvCompute);
                  b.use(cls, Use::UavCompute);
                  b.use(run.visible, Use::SrvCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.srv(count), c.srv(vis), c.srv(linear), c.uav(cls), c.srv(run.visible), width, height, 0 };
                  c.cmd->SetPipelineState(classPso);
                  c.bindFrameConstants(frameConstants);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
              });
    view.translucentVis = vis;
    view.translucentDepth = linear;
    view.translucentClass = cls;
}

// Water layer (v1.61): the layer-1 triangle streams drawn with one sample per pixel over a copy of band A's depth (depth test
// and write, both faces) into FrameResources::waterVis (vis id) and waterDepth (linear view depth, +inf = none).
void waterLayer(FramePassContext& fc, State& s, const Run& r, ViewResources& view)
{
    std::vector<uint32_t> slots;
    const std::vector<TriangleStream>& streams = fc.resources.triangleStreams;
    for (uint32_t slot = 0; slot < (uint32_t)streams.size(); ++slot)
        if (streams[slot].layer == 1 && streams[slot].vertices.valid() && streams[slot].drawArgs.valid() && streams[slot].maxTriangles > 0) slots.push_back(slot);
    // v1.73: W's view-grid ocean (main view): merged into the same layer (COV_OCEAN_ID).
    const TextureRef oceanDepth = view.viewId == 0 ? fc.resources.oceanDepth : TextureRef{};
    if (slots.empty() && !oceanDepth.valid()) return;
    RenderGraph& g = fc.graph;
    const uint32_t width = view.view.width, height = view.view.height;
    const TextureRef depthA = view.depth;
    const TextureRef depth = g.createTexture({ "v.water.depth.test", width, height, 1, 1, DXGI_FORMAT_D32_FLOAT });
    const TextureRef vis = g.createTexture({ "v.water.vis", width, height, 1, 1, DXGI_FORMAT_R32_UINT });
    const TextureRef linear = g.createTexture({ "v.water.depth", width, height, 1, 1, DXGI_FORMAT_R32_FLOAT });
    g.addPass("v.water.copy", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(depthA, Use::CopySrc);
                  b.use(depth, Use::CopyDst);
              },
              [=](PassContext& c) { c.cmd->CopyResource(c.resource(depth), c.resource(depthA)); });
    MeshPipelineDesc d;
    d.meshShader = "Passes/Visibility/WaterLayer.ms";
    d.pixelShader = "Passes/Visibility/WaterLayer.ps";
    d.renderTargets = { DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_FLOAT };
    d.depthFormat = DXGI_FORMAT_D32_FLOAT;
    d.depthFunc = D3D12_COMPARISON_FUNC_GREATER;  // reversed Z: in front of band A and of nearer water
    d.cull = D3D12_CULL_MODE_NONE;
    ID3D12PipelineState* pso = fc.shaders.mesh("v.water", d);
    ID3D12PipelineState* oceanPso = nullptr;
    if (oceanDepth.valid())
    {
        MeshPipelineDesc od = d;
        od.meshShader = "Passes/Visibility/PlanarFill.ms";  // one triangle over the view
        od.pixelShader = "Passes/Visibility/OceanLayer.ps";
        oceanPso = fc.shaders.mesh("v.water.ocean", od);
    }
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = view.frameConstants;
    const uint32_t viewsSrv = r.viewsSrv;
    std::vector<TriangleStream> drawn;
    for (uint32_t slot : slots) drawn.push_back(streams[slot]);
    g.addPass("v.water.raster", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(depth, Use::DepthWrite);
                  b.use(vis, Use::RenderTarget);
                  b.use(linear, Use::RenderTarget);
                  for (const TriangleStream& st : drawn)
                  {
                      b.use(st.vertices, Use::SrvGraphics);
                      b.use(st.drawArgs, Use::SrvGraphics);
                  }
                  if (oceanDepth.valid()) b.use(oceanDepth, Use::SrvGraphics);
              },
              [=](PassContext& c) {
                  const D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = { c.rtv(vis), c.rtv(linear) }, dsv = c.dsv(depth);
                  const float none[4] = { 0, 0, 0, 0 }, infinite[4] = { INFINITY, INFINITY, INFINITY, INFINITY };
                  c.cmd->ClearRenderTargetView(rtv[0], none, 0, nullptr);
                  c.cmd->ClearRenderTargetView(rtv[1], infinite, 0, nullptr);
                  c.cmd->OMSetRenderTargets(2, rtv, FALSE, &dsv);
                  const D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0, 1 };
                  const D3D12_RECT sc{ 0, 0, (LONG)width, (LONG)height };
                  c.cmd->RSSetViewports(1, &vp);
                  c.cmd->RSSetScissorRects(1, &sc);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->SetPipelineState(pso);
                  for (size_t k = 0; k < drawn.size(); ++k)
                  {
                      const TriangleStream& st = drawn[k];
                      const uint32_t groups = (st.maxTriangles + 31) / 32;
                      const uint32_t kc[8] = { c.srv(st.vertices), c.srv(st.drawArgs), st.maxTriangles, slots[k], viewsSrv, 0, 0, 0 };
                      c.graphicsConstants(kc, 8);
                      c.cmd->DispatchMesh(std::min(groups, 65535u), (groups + 65534) / 65535, 1);
                  }
                  if (oceanPso)
                  {
                      const uint32_t kc[4] = { c.srv(oceanDepth), 0, 0, 0 };
                      c.cmd->SetPipelineState(oceanPso);
                      c.graphicsConstants(kc, 4);
                      c.cmd->DispatchMesh(1, 1, 1);
                  }
              });
    if (oceanDepth.valid())
    {
        // Ocean edge pixels for W's subsample pass (OceanEdges.hlsl): capacity 1.5 x the latest need, at least the floor.
        ViewState& vs = s.views[view.viewId];
        const std::string statsName = view.viewId == 0 ? std::string("main") : "view" + std::to_string(view.viewId);
        readStats(fc, s, statsName);
        uint64_t want = r.cfg.oceanEdgesMin;
        const auto it = s.stats.find(statsName);
        if (it != s.stats.end() && it->second.latest.frameIndex != UINT64_MAX) want = std::max<uint64_t>(want, (it->second.latest.oceanEdges * 3ull + 1) / 2);
        constexpr uint64_t kStep = 1u << 14;
        want = std::min<uint64_t>((want + kStep - 1) / kStep * kStep, (uint64_t)width * height);
        if (want > vs.coverage.oceanEdgeCapacity || want * 2 < vs.coverage.oceanEdgeCapacity) vs.coverage.oceanEdgeCapacity = (uint32_t)want;
        const uint32_t cap = vs.coverage.oceanEdgeCapacity;
        const BufferRef list = g.createBuffer({ "v.water.oceanEdges", 16 + (uint64_t)cap * 4, 0 });
        const BufferRef state = r.state;
        for (uint32_t mode = 0; mode < 2; ++mode)
        {
            ID3D12PipelineState* epso = fc.shaders.compute("Passes/Visibility/OceanEdges.MODE" + std::to_string(mode));
            g.addPass(mode == 0 ? "v.water.oceanEdges" : "v.water.oceanEdges.args", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          if (mode == 0)
                          {
                              b.use(vis, Use::SrvCompute);
                              b.use(linear, Use::SrvCompute);
                              b.use(depthA, Use::SrvCompute);
                          }
                          b.use(list, Use::UavCompute);
                          b.use(state, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[8] = { mode == 0 ? c.srv(vis) : kNone, mode == 0 ? c.srv(linear) : kNone, mode == 0 ? c.srv(depthA) : kNone, c.uav(list),
                                                  c.uav(state), cap, width, height };
                          c.cmd->SetPipelineState(epso);
                          c.bindFrameConstants(frameConstants);
                          c.computeConstants(k, 8);
                          if (mode == 0) c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                          else c.cmd->Dispatch(1, 1, 1);
                      });
        }
        view.oceanEdgePixels = list;
    }
    view.waterVis = vis;
    view.waterDepth = linear;
    if (view.viewId == 0)
    {
        fc.resources.waterVis = vis;
        fc.resources.waterDepth = linear;
    }
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
    if (request.views.size() > kDepthRasterMaxViews)
        fail("rasterizeDepth '%s': %zu views (limit %u: DepthRaster.h kDepthRasterMaxViews)", request.name.c_str(), request.views.size(), kDepthRasterMaxViews);
    if (request.coverage || request.bands != 7)
        fail("rasterizeDepth '%s': coverage mode and band selection (v1.26) are not implemented yet (V)", request.name.c_str());
    if (request.tileLocal && (!request.cullMask.valid() || request.cullTilePx == 0))
        fail("rasterizeDepth '%s': tileLocal needs a tile mask (cullMask, cullTilePx)", request.name.c_str());
    // Tile-local runs: DepthRaster.as.hlsl launches one mesh group per (cluster, tile run) pair from a per-row prefix of
    // at most 256 rows (DepthRasterPayload.hlsli DR_MAX_ROWS), so every view's tile grid is at most 256 x 256 tiles: one
    // cluster launches at most 65536 mesh groups (D3D12: 2^22) - a structural bound, not a capacity.
    const bool amplify = request.tileLocal && cfg.rasterAmplification;
    if (amplify)
        for (const RasterView& v : request.views)
            if ((v.viewportWidth + request.cullTilePx - 1) / request.cullTilePx > 256 || (v.viewportHeight + request.cullTilePx - 1) / request.cullTilePx > 256)
                fail("rasterizeDepth '%s': a %ux%u viewport in %u px tiles exceeds the 256 x 256 tile grid of the tile-local raster", request.name.c_str(),
                     v.viewportWidth, v.viewportHeight, request.cullTilePx);
    const DXGI_FORMAT depthFormat = request.depthTarget.valid() ? fc.graph.desc(request.depthTarget).format : DXGI_FORMAT_UNKNOWN;
    if (request.depthTarget.valid() && depthFormat != DXGI_FORMAT_D32_FLOAT && depthFormat != DXGI_FORMAT_D16_UNORM)
        fail("rasterizeDepth '%s': depth target format %u (D32_FLOAT or D16_UNORM)", request.name.c_str(), (unsigned)depthFormat);
    if (request.colorTargets.size() > 4 || (!request.colorTargets.empty() && request.pixelKernel.empty()))
        fail("rasterizeDepth '%s': %zu render targets (at most 4, written by a pixel kernel)", request.name.c_str(), request.colorTargets.size());
    if (request.pixelNormals && request.pixelKernel.empty()) fail("rasterizeDepth '%s': pixelNormals without a pixel kernel", request.name.c_str());
    for (const DepthRasterRequest::PixelView& pv : request.pixelViews)
        if (pv.word >= 16 || pv.texture.valid() == pv.buffer.valid())
            fail("rasterizeDepth '%s': a pixel view names one resource and a word of pixelConstants (word %u)", request.name.c_str(), pv.word);
    const bool atlas = request.atlasSlots.valid();
    if (request.proxies && (!request.pixelKernel.empty() || !request.depthTarget.valid() || (request.tileLocal && !atlas)))
        fail("rasterizeDepth '%s': proxies are for depth-only requests over whole viewports or the tile atlas", request.name.c_str());
    if (request.proxies && !(request.proxyCoverage > 0 && request.proxyCoverage <= 1.27324f))
        fail("rasterizeDepth '%s': proxyCoverage %g (a share of the bounding disc; at most 4 / pi: its square)", request.name.c_str(), request.proxyCoverage);
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

    prepareCullScene(fc, s, s.mainFrameConstants);
    Run r = createRun(fc, cfg, request.name + ".", s.mainFrameConstants);
    r.tileMask = request.cullMask;
    // tile-local: each visible entry's tile rectangle (CullClusters); the amplification stage expands its pairs
    if (amplify) r.tilePairs = fc.graph.createBuffer({ "v.cull.tileRects", (uint64_t)cfg.capVisible * 8, 8 });
    else if (request.tileLocal) r.tilePairs = fc.graph.createBuffer({ "v.cull.tilePairs", (uint64_t)cfg.capVisible * 12, 12 });  // (A/B)
    r.storedPairs = request.tileLocal && !amplify;
    r.instanceMask = request.instanceMask;
    if (request.tileOccluders.valid() && request.atlasSlots.valid())
    {
        r.tileOccluders = request.tileOccluders;
        r.occluderSlots = request.atlasSlots;
    }
    std::vector<CullView> views;
    for (const RasterView& v : request.views) views.push_back(viewOf(v, request, cfg));
    // Tile occluders in two phases (DepthRaster.h): a second cull phase and raster after the requester's rebuild.
    bool twoPhase = false;
    for (const CullView& v : views) twoPhase = twoPhase || (v.flags & kViewTileTwoPhase) != 0;
    if (twoPhase) r.tileGuess = request.tileGuess;
    r.viewCount = (uint32_t)views.size();
    r.viewsSrv = uploadViews(s, fc, views);
    if (r.tileMask.valid()) tileCoarsePass(fc, r, views);
    cullPhase(fc, s, r, 1);

    // Back-face lists exist only when BACK was requested; shadow casters need every band (band mode A puts all visible
    // clusters in band A lists).
    const bool depthOut = request.depthTarget.valid();
    ID3D12PipelineState* pso[kBandLists];
    const bool out64 = fc.scene.maxClusterVertices() <= 64 && fc.scene.maxClusterTriangles() <= 64;  // OUT64
    for (uint32_t l = 0; l < kBandLists; ++l)
    {
        const bool back = request.cull == D3D12_CULL_MODE_BACK && (l == kListABack || l == kListAAlphaBack);
        MeshPipelineDesc d;
        // DEPTH1: no pixel kernel reads the attributes (hardware depth only), so the kernel exports none of them
        if (amplify) d.amplificationShader = std::string("Passes/Visibility/DepthRaster.as.TILE") + (atlas ? "2" : "1");
        d.meshShader = std::string("Passes/Visibility/DepthRaster.ms.TILE") + (atlas ? "2" : request.tileLocal ? "1" : "0") +
                       (request.pixelKernel.empty() ? ".DEPTH1" : request.pixelNormals ? ".DEPTH2" : ".DEPTH0") + (out64 ? ".OUT64" : ".OUT128") +
                       (amplify ? ".AS1" : ".AS0");
        d.pixelShader = request.pixelKernel;
        d.depthFormat = depthOut ? depthFormat : DXGI_FORMAT_UNKNOWN;
        d.depthWrite = depthOut && request.depthWrite;
        d.cull = back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
        d.conservative = request.conservative;
        d.multiplyBlend = request.colorMultiply && !request.colorTargets.empty();
        std::string targets;  // (the render targets' formats are part of the pipeline)
        for (const TextureRef& t : request.colorTargets)
        {
            d.renderTargets.push_back(fc.graph.desc(t).format);
            targets += "|rt" + std::to_string((unsigned)d.renderTargets.back());
        }
        pso[l] = fc.shaders.mesh("v.depth|" + request.pixelKernel + (back ? "|back" : "|none") +
                                     (depthOut ? (depthFormat == DXGI_FORMAT_D16_UNORM ? "|d16" : "|d32") : "|uav") + (request.conservative ? "|cons" : "") +
                                     (atlas ? "|atlas" : request.tileLocal ? "|tile" : "") + (amplify ? "|as" : "") + (out64 ? "|out64" : "") +
                                     (request.pixelNormals ? "|normals" : "") + (d.depthWrite || !depthOut ? "" : "|test") + (d.multiplyBlend ? "|multiply" : "") + targets,
                                 d);
    }
    // The proxies of the views' small chunk members (DepthProxy.ms.hlsl), drawn with the first phase's lists.
    ID3D12PipelineState* proxyPso = nullptr;
    if (request.proxies && r.chunkCount > 0)
    {
        MeshPipelineDesc d;
        d.meshShader = std::string("Passes/Visibility/DepthProxy.ms.TILE") + (atlas ? "2" : "0");
        d.depthFormat = depthFormat;
        d.depthWrite = request.depthWrite;
        d.cull = D3D12_CULL_MODE_NONE;
        proxyPso = fc.shaders.mesh(std::string("v.proxy") + (depthFormat == DXGI_FORMAT_D16_UNORM ? "|d16" : "|d32") + (atlas ? "|atlas" : "") + (request.depthWrite ? "" : "|test"), d);
    }
    uint32_t proxyCoverageBits;
    std::memcpy(&proxyCoverageBits, &request.proxyCoverage, 4);
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
    // visibility.cull_pass_merge: the run's statistics copy is the last raster pass's last command (the raster only reads
    // the cull state, so the state is final before it), not a pass of its own.
    const StatsCopy statsOfRun = cfg.passMerge ? statsCopy(fc, s, request.name) : StatsCopy{};
    // The raster of a phase's list entries (phase 1: all of a one-phase run).
    auto raster = [&](uint32_t phase, bool last) {
    const StatsCopy stats = last ? statsOfRun : StatsCopy{};
    fc.graph.addPass(request.name + (phase == 1 ? ".raster" : ".raster.p2"), QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(r.args, Use::IndirectArgs);
                         b.use(r.visible, Use::SrvGraphics);
                         b.use(r.lists, Use::SrvGraphics);
                         b.use(r.state, Use::SrvGraphics);
                         if (stats.readback)
                         {
                             b.use(r.state, Use::CopySrc);
                             b.keep();
                         }
                         if (r.tilePairs.valid()) b.use(r.tilePairs, Use::SrvGraphics);
                         if (amplify) b.use(req.cullMask, Use::SrvGraphics);  // (the amplification stage's pairs)
                         if (proxyPso && phase == 1)
                         {
                             b.use(r.chunkWork, Use::SrvGraphics);
                             if (r.chunks.valid()) b.use(r.chunks, Use::SrvGraphics);
                             if (req.cullMask.valid()) b.use(req.cullMask, Use::SrvGraphics);
                         }
                         if (req.atlasSlots.valid()) b.use(req.atlasSlots, Use::SrvGraphics);
                         if (req.depthTarget.valid()) b.use(req.depthTarget, req.depthWrite ? Use::DepthWrite : Use::DepthRead);
                         for (const TextureRef& t : req.colorTargets) b.use(t, Use::RenderTarget);
                         for (const auto& [t, u] : req.textureUses) b.use(t, u);
                         for (const auto& [bu, u] : req.bufferUses) b.use(bu, u);
                         // (a pixel view the uses do not name is read)
                         for (const DepthRasterRequest::PixelView& pv : req.pixelViews)
                         {
                             bool named = false;
                             for (const auto& [t, u] : req.textureUses) named = named || (pv.texture.valid() && t.id == pv.texture.id);
                             for (const auto& [bu, u] : req.bufferUses) named = named || (pv.buffer.valid() && bu.id == pv.buffer.id);
                             if (named) continue;
                             if (pv.texture.valid()) b.use(pv.texture, Use::SrvGraphics);
                             else b.use(pv.buffer, Use::SrvGraphics);
                         }
                     },
                     [=](PassContext& c) {
                         D3D12_CPU_DESCRIPTOR_HANDLE rtv[4] = {};
                         for (size_t i = 0; i < req.colorTargets.size(); ++i) rtv[i] = c.rtv(req.colorTargets[i]);
                         if (req.depthTarget.valid())
                         {
                             const D3D12_CPU_DESCRIPTOR_HANDLE dsv = req.depthWrite ? c.dsv(req.depthTarget) : c.dsvReadOnly(req.depthTarget);
                             c.cmd->OMSetRenderTargets((UINT)req.colorTargets.size(), rtv, FALSE, &dsv);
                         }
                         else
                             c.cmd->OMSetRenderTargets((UINT)req.colorTargets.size(), rtv, FALSE, nullptr);
                         // The pixel kernel's constants with this pass's view indices (DepthRasterRequest::pixelViews).
                         uint32_t pixelConstants[16];
                         std::memcpy(pixelConstants, req.pixelConstants, sizeof pixelConstants);
                         for (const DepthRasterRequest::PixelView& pv : req.pixelViews)
                         {
                             bool written = false;
                             for (const auto& [t, u] : req.textureUses) written = written || (pv.texture.valid() && t.id == pv.texture.id && u == Use::UavGraphics);
                             for (const auto& [bu, u] : req.bufferUses) written = written || (pv.buffer.valid() && bu.id == pv.buffer.id && u == Use::UavGraphics);
                             pixelConstants[pv.word] = pv.texture.valid() ? (written ? c.uav(pv.texture) : c.srv(pv.texture)) : (written ? c.uav(pv.buffer) : c.srv(pv.buffer));
                         }
                         c.cmd->RSSetViewports((UINT)viewports.size(), viewports.data());
                         c.cmd->RSSetScissorRects((UINT)scissors.size(), scissors.data());
                         c.bindFrameConstants(r.frameConstants);
                         for (uint32_t l = 0; l < kBandLists; ++l)
                         {
                             uint32_t k[32] = { c.srv(r.visible), c.srv(r.lists), c.srv(r.state), l, phase, r.cfg.capVisible, r.viewsSrv, sameViewport ? 0u : 1u,
                                                r.tilePairs.valid() ? c.srv(r.tilePairs) : kNone, req.atlasSlots.valid() ? c.srv(req.atlasSlots) : kNone,
                                                req.atlasTilesPerRow, atlasWidth | atlasHeight << 16, amplify ? c.srv(req.cullMask) : kNone };
                             std::memcpy(&k[16], pixelConstants, sizeof pixelConstants);
                             c.cmd->SetPipelineState(pso[l]);
                             c.graphicsConstants(k, 32);
                             c.cmd->ExecuteIndirect(sig, 1, c.resource(r.args), (kArgMesh + 3 * l) * 4, nullptr, 0);
                         }
                         if (proxyPso && phase == 1)
                         {
                             const uint32_t k[16] = { c.srv(r.chunkWork), r.instanceMask, c.srv(r.state), r.cfg.capDeferred,
                                                      proxyCoverageBits, 0, r.viewsSrv, sameViewport ? 0u : 1u,
                                                      kNone, req.atlasSlots.valid() ? c.srv(req.atlasSlots) : kNone, req.atlasTilesPerRow, atlasWidth | atlasHeight << 16,
                                                      req.cullMask.valid() ? c.srv(req.cullMask) : kNone, 0, 0, 0 };
                             c.cmd->SetPipelineState(proxyPso);
                             c.graphicsConstants(k, 16);
                             c.cmd->ExecuteIndirect(sig, 1, c.resource(r.args), kArgProxies * 4, nullptr, 0);
                         }
                         if (stats.readback) c.cmd->CopyBufferRegion(stats.readback, stats.offset, c.resource(r.state), 0, kStateWords * 4);
                     });
    };
    raster(1, !twoPhase);
    if (twoPhase)
    {
        request.buildTileOccluders();  // (the requester's pass: the occluders of what phase 1 drew)
        cullPhase(fc, s, r, 2);
        raster(2, true);
    }
    if (!statsOfRun.readback) recordStats(fc, s, r, request.name);
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

std::vector<std::pair<std::string, Stats>> latestStatsOfRuns(render::TrackState& trackState)
{
    std::vector<std::pair<std::string, Stats>> out;
    for (const auto& [name, run] : trackState.get<render::tracks::State>("v.state").stats) out.push_back({ name, run.latest });
    return out;
}
} // namespace unx::visibility
