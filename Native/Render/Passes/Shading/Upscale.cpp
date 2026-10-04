// Temporal upscale of M (user decisions 2026-09-28: internal resolution + temporal upscale for the 4K budget, then for
// 1440p and 1080p; output.render_scale, output.render_height_max, FrameContext::Upscale set by
// FrameRenderer::setupUpscale). The main view renders at the output height x render_scale (at most render_height_max)
// with a Halton (2, 3) sub-pixel jitter of its projection; every system of the frame sees that internal view, and M's
// texture footprints are the output pixel's (FrameConstants::upscaleRatio). After the shading chain (haze, depth of field at the internal resolution) two passes
// reconstruct the output resolution:
//   m.upscale.motion  per internal sample, the output-UV motion to the previous frame's unjittered view
//                     (UpscaleMotion.hlsl: the vis buffer's triangle in its previous-tick vertices);
//   m.upscale         per output pixel, the jittered samples around it (a Gaussian window), the reprojected previous
//                     output clipped to their YCoCg box, blended by how well this frame's samples cover the pixel
//                     against the history's weight (Upscale.hlsl).
// The output (RGBA16F at the output resolution, persistent ping-pong) is the post chain's input and the next frame's
// history. Cost per output pixel is fixed (9 colour + 9 depth loads, 1 motion load, 5 bilinear history taps); the
// shading, reflections, GI screen passes, shadows' projections and resolve run on 0.44 of the output pixels (2/3 height).
#include "unx/shading/Upscale.h"
#include "TsrRejectPolicy.h"

#include "unx/core/Config.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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

// The history's ring (output.upscale_tsr_resurrection; the reference's TSR history slices, its
// r.TSR.Resurrection.PersistentFrameInterval 31 and PersistentFrameCount 2): a frame's place in the cycle is its rolling
// index; every kResurrectionPeriod-th frame is kept, in slots 1 and 2 alternately, the others take slots 0 and 3 in
// turn (the period is odd: two frames in a row never share a slot). Without resurrection only slots 0 and 3 exist.
constexpr uint32_t kResurrectionPeriod = 31, kResurrectionCycle = 2 * kResurrectionPeriod, kRingSlots = 4;
uint32_t ringSlot(uint32_t rolling, bool ring)
{
    if (ring && rolling % kResurrectionPeriod == 0) return (rolling / kResurrectionPeriod) % 2 ? 2u : 1u;
    return rolling % 2 ? 3u : 0u;
}
bool ringSlotUsed(uint32_t slot, bool ring) { return ring || slot == 0 || slot == 3; }

struct UpscaleState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> history[kRingSlots];
    uint32_t width = 0, height = 0, parity = 0;  // (the history's size: the output's, or above it)
    bool ring = false;
    bool fresh = true;  // the textures hold nothing yet
    // the ring: the slot the last frame wrote, that frame's rolling index, the frames since the last cut (up to the
    // cycle), and each slot's frame - its unjittered view-projection and exposure (what a later frame reprojects by)
    uint32_t last = 0, rolling = 0, accumulated = 0;
    struct Kept
    {
        float4x4 viewProj{};
        float exposure = 1;
        bool valid = false;
        float lensTanX = 0, lensTanY = 0, lensScale = 0;  // the frame's lens projection (upscaleLens; scale 0: none)
    } kept[kRingSlots];
    float lensD = 0, lensS = 0;  // the lens the history is under (a change restarts the history)
    // The references below are of one recording's graph: they are keyed by the frame index and the recording's serial
    // (TrackState::recordSerial) - a frame whose recording failed is recorded again under the same index, and a
    // reference of the failed graph names another resource in the new one.
    TextureRef previous;           // history[last] in the graph of frame 'previousFrame' (upscalePreviousColor)
    RecordKey previousFrame;
    // output.screen_trace_source = 0: the lit opaque scene colour (the view's resolution; before translucency, the air,
    // the upscale and the display transform), kept for the next frame's screen-space traces - the reference's default
    // source (keepSceneColor, UpscaleSceneKeep.hlsl). scene[parity] is the last one written.
    ComPtr<ID3D12Resource> scene[2];
    uint32_t sceneWidth[2] = {}, sceneHeight[2] = {};  // per slot: the internal size of the frame that wrote it
    DXGI_FORMAT sceneFormat = DXGI_FORMAT_UNKNOWN;
    bool sceneFresh = true;
    TextureRef previousScene;
    RecordKey previousSceneFrame;
    UpscaleMotion motion;  // m.upscale.motion's outputs in the graph of frame 'motionFrame' (upscaleMotion)
    RecordKey motionFrame;
    // The internal-resolution textures (the kept scene colour, the guide ring, the flickering and thin-coverage pairs)
    // under dynamic resolution: the internal size is each frame's own, so a slot has the size of the frame that wrote it.
    // The frame that writes a slot next recreates it at its size (the other slots keep theirs); the readers reproject
    // by UV and take each texture's size as it is (TsrDecimate.hlsl; the screen traces read by UV too). Nothing restarts.
    ComPtr<ID3D12Resource> internalTexture(Device& d, uint32_t w, uint32_t h, DXGI_FORMAT format, const wchar_t* name, const char* what)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> texture;
        check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())),
              what);
        texture->SetName(name);
        return texture;
    }
    // 'slot': the one this frame writes.
    void ensureScene(Device& d, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t slot)
    {
        const bool all = !scene[0] || sceneFormat != format;
        if (!all && sceneWidth[slot] == w && sceneHeight[slot] == h) return;
        device = &d;
        for (uint32_t k = 0; k < 2; ++k)
        {
            if (!all && k != slot) continue;
            if (scene[k]) d.deferRelease(scene[k]);
            scene[k] = internalTexture(d, w, h, format, k ? L"M previous scene colour 1" : L"M previous scene colour 0", "M previous scene colour");
            sceneWidth[k] = w;
            sceneHeight[k] = h;
        }
        sceneFormat = format;
        if (all) sceneFresh = true;
    }
    // output.upscale_tsr (Tsr.hlsli): the guide history at the internal resolution (R10G10B10A2: the scene colour in
    // the guide space at low frequency, a = the reprojection edge), in the history's ring: guide[last] is the last one.
    ComPtr<ID3D12Resource> guide[kRingSlots];
    ComPtr<ID3D12Resource> flicker[2];  // the flickering heuristic's history (RGBA8, TsrFlicker.hlsl), as the guide
    ComPtr<ID3D12Resource> thin[2];     // the thin geometry's coverage history (R8, TsrThin.hlsl), as the guide
    uint32_t guideWidth[kRingSlots] = {}, guideHeight[kRingSlots] = {};  // per slot, as the scene colour's
    uint32_t pairWidth[2] = {}, pairHeight[2] = {};                      // flicker[k] and thin[k]
    bool guideRing = false;
    void makeGuide(Device& d, uint32_t k, uint32_t w, uint32_t h)
    {
        static const wchar_t* const guideNames[kRingSlots] = { L"M upscale guide 0", L"M upscale guide 1 (kept)", L"M upscale guide 2 (kept)", L"M upscale guide 3" };
        if (guide[k]) d.deferRelease(guide[k]);
        guide[k] = internalTexture(d, w, h, DXGI_FORMAT_R10G10B10A2_UNORM, guideNames[k], "M upscale guide");
        guideWidth[k] = w;
        guideHeight[k] = h;
    }
    void makePair(Device& d, uint32_t k, uint32_t w, uint32_t h)
    {
        if (flicker[k]) d.deferRelease(flicker[k]);
        flicker[k] = internalTexture(d, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, k ? L"M upscale flickering history 1" : L"M upscale flickering history 0",
                                     "M upscale flickering history");
        if (thin[k]) d.deferRelease(thin[k]);
        thin[k] = internalTexture(d, w, h, DXGI_FORMAT_R8_UNORM, k ? L"M upscale thin coverage history 1" : L"M upscale thin coverage history 0",
                                  "M upscale thin coverage history");
        pairWidth[k] = w;
        pairHeight[k] = h;
    }
    // All of them at this frame's size, when there are none or the ring's shape changed (a reset).
    void ensureGuide(Device& d, uint32_t w, uint32_t h, bool withRing)
    {
        if (guide[0] && guideRing == withRing) return;
        device = &d;
        for (uint32_t k = 0; k < kRingSlots; ++k)
        {
            if (guide[k]) d.deferRelease(guide[k]);
            guide[k].Reset();
            guideWidth[k] = guideHeight[k] = 0;
            if (ringSlotUsed(k, withRing)) makeGuide(d, k, w, h);
        }
        for (uint32_t k = 0; k < 2; ++k) makePair(d, k, w, h);
        guideRing = withRing;
        fresh = true;  // (the guides hold nothing: the frame is a reset)
    }
    // The slots this frame writes, at this frame's size (the internal size changed since they were last written).
    void resizeGuide(Device& d, uint32_t slot, uint32_t pair, uint32_t w, uint32_t h)
    {
        if (guideWidth[slot] != w || guideHeight[slot] != h) makeGuide(d, slot, w, h);
        if (pairWidth[pair] != w || pairHeight[pair] != h) makePair(d, pair, w, h);
    }
    ~UpscaleState()
    {
        if (!device) return;
        for (auto& t : guide)
            if (t) device->deferRelease(t);
        for (auto& t : flicker)
            if (t) device->deferRelease(t);
        for (auto& t : thin)
            if (t) device->deferRelease(t);
        for (auto& t : history)
            if (t) device->deferRelease(t);
        for (auto& t : scene)
            if (t) device->deferRelease(t);
    }
    void ensure(Device& d, uint32_t w, uint32_t h, bool withRing)
    {
        if (history[0] && width == w && height == h && ring == withRing) return;
        device = &d;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        static const wchar_t* const names[kRingSlots] = { L"M upscale history 0", L"M upscale history 1 (kept)", L"M upscale history 2 (kept)", L"M upscale history 3" };
        for (uint32_t k = 0; k < kRingSlots; ++k)
        {
            if (history[k]) d.deferRelease(history[k]);
            history[k].Reset();
            if (!ringSlotUsed(k, withRing)) continue;
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(history[k].ReleaseAndGetAddressOf())),
                  "M upscale history");
            history[k]->SetName(names[k]);
        }
        width = w;
        height = h;
        ring = withRing;
        fresh = true;
    }
};

// output.upscale_tsr: the temporal super resolution's structure (Tsr.hlsli) in place of the one-pass accumulation.
bool tsrOn(FramePassContext& fc) { return !fc.quality.has("output.upscale_tsr") || fc.quality.boolean("output.upscale_tsr"); }

// The history's size: the output's, or with upscale_tsr output.upscale_tsr_history_percent of it per axis (the
// reference's r.TSR.History.ScreenPercentage, 100 .. 200; back to the output's size where a texture cannot be that large).
void historySize(FramePassContext& fc, uint32_t W, uint32_t H, uint32_t& hw, uint32_t& hh)
{
    const double percent = tsrOn(fc) && fc.quality.has("output.upscale_tsr_history_percent") ? fc.quality.number("output.upscale_tsr_history_percent") : 100.0;
    if (!(percent >= 100 && percent <= 200)) fail("output.upscale_tsr_history_percent must be in [100, 200]");
    hw = (uint32_t)std::ceil((double)W * percent / 100.0);
    hh = (uint32_t)std::ceil((double)H * percent / 100.0);
    if (hw > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || hh > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
    {
        hw = W;
        hh = H;
    }
}
} // namespace

bool upscaleActive(FramePassContext& fc, const ViewResources& view)
{
    return view.view.kind == gpu::ViewKind::Main && fc.frame.upscale.outputWidth != 0 && !fc.frame.outputLinearHdr;
}

LensProjection upscaleLens(FramePassContext& fc, const ViewResources& view)
{
    LensProjection lens;
    if (!upscaleActive(fc, view) || !tsrOn(fc)) return lens;
    const double d = fc.quality.has("output.lens_panini_d") ? fc.quality.number("output.lens_panini_d") : 0.0;
    const double s = fc.quality.has("output.lens_panini_s") ? fc.quality.number("output.lens_panini_s") : 0.0;
    if (!(d >= 0 && d <= 4) || !(s >= -1 && s <= 1)) fail("output.lens_panini_d %g in [0, 4], output.lens_panini_s %g in [-1, 1]", d, s);
    if (!(d > 0.01)) return lens;  // (the reference: on above 0.01)
    const float4x4& proj = fc.frame.upscale.proj;
    const float tanX = 1.0f / proj.m[0][0], tanY = 1.0f / proj.m[1][1];
    if (!std::isfinite(tanX) || !std::isfinite(tanY) || !(tanX > 0) || !(tanY > 0)) return lens;
    // the rendered view's width fills the picture: the scale that brings the side's place on the lens plane to the side
    const float D = (float)d;
    const float invLength = 1.0f / std::sqrt(1.0f + tanX * tanX);
    const float sinPhi = tanX * invLength, cosPhi = std::sqrt(std::max(1.0f - sinPhi * sinPhi, 0.0f));
    const float scale = tanX / ((D + 1.0f) / (D + cosPhi) * sinPhi);
    // (the centre's magnification; the reference gives up outside (1, 2): a field of view the projection cannot hold)
    if (!std::isfinite(scale) || !(scale > 1.0f) || !(scale < 2.0f)) return lens;
    lens.active = true;
    lens.tanX = tanX;
    lens.tanY = tanY;
    lens.d = D;
    lens.s = (float)s;
    lens.scale = scale;
    return lens;
}

ViewResources upscaleOutputView(FramePassContext& fc, const ViewResources& view)
{
    ViewResources out = view;
    if (!upscaleActive(fc, view)) return out;
    out.view.width = fc.frame.upscale.outputWidth;
    out.view.height = fc.frame.upscale.outputHeight;
    out.view.proj = fc.frame.upscale.proj;
    out.view.viewProj = fc.frame.upscale.viewProj;
    out.view.prevViewProj = fc.frame.upscale.prevViewProj;
    out.view.invViewProj = inverse(out.view.viewProj);
    return out;
}

namespace
{
// output.screen_trace_source: 0 = the scene colour before the upscale (the reference's default: scene colour ahead of
// post-processing, its r.Lumen.ScreenTracingSource 0), 1 = the upscale's history (anti-aliased, output resolution).
bool sceneColorSource(FramePassContext& fc) { return fc.quality.has("output.screen_trace_source") && fc.quality.integer("output.screen_trace_source") == 0; }
// (this recording, for the references UpscaleState keeps of the frame's graph)
uint64_t recordSerialOf(FramePassContext& fc) { return fc.trackState ? fc.trackState->recordSerial() : 0; }
} // namespace

TextureRef upscalePreviousColor(FramePassContext& fc, const ViewResources& view)
{
    if (!upscaleActive(fc, view)) return TextureRef{};
    const FrameContext::Upscale& u = fc.frame.upscale;
    UpscaleState& s = fc.state<UpscaleState>("M.upscale");
    if (sceneColorSource(fc))
    {
        // (the size is the texture's own - RenderGraph::desc - neither the output's nor, under dynamic resolution, this
        // frame's internal size: the readers sample it by UV)
        if (!s.scene[0] || s.sceneFresh || u.reset || s.sceneWidth[s.parity] == 0) return TextureRef{};
        if (!(s.previousSceneFrame == RecordKey::of(fc)) || !s.previousScene.valid())
        {
            s.previousScene = fc.graph.importTexture(s.scene[s.parity].Get(),
                                                     { "m.scenecolor (previous)", s.sceneWidth[s.parity], s.sceneHeight[s.parity], 1, 1, s.sceneFormat },
                                                     D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            s.previousSceneFrame = RecordKey::of(fc);
        }
        return s.previousScene;
    }
    // (the history under a lens projection is not the rendered view's picture: no previous colour for the traces -
    // this frame's lens, or the one the history was written under: the frame the lens goes off, temporalUpscale starts
    // the history again and imports it itself)
    if (upscaleLens(fc, view).active || s.lensD != 0.0f || s.lensS != 0.0f) return TextureRef{};
    // (the history's own size: above the output's with output.upscale_tsr_history_percent - readers take the size from
    // RenderGraph::desc and sample by UV)
    uint32_t hw, hh;
    historySize(fc, u.outputWidth, u.outputHeight, hw, hh);
    if (!s.history[s.last] || s.width != hw || s.height != hh || s.fresh || u.reset) return TextureRef{};
    if (!(s.previousFrame == RecordKey::of(fc)) || !s.previous.valid())
    {
        s.previous = fc.graph.importTexture(s.history[s.last].Get(), { "m.upscale.history (previous)", hw, hh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                            D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        s.previousFrame = RecordKey::of(fc);
    }
    return s.previous;
}

void keepSceneColor(FramePassContext& fc, const ViewResources& view, TextureRef lit)
{
    if (!upscaleActive(fc, view) || !sceneColorSource(fc) || !view.depth.valid()) return;
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height;
    UpscaleState& s = fc.state<UpscaleState>("M.upscale");
    // slot parity ^ 1: temporalUpscale, later in this frame, makes it the current one (read as scene[parity] next frame)
    s.ensureScene(fc.device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, s.parity ^ 1u);
    const TextureRef kept = g.importTexture(s.scene[s.parity ^ 1u].Get(), { "m.scenecolor", w, h, 1, 1, s.sceneFormat }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const TextureRef depth = view.depth;
    const FrameResources& r = fc.resources;
    const bool air = r.transmittanceLut.valid() && r.multiScatterLut.valid() && view.airVolume.valid();
    const TextureRef transmittance = r.transmittanceLut, multiScatter = r.multiScatterLut, airVolume = view.airVolume;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/UpscaleSceneKeep");
    g.addPass("m.scenecolor", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(lit, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(kept, Use::UavCompute);
                  if (air)
                      for (TextureRef t : { transmittance, multiScatter, airVolume }) b.use(t, Use::SrvCompute);
                  if (air) declareFog(b, fc.resources, Use::SrvCompute);
                  b.keep();
              },
              [=](PassContext& c) {
                  const uint32_t none = 0xFFFFFFFFu;
                  const uint32_t k[12] = { c.srv(lit), c.srv(depth), c.uav(kept), 0, w, h, 0, 0,
                                           air ? c.srv(transmittance) : none, air ? c.srv(multiScatter) : none, air ? c.srv(airVolume) : none, 0 };
                  c.cmd->SetPipelineState(pso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 12);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
    s.sceneFresh = false;
}

UpscaleMotion upscaleMotion(FramePassContext& fc, const ViewResources& view)
{
    if (!upscaleActive(fc, view) || !view.depth.valid()) return UpscaleMotion{};
    UpscaleState& s = fc.state<UpscaleState>("M.upscale");
    if (s.motionFrame == RecordKey::of(fc) && s.motion.motion.valid()) return s.motion;
    const FrameContext::Upscale& u = fc.frame.upscale;
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height;
    const bool tsr = tsrOn(fc);
    const TextureRef motion = g.createTexture(TextureDesc{ "m.upscale.motion", w, h, 1, 1, DXGI_FORMAT_R32G32_FLOAT });
    const TextureRef previousDepth = tsr ? g.createTexture(TextureDesc{ "m.upscale.previous depth", w, h, 1, 1, DXGI_FORMAT_R32G32_FLOAT }) : TextureRef{};
    const TextureRef depth = view.depth, vis = view.visId;
    const BufferRef clusters = view.visibleClusters;
    const bool hasVis = vis.valid() && clusters.valid();
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const float jx = u.jitterX, jy = u.jitterY;
    const float4x4 prevViewProj = u.prevViewProj;
    // output.upscale_layer_motion (with upscale_tsr): the layers over the opaque surface - glass, water, the coverage
    // layer's thin fragments - give the pixel their own vector and depth, and the layers without a vector (particles,
    // see-through fragments) mark it (UpscaleMotion.hlsl); the reference's translucent velocity and its
    // has-pixel-animation mark. Off: every pixel moves as its opaque surface.
    const bool layerMotion = tsr && fc.quality.has("output.upscale_layer_motion") && fc.quality.boolean("output.upscale_layer_motion");
    const TextureRef trackedDepth = layerMotion ? g.createTexture(TextureDesc{ "m.upscale.tracked depth", w, h, 1, 1, DXGI_FORMAT_R32_FLOAT }) : TextureRef{};
    const TextureRef layers = layerMotion ? g.createTexture(TextureDesc{ "m.upscale.layers", w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM }) : TextureRef{};
    const bool glass = layerMotion && hasVis && view.translucentVis.valid() && view.translucentDepth.valid() && view.translucentClass.valid();
    const bool water = layerMotion && view.waterDepth.valid();
    const bool coverage = layerMotion && view.coverageTiles.valid() && view.coverageTilePixels.valid() && view.coverageRecords.valid() &&
                          view.coverageDepthRange.valid() && view.coverageTilesX != 0;
    const bool particles = layerMotion && view.particleLayer.valid() && view.particleEdges.valid();
    const TextureRef glassVis = view.translucentVis, glassDepth = view.translucentDepth, glassClass = view.translucentClass, waterDepth = view.waterDepth;
    const TextureRef coverageRange = view.coverageDepthRange, particleLayer = view.particleLayer;
    const BufferRef coverageTiles = view.coverageTiles, coveragePixels = view.coverageTilePixels, coverageRecords = view.coverageRecords;
    const BufferRef particleEdges = view.particleEdges;
    // (the particles' own vector where they hold the pixel: FX's layer motion and depth range)
    const bool particleVectors = particles && view.particleMotion.valid() && view.particleDepthRange.valid();
    const TextureRef particleMotion = view.particleMotion, particleRange = view.particleDepthRange;
    const uint32_t coverageTilesX = view.coverageTilesX;
    ID3D12PipelineState* motionPso = fc.shaders.compute("Passes/Shading/UpscaleMotion");
    // (the coverage records a pixel's layers are read from are bounded - UpscaleMotion.hlsl LAYER_FRAGMENTS: the pixels
    // past the bound are counted)
    const BufferRef status = coverage ? g.createBuffer(BufferDesc{ "m.upscale.motion status", 16, 0 }) : BufferRef{};
    if (coverage)
    {
        ID3D12PipelineState* clear = fc.shaders.compute("Passes/Shading/ExposureClear");  // (zeroes a raw buffer)
        g.addPass("m.upscale.motion.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(status, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(status), 4, 0, 0 };
                      c.cmd->SetPipelineState(clear);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    }
    g.addPass("m.upscale.motion", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  if (coverage) b.use(status, Use::UavCompute);
                  if (hasVis)
                  {
                      b.use(vis, Use::SrvCompute);
                      b.use(clusters, Use::SrvCompute);
                  }
                  b.use(motion, Use::UavCompute);
                  if (tsr) b.use(previousDepth, Use::UavCompute);
                  if (layerMotion)
                  {
                      b.use(trackedDepth, Use::UavCompute);
                      b.use(layers, Use::UavCompute);
                  }
                  if (glass)
                      for (TextureRef t : { glassVis, glassDepth, glassClass }) b.use(t, Use::SrvCompute);
                  if (water) b.use(waterDepth, Use::SrvCompute);
                  if (coverage)
                  {
                      for (BufferRef buffer : { coverageTiles, coveragePixels, coverageRecords }) b.use(buffer, Use::SrvCompute);
                      b.use(coverageRange, Use::SrvCompute);
                  }
                  if (particles)
                  {
                      b.use(particleLayer, Use::SrvCompute);
                      b.use(particleEdges, Use::SrvCompute);
                  }
                  if (particleVectors)
                  {
                      b.use(particleMotion, Use::SrvCompute);
                      b.use(particleRange, Use::SrvCompute);
                  }
              },
              [=](PassContext& c) {
                  const uint32_t none = 0xFFFFFFFFu;
                  uint32_t k[44] = { hasVis ? c.srv(vis) : none, hasVis ? c.srv(clusters) : none, c.srv(depth), c.uav(motion), w, h, asUint(jx), asUint(jy) };
                  for (int r = 0; r < 4; ++r)
                      for (int col = 0; col < 4; ++col) k[8 + 4 * r + col] = asUint(prevViewProj.m[r][col]);
                  k[24] = tsr ? c.uav(previousDepth) : none;
                  k[25] = layerMotion ? c.uav(trackedDepth) : none;
                  k[26] = layerMotion ? c.uav(layers) : none;
                  k[27] = coverageTilesX;
                  k[28] = glass ? c.srv(glassVis) : none;
                  k[29] = glass ? c.srv(glassDepth) : none;
                  k[30] = glass ? c.srv(glassClass) : none;
                  k[31] = water ? c.srv(waterDepth) : none;
                  k[32] = coverage ? c.srv(coverageTiles) : none;
                  k[33] = coverage ? c.srv(coveragePixels) : none;
                  k[34] = coverage ? c.srv(coverageRecords) : none;
                  k[35] = coverage ? c.srv(coverageRange) : none;
                  k[36] = particles ? c.srv(particleLayer) : none;
                  k[37] = particles ? c.srv(particleEdges) : none;
                  k[38] = particleVectors ? c.srv(particleMotion) : none;
                  k[39] = particleVectors ? c.srv(particleRange) : none;
                  k[40] = coverage ? c.uav(status) + 1 : 0;
                  c.cmd->SetPipelineState(motionPso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 44);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
    // (the surface the vectors are of: the layers' depth where a layer has the pixel)
    const TextureRef motionDepth = layerMotion ? trackedDepth : depth;
    UpscaleMotion out;
    out.motion = motion;
    out.depth = motionDepth;
    out.previousDepth = previousDepth;
    out.layers = layers;
    out.status = status;
    out.layerMotion = layerMotion;
    s.motion = out;
    s.motionFrame = RecordKey::of(fc);
    return out;
}

TextureRef temporalUpscale(FramePassContext& fc, const ViewResources& view, TextureRef src, UpscaleProducts* products)
{
    if (!view.depth.valid()) fail("M.upscale: the main view has no depth");
    const FrameContext::Upscale& u = fc.frame.upscale;
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height, W = u.outputWidth, H = u.outputHeight;
    const int64_t frames = fc.quality.has("output.upscale_history_frames") ? fc.quality.integer("output.upscale_history_frames") : 64;
    const int64_t framesMoving = fc.quality.has("output.upscale_history_frames_moving") ? fc.quality.integer("output.upscale_history_frames_moving") : 4;
    const double kernel = fc.quality.has("output.upscale_kernel") ? fc.quality.number("output.upscale_kernel") : 60.0;
    if (frames < 1 || frames > 256 || framesMoving < 1 || framesMoving > frames) fail("output.upscale_history_frames(_moving) must be in [1, 256], moving <= still");
    if (!(kernel >= 1 && kernel <= 1000)) fail("output.upscale_kernel must be in [1, 1000]");
    const bool tsr = tsrOn(fc);
    // output.upscale_tsr_resurrection (the reference's r.TSR.Resurrection): frames kept in the history's ring; a pixel
    // whose history is rejected or hidden takes the kept frame's where that matches this frame (TsrResurrect.hlsl)
    const bool resurrection = tsr && fc.quality.has("output.upscale_tsr_resurrection") && fc.quality.boolean("output.upscale_tsr_resurrection");
    uint32_t HW, HH;
    historySize(fc, W, H, HW, HH);
    UpscaleState& s = fc.state<UpscaleState>("M.upscale");
    s.ensure(fc.device, HW, HH, resurrection);
    if (tsr) s.ensureGuide(fc.device, w, h, resurrection);
    // output.lens_panini_d: the history is the lens picture (Lens.hlsli); another lens, another history
    const LensProjection lens = upscaleLens(fc, view);
    const float lensD = lens.active ? lens.d : 0.0f, lensS = lens.active ? lens.s : 0.0f;
    const bool lensChanged = s.lensD != lensD || s.lensS != lensS;
    s.lensD = lensD;
    s.lensS = lensS;
    const bool reset = u.reset || s.fresh || lensChanged;
    // (an earlier pass of this frame may hold the last frame's slot already: upscalePreviousColor - one import of a
    // resource per frame, also on a frame that resets here for a reason upscalePreviousColor does not see - the lens)
    const bool held = s.previousFrame == RecordKey::of(fc) && s.previous.valid();
    const uint32_t heldSlot = s.last;
    s.fresh = false;
    const uint32_t prev = s.parity, next = prev ^ 1u;  // (the two-frame histories: flickering, thin coverage, keepSceneColor's)
    s.parity = next;
    // The ring: this frame's slot, the previous frame's, and the kept frame a pixel may be resurrected from.
    const uint32_t cycle = resurrection ? kResurrectionCycle : 2u;
    if (reset)
    {
        s.accumulated = 0;
        for (UpscaleState::Kept& kept : s.kept) kept.valid = false;
    }
    else if (fc.frame.originShift.x != 0 || fc.frame.originShift.y != 0 || fc.frame.originShift.z != 0)
    {
        // C9 origin rebase: the kept views in the new coordinates (a point p now was p + shift then; FrameRenderer does
        // the same to the previous frame's view)
        for (UpscaleState::Kept& kept : s.kept)
            for (int r = 0; r < 4; ++r)
                kept.viewProj.m[r][3] += kept.viewProj.m[r][0] * fc.frame.originShift.x + kept.viewProj.m[r][1] * fc.frame.originShift.y +
                                         kept.viewProj.m[r][2] * fc.frame.originShift.z;
    }
    const uint32_t rolling = reset ? 0u : (s.rolling + 1u) % cycle;
    const uint32_t slot = ringSlot(rolling, resurrection);
    // (a reset frame's history is bound and not read: a slot other than this frame's that exists with or without the
    // kept slots - the last slot may be this frame's, or one a ring without them does not have)
    const uint32_t prevSlot = reset ? (slot == 0 ? 3u : 0u) : s.last;
    const float exposure = 1.0f / (1.2f * std::exp2(view.view.ev100));
    uint32_t keptSlot = prevSlot;
    bool canResurrect = false;
    float4x4 toKept{};
    float keptExposureRatio = 1;
    if (resurrection && !reset)
    {
        // the newest kept frame that is not the previous frame (the reference's GetResurrectionFrameRollingIndex; before
        // the cycle's second kept frame exists, the first)
        uint32_t keptRolling = ((s.rolling + 2 * kResurrectionPeriod - 1) / kResurrectionPeriod * kResurrectionPeriod) % cycle;
        if (!s.kept[ringSlot(keptRolling, true)].valid) keptRolling = 0;
        keptSlot = ringSlot(keptRolling, true);
        const UpscaleState::Kept& kept = s.kept[keptSlot];
        canResurrect = keptSlot != prevSlot && keptSlot != slot && kept.valid && kept.exposure > 0;
        if (canResurrect)
        {
            toKept = mul(kept.viewProj, inverse(u.viewProj));  // this frame's unjittered clip space to the kept frame's
            keptExposureRatio = exposure / kept.exposure;
        }
    }
    s.rolling = rolling;
    s.accumulated = std::min(s.accumulated + 1u, cycle);
    s.last = slot;
    // (the lens the previous and the kept frame's histories were written under: the field of view may have changed)
    const UpscaleState::Kept lensPrevious = s.kept[prevSlot], lensKept = s.kept[keptSlot];
    s.kept[slot].viewProj = u.viewProj;
    s.kept[slot].exposure = exposure;
    s.kept[slot].valid = true;
    s.kept[slot].lensTanX = lens.tanX;
    s.kept[slot].lensTanY = lens.tanY;
    s.kept[slot].lensScale = lens.active ? lens.scale : 0.0f;
    const TextureRef history = held && prevSlot == heldSlot ? s.previous
                                                            : g.importTexture(s.history[prevSlot].Get(), { "m.upscale.history (previous)", HW, HH, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                                                              D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const TextureRef output = held && slot == heldSlot ? s.previous
                                                       : g.importTexture(s.history[slot].Get(), { "m.upscale.history", HW, HH, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                                                         D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    // (m.upscale.motion: recorded here unless a pass before the upscale asked for the vectors - upscaleMotion)
    const UpscaleMotion vectors = upscaleMotion(fc, view);
    s.motionFrame = RecordKey{};  // (this frame's last reader: a later graph records its own)
    const TextureRef motion = vectors.motion, previousDepth = vectors.previousDepth, layers = vectors.layers;
    const bool layerMotion = vectors.layerMotion;
    const TextureRef depth = view.depth;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const float jx = u.jitterX, jy = u.jitterY;
    // (the surface the vectors are of: the layers' depth where a layer has the pixel)
    const TextureRef motionDepth = vectors.depth;
    if (products)
    {
        products->motion = motion;
        products->depth = motionDepth;
        products->status = vectors.status;
    }
    if (tsr)
    {
        // Tsr.hlsli: dilate (+ the closest occluder scatter) -> decimate -> [resurrect] -> reject -> spatial
        // anti-aliasing -> update -> [resolve].
        const bool guideReset = reset;
        // (dynamic resolution: the slots this frame writes take this frame's internal size; the previous frame's guide,
        // flickering and thin-coverage histories are read at the size they were written - TsrDecimate.hlsl reprojects
        // by UV)
        s.resizeGuide(fc.device, slot, next, w, h);
        const uint32_t prevGuideW = s.guideWidth[prevSlot], prevGuideH = s.guideHeight[prevSlot], prevPairW = s.pairWidth[prev], prevPairH = s.pairHeight[prev];
        const TextureRef previousGuide = g.importTexture(s.guide[prevSlot].Get(), { "m.tsr.guide (previous)", prevGuideW, prevGuideH, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM },
                                                         D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const TextureRef nextGuide = g.importTexture(s.guide[slot].Get(), { "m.tsr.guide", w, h, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        // the kept frame: its history and guide, the guide reprojected and its measure against this frame
        TextureRef keptHistory, keptGuide, resurrectedGuide, resurrectionMeasure;
        if (canResurrect)
        {
            keptHistory = g.importTexture(s.history[keptSlot].Get(), { "m.upscale.history (kept)", HW, HH, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                          D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            keptGuide = g.importTexture(s.guide[keptSlot].Get(), { "m.tsr.guide (kept)", s.guideWidth[keptSlot], s.guideHeight[keptSlot], 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM },
                                        D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            resurrectedGuide = g.createTexture(TextureDesc{ "m.tsr.resurrected guide", w, h, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
            resurrectionMeasure = g.createTexture(TextureDesc{ "m.tsr.resurrection measure", w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM });
        }
        ID3D12PipelineState* resurrectPso = canResurrect ? fc.shaders.compute("Passes/Shading/TsrResurrect") : nullptr;
        ID3D12PipelineState* resolvePso = HW != W || HH != H ? fc.shaders.compute("Passes/Shading/TsrResolve") : nullptr;
        const TextureRef scatter = g.createTexture(TextureDesc{ "m.tsr.closest occluder", w, h, 1, 1, DXGI_FORMAT_R32_UINT });
        const TextureRef dilated = g.createTexture(TextureDesc{ "m.tsr.dilated motion", w, h, 1, 1, DXGI_FORMAT_R32G32_FLOAT });
        const TextureRef info = g.createTexture(TextureDesc{ "m.tsr.dilate info", w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        const TextureRef reprojected = g.createTexture(TextureDesc{ "m.tsr.reprojected guide", w, h, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
        const TextureRef decimateMask = g.createTexture(TextureDesc{ "m.tsr.decimate mask", w, h, 1, 1, DXGI_FORMAT_R8G8_UNORM });
        const TextureRef rejection = g.createTexture(TextureDesc{ "m.tsr.rejection", w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM });
        const TextureRef aaInput = g.createTexture(TextureDesc{ "m.tsr.aa input", w, h, 1, 1, DXGI_FORMAT_R8G8_UNORM });
        const TextureRef aa = g.createTexture(TextureDesc{ "m.tsr.aa", w, h, 1, 1, DXGI_FORMAT_R8G8_UINT });
        // output.upscale_tsr_reprojection_field (the reference's r.TSR.ReprojectionField): the vector's jacobian and the
        // dilation's boundary per internal pixel (Tsr.hlsli); the boundary where the two sides of an edge move apart by
        // more than upscale_tsr_reprojection_field_aa_speed output pixels a frame
        const bool reprojectionField = fc.quality.has("output.upscale_tsr_reprojection_field") && fc.quality.boolean("output.upscale_tsr_reprojection_field");
        const double aaSpeed = fc.quality.has("output.upscale_tsr_reprojection_field_aa_speed") ? fc.quality.number("output.upscale_tsr_reprojection_field_aa_speed") : 0.125;
        if (!(aaSpeed >= 0 && aaSpeed <= 16)) fail("output.upscale_tsr_reprojection_field_aa_speed must be in [0, 16] output pixels a frame");
        // (the field's texture also carries the closest depth the resurrection reprojects by)
        const bool hasField = reprojectionField || canResurrect;
        const TextureRef field = hasField ? g.createTexture(TextureDesc{ "m.tsr.reprojection field", w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT }) : TextureRef{};
        // output.upscale_tsr_hole_filling (the reference's reprojection hole filling, always on there): a disoccluded
        // pixel's history is read by its occluder's vector (TsrDecimate.hlsl writes the update's copy of the vectors)
        const bool holeFilling = !fc.quality.has("output.upscale_tsr_hole_filling") || fc.quality.boolean("output.upscale_tsr_hole_filling");
        const TextureRef updateMotion = holeFilling ? g.createTexture(TextureDesc{ "m.tsr.hole filled motion", w, h, 1, 1, DXGI_FORMAT_R32G32_FLOAT }) : dilated;
        // output.upscale_tsr_flickering (the reference's r.TSR.ShadingRejection.Flickering, default on)
        const bool flickering = !fc.quality.has("output.upscale_tsr_flickering") || fc.quality.boolean("output.upscale_tsr_flickering");
        TextureRef previousFlicker, nextFlicker, reprojectedFlicker, moireError;
        if (flickering)
        {
            previousFlicker = g.importTexture(s.flicker[prev].Get(), { "m.tsr.flicker (previous)", prevPairW, prevPairH, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM },
                                              D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            nextFlicker = g.importTexture(s.flicker[next].Get(), { "m.tsr.flicker", w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            reprojectedFlicker = g.createTexture(TextureDesc{ "m.tsr.reprojected flicker", w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM });
            moireError = g.createTexture(TextureDesc{ "m.tsr.moire error", w, h, 1, 1, DXGI_FORMAT_R16_FLOAT });
        }
        ID3D12PipelineState* flickerPso = flickering ? fc.shaders.compute("Passes/Shading/TsrFlicker") : nullptr;
        // output.upscale_tsr_thin_geometry (the reference's r.TSR.ThinGeometryDetection): TsrThin.hlsl - the coverage
        // layer's thin fragments and pixel-wide lines of depth relax the shading rejection
        const bool thinGeometry = fc.quality.has("output.upscale_tsr_thin_geometry") && fc.quality.boolean("output.upscale_tsr_thin_geometry");
        const double thinErrorMultiplier =
            fc.quality.has("output.upscale_tsr_thin_geometry_error_multiplier") ? fc.quality.number("output.upscale_tsr_thin_geometry_error_multiplier") : 200.0;
        const double thinMaxRelaxation =
            fc.quality.has("output.upscale_tsr_thin_geometry_max_relaxation") ? fc.quality.number("output.upscale_tsr_thin_geometry_max_relaxation") : 0.037;
        if (!(thinErrorMultiplier >= 1 && thinErrorMultiplier <= 1e5) || !(thinMaxRelaxation >= 0 && thinMaxRelaxation <= 1))
            fail("output.upscale_tsr_thin_geometry_*: error multiplier in [1, 1e5], max relaxation in [0, 1]");
        TextureRef previousThin, nextThin, reprojectedThin, relaxation;
        if (thinGeometry)
        {
            previousThin = g.importTexture(s.thin[prev].Get(), { "m.tsr.thin coverage (previous)", prevPairW, prevPairH, 1, 1, DXGI_FORMAT_R8_UNORM },
                                           D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            nextThin = g.importTexture(s.thin[next].Get(), { "m.tsr.thin coverage", w, h, 1, 1, DXGI_FORMAT_R8_UNORM }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            reprojectedThin = g.createTexture(TextureDesc{ "m.tsr.reprojected thin coverage", w, h, 1, 1, DXGI_FORMAT_R8_UNORM });
            relaxation = g.createTexture(TextureDesc{ "m.tsr.thin relaxation", w, h, 1, 1, DXGI_FORMAT_R8_UNORM });
        }
        ID3D12PipelineState* thinPso = thinGeometry ? fc.shaders.compute("Passes/Shading/TsrThin") : nullptr;
        // the luma lines of the thin geometry detection (the reference's r.TSR.ThinGeometryDetection.Coverage.MinKeepLineContrast
        // 0.30, HighContrastLineFadeRate 0.1, ...FadeRateInsideRegion 0.03, HighContrastLineWeight 0.6; contrast 0: none) and
        // the detection inside the flickering heuristic (its r.TSR.ThinGeometryDetection.AntiFlickering)
        auto thinNumber = [&](const char* key, double fallback) {
            const double v = fc.quality.has(key) ? fc.quality.number(key) : fallback;
            if (!(v >= 0 && v <= 1)) fail("%s %g: in [0, 1]", key, v);
            return (float)v;
        };
        const float lineContrast = thinNumber("output.upscale_tsr_thin_geometry_line_contrast", 0.30);
        const float lineFade = thinNumber("output.upscale_tsr_thin_geometry_line_fade_rate", 0.1);
        const float lineFadeInside = thinNumber("output.upscale_tsr_thin_geometry_line_fade_rate_inside", 0.03);
        const float lineWeight = thinNumber("output.upscale_tsr_thin_geometry_line_weight", 0.6);
        const bool lumaLines = thinGeometry && lineContrast > 0;
        const bool thinInFlicker = thinGeometry && flickering &&
                                   (!fc.quality.has("output.upscale_tsr_thin_geometry_anti_flickering") || fc.quality.boolean("output.upscale_tsr_thin_geometry_anti_flickering"));
        const uint32_t frameIndex = (uint32_t)fc.frame.frameIndex;
        ShaderLibrary& shaders = fc.shaders;
        ID3D12PipelineState* clearPso = shaders.compute("Passes/Shading/TsrClear");
        ID3D12PipelineState* dilatePso = shaders.compute("Passes/Shading/TsrDilate");
        ID3D12PipelineState* decimatePso = shaders.compute("Passes/Shading/TsrDecimate");
        // At 720p/960p retain the original path unless every optional input
        // matches the measured all-input class. Availability follows the exact
        // bindings below, including both textures of resurrection.
        const uint32_t rejectionOptionals =
            (flickering && moireError.valid() ? detail::TsrRejectMoire : 0u) |
            (thinGeometry && relaxation.valid() ? detail::TsrRejectThin : 0u) |
            (layerMotion && layers.valid() ? detail::TsrRejectLayers : 0u) |
            (canResurrect && resurrectionMeasure.valid() && resurrectedGuide.valid() ? detail::TsrRejectResurrection : 0u);
        const bool fusedRejection = detail::useFusedTsrRejection(w, h, rejectionOptionals);
        ID3D12PipelineState* rejectPrefixPso = fusedRejection ? shaders.compute("Passes/Shading/TsrRejectPrefix") : nullptr;
        ID3D12PipelineState* rejectPso = shaders.compute(fusedRejection ? "Passes/Shading/TsrRejectFromPrefix" : "Passes/Shading/TsrReject");
        ID3D12PipelineState* aaPso = shaders.compute("Passes/Shading/TsrAntiAlias");
        ID3D12PipelineState* updatePso = shaders.compute("Passes/Shading/TsrUpdate");
        const uint32_t updateFlags = (fc.quality.has("output.upscale_tsr_kernel_by_samples") && fc.quality.boolean("output.upscale_tsr_kernel_by_samples") ? 2u : 0u) |
                                     (reprojectionField ? 4u : 0u) | (lens.active ? 8u : 0u);
        const float historyScale = (float)HW / (float)W;
        const float exposureRatio = u.exposureRatio;
        // the guide's blend of a held history: 1 / (1 + 16 / (input / output size)^2) (the reference's TheoricBlendFactor)
        const float fraction = (float)w / (float)W;
        const float theoreticBlend = 1.0f / (1.0f + 16.0f / (fraction * fraction));
        // (the reference's ReprojectionFieldAntiAliasVelocityThreshold: the speed in input pixels, at least 1 / 64, squared)
        const float boundarySpeed = std::max((float)aaSpeed * fraction, 1.0f / 64.0f), boundaryThreshold = boundarySpeed * boundarySpeed;
        g.addPass("m.tsr.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(scatter, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(scatter), w, h, 0 };
                      c.cmd->SetPipelineState(clearPso);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                  });
        g.addPass("m.tsr.dilate", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(motionDepth, Use::SrvCompute);
                      b.use(motion, Use::SrvCompute);
                      b.use(previousDepth, Use::SrvCompute);
                      b.use(dilated, Use::UavCompute);
                      b.use(info, Use::UavCompute);
                      b.use(scatter, Use::UavCompute);
                      if (hasField) b.use(field, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[12] = { c.srv(motionDepth), c.srv(motion), c.srv(previousDepth), c.uav(dilated), c.uav(info), c.uav(scatter), w, h,
                                               hasField ? c.uav(field) : 0xFFFFFFFFu, reprojectionField ? 1u : 0u, asUint(boundaryThreshold), 0 };
                      c.cmd->SetPipelineState(dilatePso);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 12);
                      c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                  });
        g.addPass("m.tsr.decimate", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(dilated, Use::SrvCompute);
                      b.use(info, Use::SrvCompute);
                      b.use(scatter, Use::SrvCompute);
                      b.use(previousGuide, Use::SrvCompute);
                      b.use(reprojected, Use::UavCompute);
                      b.use(decimateMask, Use::UavCompute);
                      if (flickering)
                      {
                          b.use(previousFlicker, Use::SrvCompute);
                          b.use(reprojectedFlicker, Use::UavCompute);
                      }
                      if (thinGeometry)
                      {
                          b.use(previousThin, Use::SrvCompute);
                          b.use(reprojectedThin, Use::UavCompute);
                      }
                      if (holeFilling) b.use(updateMotion, Use::UavCompute);
                      if (canResurrect)
                      {
                          b.use(keptGuide, Use::SrvCompute);
                          b.use(field, Use::SrvCompute);
                          b.use(resurrectedGuide, Use::UavCompute);
                      }
                  },
                  [=](PassContext& c) {
                      const uint32_t none = 0xFFFFFFFFu;
                      uint32_t k[40] = { c.srv(dilated), c.srv(info), c.srv(scatter), c.srv(previousGuide), c.uav(reprojected), c.uav(decimateMask), w, h,
                                         asUint(jx), asUint(jy), asUint(exposureRatio), guideReset ? 1u : 0u,
                                         flickering ? c.srv(previousFlicker) : none, flickering ? c.uav(reprojectedFlicker) : none, frameIndex,
                                         prevGuideW | (prevGuideH << 16),
                                         thinGeometry ? c.srv(previousThin) : none, thinGeometry ? c.uav(reprojectedThin) : none,
                                         holeFilling ? c.uav(updateMotion) : none, prevPairW | (prevPairH << 16),
                                         canResurrect ? c.srv(keptGuide) : none, canResurrect ? c.uav(resurrectedGuide) : none, asUint(keptExposureRatio),
                                         canResurrect ? c.srv(field) : none };
                      for (int r = 0; r < 4; ++r)
                          for (int col = 0; col < 4; ++col) k[24 + 4 * r + col] = asUint(toKept.m[r][col]);
                      c.cmd->SetPipelineState(decimatePso);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 40);
                      c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                  });
        if (canResurrect)
            g.addPass("m.tsr.resurrect", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(src, Use::SrvCompute);
                          b.use(reprojected, Use::SrvCompute);
                          b.use(resurrectedGuide, Use::SrvCompute);
                          b.use(decimateMask, Use::SrvCompute);
                          b.use(resurrectionMeasure, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[8] = { c.srv(src), c.srv(reprojected), c.srv(resurrectedGuide), c.srv(decimateMask), c.uav(resurrectionMeasure), w, h, 0 };
                          c.cmd->SetPipelineState(resurrectPso);
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
                      });
        if (thinGeometry)
            g.addPass("m.tsr.thin", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          if (layerMotion) b.use(layers, Use::SrvCompute);
                          if (lumaLines) b.use(src, Use::SrvCompute);
                          b.use(motionDepth, Use::SrvCompute);
                          b.use(reprojectedThin, Use::SrvCompute);
                          b.use(decimateMask, Use::SrvCompute);
                          b.use(relaxation, Use::UavCompute);
                          b.use(nextThin, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[20] = { layerMotion ? c.srv(layers) : 0xFFFFFFFFu, c.srv(motionDepth), c.srv(reprojectedThin), c.srv(decimateMask),
                                                   c.uav(relaxation), c.uav(nextThin), w, h,
                                                   asUint((float)thinErrorMultiplier), asUint((float)thinMaxRelaxation), frameIndex, guideReset ? 1u : 0u,
                                                   lumaLines ? c.srv(src) : 0xFFFFFFFFu, asUint(lineContrast), asUint(lineFade), asUint(lineFadeInside),
                                                   asUint(lineWeight), 0, 0, 0 };
                          c.cmd->SetPipelineState(thinPso);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 20);
                          c.cmd->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
                      });
        if (flickering)
            g.addPass("m.tsr.flicker", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(src, Use::SrvCompute);
                          b.use(reprojectedFlicker, Use::SrvCompute);
                          b.use(decimateMask, Use::SrvCompute);
                          b.use(info, Use::SrvCompute);
                          if (thinInFlicker) b.use(relaxation, Use::SrvCompute);
                          b.use(moireError, Use::UavCompute);
                          b.use(nextFlicker, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[12] = { c.srv(src), c.srv(reprojectedFlicker), c.srv(decimateMask), c.srv(info), c.uav(moireError), c.uav(nextFlicker), w, h,
                                                   guideReset ? 1u : 0u, thinInFlicker ? c.srv(relaxation) : 0xFFFFFFFFu, 0, 0 };
                          c.cmd->SetPipelineState(flickerPso);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 12);
                          c.cmd->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
                      });
        const uint32_t rejectionBegin = g.passCount();
        const TextureRef rejectionPrefix = fusedRejection
            ? g.createTexture({ "m.tsr.rejection prefix", w + 10, h + 10, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT }) : TextureRef{};
        if (fusedRejection)
            g.addPass("m.tsr.reject.prepare", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(src, Use::SrvCompute);
                          b.use(reprojected, Use::SrvCompute);
                          b.use(rejectionPrefix, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[8] = { c.srv(src), c.srv(reprojected), c.uav(rejectionPrefix), w, h, 0, 0, 0 };
                          c.cmd->SetPipelineState(rejectPrefixPso);
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch((w + 25) / 16, (h + 25) / 16, 1);
                      });
        g.addPass("m.tsr.reject", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(src, Use::SrvCompute);
                      b.use(reprojected, Use::SrvCompute);
                      b.use(decimateMask, Use::SrvCompute);
                      b.use(rejection, Use::UavCompute);
                      b.use(nextGuide, Use::UavCompute);
                      b.use(aaInput, Use::UavCompute);
                      if (fusedRejection) b.use(rejectionPrefix, Use::SrvCompute);
                      if (flickering) b.use(moireError, Use::SrvCompute);
                      if (layerMotion) b.use(layers, Use::SrvCompute);
                      if (thinGeometry) b.use(relaxation, Use::SrvCompute);
                      if (canResurrect)
                      {
                          b.use(resurrectionMeasure, Use::SrvCompute);
                          b.use(resurrectedGuide, Use::SrvCompute);
                      }
                  },
                  [=](PassContext& c) {
                      const uint32_t none = 0xFFFFFFFFu;
                      const uint32_t k[16] = { c.srv(src), c.srv(reprojected), c.srv(decimateMask), c.uav(rejection), c.uav(nextGuide), c.uav(aaInput), w, h,
                                               asUint(theoreticBlend), flickering ? c.srv(moireError) : none,
                                               thinGeometry ? c.srv(relaxation) : none, layerMotion ? c.srv(layers) : none,
                                               canResurrect ? c.srv(resurrectionMeasure) : none, canResurrect ? c.srv(resurrectedGuide) : none,
                                               fusedRejection ? c.srv(rejectionPrefix) : none, 0 };
                      c.cmd->SetPipelineState(rejectPso);
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
                  });
        // Preserve the existing profiler meaning: rejection includes preparation
        // and its barrier, not merely the shorter tail kernel.
        if (fusedRejection) g.joinPasses(rejectionBegin, 2, "m.tsr.reject");
        g.addPass("m.tsr.aa", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(aaInput, Use::SrvCompute);
                      b.use(aa, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.srv(aaInput), c.uav(aa), w, h };
                      c.cmd->SetPipelineState(aaPso);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                  });
        g.addPass("m.upscale", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(src, Use::SrvCompute);
                      b.use(rejection, Use::SrvCompute);
                      b.use(updateMotion, Use::SrvCompute);
                      b.use(aa, Use::SrvCompute);
                      b.use(history, Use::SrvCompute);
                      b.use(output, Use::UavCompute);
                      if (hasField) b.use(field, Use::SrvCompute);
                      if (canResurrect) b.use(keptHistory, Use::SrvCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t none = 0xFFFFFFFFu;
                      uint32_t k[48] = { c.srv(src), c.srv(rejection), c.srv(updateMotion), c.srv(history), c.uav(output), w, h, (reset ? 1u : 0u) | updateFlags,
                                         HW, HH, asUint(jx), asUint(jy), asUint(exposureRatio), c.srv(aa), hasField ? c.srv(field) : none, 0,
                                         canResurrect ? c.srv(keptHistory) : none, asUint(keptExposureRatio), asUint(historyScale), 0 };
                      for (int r = 0; r < 4; ++r)
                          for (int col = 0; col < 4; ++col) k[20 + 4 * r + col] = asUint(toKept.m[r][col]);
                      const float lensConstants[12] = { lens.tanX, lens.tanY, lens.d, lens.s,
                                                        lens.scale, lensPrevious.lensTanX, lensPrevious.lensTanY, lensPrevious.lensScale,
                                                        lensKept.lensTanX, lensKept.lensTanY, lensKept.lensScale, 0 };
                      std::memcpy(&k[36], lensConstants, sizeof lensConstants);
                      c.cmd->SetPipelineState(updatePso);
                      c.computeConstants(k, 48);
                      c.cmd->Dispatch((HW + 7) / 8, (HH + 7) / 8, 1);
                  });
        if (!resolvePso) return output;
        // the history above the output resolution, filtered down (TsrResolve.hlsl)
        const TextureRef resolved = g.createTexture(TextureDesc{ "m.upscale.resolved", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        g.addPass("m.tsr.resolve", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(output, Use::SrvCompute);
                      b.use(resolved, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[8] = { c.srv(output), c.uav(resolved), W, H, HW, HH, 0, 0 };
                      c.cmd->SetPipelineState(resolvePso);
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                  });
        return resolved;
    }
    const float ratio = u.exposureRatio, capStill = (float)frames, capMoving = (float)framesMoving, kernelK = (float)kernel;
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/Upscale");
    g.addPass("m.upscale", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(src, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(motion, Use::SrvCompute);
                  b.use(history, Use::SrvCompute);
                  b.use(output, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[16] = { c.srv(src), c.srv(depth), c.srv(motion), c.srv(history), c.uav(output), w, h, reset ? 1u : 0u,
                                           W, H, asUint(jx), asUint(jy), asUint(ratio), asUint(capStill), asUint(capMoving), asUint(kernelK) };
                  c.cmd->SetPipelineState(pso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 16);
                  c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
              });
    return output;
}
} // namespace unx::render::shading
