#include "VsmSystem.h"

#include "FroxelSystem.h"
#include "SResources.h"

#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace unx::render::shadow
{
using namespace s_detail;

namespace
{
const char* const kStateKey = "s.vsm";
constexpr uint32_t kRingSlots = 16, kRingStride = 1024;  // per-frame constants; frames in flight must be < kRingSlots
constexpr uint32_t kStatsSlots = 4;
constexpr uint32_t kMetaBytes = 32;  // VsmPageMeta
constexpr uint32_t kBlockBytes = 341 * 32;  // VSM_BLOCK_ENTRIES x VsmBlock

struct State
{
    bool initialized = false;
    bool needsInit = false;  // pool (re)created: the next frame resets tables and free list
    uint32_t poolPagesX = 0, poolPagesY = 0;
    ComPtr<ID3D12Resource> pool, table, requests, meta, blocks, freeList, dirtyList, cullMask, args, stats, lastRevision, movedList, ring;
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    uint32_t poolUav = UINT32_MAX, tableSrv = UINT32_MAX, ringSrv = UINT32_MAX, metaUav = UINT32_MAX;
    uint32_t ringCbv[kRingSlots] = {};  // constant buffer view of each ring slot (ConstantBuffer<VsmConstants>)
    bool ringCbvs = false;
    uint32_t instanceCapacity = 0;
    uint8_t* ringMapped = nullptr;
    // Stats readback ring: slot i holds the counters of frame statsFrame[i], complete once the graphics queue passes
    // statsFence[i] (the fence of the frame after it was recorded is known at the next record).
    ComPtr<ID3D12Resource> statsReadback;
    uint64_t statsFrame[kStatsSlots] = {};
    uint64_t statsFence[kStatsSlots] = {};
    int lastStatsSlot = -1;
    VsmStats latest;
    // CPU view of the clipmap.
    float3 sun{};
    float3 windDirection{};
    float windSpeed = -1;
    uint32_t sceneRevision = UINT32_MAX;
    float hMin = 0, hMax = 0;
    uint64_t frames = 0;
    VsmConstantsCpu constants{};
    uint32_t constantsOffset = 0;
    // This frame's graph handles (valid between shadowPages and the end of the frame's recording).
    uint64_t recordedFrame = UINT64_MAX;
    BufferRef poolRef;
    BufferRef tableRef, metaRef, boundRef, blocksRef, statsRef;
    bool pagesRecorded = false;
    bool debugPaths = false;
};

void createViews(Device& device, State& s)
{
    DescriptorHeaps& h = device.descriptors();
    if (s.poolUav == UINT32_MAX) s.poolUav = h.allocateResource();
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = s.poolPagesX * s.poolPagesY * kPage * kPage;
    ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device.d3d()->CreateUnorderedAccessView(s.pool.Get(), nullptr, &ud, h.resourceCpu(s.poolUav));
    auto raw = [&](ID3D12Resource* r, uint64_t bytes, uint32_t& index) {
        if (index == UINT32_MAX) index = h.allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.Buffer.NumElements = (UINT)(bytes / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        device.d3d()->CreateShaderResourceView(r, &sd, h.resourceCpu(index));
    };
    raw(s.table.Get(), (uint64_t)kSlots * 8, s.tableSrv);
    if (s.metaUav == UINT32_MAX) s.metaUav = h.allocateResource();
    D3D12_UNORDERED_ACCESS_VIEW_DESC md{};
    md.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    md.Format = DXGI_FORMAT_UNKNOWN;
    md.Buffer.NumElements = s.poolPagesX * s.poolPagesY;
    md.Buffer.StructureByteStride = kMetaBytes;
    device.d3d()->CreateUnorderedAccessView(s.meta.Get(), nullptr, &md, h.resourceCpu(s.metaUav));
    raw(s.ring.Get(), (uint64_t)kRingSlots * kRingStride, s.ringSrv);
    for (uint32_t i = 0; i < kRingSlots; ++i)
    {
        if (!s.ringCbvs) s.ringCbv[i] = h.allocateResource();
        D3D12_CONSTANT_BUFFER_VIEW_DESC cd{ s.ring->GetGPUVirtualAddress() + (uint64_t)i * kRingStride, kRingStride };
        device.d3d()->CreateConstantBufferView(&cd, h.resourceCpu(s.ringCbv[i]));
    }
    s.ringCbvs = true;
}

void createState(FramePassContext& fc, State& s)
{
    const QualityConfig& q = fc.quality;
    if (q.integer("shadow.vsm.virtual_resolution") != kVirtual || q.integer("shadow.vsm.page_texels") != kPage || q.integer("shadow.vsm.clipmap_levels") != kLevels)
        fail("shadow.vsm: virtual_resolution / page_texels / clipmap_levels are compiled into the S kernels (16384 / 128 / 12)");
    const uint32_t pages = (uint32_t)q.integer("shadow.vsm.pool_pages");
    const uint32_t perRow = 64;  // 8192 texels
    if (pages == 0 || pages % perRow) fail("shadow.vsm.pool_pages must be a positive multiple of %u", perRow);
    Device& d = fc.device;
    s.poolPagesX = perRow;
    s.poolPagesY = pages / perRow;
    s.pool = createBuffer(d, L"S VSM pool", (uint64_t)pages * kPage * kPage * 4);
    s.table = createBuffer(d, L"S VSM page table", (uint64_t)kSlots * 8);
    s.requests = createBuffer(d, L"S VSM requests", (uint64_t)kSlots * 4);
    s.meta = createBuffer(d, L"S VSM page metadata", (uint64_t)pages * kMetaBytes);
    s.blocks = createBuffer(d, L"S VSM page blocks", (uint64_t)pages * kBlockBytes);
    s.freeList = createBuffer(d, L"S VSM free list", 4 + (uint64_t)pages * 4);
    s.dirtyList = createBuffer(d, L"S VSM dirty list", 8 + (uint64_t)pages * 8);
    s.cullMask = createBuffer(d, L"S VSM cull mask", (uint64_t)kSlots / 8);
    s.args = createBuffer(d, L"S VSM indirect args", 32);  // dirty pages at 0, moved instances x levels at 16
    s.stats = createBuffer(d, L"S VSM stats", 80);
    s.ring = createBuffer(d, L"S VSM constants ring", (uint64_t)kRingSlots * kRingStride, D3D12_HEAP_TYPE_UPLOAD);
    s.statsReadback = createBuffer(d, L"S VSM stats readback", (uint64_t)kStatsSlots * 80, D3D12_HEAP_TYPE_READBACK);
    D3D12_RANGE none{ 0, 0 };
    check(s.ring->Map(0, &none, reinterpret_cast<void**>(&s.ringMapped)), "map VSM ring");
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sig{};
    sig.ByteStride = 12;
    sig.NumArgumentDescs = 1;
    sig.pArgumentDescs = &arg;
    check(d.d3d()->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&s.dispatchSignature)), "VSM dispatch signature");
    createViews(d, s);
    s.initialized = false;
    s.needsInit = true;
}

// Light basis: z towards the sun, x horizontal, y completing a right-handed frame.
void lightBasis(float3 z, float3& x, float3& y)
{
    const float3 up = std::abs(z.y) < 0.999f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    x = normalize(cross(up, z));
    y = cross(z, x);
}

// Light-space height range of every shadow caster (bounding spheres), with a margin for dynamic motion.
void casterHeightRange(const GpuScene& scene, float3 z, float& lo, float& hi)
{
    lo = 1e30f;
    hi = -1e30f;
    const auto& inst = scene.instances();
    const auto& meshes = scene.meshes();
    for (const gpu::Instance& i : inst)
    {
        if ((i.flags & scene::InstanceCastShadow) == 0) continue;
        const gpu::Mesh& m = meshes[i.mesh];
        const float3 c{ m.boundsSphere.x, m.boundsSphere.y, m.boundsSphere.z };
        const float3 w{ i.objectToWorld[0].x * c.x + i.objectToWorld[0].y * c.y + i.objectToWorld[0].z * c.z + i.objectToWorld[0].w,
                        i.objectToWorld[1].x * c.x + i.objectToWorld[1].y * c.y + i.objectToWorld[1].z * c.z + i.objectToWorld[1].w,
                        i.objectToWorld[2].x * c.x + i.objectToWorld[2].y * c.y + i.objectToWorld[2].z * c.z + i.objectToWorld[2].w };
        const float scale = length(float3{ i.objectToWorld[0].x, i.objectToWorld[0].y, i.objectToWorld[0].z });
        const float h = dot(w, z), r = m.boundsSphere.w * scale;
        lo = std::min(lo, h - r);
        hi = std::max(hi, h + r);
    }
    if (lo > hi) lo = hi = 0;
}

float4x4 levelViewProj(const VsmConstantsCpu& c, uint32_t k)
{
    const float t = c.level[k].texel, V = (float)kVirtual, range = c.hMax - c.hMin;
    const float ox = (float)c.level[k].origin[0] * kPage, oy = (float)c.level[k].origin[1] * kPage;
    float4x4 m;
    m.m[0][0] = 2 * c.lightX.x / (t * V); m.m[0][1] = 2 * c.lightX.y / (t * V); m.m[0][2] = 2 * c.lightX.z / (t * V); m.m[0][3] = -2 * ox / V - 1;
    m.m[1][0] = -2 * c.lightY.x / (t * V); m.m[1][1] = -2 * c.lightY.y / (t * V); m.m[1][2] = -2 * c.lightY.z / (t * V); m.m[1][3] = 1 + 2 * oy / V;
    m.m[2][0] = -c.lightZ.x / range; m.m[2][1] = -c.lightZ.y / range; m.m[2][2] = -c.lightZ.z / range; m.m[2][3] = c.hMax / range;
    m.m[3][0] = 0; m.m[3][1] = 0; m.m[3][2] = 0; m.m[3][3] = 1;
    return m;
}

uint32_t groups(uint64_t n, uint32_t size) { return (uint32_t)((n + size - 1) / size); }
} // namespace

const VsmStats& stats(TrackState& state)
{
    State& s = state.get<State>(kStateKey);
    return s.latest;
}

void setDebugPaths(TrackState& state, bool enabled) { state.get<State>(kStateKey).debugPaths = enabled; }

const VsmConstantsCpu& lastConstants(TrackState& state) { return state.get<State>(kStateKey).constants; }

bool frameRefs(FramePassContext& fc, VsmFrameRefs& out)
{
    State& s = fc.state<State>(kStateKey);
    if (!s.pagesRecorded || s.recordedFrame != fc.frame.frameIndex) return false;
    out.pool = s.poolRef;
    out.table = s.tableRef;
    out.blocks = s.blocksRef;
    out.bound = s.boundRef;
    out.constantsCbv = s.ringCbv[s.constantsOffset / kRingStride];
    return true;
}

void recordPages(FramePassContext& fc, const ViewResources& main)
{
    State& s = fc.state<State>(kStateKey);
    const QualityConfig& q = fc.quality;
    const uint32_t pages = (uint32_t)q.integer("shadow.vsm.pool_pages");
    if (!s.pool || s.poolPagesX * s.poolPagesY != pages) createState(fc, s);

    // Harvest completed stats (no stall): the newest slot whose frame the GPU has finished.
    const uint64_t completed = fc.device.queue(QueueType::Graphics).completed();
    if (s.lastStatsSlot >= 0) s.statsFence[s.lastStatsSlot] = fc.graph.lastFence(QueueType::Graphics);
    for (uint32_t i = 0; i < kStatsSlots; ++i)
    {
        if (s.statsFence[i] == 0 || s.statsFence[i] > completed || s.statsFrame[i] <= s.latest.frame) continue;
        uint32_t* p = nullptr;
        D3D12_RANGE r{ i * 80, i * 80 + 80 };
        check(s.statsReadback->Map(0, &r, reinterpret_cast<void**>(&p)), "map VSM stats");
        const uint32_t* w = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + i * 80);
        s.latest = { s.statsFrame[i], w[0], w[1], w[2], w[3], w[4], w[5], w[8], w[9], w[10], w[11], w[12], w[13], w[14] };
        D3D12_RANGE none{ 0, 0 };
        s.statsReadback->Unmap(0, &none);
    }

    // Light basis, caster range, scene-wide invalidation.
    const scene::Scene* src = fc.scene.source();
    const float3 sunDir = normalize(src ? src->sun.direction : scene::Sun{}.direction);
    bool invalidate = !s.initialized;
    if (std::memcmp(&sunDir, &s.sun, sizeof sunDir) != 0 || fc.scene.revision() != s.sceneRevision)
    {
        invalidate = true;
        s.sun = sunDir;
        s.sceneRevision = fc.scene.revision();
        float lo, hi;
        casterHeightRange(fc.scene, sunDir, lo, hi);
        const float margin = (float)q.number("shadow.vsm.height_margin_m");
        s.hMin = lo - margin;
        s.hMax = hi + margin;
    }
    const float3 windDir = src ? src->windDirection : float3{};
    const float windSpeed = src ? src->windSpeed : 0.0f;
    const bool windChanged = std::memcmp(&windDir, &s.windDirection, sizeof windDir) != 0 || windSpeed != s.windSpeed;
    s.windDirection = windDir;
    s.windSpeed = windSpeed;
    VsmConstantsCpu& c = s.constants;
    c = {};
    c.windChanged = windChanged ? 1u : 0u;
    c.lightZ = sunDir;
    lightBasis(sunDir, c.lightX, c.lightY);
    c.hMin = s.hMin;
    c.hMax = s.hMax;
    c.tanSunRadius = std::tan(src ? src->sun.angularRadius : scene::Sun{}.angularRadius);
    c.poolPagesX = s.poolPagesX;
    c.poolPagesY = s.poolPagesY;
    c.frame = (uint32_t)++s.frames;
    c.sceneInvalidate = invalidate ? 1u : 0u;
    c.time = (float)fc.frame.time;
    c.lodBias = (float)q.number("shadow.vsm.lod_bias");
    c.receiverBiasTexels = (float)q.number("shadow.vsm.receiver_bias_texels");
    c.maxReceiverSlope = (float)q.number("shadow.vsm.max_receiver_slope");
    c.cacheFrames = (uint32_t)q.integer("shadow.vsm.cache_frames");
    c.instanceCount = (uint32_t)fc.scene.instances().size();
    c.windTexels = (uint32_t)q.integer("shadow.vsm.wind_texels");
    const float3 cam = main.view.position;
    const float cu = dot(cam, c.lightX), cv = dot(cam, c.lightY);
    c.cameraUV[0] = cu;
    c.cameraUV[1] = cv;
    for (uint32_t k = 0; k < kLevels; ++k)
    {
        const float texel = std::ldexp(1.0f, (int)k - 10);
        const float pageSize = texel * kPage;
        c.level[k].texel = texel;
        c.level[k].origin[0] = (int32_t)std::floor(cu / pageSize) - (int32_t)kTable / 2;
        c.level[k].origin[1] = (int32_t)std::floor(cv / pageSize) - (int32_t)kTable / 2;
    }
    s.constantsOffset = (uint32_t)(fc.frame.frameIndex % kRingSlots) * kRingStride;
    std::memcpy(s.ringMapped + s.constantsOffset, &c, sizeof c);
    s.initialized = true;

    // Instances' last seen revisions (dirty rule a).
    if (c.instanceCount > s.instanceCapacity)
    {
        if (s.lastRevision) fc.device.deferRelease(s.lastRevision);
        s.instanceCapacity = std::max(c.instanceCount, 1u);
        s.lastRevision = createBuffer(fc.device, L"S VSM instance revisions", (uint64_t)s.instanceCapacity * 8);
        if (s.movedList) fc.device.deferRelease(s.movedList);
        s.movedList = createBuffer(fc.device, L"S VSM moved instances", 4 + (uint64_t)s.instanceCapacity * 4);
    }

    RenderGraph& g = fc.graph;
    const BufferRef pool = g.importBuffer(s.pool.Get(), BufferDesc{ "S VSM pool", (uint64_t)pages * kPage * kPage * 4, 0 });
    const BufferRef table = g.importBuffer(s.table.Get(), BufferDesc{ "S VSM page table", (uint64_t)kSlots * 8, 0 });
    const BufferRef requests = g.importBuffer(s.requests.Get(), BufferDesc{ "S VSM requests", (uint64_t)kSlots * 4, 0 });
    const BufferRef meta = g.importBuffer(s.meta.Get(), BufferDesc{ "S VSM page metadata", (uint64_t)pages * kMetaBytes, kMetaBytes });
    const BufferRef blocks = g.importBuffer(s.blocks.Get(), BufferDesc{ "S VSM page blocks", (uint64_t)pages * kBlockBytes, 0 });
    s.blocksRef = blocks;
    const BufferRef freeList = g.importBuffer(s.freeList.Get(), BufferDesc{ "S VSM free list", 4 + (uint64_t)pages * 4, 0 });
    const BufferRef dirty = g.importBuffer(s.dirtyList.Get(), BufferDesc{ "S VSM dirty list", 8 + (uint64_t)pages * 8, 0 });
    const BufferRef mask = g.importBuffer(s.cullMask.Get(), BufferDesc{ "S VSM cull mask", (uint64_t)kSlots / 8, 0 });
    const BufferRef args = g.importBuffer(s.args.Get(), BufferDesc{ "S VSM indirect args", 32, 0 });
    const BufferRef moved = g.importBuffer(s.movedList.Get(), BufferDesc{ "S VSM moved instances", 4 + (uint64_t)s.instanceCapacity * 4, 0 });
    const BufferRef statsBuf = g.importBuffer(s.stats.Get(), BufferDesc{ "S VSM stats", 80, 0 });
    s.statsRef = statsBuf;
    const BufferRef revisions = g.importBuffer(s.lastRevision.Get(), BufferDesc{ "S VSM instance revisions", (uint64_t)s.instanceCapacity * 8, 8 });
    s.recordedFrame = fc.frame.frameIndex;
    s.poolRef = pool;
    s.tableRef = table;
    s.metaRef = meta;
    s.pagesRecorded = true;
    // FrameResources::vsmPool is a TextureRef; the pool is a raw buffer (no layout transitions): left unset, readers use
    // S's HLSL API (S_STATUS_KO.md).
    fc.resources.vsmPageTable = table;

    ShaderLibrary& sh = fc.shaders;
    const uint32_t ring = s.ringCbv[s.constantsOffset / kRingStride], off = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS mainConstants = main.frameConstants;

    if (s.needsInit)
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmInit");
        g.addPass("s.vsm.init", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::UavCompute);
                      b.use(requests, Use::UavCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(freeList, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(table), ctx.uav(requests), ctx.uav(meta), ctx.uav(freeList), kSlots, pages, 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(std::max(kSlots, pages), 256), 1, 1);
                  });
        s.needsInit = false;
    }
    if (c.frame > 1)
    {
        // Counters of the previous frame (page updates and every visibility pass) to the readback ring, before VsmBegin
        // clears them; read at a later record once the GPU passed this frame.
        const uint32_t slot = (uint32_t)(c.frame % kStatsSlots);
        ID3D12Resource* rb = s.statsReadback.Get();
        g.addPass("s.vsm.stats", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(statsBuf, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(rb, slot * 80ull, ctx.resource(statsBuf), 0, 80); });
        s.statsFrame[slot] = c.frame - 1;
        s.statsFence[slot] = 0;
        s.lastStatsSlot = (int)slot;
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmBegin");
        g.addPass("s.vsm.begin", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(dirty, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.use(moved, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(dirty), ctx.uav(statsBuf), ctx.uav(moved), 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
    }
    if (main.depth.valid())
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmMark");
        const TextureRef depth = main.depth;
        const uint32_t w = main.view.width, h = main.view.height;
        g.addPass("s.vsm.mark", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(depth, Use::SrvCompute);
                      b.use(requests, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(depth), ctx.uav(requests), ring, off };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(groups(w, 8), groups(h, 8), 1);
                  });
    }
    {
        // Air of the froxel integration (VsmMarkAir, VsmAir.hlsli): after the pixel marks, which store plainly.
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmMarkAir");
        const FroxelGridCpu grid = froxelGridFor(q, main.view.width, main.view.height);
        uint32_t nearBits, farBits, texelBits;
        std::memcpy(&nearBits, &grid.nearM, 4);
        std::memcpy(&farBits, &grid.farM, 4);
        std::memcpy(&texelBits, &grid.shadowTexelsPerTile, 4);
        g.addPass("s.vsm.markair", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(requests), ring, grid.gridX | grid.gridY << 16, grid.slices | grid.tilePx << 16, nearBits, farBits, texelBits, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
                  });
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmPropagate");
        g.addPass("s.vsm.propagate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(requests), ring, off, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(groups(kSlots, 256), 1, 1);
                  });
    }
    ID3D12CommandSignature* signature = s.dispatchSignature.Get();
    if (c.instanceCount > 0)
    {
        // Dirty rule (a): instances whose revisions changed -> list -> one group per (instance, level).
        ID3D12PipelineState* pm = sh.compute("Passes/Shadow/VsmMoved");
        ID3D12PipelineState* pa = sh.compute("Passes/Shadow/VsmMovedArgs");
        ID3D12PipelineState* pi = sh.compute("Passes/Shadow/VsmInvalidate");
        const uint32_t n = c.instanceCount;
        g.addPass("s.vsm.moved", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(revisions, Use::UavCompute);
                      b.use(moved, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(revisions), ctx.uav(moved), n, 0 };
                      ctx.cmd->SetPipelineState(pm);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(groups(n, 64), 1, 1);
                  });
        g.addPass("s.vsm.movedargs", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(moved, Use::SrvCompute);
                      b.use(args, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(moved), ctx.uav(args), 0, 0 };
                      ctx.cmd->SetPipelineState(pa);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("s.vsm.invalidate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::UavCompute);
                      b.use(moved, Use::SrvCompute);
                      b.use(args, Use::IndirectArgs);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(table), ctx.srv(moved), ring, off };
                      ctx.cmd->SetPipelineState(pi);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 16, nullptr, 0);
                  });
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmRelease");
        g.addPass("s.vsm.release", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::UavCompute);
                      b.use(requests, Use::SrvCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(freeList, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(table), ctx.srv(requests), ctx.uav(meta), ctx.uav(freeList), ring, off, 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(kSlots, 256), 1, 1);
                  });
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmAllocate");
        g.addPass("s.vsm.allocate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::UavCompute);
                      b.use(requests, Use::UavCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(freeList, Use::UavCompute);
                      b.use(dirty, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(table), ctx.uav(requests), ctx.uav(meta), ctx.uav(freeList), ctx.uav(dirty), ctx.uav(statsBuf), ring, off };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(kSlots, 256), 1, 1);
                  });
    }
    {
        ID3D12PipelineState* pm = sh.compute("Passes/Shadow/VsmCullMask");
        ID3D12PipelineState* pf = sh.compute("Passes/Shadow/VsmFinalize");
        g.addPass("s.vsm.cullmask", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::SrvCompute);
                      b.use(mask, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(table), ctx.uav(mask), ring, off };
                      ctx.cmd->SetPipelineState(pm);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(groups(kSlots / 32, 64), 1, 1);
                  });
        g.addPass("s.vsm.finalize", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(dirty, Use::SrvCompute);
                      b.use(args, Use::UavCompute);
                      b.use(freeList, Use::SrvCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(dirty), ctx.uav(args), ctx.srv(freeList), ctx.uav(statsBuf) };
                      ctx.cmd->SetPipelineState(pf);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmClear");
        g.addPass("s.vsm.clear", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(dirty, Use::SrvCompute);
                      b.use(args, Use::IndirectArgs);
                      b.use(pool, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(dirty), ctx.uav(pool), ring, off };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
                  });
    }

    // Dirty pages through V's cluster pipeline: one orthographic view per level over its whole window; the tile mask
    // limits the raster to dirty pages, the pixel kernel writes only into dirty pages.
    if (fc.services.rasterizeDepth)
    {
        DepthRasterRequest r;
        r.name = "s.vsm.raster";
        r.instanceMask = scene::InstanceCastShadow;
        r.pixelKernel = "Passes/Shadow/VsmPagePixel";
        r.bufferUses.push_back({ pool, Use::UavGraphics });
        r.bufferUses.push_back({ table, Use::SrvGraphics });
        r.bufferUses.push_back({ meta, Use::UavGraphics });
        r.pixelConstants[0] = s.poolUav;
        r.pixelConstants[1] = s.tableSrv;
        std::memcpy(&r.pixelConstants[2], &c.hMin, 4);
        std::memcpy(&r.pixelConstants[3], &c.hMax, 4);
        r.pixelConstants[4] = s.poolPagesX;
        r.pixelConstants[5] = s.metaUav;
        r.cullMask = mask;
        r.cullTilePx = kPage;
        r.cull = D3D12_CULL_MODE_NONE;
        for (uint32_t k = 0; k < kLevels; ++k)
        {
            RasterView v;
            v.viewProj = levelViewProj(c, k);
            v.viewportX = v.viewportY = 0;
            v.viewportWidth = v.viewportHeight = kVirtual;
            v.lodPixelsPerMetre = 1.0f / c.level[k].texel;
            v.userData = k | ((uint32_t)(c.level[k].origin[0] & 127) << 4) | ((uint32_t)(c.level[k].origin[1] & 127) << 11);
            v.cullMaskOffset = k * (kTable * kTable / 32);
            r.views.push_back(v);
        }
        fc.services.rasterizeDepth(fc, r);
    }

    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmPageMax");
        g.addPass("s.vsm.pagemax", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(dirty, Use::SrvCompute);
                      b.use(args, Use::IndirectArgs);
                      b.use(pool, Use::SrvCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(blocks, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.srv(dirty), ctx.srv(pool), ctx.uav(meta), ring, off, ctx.uav(blocks), 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
                  });
    }
    {
        // Search bound grid for the visibility pass (VsmSearchGrid): fill, then 3 x 3 dilation.
        ID3D12PipelineState* pf = sh.compute("Passes/Shadow/VsmSearchGrid.MODE0");
        ID3D12PipelineState* pd = sh.compute("Passes/Shadow/VsmSearchGrid.MODE1");
        const BufferRef fill = g.createBuffer(BufferDesc{ "S VSM search fill", (uint64_t)kSlots * 4, 0 });
        const BufferRef bound = g.createBuffer(BufferDesc{ "S VSM search bound", (uint64_t)kSlots * 4, 0 });
        s.boundRef = bound;
        g.addPass("s.vsm.searchgrid", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::SrvCompute);
                      b.use(meta, Use::SrvCompute);
                      b.use(fill, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.srv(table), ctx.srv(meta), ctx.uav(fill), 0, ring, off, 0, 0 };
                      ctx.cmd->SetPipelineState(pf);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(kSlots, 256), 1, 1);
                  });
        g.addPass("s.vsm.searchdilate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(fill, Use::SrvCompute);
                      b.use(bound, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { 0, 0, ctx.srv(fill), ctx.uav(bound), ring, off, 0, 0 };
                      ctx.cmd->SetPipelineState(pd);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(kSlots, 256), 1, 1);
                  });
    }
}

void recordVisibility(FramePassContext& fc, ViewResources& view)
{
    State& s = fc.state<State>(kStateKey);
    if (!s.pagesRecorded || s.recordedFrame != fc.frame.frameIndex) fail("S.shadowVisibility: shadowPages was not recorded this frame");
    if (!view.depth.valid() || !view.gbuffer.valid())
    {
        tracks::pending("S.shadowVisibility (view without depth or G-buffer)");
        return;
    }
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height;
    const TextureRef out = g.createTexture(TextureDesc{ "S shadow visibility", w, h, 1, 1, DXGI_FORMAT_R32_UINT });
    view.shadowVisibility = out;
    const TextureRef depth = view.depth, gbuffer = view.gbuffer;
    const BufferRef pool = s.poolRef;
    const BufferRef table = s.tableRef, bound = s.boundRef, blocks = s.blocksRef, statsBuf = s.statsRef;
    const uint32_t ring = s.ringCbv[s.constantsOffset / kRingStride], off = 0;
    const uint32_t rays = (uint32_t)fc.quality.integer("shadow.vsm.search_taps"), steps = (uint32_t)fc.quality.integer("shadow.vsm.filter_taps");
    const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
    const char* variant = s.debugPaths ? ".PATHS1" : ".PATHS0";
    ID3D12PipelineState* pc = fc.shaders.compute("Passes/Shadow/ShadowListClear");
    ID3D12PipelineState* p1 = fc.shaders.compute(std::string("Passes/Shadow/ShadowVisibility") + variant);
    ID3D12PipelineState* pa = fc.shaders.compute("Passes/Shadow/ShadowListArgs");
    ID3D12PipelineState* p2 = fc.shaders.compute(std::string("Passes/Shadow/ShadowPenumbra") + variant);
    ID3D12CommandSignature* signature = s.dispatchSignature.Get();
    // Pass 1 settles the pixels the page structures decide; the mixed ones go to a list for pass 2 (indirect).
    const BufferRef list = g.createBuffer(BufferDesc{ "S penumbra list", 4 + (uint64_t)w * h * 4, 0 });
    const BufferRef args = g.createBuffer(BufferDesc{ "S penumbra args", 16, 0 });
    g.addPass("s.shadow.listclear", QueueType::Compute, [&](PassBuilder& b) { b.use(list, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(list), 0, 0, 0 };
                  ctx.cmd->SetPipelineState(pc);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(1, 1, 1);
              });
    g.addPass("s.shadow.visibility", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(pool, Use::SrvCompute);
                  b.use(table, Use::SrvCompute);
                  b.use(bound, Use::SrvCompute);
                  b.use(blocks, Use::SrvCompute);
                  b.use(statsBuf, Use::UavCompute);
                  b.use(list, Use::UavCompute);
                  b.use(out, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[12] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, off, ctx.srv(table), ctx.srv(pool), ctx.srv(bound),
                                           ctx.uav(list), ctx.srv(blocks), ctx.uav(statsBuf), 0 };
                  ctx.cmd->SetPipelineState(p1);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 12);
                  ctx.cmd->Dispatch(groups(w, 8), groups(h, 8), 1);
              });
    g.addPass("s.shadow.listargs", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(list, Use::SrvCompute);
                  b.use(args, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.srv(list), ctx.uav(args), 0, 0 };
                  ctx.cmd->SetPipelineState(pa);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(1, 1, 1);
              });
    g.addPass("s.shadow.penumbra", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(pool, Use::SrvCompute);
                  b.use(table, Use::SrvCompute);
                  b.use(bound, Use::SrvCompute);
                  b.use(blocks, Use::SrvCompute);
                  b.use(list, Use::SrvCompute);
                  b.use(args, Use::IndirectArgs);
                  b.use(statsBuf, Use::UavCompute);
                  b.use(out, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[16] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, off, ctx.srv(table), ctx.srv(pool), ctx.srv(bound),
                                           ctx.srv(list), ctx.srv(blocks), ctx.uav(statsBuf), 0, rays, steps, 0, 0 };
                  ctx.cmd->SetPipelineState(p2);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 16);
                  ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
              });
}
} // namespace unx::render::shadow
