// The Lumen translucency volume (unx/gi/LumenTranslucencyVolume.h; LumenTranslucencyVolume.hlsli).
#include "unx/gi/LumenTranslucencyVolume.h"

#include "unx/core/Config.h"
#include "unx/render/Device.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>

namespace unx::render::gi
{
namespace
{
constexpr uint32_t kRing = 4, kParamBytes = 96, kPixelShift = 5, kTraceRes = 3;  // (cells of 32 pixels at the reference height)
constexpr uint32_t kMaxRaysPerDispatch = 262144 / 3;  // threads: one traces at most 3 rays (its own; at a hit without cards the sun's and a light sample's)
const char* kTraceLibrary[2] = { "Passes/GI/LumenTranslucencyVolumeTrace.SKY0", "Passes/GI/LumenTranslucencyVolumeTrace.SKY1" };

// LumenTranslucencyVolume.hlsli LtvParams
struct Params
{
    float worldToClip[4][4];
    uint32_t gridX, gridY, gridZ, frame;
    uint32_t ambientSrv, directionalSrv;
    float uvScale[2];
};
static_assert(sizeof(Params) == kParamBytes, "LtvParams is 96 B (LumenTranslucencyVolume.hlsli)");

struct Settings
{
    bool enabled = false;
    float endDistance = 80.0f;       // EndDistanceFromCamera 8000 cm
    uint32_t filterSamples = 3;      // SpatialFilter.SampleCount
    float filterDeviation = 5.0f;    // SpatialFilter.StandardDeviation
    bool filter = true;              // SpatialFilter
    bool temporal = true;            // TemporalReprojection
    float historyWeight = 0.9f;      // Temporal.HistoryWeight
    bool jitter = true;              // Temporal.Jitter
    float maxRayIntensity = 20.0f;   // MaxRayIntensity
    uint32_t clipmapBias = 3;        // ShareRadianceCacheWithOpaque.ClipmapBias
    float depthThreshold = 64.0f;    // OffsetThresholdToAcceptDepthBufferOffset (reference 1; LumenTranslucencyVolumeGrid.hlsli)
    float traceDistance = 200.0f;
    float farStart = 0;              // lumen.radiance_cache_far_field: the far field's start (m), 0: none
    uint32_t referenceHeight = 0;    // translucency_volume_grid_reference_height: the cells are 32 px up to 1.4 x this view height,
                                     // then the power of two that keeps their angle (0: 32 px everywhere)
};
Settings settingsOf(const QualityConfig& q)
{
    Settings s;
    auto flag = [&](const char* key, bool fallback) { return q.has(key) ? q.boolean(key) : fallback; };
    auto num = [&](const char* key, double fallback) { return q.has(key) ? q.number(key) : fallback; };
    s.enabled = flag("lumen.translucency_volume", false);
    s.endDistance = (float)num("lumen.translucency_volume_end_distance_m", 80.0);
    s.referenceHeight = (uint32_t)num("lumen.translucency_volume_grid_reference_height", 0.0);
    s.filter = flag("lumen.translucency_volume_spatial_filter", true);
    s.filterSamples = (uint32_t)num("lumen.translucency_volume_filter_samples", 3);
    s.filterDeviation = (float)num("lumen.translucency_volume_filter_deviation", 5.0);
    s.temporal = flag("lumen.translucency_volume_temporal", true);
    s.historyWeight = (float)num("lumen.translucency_volume_history_weight", 0.9);
    s.jitter = flag("lumen.translucency_volume_jitter", true);
    s.maxRayIntensity = (float)num("lumen.translucency_volume_max_ray_intensity", 20.0);
    s.clipmapBias = (uint32_t)num("lumen.translucency_volume_clipmap_bias", 3);
    s.depthThreshold = (float)num("lumen.translucency_volume_depth_offset_threshold", 64.0);
    s.traceDistance = (float)num("lumen.radiance_cache_trace_distance_m", 200.0);
    if (flag("lumen.radiance_cache_far_field", false))
    {
        s.traceDistance = std::max(s.traceDistance, (float)num("lumen.radiance_cache_far_field_distance_m", 10000.0));
        s.farStart = (float)num("surface_cache.mesh_cards_max_distance_m", 300.0);
    }
    if (!(s.endDistance > 1) || s.filterSamples > 8 || !(s.historyWeight >= 0 && s.historyWeight < 1)) fail("lumen.translucency_volume: parameters out of range");
    return s;
}

uint32_t bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float halton(uint32_t index, uint32_t base)
{
    float f = 1, r = 0;
    for (uint32_t i = index; i > 0; i /= base)
    {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
}

struct TvState
{
    Device* device = nullptr;
    uint32_t gridX = 0, gridY = 0, gridZ = 0;
    // the volume's two textures, twice (this frame's and the last one's), with stable SRVs: the parameters name them
    ComPtr<ID3D12Resource> ambient[2], directional[2], ring;
    uint32_t ambientSrv[2] = {}, directionalSrv[2] = {}, ringSrv[kRing] = {};
    uint8_t* ringMapped = nullptr;
    bool created = false, history = false;
    uint32_t parity = 0, frame = 0, revision = 0xFFFFFFFFu;
    RecordKey recordedFrame, markedFrame;  // the recordings (frame and serial) the update and the mark were recorded in
    uint64_t stateFrame = UINT64_MAX;      // the frame the counter and the jitter were advanced for
    // lumenTranslucencyVolumePrevious: the last volume's textures as imported into frame previousImportFrame's graph, and
    // the ring slot whose parameters describe it
    RecordKey previousImportFrame;
    uint64_t publishedFrame = UINT64_MAX;
    TextureRef previousAmbient, previousDirectional;
    uint32_t publishedSlot = 0;
    float jitter[3] = { 0.5f, 0.5f, 0.5f };
    ~TvState() { release(); }
    void release()
    {
        if (!device) return;
        for (int k = 0; k < 2; ++k)
        {
            if (ambient[k]) device->deferRelease(ambient[k]);
            if (directional[k]) device->deferRelease(directional[k]);
            ambient[k].Reset();
            directional[k].Reset();
        }
        if (ring) device->deferRelease(ring);
        ring.Reset();
        if (created)
        {
            DescriptorHeaps* h = &device->descriptors();
            for (uint32_t s : ambientSrv) device->deferCall([h, s] { h->freeResource(s); });
            for (uint32_t s : directionalSrv) device->deferCall([h, s] { h->freeResource(s); });
            for (uint32_t s : ringSrv) device->deferCall([h, s] { h->freeResource(s); });
        }
        created = false;
    }
    void ensure(Device& d, uint32_t x, uint32_t y, uint32_t z)
    {
        if (created && x == gridX && y == gridY && z == gridZ) return;
        release();
        device = &d;
        gridX = x, gridY = y, gridZ = z;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, upload{ D3D12_HEAP_TYPE_UPLOAD };
        auto volume = [&](ComPtr<ID3D12Resource>& out, uint32_t& srv, const wchar_t* name) {
            D3D12_RESOURCE_DESC1 desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            desc.Width = x;
            desc.Height = y;
            desc.DepthOrArraySize = (UINT16)z;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            out.Reset();
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&out)),
                  "R translucency volume");
            out->SetName(name);
            srv = d.descriptors().allocateResource();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            sd.Texture3D.MipLevels = 1;
            d.d3d()->CreateShaderResourceView(out.Get(), &sd, d.descriptors().resourceCpu(srv));
        };
        volume(ambient[0], ambientSrv[0], L"R translucency GI ambient 0");
        volume(ambient[1], ambientSrv[1], L"R translucency GI ambient 1");
        volume(directional[0], directionalSrv[0], L"R translucency GI directional 0");
        volume(directional[1], directionalSrv[1], L"R translucency GI directional 1");
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = (uint64_t)kRing * kParamBytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d.d3d()->CreateCommittedResource3(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&ring)),
              "R translucency volume parameters");
        ring->SetName(L"R translucency volume parameters ring");
        D3D12_RANGE none{ 0, 0 };
        check(ring->Map(0, &none, reinterpret_cast<void**>(&ringMapped)), "map R translucency volume parameters");
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
        created = true;
        history = false;
    }
};

// The grid of a view and this frame's cell jitter (the same in the mark and the trace).
// log2 of the cells' pixel size: 32 px at the reference height, the nearest power of two to the same angle above it.
uint32_t pixelShiftOf(const ViewResources& main, const Settings& s)
{
    if (s.referenceHeight == 0 || main.view.height <= s.referenceHeight) return kPixelShift;
    const long steps = std::lround(std::log2((double)main.view.height / (double)s.referenceHeight));
    return kPixelShift + (uint32_t)std::clamp(steps, 0l, 3l);
}
void gridOf(const ViewResources& main, const Settings& s, uint32_t& x, uint32_t& y, uint32_t& z)
{
    const uint32_t pixels = 1u << pixelShiftOf(main, s);
    x = (main.view.width + pixels - 1) / pixels;
    y = (main.view.height + pixels - 1) / pixels;
    // slices to the end distance: log2(distance x 1 / m + 1) x 4 (LumenTranslucencyVolume.hlsli ltvSliceOfDepth)
    z = std::clamp((uint32_t)(std::log2(s.endDistance + 1.0f) * 4.0f) + 1u, 4u, 64u);
}
TvState& stateOf(FramePassContext& fc, const ViewResources& main, const Settings& s)
{
    TvState& st = fc.state<TvState>("R.lumenTranslucencyVolume");
    uint32_t x, y, z;
    gridOf(main, s, x, y, z);
    st.ensure(fc.device, x, y, z);
    if (st.stateFrame != fc.frame.frameIndex)
    {
        // a new frame: its counter and jitter (once a frame, whatever the recordings of it)
        st.stateFrame = fc.frame.frameIndex;
        ++st.frame;
        const uint32_t k = st.frame % 16u + 1u;
        st.jitter[0] = s.jitter ? halton(k, 2) : 0.5f;
        st.jitter[1] = s.jitter ? halton(k, 3) : 0.5f;
        st.jitter[2] = s.jitter ? halton(k, 5) : 0.5f;
    }
    return st;
}
bool usable(FramePassContext& fc, const ViewResources& main)
{
    return main.view.kind == gpu::ViewKind::Main && fc.trackState && main.depth.valid() && main.hiz.valid() && fc.resources.tlasStatic.valid();
}
} // namespace

LumenTvPrevious lumenTranslucencyVolumePrevious(FramePassContext& fc, const ViewResources& main)
{
    LumenTvPrevious out;
    const Settings s = settingsOf(fc.quality);
    if (!s.enabled || !fc.trackState || main.view.kind != gpu::ViewKind::Main) return out;
    TvState& st = fc.state<TvState>("R.lumenTranslucencyVolume");
    // the last frame's volume: published then, the same grid and scene, no cut now
    if (!st.created || !st.history || st.publishedFrame + 1 != fc.frame.frameIndex || fc.frame.discontinuity != 0 || fc.scene.revision() != st.revision) return out;
    uint32_t x, y, z;
    gridOf(main, s, x, y, z);
    if (x != st.gridX || y != st.gridY || z != st.gridZ) return out;
    if (!(st.previousImportFrame == RecordKey::of(fc)))
    {
        const TextureDesc d{ "R translucency GI ambient (previous)", st.gridX, st.gridY, (uint16_t)st.gridZ, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D };
        TextureDesc dd = d;
        dd.name = "R translucency GI directional (previous)";
        st.previousAmbient = fc.graph.importTexture(st.ambient[st.parity].Get(), d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        st.previousDirectional = fc.graph.importTexture(st.directional[st.parity].Get(), dd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        st.previousImportFrame = RecordKey::of(fc);
    }
    out.params = st.ringSrv[st.publishedSlot];
    out.ambient = st.previousAmbient;
    out.directional = st.previousDirectional;
    return out;
}

void lumenTranslucencyVolumeMark(FramePassContext& fc, const ViewResources& main, const LumenRcFrame& rc)
{
    const Settings s = settingsOf(fc.quality);
    if (!s.enabled || !rc.on || !usable(fc, main)) return;
    TvState& st = stateOf(fc, main, s);
    if (st.markedFrame == RecordKey::of(fc)) return;
    st.markedFrame = RecordKey::of(fc);
    const TextureRef indirection = rc.indirection, hiz = main.hiz;
    const uint32_t rcParams = rc.params, gridX = st.gridX, gridY = st.gridY, gridZ = st.gridZ, frame = st.frame, bias = s.clipmapBias;
    const uint32_t gridZWord = gridZ | pixelShiftOf(main, s) << 16;  // P[4].z (LumenTranslucencyVolumeGrid.hlsli)
    const std::array<float, 3> jitter = { st.jitter[0], st.jitter[1], st.jitter[2] };
    const D3D12_GPU_VIRTUAL_ADDRESS cb = main.frameConstants;
    ShaderLibrary& shaders = fc.shaders;
    fc.graph.addPass("r.gi.ltv.mark", QueueType::Compute,
                     [&](PassBuilder& b) {
                         b.use(indirection, Use::UavCompute);
                         b.use(hiz, Use::SrvCompute);
                     },
                     [=, &shaders](PassContext& c) {
                         uint32_t k[36] = {};
                         k[0] = c.uav(indirection), k[1] = rcParams, k[2] = c.srv(hiz), k[3] = bias;
                         k[16] = gridX, k[17] = gridY, k[18] = gridZWord;
                         k[32] = bits(jitter[0]), k[33] = bits(jitter[1]), k[34] = bits(jitter[2]), k[35] = frame;
                         c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenTranslucencyVolumeMark"));
                         c.bindFrameConstants(cb);
                         c.computeConstants(k, 36);
                         c.cmd->Dispatch((gridX + 3) / 4, (gridY + 3) / 4, (gridZ + 3) / 4);
                     });
}

void lumenTranslucencyVolume(FramePassContext& fc, const ViewResources& main, rt::RayScene& rays, const LumenTvInputs& inputs, const LumenRcFrame& rc)
{
    const Settings s = settingsOf(fc.quality);
    if (!s.enabled || !usable(fc, main)) return;
    TvState& st = stateOf(fc, main, s);
    if (st.recordedFrame == RecordKey::of(fc)) return;
    st.recordedFrame = RecordKey::of(fc);
    RenderGraph& g = fc.graph;
    ShaderLibrary& shaders = fc.shaders;
    const uint32_t gridX = st.gridX, gridY = st.gridY, gridZ = st.gridZ, frame = st.frame;
    const uint32_t pixelSize = 1u << pixelShiftOf(main, s), gridZWord = gridZ | pixelShiftOf(main, s) << 16;  // P[4].z
    // history: none on the first frame, after another grid, a scene revision, a cut or a restore
    if (fc.scene.revision() != st.revision || fc.frame.discontinuity != 0) st.history = false;
    st.revision = fc.scene.revision();
    const bool noHistory = !st.history || !s.temporal;
    st.history = true;
    const uint32_t previous = st.parity, current = st.parity ^ 1u;
    st.parity = current;

    const TextureDesc volumeDesc{ "R translucency GI", gridX, gridY, (uint16_t)gridZ, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D };
    auto import = [&](ComPtr<ID3D12Resource>& r, const char* name) {
        TextureDesc d = volumeDesc;
        d.name = name;
        return g.importTexture(r.Get(), d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    };
    // (lumenTranslucencyVolumePrevious may have imported the last volume into this frame's graph already)
    const bool imported = st.previousImportFrame == RecordKey::of(fc) && st.previousAmbient.valid();
    const TextureRef prevAmbient = imported ? st.previousAmbient : import(st.ambient[previous], "R translucency GI ambient (previous)");
    const TextureRef prevDirectional = imported ? st.previousDirectional : import(st.directional[previous], "R translucency GI directional (previous)");
    const TextureRef ambient = import(st.ambient[current], "R translucency GI ambient");
    const TextureRef directional = import(st.directional[current], "R translucency GI directional");
    const TextureDesc traceDesc{ "r.gi.ltv trace", gridX * kTraceRes, gridY * kTraceRes, (uint16_t)gridZ, 1, DXGI_FORMAT_R11G11B10_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D };
    TextureRef trace = g.createTexture(traceDesc);
    TextureDesc pingDesc = traceDesc;
    pingDesc.name = "r.gi.ltv trace filtered";
    const TextureRef ping = g.createTexture(pingDesc);

    // the frame's parameters (readers: ltvParams)
    Params params{};
    const float4x4& vp = main.view.viewProj;
    std::memcpy(params.worldToClip, vp.m, sizeof params.worldToClip);
    params.gridX = gridX, params.gridY = gridY, params.gridZ = gridZ, params.frame = frame;
    params.ambientSrv = st.ambientSrv[current];
    params.directionalSrv = st.directionalSrv[current];
    params.uvScale[0] = (float)main.view.width / (float)(gridX * pixelSize);
    params.uvScale[1] = (float)main.view.height / (float)(gridY * pixelSize);
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % kRing);
    std::memcpy(st.ringMapped + (size_t)slot * kParamBytes, &params, sizeof params);

    uint32_t sceneWords[8];
    rays.recordHair(fc);  // E's grooms on the rays (HitHair.hlsli): the header's words 22, 23, before rootConstants
    rays.rootConstants(sceneWords);
    const std::array<uint32_t, 8> sceneSrvs = std::to_array(sceneWords);
    const FrameResources& fr = fc.resources;
    const bool atmosphere = fr.transmittanceLut.valid() && fr.multiScatterLut.valid() && fr.skyViewLut.valid() && fr.aerialPerspective.valid();
    const std::array<TextureRef, 4> luts = { fr.transmittanceLut, fr.multiScatterLut, fr.skyViewLut, fr.aerialPerspective };
    rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(kTraceLibrary[atmosphere ? 0 : 1], { "LumenTranslucencyVolumeTraceGen" }));
    const TextureRef depth = main.depth, hiz = main.hiz;
    const SurfaceCacheCardRefs cards = inputs.cards;
    const bool cache = rc.on && rc.updated;
    const TextureRef rcIndirection = rc.indirection, rcAtlas = rc.atlas, rcDepth = rc.depth;
    const uint32_t rcParams = rc.params;
    const float3 sky = inputs.skyRadiance, sun = inputs.sunIlluminance;
    const std::array<float, 3> jitter = { st.jitter[0], st.jitter[1], st.jitter[2] };
    const D3D12_GPU_VIRTUAL_ADDRESS cb = main.frameConstants;

    // Compact at the cell boundary. HiZ visibility and the depth-constrained
    // origin are identical for all nine rays; compute them once and retain the
    // original cell identity for ray seeds and atlas addressing.
    const BufferRef cells = g.createBuffer({ "r.gi.ltv visible cells", 16 + (uint64_t)gridX * gridY * gridZ * 16, 0 });
    g.addPass("r.gi.ltv.compact.begin", QueueType::Compute,
              [&](PassBuilder& b) { b.use(cells, Use::UavCompute); },
              [=, &shaders](PassContext& c) {
                  const uint32_t k[4] = { c.uav(cells), 0, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenTranslucencyVolumeCompact.MODE0"));
                  c.computeConstants(k, 4); c.cmd->Dispatch(1, 1, 1);
              });
    g.addPass("r.gi.ltv.compact", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cells, Use::UavCompute); b.use(depth, Use::SrvCompute); b.use(hiz, Use::SrvCompute); b.use(trace, Use::UavCompute);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[40] = {};
                  k[0] = c.uav(cells), k[1] = c.srv(depth), k[2] = c.srv(hiz), k[3] = c.uav(trace);
                  k[16] = gridX, k[17] = gridY, k[18] = gridZWord;
                  k[32] = bits(jitter[0]), k[33] = bits(jitter[1]), k[34] = bits(jitter[2]), k[35] = frame;
                  k[38] = bits(s.depthThreshold);
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenTranslucencyVolumeCompact.MODE1"));
                  c.bindFrameConstants(cb); c.computeConstants(k, 40);
                  c.cmd->Dispatch((gridX + 3) / 4, (gridY + 3) / 4, (gridZ + 3) / 4);
              });
    const auto traceBatch = pipeline.prepareBatch(g, shaders, "r.gi.ltv.compact.args", cells,
        gridX * gridY * gridZ, kMaxRaysPerDispatch, 19, kTraceRes * kTraceRes);
    const BufferRef traceArgs = traceBatch.arguments;
    g.addPass("r.gi.ltv.trace", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cells, Use::SrvGraphics);
                  b.use(traceArgs, Use::IndirectArgs);
                  b.use(trace, Use::UavGraphics);
                  declareSurfaceCacheCards(b, cards, Use::SrvGraphics);
                  if (cache)
                  {
                      b.use(rcIndirection, Use::SrvGraphics);
                      b.use(rcAtlas, Use::SrvGraphics);
                      b.use(rcDepth, Use::SrvGraphics);
                  }
                  rays.declareTraversal(b);
                  rays.declareHair(b);
                  if (atmosphere)
                      for (const TextureRef& t : luts) b.use(t, Use::SrvGraphics);
              },
              [=, &pipeline](PassContext& c) {
                  uint32_t k[44] = {};
                  k[0] = c.uav(trace), k[3] = s.clipmapBias;
                  k[4] = bits(sky.x), k[5] = bits(sky.y), k[6] = bits(sky.z), k[7] = bits(s.traceDistance);
                  for (int i = 0; i < 4; ++i) k[8 + i] = atmosphere ? c.srv(luts[i]) : 0xFFFFFFFFu;
                  k[12] = bits(sun.x), k[13] = bits(sun.y), k[14] = bits(sun.z);
                  k[16] = gridX, k[17] = gridY, k[18] = gridZWord;
                  k[20] = cards.valid() ? c.srv(cards.frame) : 0xFFFFFFFFu;
                  k[21] = cache ? rcParams : 0xFFFFFFFFu;
                  k[22] = cache ? c.srv(rcIndirection) : 0xFFFFFFFFu;
                  k[23] = cache ? c.srv(rcAtlas) : 0xFFFFFFFFu;
                  std::memcpy(&k[24], sceneSrvs.data(), 32);
                  k[32] = bits(jitter[0]), k[33] = bits(jitter[1]), k[34] = bits(jitter[2]), k[35] = frame;
                  k[36] = bits(s.maxRayIntensity);
                  k[37] = cache ? c.srv(rcDepth) : 0xFFFFFFFFu;
                  k[38] = bits(s.depthThreshold);
                  k[39] = bits(s.farStart);
                  k[40] = c.srv(cells);
                  c.bindFrameConstants(cb);
                  // The same upper bound per dispatch, including partial cells
                  // at a chunk boundary; ltvCompactRay restores sample identity.
                  c.computeConstants(k, 44);
                  pipeline.dispatchBatch(c, traceBatch);
              });
    if (s.filter && s.filterSamples > 0)
    {
        static const char* const kNames[3] = { "r.gi.ltv.filter.x", "r.gi.ltv.filter.y", "r.gi.ltv.filter.z" };
        TextureRef from = trace, to = ping;
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            g.addPass(kNames[axis], QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(from, Use::SrvCompute);
                          b.use(hiz, Use::SrvCompute);
                          b.use(to, Use::UavCompute);
                      },
                      [=, &shaders](PassContext& c) {
                          uint32_t k[40] = {};
                          k[0] = c.srv(from), k[1] = c.uav(to), k[2] = axis, k[3] = s.filterSamples;
                          k[4] = c.srv(hiz);
                          k[16] = gridX, k[17] = gridY, k[18] = gridZWord;
                          k[37] = bits(s.filterDeviation);
                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenTranslucencyVolumeFilter"));
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 40);
                          c.cmd->Dispatch((gridX * kTraceRes + 7) / 8, (gridY * kTraceRes + 7) / 8, gridZ);
                      });
            std::swap(from, to);
        }
        trace = from;  // (the last pass's output)
    }
    const TextureRef filtered = trace;
    g.addPass("r.gi.ltv.integrate", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(filtered, Use::SrvCompute);
                  b.use(hiz, Use::SrvCompute);
                  b.use(prevAmbient, Use::SrvCompute);
                  b.use(prevDirectional, Use::SrvCompute);
                  b.use(ambient, Use::UavCompute);
                  b.use(directional, Use::UavCompute);
                  b.keep();
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[44] = {};
                  k[0] = c.srv(filtered), k[1] = c.srv(hiz), k[2] = c.srv(prevAmbient), k[3] = c.srv(prevDirectional);
                  k[4] = c.uav(ambient), k[5] = c.uav(directional), k[6] = noHistory ? 1u : 0u;
                  k[16] = gridX, k[17] = gridY, k[18] = gridZWord;
                  k[32] = bits(jitter[0]), k[33] = bits(jitter[1]), k[34] = bits(jitter[2]), k[35] = frame;
                  k[38] = bits(s.historyWeight);
                  k[40] = bits(params.uvScale[0]), k[41] = bits(params.uvScale[1]);
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/LumenTranslucencyVolumeIntegrate"));
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 44);
                  c.cmd->Dispatch((gridX + 3) / 4, (gridY + 3) / 4, (gridZ + 3) / 4);
              });
    fc.resources.translucencyGiAmbient = ambient;
    fc.resources.translucencyGiDirectional = directional;
    fc.resources.translucencyGiParams = st.ringSrv[slot];
    st.publishedFrame = fc.frame.frameIndex;
    st.publishedSlot = slot;
}
} // namespace unx::render::gi
