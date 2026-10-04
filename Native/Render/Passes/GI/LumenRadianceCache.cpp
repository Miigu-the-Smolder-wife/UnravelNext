// Far-field radiance cache (lumen.radiance_cache; LumenRadianceCache.hlsli states the structure; owner A, for R's
// gi.lumen final gather). Persistent state: the indirection volume, the probe atlas and its depth atlas, the probe slots,
// free list and counters. The cache is emptied on a new scene revision, an origin shift or a change of its sizes; a
// history discontinuity (a camera cut) keeps it: the probes are world-space.
#include "unx/gi/LumenRadianceCache.h"
#include "LumenRadianceCacheDispatch.h"

#include "unx/core/Config.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/render/PassChain.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace unx::render::gi
{
namespace
{
constexpr uint32_t kMaxClipmaps = 6, kRing = 4, kParamBytes = 256;
constexpr uint32_t kDescStride = (uint32_t)((sizeof(D3D12_DISPATCH_RAYS_DESC) + 7) / 8 * 8);
// The structural bound of one dispatch (2026-10-01, the bath lounge's device-hung report): the probe rays, the filter and
// the store run in chunks of probes, each at most this many rays (probe texels); the chunks past the frame's trace count
// launch nothing.
constexpr uint32_t kMaxRaysPerDispatch = 262144;
// A probe texel's thread traces up to this many rays: the line to its ray's start (probe occlusion), its own, and at a
// hit the sun's and one local light's shadow ray.
constexpr uint32_t kRaysPerTexel = 4;
// LumenRadianceCache.hlsli: a probe's irradiance map with its border; the hit-mark list (a 16 B header, 16 B a position)
constexpr uint32_t kIrradianceBordered = 8, kHitMarks = 2048, kHitMarkBytes = 16 + kHitMarks * 16;

// LumenRadianceCache.hlsli LrcParams
struct Params
{
    float cornerCell[kMaxClipmaps][4];
    float prevCornerCell[kMaxClipmaps][4];
    uint32_t clipmaps, grid, probeResolution, atlasProbes;
    float reprojectionRadiusScale, invFadeSize, tMinScale, traceDistance;
    uint32_t frame, maxProbes, budget, keepFrames;
    float downsampleDistance;
    uint32_t traceCapacity, tempProbes, finalResolution;
};
static_assert(sizeof(Params) == kParamBytes, "LrcParams is 256 B (LumenRadianceCache.hlsli)");

struct Settings
{
    uint32_t clipmaps, grid, probeResolution, atlasProbes, budget, keepFrames, traceCapacity, markTile;
    float extent, base, reprojection, traceDistance, downsampleDistance, maxHitAngle;
    float farStart;  // lumen.radiance_cache_far_field: the far field's start (the mesh cards' end), 0: no far field
    bool filter, occlusion;
    bool irradiance;       // lumen.hit_indirect_radiance_cache: irradiance maps and the hits' mark list
    uint32_t hitMarkBias;  // lumen.hit_indirect_mark_clipmap_bias
    bool operator==(const Settings&) const = default;
};
Settings settings(const QualityConfig& q)
{
    Settings s{};
    s.clipmaps = (uint32_t)q.integer("lumen.radiance_cache_clipmaps");
    s.grid = (uint32_t)q.integer("lumen.radiance_cache_grid");
    s.probeResolution = (uint32_t)q.integer("lumen.radiance_cache_probe_resolution");
    s.atlasProbes = (uint32_t)q.integer("lumen.radiance_cache_atlas_probes");
    s.budget = (uint32_t)q.integer("lumen.radiance_cache_probes_per_frame");
    s.keepFrames = (uint32_t)q.integer("lumen.radiance_cache_keep_frames");
    s.traceCapacity = (uint32_t)q.integer("lumen.radiance_cache_trace_capacity");
    s.markTile = (uint32_t)q.integer("lumen.radiance_cache_mark_tile_px");
    s.extent = (float)q.number("lumen.radiance_cache_extent_m");
    s.base = (float)q.number("lumen.radiance_cache_distribution_base");
    s.reprojection = (float)q.number("lumen.radiance_cache_reprojection_radius_scale");
    s.traceDistance = (float)q.number("lumen.radiance_cache_trace_distance_m");
    // the far field: the rays run on to its distance; hits past the mesh cards take the sky's light (GiSky.hlsli)
    s.farStart = 0;
    if (q.has("lumen.radiance_cache_far_field") && q.boolean("lumen.radiance_cache_far_field"))
    {
        s.traceDistance = std::max(s.traceDistance, (float)q.number("lumen.radiance_cache_far_field_distance_m"));
        s.farStart = (float)q.number("surface_cache.mesh_cards_max_distance_m");
    }
    s.downsampleDistance = (float)q.number("lumen.radiance_cache_downsample_distance_m");
    s.maxHitAngle = (float)q.number("lumen.radiance_cache_filter_max_hit_angle");
    s.filter = q.boolean("lumen.radiance_cache_filter");
    s.occlusion = q.boolean("lumen.radiance_cache_probe_occlusion");
    s.irradiance = (!q.has("lumen.hit_indirect") || q.boolean("lumen.hit_indirect")) &&
                   (!q.has("lumen.hit_indirect_radiance_cache") || q.boolean("lumen.hit_indirect_radiance_cache"));
    s.hitMarkBias = q.has("lumen.hit_indirect_mark_clipmap_bias") ? (uint32_t)std::clamp((int)q.integer("lumen.hit_indirect_mark_clipmap_bias"), 0, (int)kMaxClipmaps) : 2u;
    if (s.clipmaps < 1 || s.clipmaps > kMaxClipmaps) fail("lumen.radiance_cache_clipmaps must be in [1, 6]");
    if (s.grid < 8 || s.grid > 252 || s.grid % 4 != 0) fail("lumen.radiance_cache_grid must be a multiple of 4 in [8, 252]");
    if (s.probeResolution < 8 || s.probeResolution > 64 || s.probeResolution % 8 != 0) fail("lumen.radiance_cache_probe_resolution must be 8, 16, ... 64");
    if (s.atlasProbes < 8 || s.atlasProbes > 256) fail("lumen.radiance_cache_atlas_probes must be in [8, 256]");
    if (s.traceCapacity < 16 || s.traceCapacity > 16384) fail("lumen.radiance_cache_trace_capacity must be in [16, 16384]");
    if (s.markTile < 4 || s.markTile > 64) fail("lumen.radiance_cache_mark_tile_px must be in [4, 64]");
    if (!(s.extent > 0) || !(s.base > 1) || !(s.traceDistance > 0)) fail("lumen.radiance_cache: extent and trace distance must be positive, distribution base > 1");
    return s;
}

uint32_t bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct RcState
{
    Device* device = nullptr;
    Settings settings{};
    ComPtr<ID3D12Resource> indirection, atlas, depth, slots, counters, freeList, ring, descTemplate, irradiance, hitMarks;
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    uint8_t* ringMapped = nullptr;
    uint32_t ringSrv[kRing] = {};
    bool created = false, fresh = true;
    uint32_t revision = 0xFFFFFFFFu, frame = 0;
    float prevCornerCell[kMaxClipmaps][4] = {};
    // the frame's record (idempotent Begin / Update)
    RecordKey recordedFrame;
    LumenRcFrame record;
    BufferRef slotsRef, countersRef;
    Params params{};
    ~RcState() { release(); }
    void release()
    {
        if (!device) return;
        for (ComPtr<ID3D12Resource>* r : { std::addressof(indirection), std::addressof(atlas), std::addressof(depth), std::addressof(slots), std::addressof(counters),
                                           std::addressof(freeList), std::addressof(ring), std::addressof(descTemplate), std::addressof(irradiance), std::addressof(hitMarks) })
            if (*r) device->deferRelease(*r);
        irradiance.Reset();
        hitMarks.Reset();
        if (created)
        {
            DescriptorHeaps* h = &device->descriptors();
            for (uint32_t s : ringSrv) device->deferCall([h, s] { h->freeResource(s); });
        }
        created = false;
    }
    void ensure(Device& d, const Settings& s)
    {
        if (created && s == settings) return;
        release();
        device = &d;
        settings = s;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, upload{ D3D12_HEAP_TYPE_UPLOAD };
        auto texture = [&](ComPtr<ID3D12Resource>& out, D3D12_RESOURCE_DIMENSION dim, uint32_t w, uint32_t h, uint32_t depthSlices, DXGI_FORMAT format, const wchar_t* name) {
            D3D12_RESOURCE_DESC1 desc{};
            desc.Dimension = dim;
            desc.Width = w;
            desc.Height = h;
            desc.DepthOrArraySize = (UINT16)depthSlices;
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            out.Reset();
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&out)),
                  "R radiance cache texture");
            out->SetName(name);
        };
        auto buffer = [&](ComPtr<ID3D12Resource>& out, uint64_t bytes, bool up, const wchar_t* name) {
            D3D12_RESOURCE_DESC1 desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = up ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            out.Reset();
            check(d.d3d()->CreateCommittedResource3(up ? &upload : &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&out)),
                  "R radiance cache buffer");
            out->SetName(name);
        };
        const uint32_t maxProbes = s.atlasProbes * s.atlasProbes, finalRes = s.probeResolution + 2;
        texture(indirection, D3D12_RESOURCE_DIMENSION_TEXTURE3D, s.grid * s.clipmaps, s.grid, s.grid, DXGI_FORMAT_R32_UINT, L"R rc indirection");
        texture(atlas, D3D12_RESOURCE_DIMENSION_TEXTURE2D, s.atlasProbes * finalRes, s.atlasProbes * finalRes, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, L"R rc atlas");
        texture(depth, D3D12_RESOURCE_DIMENSION_TEXTURE2D, s.atlasProbes * s.probeResolution, s.atlasProbes * s.probeResolution, 1, DXGI_FORMAT_R16_UINT, L"R rc depth atlas");
        buffer(slots, (uint64_t)maxProbes * 16, false, L"R rc probe slots");
        buffer(counters, 128, false, L"R rc counters");
        buffer(freeList, (uint64_t)maxProbes * 4, false, L"R rc free list");
        buffer(ring, (uint64_t)kRing * kParamBytes, true, L"R rc parameters ring");
        buffer(descTemplate, 2 * kDescStride, true, L"R rc ray dispatch template");
        if (s.irradiance)
        {
            texture(irradiance, D3D12_RESOURCE_DIMENSION_TEXTURE2D, s.atlasProbes * kIrradianceBordered, s.atlasProbes * kIrradianceBordered, 1, DXGI_FORMAT_R16G16B16A16_FLOAT,
                    L"R rc irradiance atlas");
            buffer(hitMarks, kHitMarkBytes, false, L"R rc hit marks");
        }
        D3D12_RANGE none{ 0, 0 };
        check(ring->Map(0, &none, reinterpret_cast<void**>(&ringMapped)), "map R rc parameters ring");
        for (uint32_t k = 0; k < kRing; ++k)
        {
            ringSrv[k] = d.descriptors().allocateResource();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.Buffer.FirstElement = k * (kParamBytes / 4);
            sd.Buffer.NumElements = kParamBytes / 4;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            d.d3d()->CreateShaderResourceView(ring.Get(), &sd, d.descriptors().resourceCpu(ringSrv[k]));
        }
        if (!dispatchSignature)
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg{};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
            D3D12_COMMAND_SIGNATURE_DESC desc{};
            desc.ByteStride = 16;
            desc.NumArgumentDescs = 1;
            desc.pArgumentDescs = &arg;
            check(d.d3d()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&dispatchSignature)), "R radiance cache dispatch signature");
        }
        created = true;
        fresh = true;
    }
};

const char* kTraceLibrary[2] = { "Passes/GI/LumenRadianceCacheTrace.SKY0", "Passes/GI/LumenRadianceCacheTrace.SKY1" };
} // namespace

LumenRcFrame lumenRadianceCacheBegin(FramePassContext& fc, const ViewResources& main)
{
    const QualityConfig& q = fc.quality;
    if (!q.has("lumen.radiance_cache") || !q.boolean("lumen.radiance_cache")) return {};
    if (main.view.kind != gpu::ViewKind::Main || !fc.trackState || !main.depth.valid() || !fc.resources.tlasStatic.valid()) return {};
    RcState& st = fc.state<RcState>("R.lumenRadianceCache");
    if (st.recordedFrame == RecordKey::of(fc)) return st.record;
    const Settings s = settings(q);
    st.ensure(fc.device, s);
    const float3 shift = fc.frame.originShift;
    const bool reset = st.fresh || st.revision != fc.scene.revision() || shift.x != 0 || shift.y != 0 || shift.z != 0;
    st.fresh = false;
    st.revision = fc.scene.revision();
    ++st.frame;  // (frame 0 = never traced)

    // ---- this frame's clipmaps: each level's cells twice the last, centred on the camera snapped to the level's grid
    Params p{};
    const uint32_t maxProbes = s.atlasProbes * s.atlasProbes;
    const float3 eye = main.view.position;
    for (uint32_t c = 0; c < s.clipmaps; ++c)
    {
        const double cell = 2.0 * s.extent / s.grid * std::pow((double)s.base, (double)c);
        const double origin[3] = { std::floor(eye.x / cell + 0.5) * cell, std::floor(eye.y / cell + 0.5) * cell, std::floor(eye.z / cell + 0.5) * cell };
        for (int a = 0; a < 3; ++a) p.cornerCell[c][a] = (float)(origin[a] - 0.5 * s.grid * cell);
        p.cornerCell[c][3] = (float)cell;
    }
    std::memcpy(p.prevCornerCell, reset ? p.cornerCell : st.prevCornerCell, sizeof p.prevCornerCell);
    std::memcpy(st.prevCornerCell, p.cornerCell, sizeof p.cornerCell);
    p.clipmaps = s.clipmaps;
    p.grid = s.grid;
    p.probeResolution = s.probeResolution;
    p.atlasProbes = s.atlasProbes;
    p.reprojectionRadiusScale = s.reprojection;
    p.invFadeSize = 1.0f;
    p.tMinScale = 1.0f;
    p.traceDistance = s.traceDistance;
    p.frame = st.frame;
    p.maxProbes = maxProbes;
    p.budget = s.budget;
    p.keepFrames = s.keepFrames;
    p.downsampleDistance = s.downsampleDistance;
    p.traceCapacity = s.traceCapacity;
    p.tempProbes = (uint32_t)std::ceil(std::sqrt((double)s.traceCapacity));
    p.finalResolution = s.probeResolution + 2;
    st.params = p;
    const uint32_t ringSlot = (uint32_t)(fc.frame.frameIndex % kRing);
    std::memcpy(st.ringMapped + (size_t)ringSlot * kParamBytes, &p, sizeof p);
    const uint32_t paramsSrv = st.ringSrv[ringSlot];

    RenderGraph& g = fc.graph;
    LumenRcFrame f;
    f.on = true;
    f.params = paramsSrv;
    f.indirection = g.importTexture(st.indirection.Get(), { "r.gi.rc indirection", s.grid * s.clipmaps, s.grid, (uint16_t)s.grid, 1, DXGI_FORMAT_R32_UINT, D3D12_RESOURCE_DIMENSION_TEXTURE3D },
                                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    f.atlas = g.importTexture(st.atlas.Get(), { "r.gi.rc atlas", s.atlasProbes * p.finalResolution, s.atlasProbes * p.finalResolution, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                              D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    f.depth = g.importTexture(st.depth.Get(), { "r.gi.rc depth atlas", s.atlasProbes * s.probeResolution, s.atlasProbes * s.probeResolution, 1, 1, DXGI_FORMAT_R16_UINT },
                              D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    if (s.irradiance)
    {
        f.irradiance = g.importTexture(st.irradiance.Get(),
                                       { "r.gi.rc irradiance atlas", s.atlasProbes * kIrradianceBordered, s.atlasProbes * kIrradianceBordered, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                       D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        f.hitMarks = g.importBuffer(st.hitMarks.Get(), BufferDesc{ "r.gi.rc hit marks", kHitMarkBytes, 0 });
    }
    st.recordedFrame = RecordKey::of(fc);
    st.record = f;

    const BufferRef slots = g.importBuffer(st.slots.Get(), BufferDesc{ "r.gi.rc probe slots", (uint64_t)maxProbes * 16, 0 });
    const BufferRef counters = g.importBuffer(st.counters.Get(), BufferDesc{ "r.gi.rc counters", 128, 0 });
    st.slotsRef = slots;
    st.countersRef = counters;
    const TextureRef indirection = f.indirection, depth = main.depth;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = main.frameConstants;
    ShaderLibrary& shaders = fc.shaders;
    // lumen.radiance_cache_fold_passes: the reset, the cells' clear, the mark and the hit marks are one scope
    // (PassChain.h: the same passes in order, timed together).
    PassChain chain(g, QueueType::Compute, !fc.quality.has("lumen.radiance_cache_fold_passes") || fc.quality.boolean("lumen.radiance_cache_fold_passes"));
    if (reset)
        chain.add("r.gi.rc.reset",
                  [&](PassBuilder& b) {
                      b.use(slots, Use::UavCompute);
                      b.use(counters, Use::UavCompute);
                      b.keep();
                  },
                  [=, &shaders](PassContext& c) {
                      const uint32_t k[4] = { paramsSrv, 0xFFFFFFFFu, c.uav(slots), c.uav(counters) };
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenRadianceCacheUpdate.MODE7"));
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((maxProbes + 63) / 64, 1, 1);
                  });
    chain.add("r.gi.rc.clear",
              [&](PassBuilder& b) {
                  b.use(indirection, Use::UavCompute);
                  b.use(counters, Use::UavCompute);
                  b.keep();
              },
              [=, &shaders](PassContext& c) {
                  const uint32_t k[4] = { paramsSrv, c.uav(indirection), 0xFFFFFFFFu, c.uav(counters) };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenRadianceCacheUpdate.MODE0"));
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(s.grid * s.clipmaps / 4, s.grid / 4, s.grid / 4);
              });
    const uint32_t W = main.view.width, H = main.view.height, tile = s.markTile;
    chain.add("r.gi.rc.mark",
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(indirection, Use::UavCompute);
                  b.keep();
              },
              [=, &shaders](PassContext& c) {
                  const uint32_t k[8] = { paramsSrv, c.uav(indirection), 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, c.srv(depth), tile };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenRadianceCacheUpdate.MODE1"));
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch(((W + tile - 1) / tile + 7) / 8, ((H + tile - 1) / tile + 7) / 8, 1);
              });
    if (f.hitMarks.valid())
    {
        // The positions last frame's ray hits asked for probes at (lrcHitMark), marked like the screen's, then the list
        // emptied for this frame's hits. A reset frame (a new cache, another scene, an origin shift: the list is not
        // written yet, or speaks of other coordinates) only empties it.
        const BufferRef hitMarks = f.hitMarks;
        const uint32_t bias = s.hitMarkBias;
        for (uint32_t clear = reset ? 1u : 0u; clear < 2; ++clear)
            chain.add(clear ? "r.gi.rc.hitmark.clear" : "r.gi.rc.hitmark",
                      [&](PassBuilder& b) {
                          if (!clear) b.use(indirection, Use::UavCompute);
                          b.use(hitMarks, Use::UavCompute);
                          b.keep();
                      },
                      [=, &shaders](PassContext& c) {
                          const uint32_t k[12] = { paramsSrv, clear ? 0xFFFFFFFFu : c.uav(indirection), 0xFFFFFFFFu, 0xFFFFFFFFu,
                                                   0xFFFFFFFFu, 0xFFFFFFFFu, c.uav(hitMarks), bias, clear, 0, 0, 0 };
                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenRadianceCacheUpdate.MODE9"));
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 12);
                          c.cmd->Dispatch(clear ? 1u : (kHitMarks + 63) / 64, 1, 1);
                      });
    }
    chain.flush("r.gi.rc.mark");
    return f;
}

void lumenRadianceCacheUpdate(FramePassContext& fc, const ViewResources& main, rt::RayScene& rays, const LumenRcInputs& in, LumenRcFrame& frame)
{
    if (!frame.on) return;
    RcState& st = fc.state<RcState>("R.lumenRadianceCache");
    if (st.record.updated)
    {
        frame = st.record;
        return;
    }
    const Settings s = st.settings;
    const Params p = st.params;
    RenderGraph& g = fc.graph;
    ShaderLibrary& shaders = fc.shaders;
    const uint32_t paramsSrv = frame.params, maxProbes = p.maxProbes;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = main.frameConstants;
    const TextureRef indirection = frame.indirection, atlas = frame.atlas, depthAtlas = frame.depth;
    const BufferRef slots = st.slotsRef, counters = st.countersRef;  // (this frame's imports, by Begin)
    const BufferRef freeList = g.importBuffer(st.freeList.Get(), BufferDesc{ "r.gi.rc free list", (uint64_t)maxProbes * 4, 0 });
    const BufferRef traces = g.createBuffer({ "r.gi.rc traces", (uint64_t)s.traceCapacity * 16, 0 });
    const uint32_t probesPerDispatch = std::max(1u, kMaxRaysPerDispatch / (s.probeResolution * s.probeResolution * kRaysPerTexel));
    const uint32_t chunks = (s.traceCapacity + probesPerDispatch - 1) / probesPerDispatch;
    const BufferRef rayArgs = g.createBuffer({ "r.gi.rc ray dispatch", (uint64_t)chunks * kDescStride, 0 });
    const BufferRef filterArgs = g.createBuffer({ "r.gi.rc filter dispatch", (uint64_t)chunks * LRC_COMPUTE_ARGS_STRIDE, 0 });
    const uint32_t tempSize = p.tempProbes * s.probeResolution;
    const TextureRef traced = g.createTexture({ "r.gi.rc traced", tempSize, tempSize, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef filtered = g.createTexture({ "r.gi.rc filtered", tempSize, tempSize, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });

    // lumen.radiance_cache_fold_passes: the four bookkeeping steps, the copy of the ray dispatch descriptions and the
    // step that writes their sizes are one scope (PassChain.h).
    PassChain chain(g, QueueType::Compute, !fc.quality.has("lumen.radiance_cache_fold_passes") || fc.quality.boolean("lumen.radiance_cache_fold_passes"));
    auto bookkeeping = [&](const char* name, const char* kernel, uint32_t x, uint32_t y, uint32_t z, bool cells, bool list, bool queue) {
        ID3D12PipelineState* pso = shaders.compute(kernel);
        chain.add(name,
                  [=](PassBuilder& b) {
                      if (cells) b.use(indirection, Use::UavCompute);
                      b.use(slots, Use::UavCompute);
                      b.use(counters, Use::UavCompute);
                      if (list) b.use(freeList, Use::UavCompute);
                      if (queue) b.use(traces, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& c) {
                      const uint32_t k[8] = { paramsSrv, cells ? c.uav(indirection) : 0xFFFFFFFFu, c.uav(slots), c.uav(counters),
                                              list ? c.uav(freeList) : 0xFFFFFFFFu, queue ? c.uav(traces) : 0xFFFFFFFFu, 0xFFFFFFFFu, 0 };
                      c.cmd->SetPipelineState(pso);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch(x, y, z);
                  });
    };
    const uint32_t cellsX = s.grid * s.clipmaps / 4, cellsYZ = s.grid / 4, slotGroups = (maxProbes + 63) / 64;
    bookkeeping("r.gi.rc.reuse", "Passes/GI/LumenRadianceCacheUpdate.MODE2", slotGroups, 1, 1, true, true, false);
    bookkeeping("r.gi.rc.allocate", "Passes/GI/LumenRadianceCacheUpdate.MODE3", cellsX, cellsYZ, cellsYZ, true, true, false);
    bookkeeping("r.gi.rc.select", "Passes/GI/LumenRadianceCacheUpdate.MODE4", 1, 1, 1, false, false, false);
    bookkeeping("r.gi.rc.traces", "Passes/GI/LumenRadianceCacheUpdate.MODE5", slotGroups, 1, 1, false, false, true);

    // ---- the probe rays: the dispatch description's size is the queued trace count (written on the GPU)
    const FrameResources& fr = fc.resources;
    const bool atmosphere = fr.transmittanceLut.valid() && fr.multiScatterLut.valid() && fr.skyViewLut.valid() && fr.aerialPerspective.valid();
    const TextureRef luts[4] = { fr.transmittanceLut, fr.multiScatterLut, fr.skyViewLut, fr.aerialPerspective };
    const uint32_t variant = atmosphere ? 0u : 1u;
    rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(kTraceLibrary[variant], { "LumenRadianceCacheTraceGen" }));
    {
        // the description's shader tables, per variant, in the template the frame's buffer is copied from
        uint8_t* m = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(st.descTemplate->Map(0, &none, reinterpret_cast<void**>(&m)), "map R rc ray dispatch template");
        const D3D12_DISPATCH_RAYS_DESC desc = pipeline.dispatchDesc(0, 0, 1, 1);
        std::memcpy(m + variant * kDescStride, &desc, sizeof desc);
        st.descTemplate->Unmap(0, nullptr);
    }
    ID3D12Resource* descTemplate = st.descTemplate.Get();
    chain.add("r.gi.rc.args", [&](PassBuilder& b) { b.use(rayArgs, Use::CopyDst); },
              [=](PassContext& c) {
                  for (uint32_t chunk = 0; chunk < chunks; ++chunk)
                      c.cmd->CopyBufferRegion(c.resource(rayArgs), (uint64_t)chunk * kDescStride, descTemplate, variant * kDescStride, kDescStride);
              });
    chain.add("r.gi.rc.finish",
              [&](PassBuilder& b) {
                  b.use(counters, Use::UavCompute);
                  b.use(rayArgs, Use::UavCompute);
                  b.use(filterArgs, Use::UavCompute);
                  b.keep();
              },
              [=, &shaders](PassContext& c) {
                  const uint32_t k[16] = { paramsSrv, 0xFFFFFFFFu, 0xFFFFFFFFu, c.uav(counters), 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0,
                                           c.uav(rayArgs), (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width), c.uav(filterArgs), probesPerDispatch,
                                           chunks, kDescStride, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenRadianceCacheUpdate.MODE6"));
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 16);
                  c.cmd->Dispatch(1, 1, 1);
              });
    chain.flush("r.gi.rc.bookkeeping");
    const BufferRef worldCache = in.worldCache;
    const SurfaceCacheCardRefs cards = in.cards;
    const float3 sky = in.skyRadiance, sun = in.sunIlluminance;
    const uint32_t experiment = in.experiment;
    const float traceDistance = s.traceDistance;
    const bool occlusion = s.occlusion;
    const float farStart = s.farStart;
    uint32_t sceneWords[8];
    rays.recordHair(fc);  // E's grooms on the rays (HitHair.hlsli): the header's words 22, 23, before rootConstants
    rays.rootConstants(sceneWords);
    const std::array<uint32_t, 8> sceneSrvs = std::to_array(sceneWords);
    g.addPass("r.gi.rc.trace", QueueType::Compute,
              [&](PassBuilder& b) {
                  if (worldCache.valid()) b.use(worldCache, Use::SrvGraphics);
                  b.use(traces, Use::SrvGraphics);
                  b.use(counters, Use::SrvGraphics);
                  b.use(traced, Use::UavGraphics);
                  b.use(depthAtlas, Use::UavGraphics);
                  declareSurfaceCacheCards(b, cards, Use::SrvGraphics);
                  b.use(rayArgs, Use::IndirectArgs);
                  rays.declareTraversal(b);
                  rays.declareDecals(b);
                  rays.declareHair(b);
                  if (atmosphere)
                      for (const TextureRef& t : luts) b.use(t, Use::SrvGraphics);
                  b.keep();
              },
              [=, &pipeline](PassContext& c) {
                  uint32_t k[32] = {};
                  k[0] = worldCache.valid() ? c.srv(worldCache) : 0xFFFFFFFFu;
                  k[1] = c.srv(traces);
                  k[2] = c.uav(traced);
                  k[3] = c.uav(depthAtlas);
                  k[4] = bits(sky.x);
                  k[5] = bits(sky.y);
                  k[6] = bits(sky.z);
                  k[7] = bits(traceDistance);
                  for (int i = 0; i < 4; ++i) k[8 + i] = atmosphere ? c.srv(luts[i]) : 0xFFFFFFFFu;
                  k[12] = bits(sun.x);
                  k[13] = bits(sun.y);
                  k[14] = bits(sun.z);
                  k[15] = experiment;
                  k[16] = paramsSrv;
                  k[17] = c.srv(counters);
                  k[19] = occlusion ? 1u : 0u;  // P[4].w
                  k[21] = bits(farStart);       // P[5].y
                  k[20] = cards.valid() ? c.srv(cards.frame) : 0xFFFFFFFFu;
                  std::memcpy(&k[24], sceneSrvs.data(), 32);
                  c.bindFrameConstants(cb);
                  for (uint32_t chunk = 0; chunk < chunks; ++chunk)
                  {
                      k[18] = chunk * probesPerDispatch;  // P[4].z: the dispatch's first trace record
                      c.computeConstants(k, 32);
                      pipeline.dispatchIndirect(c.cmd, c.resource(rayArgs), (uint64_t)chunk * kDescStride);
                  }
              });

    // ---- filter, then store with the border
    ID3D12CommandSignature* signature = st.dispatchSignature.Get();
    const float maxHitAngle = s.filter ? s.maxHitAngle : 0.0f;
    g.addPass("r.gi.rc.filter", QueueType::Compute,
              [&](PassBuilder& b) {
                  for (BufferRef r : { counters, traces, slots }) b.use(r, Use::SrvCompute);
                  for (TextureRef t : { indirection, traced, depthAtlas, atlas }) b.use(t, Use::SrvCompute);
                  b.use(filtered, Use::UavCompute);
                  b.use(filterArgs, Use::IndirectArgs);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[12] = { paramsSrv, c.srv(counters), c.srv(traces), c.srv(indirection),
                                     c.srv(traced), c.srv(depthAtlas), c.srv(slots), c.uav(filtered),
                                     c.srv(atlas), bits(maxHitAngle), 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenRadianceCacheFilter.MODE0"));
                  c.bindFrameConstants(cb);
                  for (uint32_t chunk = 0; chunk < chunks; ++chunk)
                  {
                      k[10] = chunk * probesPerDispatch;  // P[2].z: the dispatch's first trace record
                      c.computeConstants(k, 12);
                      c.cmd->ExecuteIndirect(signature, 1, c.resource(filterArgs), (uint64_t)chunk * LRC_COMPUTE_ARGS_STRIDE + LRC_FILTER_ARGS_OFFSET, nullptr, 0);
                  }
              });
    g.addPass("r.gi.rc.store", QueueType::Compute,
              [&](PassBuilder& b) {
                  for (BufferRef r : { counters, traces }) b.use(r, Use::SrvCompute);
                  b.use(filtered, Use::SrvCompute);
                  b.use(atlas, Use::UavCompute);
                  b.use(filterArgs, Use::IndirectArgs);
                  b.keep();
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[12] = { paramsSrv, c.srv(counters), c.srv(traces), 0xFFFFFFFFu, c.srv(filtered), 0xFFFFFFFFu, 0xFFFFFFFFu, c.uav(atlas), 0xFFFFFFFFu, 0, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenRadianceCacheFilter.MODE1"));
                  c.bindFrameConstants(cb);
                  for (uint32_t chunk = 0; chunk < chunks; ++chunk)
                  {
                      k[10] = chunk * probesPerDispatch;
                      c.computeConstants(k, 12);
                      c.cmd->ExecuteIndirect(signature, 1, c.resource(filterArgs), (uint64_t)chunk * LRC_COMPUTE_ARGS_STRIDE + LRC_STORE_ARGS_OFFSET, nullptr, 0);
                  }
              });
    if (frame.irradiance.valid())
    {
        // The traced probes' irradiance maps, from the radiance the store left in the atlas.
        // Dedicated arguments launch one group per probe; the store's atlas-sized
        // XY grid would launch 25 groups at resolution 32, of which 24 are idle.
        const TextureRef irradiance = frame.irradiance;
        g.addPass("r.gi.rc.irradiance", QueueType::Compute,
                  [&](PassBuilder& b) {
                      for (BufferRef r : { counters, traces }) b.use(r, Use::SrvCompute);
                      b.use(atlas, Use::SrvCompute);
                      b.use(irradiance, Use::UavCompute);
                      b.use(filterArgs, Use::IndirectArgs);
                      b.keep();
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[8] = { paramsSrv, c.srv(counters), c.srv(traces), c.srv(atlas), c.uav(irradiance), 0, 0, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenRadianceCacheIrradiance"));
                      c.bindFrameConstants(cb);
                      for (uint32_t chunk = 0; chunk < chunks; ++chunk)
                      {
                          k[5] = chunk * probesPerDispatch;  // P[1].y: the dispatch's first trace record
                          c.computeConstants(k, 8);
                          c.cmd->ExecuteIndirect(signature, 1, c.resource(filterArgs), (uint64_t)chunk * LRC_COMPUTE_ARGS_STRIDE + LRC_IRRADIANCE_ARGS_OFFSET, nullptr, 0);
                      }
                  });
    }
    bookkeeping("r.gi.rc.validate", "Passes/GI/LumenRadianceCacheUpdate.MODE8", cellsX, cellsYZ, cellsYZ, true, false, false);
    chain.flush("r.gi.rc.validate");

    frame.updated = true;
    st.record = frame;
    fc.resources.lumenRcIndirection = frame.indirection;
    fc.resources.lumenRcAtlas = frame.atlas;
    fc.resources.lumenRcDepth = frame.depth;
    fc.resources.lumenRcParams = frame.params;
}
} // namespace unx::render::gi
