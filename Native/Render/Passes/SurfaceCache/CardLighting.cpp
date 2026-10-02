// surface_cache.mesh_cards (unx/refl/CardLighting.h; Passes/SurfaceCache/CardLighting.hlsli): the card lighting's
// persistent atlases and the passes of one update.
#include "unx/refl/CardLighting.h"

#include "unx/core/Config.h"
#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <cstring>

namespace unx::render::refl
{
namespace
{
// CardLighting.hlsli
constexpr uint32_t kTile = 8, kProbeSpacing = 4, kBuckets = 16;
constexpr uint32_t kPageLightBytes = 16, kUniformBytes = 32, kTileLightBytes = 64, kTileShadowBytes = 72, kTraceThreads = 576;
constexpr uint32_t kSelectHead = 64, kPageTiles = 256;  // (a page of 128 x 128 texels: the most tiles one listed page adds)
constexpr uint32_t kFrameBytes = 128;  // (CardLayout.hlsli: 28 words in use)
// The structural bound of one dispatch (Docs/Status/DISPATCH_BOUNDS_KO.md): a thread traces at most one ray.
constexpr uint32_t kThreadsPerDispatch = 262144;
const char* const kStore[2] = { "Passes/SurfaceCache/CardDirectStore.SKY0", "Passes/SurfaceCache/CardDirectStore.SKY1" };
const char* const kRadiosityTrace[2] = { "Passes/SurfaceCache/CardRadiosityTrace.SKY0", "Passes/SurfaceCache/CardRadiosityTrace.SKY1" };
const char* const kSelect[3] = { "Passes/SurfaceCache/CardSelect.STAGE0", "Passes/SurfaceCache/CardSelect.STAGE1", "Passes/SurfaceCache/CardSelect.STAGE2" };

uint32_t bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
} // namespace

struct CardLighting::Impl
{
    Device& device;
    uint32_t atlasSize = 0, pageCapacity = 0;
    ComPtr<ID3D12Resource> direct, indirect, final, trace, sh[3], frames, pageLight, uniformBits, frameBuffer, lastUsed;
    // this frame's references
    uint64_t frameIndex = UINT64_MAX, recordSerial = UINT64_MAX;
    CardSet set;
    CardLightingRefs refs;
    bool cleared = false;  // r.card.clear ran since the resources were made

    explicit Impl(Device& d) : device(d) {}

    ComPtr<ID3D12Resource> texture(uint32_t size, DXGI_FORMAT format, const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = size;
        d.Height = size;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
              "card lighting atlas");
        r->SetName(name);
        return r;
    }
    ComPtr<ID3D12Resource> buffer(uint64_t bytes, const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "card lighting buffer");
        r->SetName(name);
        return r;
    }
    void release(ComPtr<ID3D12Resource>& r)
    {
        if (r) device.deferRelease(r);
        r.Reset();
    }
    // The card frame's words 0..27 (CardLayout.hlsli) and the pass's own two (P[5].xy: the frame, the select buffer).
    void frameWords(PassContext& c, const CardSet& cards, const CardLightingRefs& r, uint32_t pageCount, uint32_t frameWord, float depthBias, uint32_t selectUav,
                    uint32_t k[32]) const
    {
        const uint32_t none = 0xFFFFFFFFu;
        const uint32_t words[32] = { c.srv(cards.instanceMap), c.srv(cards.meshCards), c.srv(cards.cards), c.srv(cards.cardPages), c.srv(cards.pageTable), c.srv(cards.depth),
                                     c.srv(cards.albedo), c.srv(cards.normal), c.srv(cards.emissive), atlasSize, pageCount, frameWord, c.srv(r.final), c.srv(r.direct),
                                     c.srv(r.indirect), c.srv(r.pageLight), cards.instances, bits(depthBias),
                                     cards.feedback.valid() ? c.uav(cards.feedback) : none, cards.feedbackDither,
                                     c.uav(r.frame), selectUav, 0, 0,
                                     bits(cards.feedbackBias), cards.feedback.valid() ? c.uav(r.lastUsed) : none, cards.hitVolumeParams, none,
                                     none, none, none, none };
        std::memcpy(k, words, sizeof words);
    }
};

CardLighting::CardLighting(Device& device) : m(std::make_unique<Impl>(device)) {}
CardLighting::~CardLighting()
{
    Impl& s = *m;
    s.release(s.direct), s.release(s.indirect), s.release(s.final), s.release(s.trace), s.release(s.frames), s.release(s.uniformBits);
    s.release(s.pageLight), s.release(s.frameBuffer), s.release(s.lastUsed);
    for (auto& t : s.sh) s.release(t);
}

BufferRef CardLighting::frame(const FramePassContext& fc) const { return m->frameIndex == fc.frame.frameIndex ? m->refs.frame : BufferRef{}; }

void CardLighting::declareRead(const FramePassContext& fc, PassBuilder& b, Use use) const
{
    const Impl& s = *m;
    if (s.frameIndex != fc.frame.frameIndex) return;
    b.use(s.refs.frame, use);
    b.use(s.set.instanceMap, use);
    b.use(s.set.meshCards, use);
    b.use(s.set.cards, use);
    b.use(s.set.cardPages, use);
    b.use(s.set.pageTable, use);
    b.use(s.set.depth, use);
    b.use(s.set.albedo, use);
    b.use(s.set.normal, use);
    b.use(s.set.emissive, use);
    b.use(s.refs.direct, use);
    b.use(s.refs.indirect, use);
    b.use(s.refs.final, use);
}

const CardLightingRefs& CardLighting::begin(FramePassContext& fc, uint32_t atlasSize, uint32_t pageCapacity)
{
    Impl& s = *m;
    const uint64_t serial = fc.trackState ? fc.trackState->recordSerial() : 0;
    if (s.frameIndex == fc.frame.frameIndex && s.recordSerial == serial && s.refs.frame.valid()) return s.refs;
    RenderGraph& g = fc.graph;
    const uint32_t atlas = atlasSize, capacity = std::max(pageCapacity, 1u);
    if (atlas == 0 || atlas % 128 != 0) fail("surface_cache.mesh_cards: atlas size %u", atlas);
    bool created = false;
    if (s.atlasSize != atlas || !s.direct)
    {
        s.release(s.direct), s.release(s.indirect), s.release(s.final), s.release(s.trace), s.release(s.frames), s.release(s.uniformBits);
        for (auto& t : s.sh) s.release(t);
        s.direct = s.texture(atlas, DXGI_FORMAT_R11G11B10_FLOAT, L"R card direct lighting");
        s.indirect = s.texture(atlas, DXGI_FORMAT_R11G11B10_FLOAT, L"R card indirect lighting");
        s.final = s.texture(atlas, DXGI_FORMAT_R11G11B10_FLOAT, L"R card final lighting");
        s.trace = s.texture(atlas, DXGI_FORMAT_R11G11B10_FLOAT, L"R card radiosity trace");
        static const wchar_t* const kShNames[3] = { L"R card radiosity SH red", L"R card radiosity SH green", L"R card radiosity SH blue" };
        for (int c = 0; c < 3; ++c) s.sh[c] = s.texture(atlas / kProbeSpacing, DXGI_FORMAT_R16G16B16A16_FLOAT, kShNames[c]);
        s.frames = s.texture(atlas / kTile, DXGI_FORMAT_R8_UINT, L"R card radiosity frames");
        s.uniformBits = s.buffer((uint64_t)(atlas / kTile) * (atlas / kTile) * kUniformBytes, L"R card shadow uniform bits");
        s.atlasSize = atlas;
        created = true;
    }
    if (s.pageCapacity != capacity || !s.pageLight)
    {
        s.release(s.pageLight);
        s.release(s.lastUsed);
        s.pageLight = s.buffer((uint64_t)capacity * kPageLightBytes, L"R card page light");
        s.lastUsed = s.buffer((uint64_t)capacity * 4, L"R card page last used");
        s.pageCapacity = capacity;
        created = true;
    }
    if (!s.frameBuffer) s.frameBuffer = s.buffer(kFrameBytes, L"R card frame");
    if (created) s.cleared = false;

    const uint32_t atlasTiles = (atlas / kTile) * (atlas / kTile);
    CardLightingRefs r;
    r.direct = g.importTexture(s.direct.Get(), { "R card direct lighting", atlas, atlas, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    r.indirect = g.importTexture(s.indirect.Get(), { "R card indirect lighting", atlas, atlas, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    r.final = g.importTexture(s.final.Get(), { "R card final lighting", atlas, atlas, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    r.trace = g.importTexture(s.trace.Get(), { "R card radiosity trace", atlas, atlas, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    static const char* const kShNames[3] = { "R card radiosity SH red", "R card radiosity SH green", "R card radiosity SH blue" };
    for (int c = 0; c < 3; ++c)
        r.sh[c] = g.importTexture(s.sh[c].Get(), { kShNames[c], atlas / kProbeSpacing, atlas / kProbeSpacing, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    r.frames = g.importTexture(s.frames.Get(), { "R card radiosity frames", atlas / kTile, atlas / kTile, 1, 1, DXGI_FORMAT_R8_UINT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    r.uniformBits = g.importBuffer(s.uniformBits.Get(), { "R card shadow uniform bits", (uint64_t)atlasTiles * kUniformBytes, 0 });
    r.pageLight = g.importBuffer(s.pageLight.Get(), { "R card page light", (uint64_t)capacity * kPageLightBytes, 0 });
    r.lastUsed = g.importBuffer(s.lastUsed.Get(), { "R card page last used", (uint64_t)capacity * 4, 0 });
    r.frame = g.importBuffer(s.frameBuffer.Get(), { "R card frame", kFrameBytes, 0 });
    r.created = created;
    s.refs = r;
    s.frameIndex = fc.frame.frameIndex;
    s.recordSerial = serial;
    return s.refs;
}

void CardLighting::recordFrame(FramePassContext& fc, const CardSet& set, bool restart, float depthBias, uint32_t frameWord)
{
    Impl& s = *m;
    RenderGraph& g = fc.graph;
    ShaderLibrary& shaders = fc.shaders;
    const CardLightingRefs r = s.refs;
    const uint32_t atlas = s.atlasSize, capacity = s.pageCapacity;
    s.set = set;
    if (restart || !s.cleared)
    {
        s.cleared = true;
        g.addPass("r.card.clear", QueueType::Compute,
                  [&](PassBuilder& b) {
                      for (const TextureRef& t : { r.direct, r.indirect, r.final, r.trace, r.sh[0], r.sh[1], r.sh[2], r.frames }) b.use(t, Use::UavCompute);
                      b.use(r.pageLight, Use::UavCompute);
                      b.use(r.uniformBits, Use::UavCompute);
                      b.use(r.lastUsed, Use::UavCompute);
                      b.keep();
                  },
                  [&shaders, r, atlas, capacity](PassContext& c) {
                      const uint32_t k[16] = { c.uav(r.direct), c.uav(r.indirect), c.uav(r.final), c.uav(r.trace), c.uav(r.sh[0]), c.uav(r.sh[1]), c.uav(r.sh[2]),
                                               c.uav(r.frames), c.uav(r.pageLight), c.uav(r.uniformBits), atlas, capacity, c.uav(r.lastUsed), 0, 0, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/CardClear"));
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch((atlas + 7) / 8, (atlas + 7) / 8, 1);
                  });
    }
    const uint32_t pageCount = std::min(set.cardPageCount, capacity);
    g.addPass("r.card.frame", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(r.frame, Use::UavCompute);
                  b.keep();
              },
              [this, &shaders, set, r, pageCount, frameWord, depthBias](PassContext& c) {
                  uint32_t k[32];
                  m->frameWords(c, set, r, pageCount, frameWord, depthBias, 0xFFFFFFFFu, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/CardFrame"));
                  c.computeConstants(k, 32);
                  c.cmd->Dispatch(1, 1, 1);
              });
}

void CardLighting::recordLighting(FramePassContext& fc, const CardLightingInputs& in)
{
    Impl& s = *m;
    if (!in.set.valid) return;
    const CardSet set = in.set;
    s.set = set;
    RenderGraph& g = fc.graph;
    ShaderLibrary& shaders = fc.shaders;
    const QualityConfig& q = fc.quality;
    const CardLightingRefs r = s.refs;
    const BufferRef frameBuffer = r.frame;
    const uint32_t atlas = s.atlasSize, capacity = s.pageCapacity;

    const uint32_t atlasTiles = (atlas / kTile) * (atlas / kTile);
    const uint32_t directBudget = std::max(atlasTiles / std::max(in.directFactor, 1u), 1u), radiosityBudget = std::max(atlasTiles / std::max(in.radiosityFactor, 1u), 1u);
    const uint32_t directCapacity = directBudget + kPageTiles, radiosityCapacity = radiosityBudget + kPageTiles;
    if (directCapacity > 65535 || radiosityCapacity > 65535) fail("surface_cache.mesh_cards: %u / %u tiles a frame exceed one dispatch row", directCapacity, radiosityCapacity);
    const float updateDistance = q.has("surface_cache.mesh_cards_update_distance_m") ? (float)q.number("surface_cache.mesh_cards_update_distance_m") : 25.0f;
    const float frustumMargin = q.has("surface_cache.mesh_cards_frustum_margin_m") ? (float)q.number("surface_cache.mesh_cards_frustum_margin_m") : 5.0f;
    const float endBias = q.has("shading.mega_lights_ray_end_bias_m") ? (float)q.number("shading.mega_lights_ray_end_bias_m") : 0.01f;

    const TextureRef direct = r.direct, indirect = r.indirect, final = r.final, trace = r.trace, frames = r.frames;
    const TextureRef sh[3] = { r.sh[0], r.sh[1], r.sh[2] };
    const BufferRef uniformBits = r.uniformBits, pageLight = r.pageLight;
    // (the pages the high levels' readers read are relit first: only when something writes the words)
    const BufferRef lastUsed = set.feedback.valid() && set.lightingFeedback ? r.lastUsed : BufferRef{};
    const uint64_t selectBytes = kSelectHead + 2ull * kBuckets * 4 + (uint64_t)capacity * 4 + ((uint64_t)directCapacity + radiosityCapacity) * 4;
    const BufferRef select = g.createBuffer({ "r.card select", (selectBytes + 15) & ~15ull, 0 });
    const BufferRef tileLights = g.createBuffer({ "r.card tile lights", (uint64_t)directCapacity * kTileLightBytes, 0 });
    const BufferRef tileShadow = g.createBuffer({ "r.card tile shadow", (uint64_t)directCapacity * kTileShadowBytes, 0 });

    const D3D12_GPU_VIRTUAL_ADDRESS cb = in.frameConstants;
    const uint32_t frame = in.frame, pageCount = std::min(set.cardPageCount, capacity);
    const auto declareShared = in.declareShared;
    const auto sharedConstants = in.sharedConstants;
    // the card set's records and geometry atlases, and the frame, as shader resources
    auto declareSet = [=](PassBuilder& b, bool rays, bool material) {
        const Use use = rays ? Use::SrvGraphics : Use::SrvCompute;
        b.use(frameBuffer, use);
        b.use(set.instanceMap, use);
        b.use(set.meshCards, use);
        b.use(set.cards, use);
        b.use(set.cardPages, use);
        b.use(set.pageTable, use);
        b.use(set.depth, use);
        b.use(set.normal, use);
        if (material)
        {
            b.use(set.albedo, use);
            b.use(set.emissive, use);
        }
    };

    // the card frame with this update's counter and page count; the select buffer's head and histograms cleared
    g.addPass("r.card.frame", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(frameBuffer, Use::UavCompute);
                  b.use(select, Use::UavCompute);
                  b.keep();
              },
              [this, &shaders, set, r, select, pageCount, frame, depthBias = in.depthBias](PassContext& c) {
                  uint32_t k[32];
                  m->frameWords(c, set, r, pageCount, frame, depthBias, c.uav(select), k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/CardFrame"));
                  c.computeConstants(k, 32);
                  c.cmd->Dispatch(1, 1, 1);
              });

    for (uint32_t stage = 0; stage < 3; ++stage)
    {
        static const char* const kNames[3] = { "r.card.select.priority", "r.card.select.bucket", "r.card.select.list" };
        g.addPass(kNames[stage], QueueType::Compute,
                  [&](PassBuilder& b) {
                      declareSet(b, false, false);
                      b.use(select, Use::UavCompute);
                      b.use(pageLight, Use::UavCompute);
                      if (stage == 0 && lastUsed.valid()) b.use(lastUsed, Use::SrvCompute);
                  },
                  [&shaders, cb, stage, frameBuffer, select, pageLight, lastUsed, frame, capacity, pageCount, updateDistance, frustumMargin, directBudget, radiosityBudget,
                   directCapacity, radiosityCapacity](PassContext& c) {
                      const uint32_t k[24] = { c.srv(frameBuffer), c.uav(select), frame, stage == 0 && lastUsed.valid() ? c.srv(lastUsed) : 0xFFFFFFFFu,
                                               0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                               c.uav(pageLight), capacity, bits(updateDistance), bits(frustumMargin), directBudget, radiosityBudget, directCapacity, radiosityCapacity };
                      c.cmd->SetPipelineState(shaders.compute(kSelect[stage]));
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 24);
                      c.cmd->Dispatch(stage == 1 ? 1 : (std::max(pageCount, 1u) + 63) / 64, 1, 1);
                  });
    }

    if (in.direct)
    {
        const uint32_t lightFlags = (in.shadowRaysOpaque ? 2048u : 0u);
        g.addPass("r.card.direct.cull", QueueType::Compute,
                  [&](PassBuilder& b) {
                      declareShared(b);  // (the light grid)
                      declareSet(b, true, false);
                      b.use(select, Use::SrvGraphics);
                      b.use(uniformBits, Use::SrvGraphics);
                      b.use(tileLights, Use::UavGraphics);
                      b.use(tileShadow, Use::UavGraphics);
                      b.use(pageLight, Use::UavGraphics);  // (CL_PAGE_ANIMATED: a light whose function changes with time)
                  },
                  [&shaders, cb, sharedConstants, frameBuffer, select, uniformBits, tileLights, tileShadow, pageLight, frame, lightFlags, capacity,
                   directCapacity](PassContext& c) {
                      uint32_t k[32] = {};
                      sharedConstants(c, k);
                      k[0] = c.srv(frameBuffer), k[1] = c.srv(select), k[2] = frame, k[3] = lightFlags;
                      k[16] = c.uav(tileLights), k[17] = c.uav(tileShadow), k[18] = c.srv(uniformBits), k[19] = capacity;
                      k[20] = directCapacity, k[21] = c.uav(pageLight), k[22] = 0, k[23] = 0;
                      c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/CardDirectCull"));
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 32);
                      c.cmd->Dispatch((directCapacity + 63) / 64, 1, 1);
                  });
        rt::RayPipeline& directTrace = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline("Passes/SurfaceCache/CardDirectTrace", { "CardDirectTraceGen" }));
        g.addPass("r.card.direct.trace", QueueType::Compute,
                  [&](PassBuilder& b) {
                      declareShared(b);
                      declareSet(b, true, false);
                      b.use(select, Use::SrvGraphics);
                      b.use(tileLights, Use::SrvGraphics);
                      b.use(tileShadow, Use::UavGraphics);
                  },
                  [&directTrace, cb, sharedConstants, frameBuffer, select, tileLights, tileShadow, frame, lightFlags, capacity, directCapacity, endBias](PassContext& c) {
                      uint32_t k[32] = {};
                      sharedConstants(c, k);
                      k[0] = c.srv(frameBuffer), k[1] = c.srv(select), k[2] = frame, k[3] = lightFlags;
                      k[16] = c.srv(tileLights), k[17] = c.uav(tileShadow), k[19] = capacity;
                      k[20] = directCapacity, k[21] = bits(endBias), k[22] = 0, k[23] = 0;
                      c.bindFrameConstants(cb);
                      const uint64_t threads = (uint64_t)directCapacity * kTraceThreads;
                      for (uint64_t first = 0; first < threads; first += kThreadsPerDispatch)
                      {
                          k[18] = (uint32_t)first;
                          c.computeConstants(k, 32);
                          directTrace.dispatch(c.cmd, 0, (uint32_t)std::min<uint64_t>(kThreadsPerDispatch, threads - first), 1);
                      }
                  });
        ID3D12PipelineState* store = shaders.compute(kStore[in.skyVariant & 1u]);
        g.addPass("r.card.direct.store", QueueType::Compute,
                  [&](PassBuilder& b) {
                      declareShared(b);  // (the sky's tables, the lights)
                      declareSet(b, true, true);
                      b.use(select, Use::SrvGraphics);
                      b.use(tileLights, Use::SrvGraphics);
                      b.use(tileShadow, Use::SrvGraphics);
                      b.use(uniformBits, Use::UavGraphics);
                      b.use(indirect, Use::SrvGraphics);
                      b.use(direct, Use::UavGraphics);
                      b.use(final, Use::UavGraphics);
                      b.keep();
                  },
                  [store, cb, sharedConstants, frameBuffer, select, uniformBits, tileLights, tileShadow, direct, indirect, final, frame, lightFlags, capacity,
                   directCapacity](PassContext& c) {
                      uint32_t k[32] = {};
                      sharedConstants(c, k);
                      k[0] = c.srv(frameBuffer), k[1] = c.srv(select), k[2] = frame, k[3] = lightFlags;
                      k[16] = c.srv(tileLights), k[17] = c.srv(tileShadow), k[18] = c.uav(uniformBits), k[19] = capacity;
                      k[20] = directCapacity, k[21] = c.uav(direct), k[22] = c.uav(final), k[23] = c.srv(indirect);
                      c.cmd->SetPipelineState(store);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 32);
                      c.cmd->Dispatch(directCapacity, 1, 1);
                  });
    }

    if (in.radiosity)
    {
        rt::RayPipeline& radiosityTrace = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(kRadiosityTrace[in.skyVariant & 1u], { "CardRadiosityTraceGen" }));
        g.addPass("r.card.radiosity.trace", QueueType::Compute,
                  [&](PassBuilder& b) {
                      declareShared(b);
                      declareSet(b, true, false);
                      b.use(select, Use::SrvGraphics);
                      b.use(pageLight, Use::SrvGraphics);
                      b.use(final, Use::SrvGraphics);
                      b.use(trace, Use::UavGraphics);
                      // (the rays' hits read the cards' high levels and report: clReadCardsHiRes; a hit without cards
                      // reads the volume the frame names)
                      if (set.feedback.valid())
                      {
                          b.use(set.feedback, Use::UavGraphics);
                          b.use(r.lastUsed, Use::UavGraphics);
                      }
                      if (set.hitVolumeAmbient.valid()) b.use(set.hitVolumeAmbient, Use::SrvGraphics);
                      if (set.hitVolumeDirectional.valid()) b.use(set.hitVolumeDirectional, Use::SrvGraphics);
                  },
                  [&radiosityTrace, cb, sharedConstants, frameBuffer, select, pageLight, trace, frame, capacity, directCapacity, radiosityCapacity,
                   cap = in.radiosityCap, skipBackFace = in.radiositySkipBackFace, skipTwoSided = in.radiositySkipTwoSided,
                   minTraceDistance = in.radiosityMinTraceDistance](PassContext& c) {
                      uint32_t k[32] = {};
                      sharedConstants(c, k);
                      k[0] = c.srv(frameBuffer), k[1] = c.srv(select), k[2] = frame, k[3] = bits(skipBackFace);
                      k[15] = bits(minTraceDistance);
                      k[16] = c.uav(trace), k[17] = bits(cap), k[19] = capacity;
                      k[20] = directCapacity, k[21] = bits(skipTwoSided), k[22] = radiosityCapacity, k[23] = c.srv(pageLight);
                      c.bindFrameConstants(cb);
                      const uint64_t threads = (uint64_t)radiosityCapacity * 64;
                      // (a thread traces at most 4 rays - its own, its re-shoot past a near back face and, at a hit
                      // without cards, the sun's and a light sample's: a dispatch holds a quarter of the threads)
                      const uint64_t perDispatch = kThreadsPerDispatch / 4;
                      for (uint64_t first = 0; first < threads; first += perDispatch)
                      {
                          k[18] = (uint32_t)first;
                          c.computeConstants(k, 32);
                          radiosityTrace.dispatch(c.cmd, 0, (uint32_t)std::min<uint64_t>(perDispatch, threads - first), 1);
                      }
                  });
        g.addPass("r.card.radiosity.probe", QueueType::Compute,
                  [&](PassBuilder& b) {
                      declareSet(b, false, false);
                      b.use(select, Use::SrvCompute);
                      b.use(pageLight, Use::SrvCompute);
                      b.use(trace, Use::SrvCompute);
                      for (const TextureRef& t : sh) b.use(t, Use::UavCompute);
                  },
                  [&shaders, cb, frameBuffer, select, pageLight, trace, sh0 = sh[0], sh1 = sh[1], sh2 = sh[2], frame, capacity, directCapacity, radiosityCapacity](PassContext& c) {
                      const uint32_t k[28] = { c.srv(frameBuffer), c.srv(select), frame, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                               c.srv(trace), c.srv(pageLight), 0, capacity, directCapacity, radiosityCapacity, 0, 0, c.uav(sh0), c.uav(sh1), c.uav(sh2), 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/CardRadiosityProbe"));
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 28);
                      c.cmd->Dispatch((radiosityCapacity * 4 + 63) / 64, 1, 1);
                  });
        g.addPass("r.card.radiosity.integrate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      declareSet(b, false, true);
                      b.use(select, Use::SrvCompute);
                      b.use(pageLight, Use::SrvCompute);
                      for (const TextureRef& t : sh) b.use(t, Use::SrvCompute);
                      b.use(direct, Use::SrvCompute);
                      b.use(frames, Use::UavCompute);
                      b.use(indirect, Use::UavCompute);
                      b.use(final, Use::UavCompute);
                      b.keep();
                  },
                  [&shaders, cb, frameBuffer, select, pageLight, sh0 = sh[0], sh1 = sh[1], sh2 = sh[2], direct, indirect, final, frames, frame, capacity, directCapacity,
                   radiosityCapacity, maxFrames = in.radiosityFrames](PassContext& c) {
                      const uint32_t k[28] = { c.srv(frameBuffer), c.srv(select), frame, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                                               c.srv(pageLight), bits(maxFrames), c.uav(frames), capacity, directCapacity, radiosityCapacity, c.uav(indirect), c.uav(final),
                                               c.srv(sh0), c.srv(sh1), c.srv(sh2), c.srv(direct) };
                      c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/CardRadiosityIntegrate"));
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 28);
                      c.cmd->Dispatch(radiosityCapacity, 1, 1);
                  });
    }
}
} // namespace unx::render::refl
