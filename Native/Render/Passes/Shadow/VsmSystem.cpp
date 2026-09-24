#include "VsmSystem.h"

#include "SResources.h"

#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace unx::render::shadow
{
using namespace s_detail;

namespace
{
const char* const kStateKey = "s.vsm";
constexpr uint32_t kRingSlots = 16, kRingStride = 1024;  // per-frame constants; frames in flight must be < kRingSlots
constexpr uint32_t kStatsSlots = 4;

struct State
{
    bool initialized = false;
    bool needsInit = false;  // pool (re)created: the next frame resets tables and free list
    uint32_t poolPagesX = 0, poolPagesY = 0;
    ComPtr<ID3D12Resource> pool, table, requests, meta, freeList, dirtyList, cullMask, args, stats, lastRevision, ring;
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    uint32_t poolUav = UINT32_MAX, tableSrv = UINT32_MAX, ringSrv = UINT32_MAX;
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
    uint32_t sceneRevision = UINT32_MAX;
    float hMin = 0, hMax = 0;
    uint64_t frames = 0;
    VsmConstantsCpu constants{};
    uint32_t constantsOffset = 0;
    // This frame's graph handles (valid between shadowPages and the end of the frame's recording).
    uint64_t recordedFrame = UINT64_MAX;
    TextureRef poolRef;
    BufferRef tableRef, metaRef;
    bool pagesRecorded = false;
};

void createViews(Device& device, State& s)
{
    DescriptorHeaps& h = device.descriptors();
    if (s.poolUav == UINT32_MAX) s.poolUav = h.allocateResource();
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_UINT;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
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
    raw(s.ring.Get(), (uint64_t)kRingSlots * kRingStride, s.ringSrv);
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
    s.pool = createTexture(d, L"S VSM pool", D3D12_RESOURCE_DIMENSION_TEXTURE2D, s.poolPagesX * kPage, s.poolPagesY * kPage, 1, DXGI_FORMAT_R32_UINT,
                           D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    s.table = createBuffer(d, L"S VSM page table", (uint64_t)kSlots * 8);
    s.requests = createBuffer(d, L"S VSM requests", (uint64_t)kSlots * 4);
    s.meta = createBuffer(d, L"S VSM page metadata", (uint64_t)pages * 16);
    s.freeList = createBuffer(d, L"S VSM free list", 4 + (uint64_t)pages * 4);
    s.dirtyList = createBuffer(d, L"S VSM dirty list", 8 + (uint64_t)pages * 8);
    s.cullMask = createBuffer(d, L"S VSM cull mask", (uint64_t)kSlots / 8);
    s.args = createBuffer(d, L"S VSM indirect args", 16);
    s.stats = createBuffer(d, L"S VSM stats", 32);
    s.ring = createBuffer(d, L"S VSM constants ring", (uint64_t)kRingSlots * kRingStride, D3D12_HEAP_TYPE_UPLOAD);
    s.statsReadback = createBuffer(d, L"S VSM stats readback", (uint64_t)kStatsSlots * 32, D3D12_HEAP_TYPE_READBACK);
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

const VsmConstantsCpu& lastConstants(TrackState& state) { return state.get<State>(kStateKey).constants; }

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
        D3D12_RANGE r{ i * 32, i * 32 + 32 };
        check(s.statsReadback->Map(0, &r, reinterpret_cast<void**>(&p)), "map VSM stats");
        const uint32_t* w = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + i * 32);
        s.latest = { s.statsFrame[i], w[0], w[1], w[2], w[3], w[4] };
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
    VsmConstantsCpu& c = s.constants;
    c = {};
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
    }

    RenderGraph& g = fc.graph;
    const TextureRef pool = g.importTexture(s.pool.Get(), TextureDesc{ "S VSM pool", s.poolPagesX * kPage, s.poolPagesY * kPage, 1, 1, DXGI_FORMAT_R32_UINT },
                                            D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    const BufferRef table = g.importBuffer(s.table.Get(), BufferDesc{ "S VSM page table", (uint64_t)kSlots * 8, 0 });
    const BufferRef requests = g.importBuffer(s.requests.Get(), BufferDesc{ "S VSM requests", (uint64_t)kSlots * 4, 0 });
    const BufferRef meta = g.importBuffer(s.meta.Get(), BufferDesc{ "S VSM page metadata", (uint64_t)pages * 16, 16 });
    const BufferRef freeList = g.importBuffer(s.freeList.Get(), BufferDesc{ "S VSM free list", 4 + (uint64_t)pages * 4, 0 });
    const BufferRef dirty = g.importBuffer(s.dirtyList.Get(), BufferDesc{ "S VSM dirty list", 8 + (uint64_t)pages * 8, 0 });
    const BufferRef mask = g.importBuffer(s.cullMask.Get(), BufferDesc{ "S VSM cull mask", (uint64_t)kSlots / 8, 0 });
    const BufferRef args = g.importBuffer(s.args.Get(), BufferDesc{ "S VSM indirect args", 16, 0 });
    const BufferRef statsBuf = g.importBuffer(s.stats.Get(), BufferDesc{ "S VSM stats", 32, 0 });
    const BufferRef revisions = g.importBuffer(s.lastRevision.Get(), BufferDesc{ "S VSM instance revisions", (uint64_t)s.instanceCapacity * 8, 8 });
    s.recordedFrame = fc.frame.frameIndex;
    s.poolRef = pool;
    s.tableRef = table;
    s.metaRef = meta;
    s.pagesRecorded = true;
    fc.resources.vsmPool = pool;
    fc.resources.vsmPageTable = table;

    ShaderLibrary& sh = fc.shaders;
    const uint32_t ring = s.ringSrv, off = s.constantsOffset;
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
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmBegin");
        g.addPass("s.vsm.begin", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(dirty, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(dirty), ctx.uav(statsBuf), 0, 0 };
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
    if (c.instanceCount > 0)
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmInvalidate");
        const uint32_t n = c.instanceCount;
        g.addPass("s.vsm.invalidate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::UavCompute);
                      b.use(meta, Use::SrvCompute);
                      b.use(revisions, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(table), ctx.srv(meta), ctx.uav(revisions), ring, off, 0, 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(n, 64), 1, 1);
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
    ID3D12CommandSignature* signature = s.dispatchSignature.Get();
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
        r.textureUses.push_back({ pool, Use::UavGraphics });
        r.bufferUses.push_back({ table, Use::SrvGraphics });
        r.pixelConstants[0] = s.poolUav;
        r.pixelConstants[1] = s.tableSrv;
        std::memcpy(&r.pixelConstants[2], &c.hMin, 4);
        std::memcpy(&r.pixelConstants[3], &c.hMax, 4);
        r.pixelConstants[4] = s.poolPagesX;
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
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.srv(dirty), ctx.srv(pool), ctx.uav(meta), ring, off, 0, 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
                  });
    }
    {
        // Counters to the readback ring (read at a later record once the GPU passed this frame).
        const uint32_t slot = (uint32_t)(c.frame % kStatsSlots);
        ID3D12Resource* rb = s.statsReadback.Get();
        g.addPass("s.vsm.stats", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(statsBuf, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(rb, slot * 32ull, ctx.resource(statsBuf), 0, 32); });
        s.statsFrame[slot] = c.frame;
        s.statsFence[slot] = 0;
        s.lastStatsSlot = (int)slot;
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
    const TextureRef depth = view.depth, gbuffer = view.gbuffer, pool = s.poolRef;
    const BufferRef table = s.tableRef, meta = s.metaRef;
    const uint32_t ring = s.ringSrv, off = s.constantsOffset;
    const uint32_t rays = (uint32_t)fc.quality.integer("shadow.vsm.search_taps"), steps = (uint32_t)fc.quality.integer("shadow.vsm.filter_taps");
    const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shadow/ShadowVisibility");
    g.addPass("s.shadow.visibility", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(pool, Use::SrvCompute);
                  b.use(table, Use::SrvCompute);
                  b.use(meta, Use::SrvCompute);
                  b.use(out, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[12] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, off, ctx.srv(table), ctx.srv(pool), ctx.srv(meta), rays, steps, 0, 0 };
                  ctx.cmd->SetPipelineState(pso);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 12);
                  ctx.cmd->Dispatch(groups(w, 8), groups(h, 8), 1);
              });
}
} // namespace unx::render::shadow
