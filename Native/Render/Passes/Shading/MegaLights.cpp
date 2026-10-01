// Stochastic direct light of the local lights (shading.mega_lights; MegaLights.hlsli states the passes and what follows
// Unreal Engine's MegaLights; owner A). This file records every pass but m.ml.shade, which the shading record dispatches
// per shade class (ShadingSystem.cpp: it is ShadeOpaque.hlsl compiled with MEGA_LIGHTS = 1 on the class tile lists).
// Persistent state per frame renderer: the filtered diffuse and specular, their luminance moments, frame counts and view
// depth (ping-pong), and the tile sets of visible / hidden lights. The history is dropped on a new scene revision, a
// history discontinuity, an origin shift or a size change.
#include "unx/shading/MegaLights.h"

#include "unx/core/Config.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#if UNX_M_HAS_RAYTRACING
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace unx::render::shading
{
namespace
{
uint32_t asUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

constexpr uint32_t kSetTile = 8, kSetBytes = 24;  // MegaLights.hlsli ML_HASH_TILE, 4 x ML_HASH_WORDS

struct PlanarOrdinal  // the planar reflection views of a frame, in the order they are shaded
{
    uint64_t frame = UINT64_MAX;
    uint32_t next = 0;
};

struct MegaLightsState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> diffuse[2], specular[2], moments[2], frames[2], depth[2], sets;
    uint32_t width = 0, height = 0, parity = 0;
    uint64_t setsBytes = 0;
    bool fresh = true;  // the textures hold nothing yet
    uint32_t revision = 0xFFFFFFFFu;
    float exposure = 0;
    ~MegaLightsState() { release(); }
    void release()
    {
        if (!device) return;
        for (int k = 0; k < 2; ++k)
            for (ComPtr<ID3D12Resource>* t : { std::addressof(diffuse[k]), std::addressof(specular[k]), std::addressof(moments[k]), std::addressof(frames[k]),
                                               std::addressof(depth[k]) })
                if (*t) device->deferRelease(*t);
        if (sets) device->deferRelease(sets);
    }
    void ensure(Device& d, uint32_t w, uint32_t h)
    {
        if (diffuse[0] && width == w && height == h) return;
        release();
        device = &d;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        auto texture = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT format, const wchar_t* name) {
            D3D12_RESOURCE_DESC1 desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = w;
            desc.Height = h;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            out.Reset();
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(&out)),
                  "M mega lights history");
            out->SetName(name);
        };
        for (int k = 0; k < 2; ++k)
        {
            texture(diffuse[k], DXGI_FORMAT_R16G16B16A16_FLOAT, k ? L"M ml diffuse 1" : L"M ml diffuse 0");
            texture(specular[k], DXGI_FORMAT_R16G16B16A16_FLOAT, k ? L"M ml specular 1" : L"M ml specular 0");
            texture(moments[k], DXGI_FORMAT_R16G16B16A16_FLOAT, k ? L"M ml moments 1" : L"M ml moments 0");
            texture(frames[k], DXGI_FORMAT_R8_UINT, k ? L"M ml frames 1" : L"M ml frames 0");
            texture(depth[k], DXGI_FORMAT_R32_FLOAT, k ? L"M ml depth 1" : L"M ml depth 0");
        }
        setsBytes = (uint64_t)((w + kSetTile - 1) / kSetTile) * ((h + kSetTile - 1) / kSetTile) * kSetBytes;
        D3D12_RESOURCE_DESC1 bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = setsBytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        sets.Reset();
        check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&sets)),
              "M mega lights tile sets");
        sets->SetName(L"M ml tile sets");
        width = w;
        height = h;
        fresh = true;
    }
};

struct Settings
{
    uint32_t factor, count;
    bool guide, merge, temporal, spatial, historyVariance;
    float minSampleWeight, hiddenWeight, hiddenWeightMiss, maxWeight, maxWeightHidden;
    float rayBias, rayNormalBias, rayEndBias;
    float maxFrames, minFramesMiss, distanceThreshold, clampScale;
    float radius, depthWeight, maxDisocclusionFrames, disocclusionDiffuse, disocclusionSpecular, historyStdDev;
    uint32_t spatialSamples;
};

Settings settings(const QualityConfig& q)
{
    Settings s{};
    s.factor = (uint32_t)q.integer("shading.mega_lights_downsample");
    s.count = (uint32_t)q.integer("shading.mega_lights_samples");
    if (s.factor != 1 && s.factor != 2) fail("shading.mega_lights_downsample must be 1 or 2");
    if (s.count != 1 && s.count != 2 && s.count != 4) fail("shading.mega_lights_samples must be 1, 2 or 4");
    s.guide = q.boolean("shading.mega_lights_guide_by_history");
    s.merge = q.boolean("shading.mega_lights_merge_rays");
    s.temporal = q.boolean("shading.mega_lights_temporal");
    s.spatial = q.boolean("shading.mega_lights_spatial");
    s.historyVariance = q.boolean("shading.mega_lights_spatial_history_variance");
    s.minSampleWeight = (float)q.number("shading.mega_lights_min_sample_weight");
    s.hiddenWeight = (float)q.number("shading.mega_lights_hidden_weight");
    s.hiddenWeightMiss = (float)q.number("shading.mega_lights_hidden_weight_history_miss");
    s.maxWeight = (float)q.number("shading.mega_lights_max_shading_weight");
    s.maxWeightHidden = (float)q.number("shading.mega_lights_max_shading_weight_hidden");
    s.rayBias = (float)q.number("shading.mega_lights_ray_bias_m");
    s.rayNormalBias = (float)q.number("shading.mega_lights_ray_normal_bias_m");
    s.rayEndBias = (float)q.number("shading.mega_lights_ray_end_bias_m");
    s.maxFrames = (float)q.number("shading.mega_lights_temporal_max_frames");
    s.minFramesMiss = (float)q.number("shading.mega_lights_temporal_min_frames_history_miss");
    s.distanceThreshold = (float)q.number("shading.mega_lights_temporal_distance_threshold");
    s.clampScale = (float)q.number("shading.mega_lights_temporal_clamp_scale");
    s.radius = (float)q.number("shading.mega_lights_spatial_radius_px");
    s.spatialSamples = (uint32_t)q.integer("shading.mega_lights_spatial_samples");
    s.depthWeight = (float)q.number("shading.mega_lights_spatial_depth_weight");
    s.maxDisocclusionFrames = (float)q.number("shading.mega_lights_spatial_max_disocclusion_frames");
    s.disocclusionDiffuse = (float)q.number("shading.mega_lights_spatial_disocclusion_scale_diffuse");
    s.disocclusionSpecular = (float)q.number("shading.mega_lights_spatial_disocclusion_scale_specular");
    s.historyStdDev = (float)q.number("shading.mega_lights_spatial_history_stddev");
    if (!(s.minSampleWeight > 0) || !(s.maxWeight > 0) || !(s.maxWeightHidden > 0)) fail("shading.mega_lights: the sample weight limits must be positive");
    if (!(s.maxFrames >= 1 && s.maxFrames <= 31) || !(s.minFramesMiss >= 1)) fail("shading.mega_lights_temporal_max_frames must be in [1, 31], min frames >= 1");
    if (s.spatialSamples > 64) fail("shading.mega_lights_spatial_samples must be <= 64");
    return s;
}
} // namespace

MegaLightsFrame megaLightsSample(FramePassContext& fc, const ViewResources& view, TextureRef materialWord, bool areaLights, uint32_t ltcSrv,
                                 ID3D12CommandSignature* dispatchSignature, const char* instance)
{
    MegaLightsFrame ml;
#if UNX_M_HAS_RAYTRACING
    if (!fc.quality.boolean("shading.mega_lights") || !fc.trackState) return ml;
    const FrameResources r = fc.resources;
    // needs S's froxel lists (the candidates) and R's ray scene (the visibility); without either the shading kernels keep
    // their loop over the lists with S's slots
    if (!view.froxelLights.valid() || !r.tlasStatic.valid() || !view.depth.valid() || !view.gbuffer.valid() || !materialWord.valid()) return ml;
    const Settings s = settings(fc.quality);
    RenderGraph& g = fc.graph;
    const uint32_t W = view.view.width, H = view.view.height;
    const uint32_t dsW = (W + s.factor - 1) / s.factor, dsH = (H + s.factor - 1) / s.factor;
    const uint32_t gridX = s.count >= 2 ? 2u : 1u, gridY = s.count >= 4 ? 2u : 1u;
    const uint32_t tilesX = (W + kSetTile - 1) / kSetTile, tilesY = (H + kSetTile - 1) / kSetTile;

    // The view's persistent state (as Unreal runs MegaLights per view): the main view and every full auxiliary view (A14:
    // render texture, mirror, portal, split - ViewResources::viewId) keep their own history. A planar reflection view has
    // no identity between frames (R renders them through FrameServices::renderView): the frame's n-th such view takes
    // the n-th planar state and never reads history (no temporal accumulation, no guiding by last frame's sets).
    const bool mainView = view.view.kind == gpu::ViewKind::Main, fullView = mainView || view.viewId != 0;
    if (mainView) ml.stateKey = "M.megaLights";
    else if (fullView) ml.stateKey = "M.megaLights.view" + std::to_string(view.viewId);
    else
    {
        PlanarOrdinal& ordinal = fc.state<PlanarOrdinal>("M.megaLights.planarOrdinal");
        if (ordinal.frame != fc.frame.frameIndex) ordinal = { fc.frame.frameIndex, 0 };
        ml.stateKey = "M.megaLights.planar" + std::to_string(ordinal.next++);
    }
    if (instance) ml.stateKey += std::string(".") + instance;  // (a second instance of the view: the coverage layer's)
    MegaLightsState& st = fc.state<MegaLightsState>(ml.stateKey);
    st.ensure(fc.device, W, H);
    const float3 shift = fc.frame.originShift;
    const bool valid = fullView && !st.fresh && st.revision == fc.scene.revision() && fc.frame.discontinuity == 0 && shift.x == 0 && shift.y == 0 && shift.z == 0;
    st.fresh = false;
    st.revision = fc.scene.revision();
    const float exposure = 1.0f / (1.2f * std::exp2(view.view.ev100));
    ml.exposureRatio = valid && st.exposure > 0 ? exposure / st.exposure : 1.0f;
    st.exposure = exposure;
    ml.previous = st.parity;
    ml.next = st.parity ^ 1u;
    st.parity = ml.next;
    ml.historyValid = valid && s.temporal;
    ml.on = true;
    ml.factor = s.factor;
    ml.count = s.count;
    ml.maxWeight = s.maxWeight;
    ml.maxWeightHidden = s.maxWeightHidden;
    ml.minSampleWeight = s.minSampleWeight;
    ml.samples = g.createTexture({ "m.ml samples", dsW * gridX, dsH * gridY, 1, 1, DXGI_FORMAT_R32G32_UINT });
    ml.keys = g.createTexture({ "m.ml keys", dsW, dsH, 1, 1, DXGI_FORMAT_R32G32_UINT });
    ml.resolvedDiffuse = g.createTexture({ "m.ml resolved diffuse", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    ml.resolvedSpecular = g.createTexture({ "m.ml resolved specular", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });

    const TextureRef samples = ml.samples, keys = ml.keys, depth = view.depth, gbuffer = view.gbuffer, visId = view.visId;
    const BufferRef froxelLights = view.froxelLights, fxLights = r.fxLights, clusters = view.visibleClusters;
    const bool hasVis = visId.valid() && clusters.valid();
    // the previous frame's sets and view depth guide the sampling (valid history only: else every light counts as visible)
    const bool guide = s.guide && valid;
    // (one import per resource and frame: the sets are read here and rewritten by m.ml.sets.filter; the previous depth is
    // read here and by m.ml.temporal)
    ml.sets = g.importBuffer(st.sets.Get(), BufferDesc{ "m.ml tile sets", st.setsBytes, 0 });
    if (valid)
        ml.prevDepth = g.importTexture(st.depth[ml.previous].Get(), { "m.ml depth (previous)", W, H, 1, 1, DXGI_FORMAT_R32_FLOAT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const BufferRef sets = ml.sets;
    const TextureRef prevDepth = ml.prevDepth;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    // (B2's mask of lights whose specular the reflections carry: the main view's; other views shade every light's specular)
    const uint32_t stable = mainView ? r.areaLightStable : gpu::kNone;
    // m.ml.tiles: the downsampled tiles with a surface (the sample kernel's dispatch list); the others' samples are emptied
    const uint32_t dsTilesX = (dsW + 7) / 8, dsTilesY = (dsH + 7) / 8;
    const BufferRef tileList = g.createBuffer({ "m.ml tiles", 16 + 4ull * dsTilesX * dsTilesY, 0 });
    ID3D12PipelineState* tilesBegin = fc.shaders.compute("Passes/Shading/MegaLightsTiles.MODE0");
    ID3D12PipelineState* tilesPso = fc.shaders.compute("Passes/Shading/MegaLightsTiles.MODE1");
    g.addPass("m.ml.tiles", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(materialWord, Use::SrvCompute);
                  b.use(tileList, Use::UavCompute);
                  b.use(samples, Use::UavCompute);
                  b.use(keys, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.srv(materialWord), c.uav(tileList), c.uav(samples), c.uav(keys), dsW, dsH, s.factor | (s.count << 8), 0 };
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 8);
                  c.cmd->SetPipelineState(tilesBegin);
                  c.cmd->Dispatch(1, 1, 1);
                  D3D12_GLOBAL_BARRIER gb{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                           D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
                  D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
                  group.pGlobalBarriers = &gb;
                  c.cmd->Barrier(1, &group);
                  c.cmd->SetPipelineState(tilesPso);
                  c.cmd->Dispatch(dsTilesX, dsTilesY, 1);
              });
    ID3D12PipelineState* samplePso = fc.shaders.compute(areaLights ? "Passes/Shading/MegaLightsSample.AREA1" : "Passes/Shading/MegaLightsSample.AREA0");
    g.addPass("m.ml.sample", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(tileList, Use::SrvCompute);
                  b.use(tileList, Use::IndirectArgs);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(materialWord, Use::SrvCompute);
                  b.use(froxelLights, Use::SrvCompute);
                  if (fxLights.valid()) b.use(fxLights, Use::SrvCompute);
                  if (hasVis)
                  {
                      b.use(visId, Use::SrvCompute);
                      b.use(clusters, Use::SrvCompute);
                  }
                  if (guide)
                  {
                      b.use(sets, Use::SrvCompute);
                      b.use(prevDepth, Use::SrvCompute);
                  }
                  b.use(samples, Use::UavCompute);
                  b.use(keys, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t none = gpu::kNone;
                  const uint32_t k[24] = { c.srv(gbuffer), c.srv(depth), c.srv(materialWord), c.srv(froxelLights),
                                           c.uav(samples), c.uav(keys), guide ? c.srv(sets) : none, guide ? c.srv(prevDepth) : none,
                                           dsW, dsH, s.factor | (s.count << 8) | ((guide ? 1u : 0u) | (s.merge ? 2u : 0u)) << 16, ltcSrv,
                                           asUint(s.minSampleWeight), asUint(s.hiddenWeight), asUint(s.hiddenWeightMiss), asUint(s.distanceThreshold),
                                           hasVis ? c.srv(visId) : none, hasVis ? c.srv(clusters) : none, tilesX, tilesY,
                                           stable, c.srv(tileList), 0, 0 };
                  c.cmd->SetPipelineState(samplePso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 24);
                  c.cmd->ExecuteIndirect(dispatchSignature, 1, c.resource(tileList), 0, nullptr, 0);
              });

    rt::RayScene* rays = &rt::RayScene::get(fc);
    rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, fc.shaders, rt::standardRayPipeline("Passes/Shading/MegaLightsTrace", { "MegaLightsTraceGen" }));
    g.addPass("m.ml.trace", QueueType::Compute,
              [&](PassBuilder& b) {
                  rays->declareTraversal(b);
                  b.use(keys, Use::SrvCompute);
                  b.use(samples, Use::UavCompute);
              },
              [=, &pipeline](PassContext& c) {
                  uint32_t k[32] = { c.uav(samples), c.srv(keys), 0, 0, dsW, dsH, s.factor | (s.count << 8), 0,
                                     asUint(s.rayBias), asUint(s.rayNormalBias), asUint(s.rayEndBias), 0 };
                  rays->rootConstants(k + 24);
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(cb);
                  pipeline.dispatch(c.cmd, 0, dsW * gridX, dsH * gridY);
              });
#else
    (void)fc;
    (void)view;
    (void)materialWord;
    (void)areaLights;
    (void)ltcSrv;
    (void)dispatchSignature;
#endif
    return ml;
}

void megaLightsDenoise(FramePassContext& fc, const ViewResources& view, TextureRef materialWord, MegaLightsFrame& ml, bool demodulated)
{
    if (!ml.on) return;
    const Settings s = settings(fc.quality);
    RenderGraph& g = fc.graph;
    MegaLightsState& st = fc.state<MegaLightsState>(ml.stateKey);
    const uint32_t W = view.view.width, H = view.view.height;
    const uint32_t dsW = (W + s.factor - 1) / s.factor, dsH = (H + s.factor - 1) / s.factor;
    const uint32_t gridX = s.count >= 2 ? 2u : 1u, gridY = s.count >= 4 ? 2u : 1u;
    const uint32_t tilesX = (W + kSetTile - 1) / kSetTile, tilesY = (H + kSetTile - 1) / kSetTile;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const TextureRef samples = ml.samples, depth = view.depth, gbuffer = view.gbuffer, visId = view.visId;
    const BufferRef clusters = view.visibleClusters;
    const bool hasVis = visId.valid() && clusters.valid();

    // ---- the tile sets for the next frame's sampling
    const BufferRef built = g.createBuffer({ "m.ml tile sets (built)", st.setsBytes, 0 });
    const BufferRef history = ml.sets;
    ID3D12PipelineState* buildPso = fc.shaders.compute("Passes/Shading/MegaLightsSets.MODE0");
    ID3D12PipelineState* filterPso = fc.shaders.compute("Passes/Shading/MegaLightsSets.MODE1");
    g.addPass("m.ml.sets", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(samples, Use::SrvCompute);
                  b.use(built, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.srv(samples), c.uav(built), tilesX, tilesY, s.factor | (s.count << 8), dsW * gridX, dsH * gridY, 0 };
                  c.cmd->SetPipelineState(buildPso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((tilesX + 7) / 8, (tilesY + 7) / 8, 1);
              });
    g.addPass("m.ml.sets.filter", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(built, Use::SrvCompute);
                  b.use(history, Use::UavCompute);
                  b.keep();  // persistent state: the next frame's m.ml.sample reads it
              },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.srv(built), c.uav(history), tilesX, tilesY };
                  c.cmd->SetPipelineState(filterPso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch((tilesX + 7) / 8, (tilesY + 7) / 8, 1);
              });

    // ---- temporal, then spatial
    auto import = [&](ComPtr<ID3D12Resource>& t, const char* name, DXGI_FORMAT format) {
        return g.importTexture(t.Get(), { name, W, H, 1, 1, format }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    };
    const uint32_t p = ml.previous, n = ml.next;
    const bool hist = ml.historyValid;
    const TextureRef prevDiffuse = hist ? import(st.diffuse[p], "m.ml diffuse (previous)", DXGI_FORMAT_R16G16B16A16_FLOAT) : TextureRef{};
    const TextureRef prevSpecular = hist ? import(st.specular[p], "m.ml specular (previous)", DXGI_FORMAT_R16G16B16A16_FLOAT) : TextureRef{};
    const TextureRef prevMoments = hist ? import(st.moments[p], "m.ml moments (previous)", DXGI_FORMAT_R16G16B16A16_FLOAT) : TextureRef{};
    const TextureRef prevFrames = hist ? import(st.frames[p], "m.ml frames (previous)", DXGI_FORMAT_R8_UINT) : TextureRef{};
    const TextureRef prevDepth = ml.prevDepth;  // (imported by megaLightsSample; valid whenever hist is)
    const TextureRef outDiffuse = import(st.diffuse[n], "m.ml diffuse", DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef outSpecular = import(st.specular[n], "m.ml specular", DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef outMoments = import(st.moments[n], "m.ml moments", DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef outFrames = import(st.frames[n], "m.ml frames", DXGI_FORMAT_R8_UINT);
    const TextureRef outDepth = import(st.depth[n], "m.ml depth", DXGI_FORMAT_R32_FLOAT);
    const TextureRef confidence = g.createTexture({ "m.ml history confidence", W, H, 1, 1, DXGI_FORMAT_R8G8_UNORM });
    const TextureRef resolvedDiffuse = ml.resolvedDiffuse, resolvedSpecular = ml.resolvedSpecular;
    const float ratio = ml.exposureRatio;
    ID3D12PipelineState* temporalPso = fc.shaders.compute("Passes/Shading/MegaLightsTemporal");
    g.addPass("m.ml.temporal", QueueType::Compute,
              [&](PassBuilder& b) {
                  for (TextureRef t : { resolvedDiffuse, resolvedSpecular, depth, gbuffer, materialWord }) b.use(t, Use::SrvCompute);
                  if (hist)
                      for (TextureRef t : { prevDiffuse, prevSpecular, prevMoments, prevFrames, prevDepth }) b.use(t, Use::SrvCompute);
                  if (hasVis)
                  {
                      b.use(visId, Use::SrvCompute);
                      b.use(clusters, Use::SrvCompute);
                  }
                  for (TextureRef t : { outDiffuse, outSpecular, outMoments, outFrames, outDepth, confidence }) b.use(t, Use::UavCompute);
                  b.keep();  // persistent state: the next frame's history
              },
              [=](PassContext& c) {
                  const uint32_t none = gpu::kNone;
                  const uint32_t k[28] = { c.srv(resolvedDiffuse), c.srv(resolvedSpecular), c.srv(depth), c.srv(gbuffer),
                                           W, H, hist ? 1u : 0u, asUint(ratio),
                                           hist ? c.srv(prevDiffuse) : none, hist ? c.srv(prevSpecular) : none, hist ? c.srv(prevMoments) : none, hist ? c.srv(prevFrames) : none,
                                           hist ? c.srv(prevDepth) : none, hasVis ? c.srv(visId) : none, hasVis ? c.srv(clusters) : none, c.srv(materialWord),
                                           c.uav(outDiffuse), c.uav(outSpecular), c.uav(outMoments), c.uav(outFrames),
                                           c.uav(outDepth), c.uav(confidence), 0, 0,
                                           asUint(s.temporal ? s.maxFrames : 1.0f), asUint(s.minFramesMiss), asUint(s.distanceThreshold), asUint(s.clampScale) };
                  c.cmd->SetPipelineState(temporalPso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 28);
                  c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
              });
    ml.lighting = g.createTexture({ "m.ml lighting", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    if (demodulated) ml.lightingSpecular = g.createTexture({ "m.ml lighting specular", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef lighting = ml.lighting, lightingSpecular = ml.lightingSpecular;
    ID3D12PipelineState* spatialPso = fc.shaders.compute("Passes/Shading/MegaLightsSpatial");
    g.addPass("m.ml.spatial", QueueType::Compute,
              [&](PassBuilder& b) {
                  for (TextureRef t : { outDiffuse, outSpecular, outMoments, outFrames, confidence, depth, gbuffer, materialWord }) b.use(t, Use::SrvCompute);
                  b.use(lighting, Use::UavCompute);
                  if (demodulated) b.use(lightingSpecular, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[24] = { c.srv(outDiffuse), c.srv(outSpecular), c.srv(outMoments), c.srv(outFrames),
                                           c.srv(confidence), c.srv(depth), c.srv(gbuffer), c.srv(materialWord),
                                           c.uav(lighting), W, H, (s.spatial ? 1u : 0u) | (s.historyVariance ? 2u : 0u) | (demodulated ? 4u : 0u),
                                           asUint(s.radius), s.spatialSamples, asUint(s.depthWeight), asUint(s.maxDisocclusionFrames),
                                           asUint(s.disocclusionDiffuse), asUint(s.disocclusionSpecular), asUint(s.historyStdDev), asUint(s.temporal ? s.maxFrames : 1.0f),
                                           demodulated ? c.uav(lightingSpecular) : gpu::kNone, 0, 0, 0 };
                  c.cmd->SetPipelineState(spatialPso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 24);
                  c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
              });
}
} // namespace unx::render::shading
