// Temporal upscale of M (user decisions 2026-09-28: internal resolution + temporal upscale for the 4K budget, then for
// 1440p and 1080p; output.render_scale, output.render_height_max, FrameContext::Upscale set by
// FrameRenderer::setupUpscale). The main view renders at the output height x render_scale (at most render_height_max)
// with a Halton (2, 3) sub-pixel jitter of its projection; every system of the frame sees that internal view, and M's
// texture footprints are the output pixel's (FrameConstants::upscaleRatio). After the shading chain (haze, motion blur, depth of field at the internal resolution) two passes
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

#include "unx/core/Config.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <algorithm>
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

struct UpscaleState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> history[2];
    uint32_t width = 0, height = 0, parity = 0;
    bool fresh = true;  // the textures hold nothing yet
    TextureRef previous;           // history[parity] in the graph of frame 'previousFrame' (upscalePreviousColor)
    uint64_t previousFrame = ~0ull;
    // output.screen_trace_source = 0: the lit opaque scene colour (the view's resolution; before translucency, the air,
    // the upscale and the display transform), kept for the next frame's screen-space traces - the reference's default
    // source (keepSceneColor, UpscaleSceneKeep.hlsl). scene[parity] is the last one written.
    ComPtr<ID3D12Resource> scene[2];
    uint32_t sceneWidth = 0, sceneHeight = 0;
    DXGI_FORMAT sceneFormat = DXGI_FORMAT_UNKNOWN;
    bool sceneFresh = true;
    TextureRef previousScene;
    uint64_t previousSceneFrame = ~0ull;
    void ensureScene(Device& d, uint32_t w, uint32_t h, DXGI_FORMAT format)
    {
        if (scene[0] && sceneWidth == w && sceneHeight == h && sceneFormat == format) return;
        device = &d;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (int k = 0; k < 2; ++k)
        {
            if (scene[k]) d.deferRelease(scene[k]);
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(scene[k].ReleaseAndGetAddressOf())),
                  "M previous scene colour");
            scene[k]->SetName(k ? L"M previous scene colour 1" : L"M previous scene colour 0");
        }
        sceneWidth = w;
        sceneHeight = h;
        sceneFormat = format;
        sceneFresh = true;
    }
    // output.upscale_tsr (Tsr.hlsli): the guide history at the internal resolution (R10G10B10A2: the scene colour in
    // the guide space at low frequency, a = the reprojection edge); guide[parity] is the last one written.
    ComPtr<ID3D12Resource> guide[2];
    ComPtr<ID3D12Resource> flicker[2];  // the flickering heuristic's history (RGBA8, TsrFlicker.hlsl), as the guide
    uint32_t guideWidth = 0, guideHeight = 0;
    bool guideFresh = true;
    void ensureGuide(Device& d, uint32_t w, uint32_t h)
    {
        if (guide[0] && guideWidth == w && guideHeight == h) return;
        device = &d;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (int k = 0; k < 2; ++k)
        {
            if (guide[k]) d.deferRelease(guide[k]);
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(guide[k].ReleaseAndGetAddressOf())),
                  "M upscale guide");
            guide[k]->SetName(k ? L"M upscale guide 1" : L"M upscale guide 0");
            if (flicker[k]) d.deferRelease(flicker[k]);
            D3D12_RESOURCE_DESC1 fd = desc;
            fd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &fd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(flicker[k].ReleaseAndGetAddressOf())),
                  "M upscale flickering history");
            flicker[k]->SetName(k ? L"M upscale flickering history 1" : L"M upscale flickering history 0");
        }
        guideWidth = w;
        guideHeight = h;
        guideFresh = true;
    }
    ~UpscaleState()
    {
        if (!device) return;
        for (auto& t : guide)
            if (t) device->deferRelease(t);
        for (auto& t : flicker)
            if (t) device->deferRelease(t);
        for (auto& t : history)
            if (t) device->deferRelease(t);
        for (auto& t : scene)
            if (t) device->deferRelease(t);
    }
    void ensure(Device& d, uint32_t w, uint32_t h)
    {
        if (history[0] && width == w && height == h) return;
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
        for (int k = 0; k < 2; ++k)
        {
            if (history[k]) d.deferRelease(history[k]);
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(&history[k])),
                  "M upscale history");
            history[k]->SetName(k ? L"M upscale history 1" : L"M upscale history 0");
        }
        width = w;
        height = h;
        fresh = true;
    }
};
} // namespace

bool upscaleActive(FramePassContext& fc, const ViewResources& view)
{
    return view.view.kind == gpu::ViewKind::Main && fc.frame.upscale.outputWidth != 0 && !fc.frame.outputLinearHdr;
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
} // namespace

TextureRef upscalePreviousColor(FramePassContext& fc, const ViewResources& view)
{
    if (!upscaleActive(fc, view)) return TextureRef{};
    const FrameContext::Upscale& u = fc.frame.upscale;
    UpscaleState& s = fc.state<UpscaleState>("M.upscale");
    if (sceneColorSource(fc))
    {
        // (the size is the texture's own - RenderGraph::desc - not the output's)
        if (!s.scene[0] || s.sceneFresh || u.reset || s.sceneWidth != view.view.width || s.sceneHeight != view.view.height) return TextureRef{};
        if (s.previousSceneFrame != fc.frame.frameIndex || !s.previousScene.valid())
        {
            s.previousScene = fc.graph.importTexture(s.scene[s.parity].Get(), { "m.scenecolor (previous)", s.sceneWidth, s.sceneHeight, 1, 1, s.sceneFormat },
                                                     D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            s.previousSceneFrame = fc.frame.frameIndex;
        }
        return s.previousScene;
    }
    if (!s.history[0] || s.width != u.outputWidth || s.height != u.outputHeight || s.fresh || u.reset) return TextureRef{};
    if (s.previousFrame != fc.frame.frameIndex || !s.previous.valid())
    {
        s.previous = fc.graph.importTexture(s.history[s.parity].Get(),
                                            { "m.upscale.history (previous)", u.outputWidth, u.outputHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                            D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        s.previousFrame = fc.frame.frameIndex;
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
    s.ensureScene(fc.device, w, h, DXGI_FORMAT_R16G16B16A16_FLOAT);
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

TextureRef temporalUpscale(FramePassContext& fc, const ViewResources& view, TextureRef src)
{
    if (!view.depth.valid()) fail("M.upscale: the main view has no depth");
    const FrameContext::Upscale& u = fc.frame.upscale;
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height, W = u.outputWidth, H = u.outputHeight;
    const int64_t frames = fc.quality.has("output.upscale_history_frames") ? fc.quality.integer("output.upscale_history_frames") : 64;
    const int64_t framesMoving = fc.quality.has("output.upscale_history_frames_moving") ? fc.quality.integer("output.upscale_history_frames_moving") : 8;
    const double kernel = fc.quality.has("output.upscale_kernel") ? fc.quality.number("output.upscale_kernel") : 60.0;
    if (frames < 1 || frames > 256 || framesMoving < 1 || framesMoving > frames) fail("output.upscale_history_frames(_moving) must be in [1, 256], moving <= still");
    if (!(kernel >= 1 && kernel <= 1000)) fail("output.upscale_kernel must be in [1, 1000]");
    UpscaleState& s = fc.state<UpscaleState>("M.upscale");
    s.ensure(fc.device, W, H);
    const bool reset = u.reset || s.fresh;
    // (an earlier pass of this frame may hold the previous history already: upscalePreviousColor - one import per frame)
    const bool imported = !reset && s.previousFrame == fc.frame.frameIndex && s.previous.valid();
    s.fresh = false;
    const uint32_t prev = s.parity, next = prev ^ 1u;
    s.parity = next;
    const TextureRef history = imported ? s.previous
                                        : g.importTexture(s.history[prev].Get(), { "m.upscale.history (previous)", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                                          D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const TextureRef output = g.importTexture(s.history[next].Get(), { "m.upscale.history", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                              D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const TextureRef motion = g.createTexture(TextureDesc{ "m.upscale.motion", w, h, 1, 1, DXGI_FORMAT_R32G32_FLOAT });
    // output.upscale_tsr: the temporal super resolution's structure (Tsr.hlsli) in place of the one-pass accumulation
    const bool tsr = !fc.quality.has("output.upscale_tsr") || fc.quality.boolean("output.upscale_tsr");
    const TextureRef previousDepth = tsr ? g.createTexture(TextureDesc{ "m.upscale.previous depth", w, h, 1, 1, DXGI_FORMAT_R32G32_FLOAT }) : TextureRef{};
    const TextureRef depth = view.depth, vis = view.visId;
    const BufferRef clusters = view.visibleClusters;
    const bool hasVis = vis.valid() && clusters.valid();
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const float jx = u.jitterX, jy = u.jitterY;
    const float4x4 prevViewProj = u.prevViewProj;
    ID3D12PipelineState* motionPso = fc.shaders.compute("Passes/Shading/UpscaleMotion");
    g.addPass("m.upscale.motion", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  if (hasVis)
                  {
                      b.use(vis, Use::SrvCompute);
                      b.use(clusters, Use::SrvCompute);
                  }
                  b.use(motion, Use::UavCompute);
                  if (tsr) b.use(previousDepth, Use::UavCompute);
              },
              [=](PassContext& c) {
                  uint32_t k[28] = { hasVis ? c.srv(vis) : 0xFFFFFFFFu, hasVis ? c.srv(clusters) : 0xFFFFFFFFu, c.srv(depth), c.uav(motion), w, h, asUint(jx), asUint(jy) };
                  for (int r = 0; r < 4; ++r)
                      for (int col = 0; col < 4; ++col) k[8 + 4 * r + col] = asUint(prevViewProj.m[r][col]);
                  k[24] = tsr ? c.uav(previousDepth) : 0xFFFFFFFFu;
                  c.cmd->SetPipelineState(motionPso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 28);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
    if (tsr)
    {
        // Tsr.hlsli: dilate (+ the closest occluder scatter) -> decimate -> reject -> spatial anti-aliasing -> update.
        s.ensureGuide(fc.device, w, h);
        const bool guideReset = reset || s.guideFresh;
        s.guideFresh = false;
        const TextureRef previousGuide = g.importTexture(s.guide[prev].Get(), { "m.tsr.guide (previous)", w, h, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM },
                                                         D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const TextureRef nextGuide = g.importTexture(s.guide[next].Get(), { "m.tsr.guide", w, h, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const TextureRef scatter = g.createTexture(TextureDesc{ "m.tsr.closest occluder", w, h, 1, 1, DXGI_FORMAT_R32_UINT });
        const TextureRef dilated = g.createTexture(TextureDesc{ "m.tsr.dilated motion", w, h, 1, 1, DXGI_FORMAT_R32G32_FLOAT });
        const TextureRef info = g.createTexture(TextureDesc{ "m.tsr.dilate info", w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        const TextureRef reprojected = g.createTexture(TextureDesc{ "m.tsr.reprojected guide", w, h, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
        const TextureRef decimateMask = g.createTexture(TextureDesc{ "m.tsr.decimate mask", w, h, 1, 1, DXGI_FORMAT_R8G8_UNORM });
        const TextureRef rejection = g.createTexture(TextureDesc{ "m.tsr.rejection", w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM });
        const TextureRef aaInput = g.createTexture(TextureDesc{ "m.tsr.aa input", w, h, 1, 1, DXGI_FORMAT_R8G8_UNORM });
        const TextureRef aa = g.createTexture(TextureDesc{ "m.tsr.aa", w, h, 1, 1, DXGI_FORMAT_R8G8_UINT });
        // output.upscale_tsr_flickering (the reference's r.TSR.ShadingRejection.Flickering, default on)
        const bool flickering = !fc.quality.has("output.upscale_tsr_flickering") || fc.quality.boolean("output.upscale_tsr_flickering");
        TextureRef previousFlicker, nextFlicker, reprojectedFlicker, moireError;
        if (flickering)
        {
            previousFlicker = g.importTexture(s.flicker[prev].Get(), { "m.tsr.flicker (previous)", w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            nextFlicker = g.importTexture(s.flicker[next].Get(), { "m.tsr.flicker", w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            reprojectedFlicker = g.createTexture(TextureDesc{ "m.tsr.reprojected flicker", w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM });
            moireError = g.createTexture(TextureDesc{ "m.tsr.moire error", w, h, 1, 1, DXGI_FORMAT_R16_FLOAT });
        }
        ID3D12PipelineState* flickerPso = flickering ? fc.shaders.compute("Passes/Shading/TsrFlicker") : nullptr;
        const uint32_t frameIndex = (uint32_t)fc.frame.frameIndex;
        ShaderLibrary& shaders = fc.shaders;
        ID3D12PipelineState* clearPso = shaders.compute("Passes/Shading/TsrClear");
        ID3D12PipelineState* dilatePso = shaders.compute("Passes/Shading/TsrDilate");
        ID3D12PipelineState* decimatePso = shaders.compute("Passes/Shading/TsrDecimate");
        ID3D12PipelineState* rejectPso = shaders.compute("Passes/Shading/TsrReject");
        ID3D12PipelineState* aaPso = shaders.compute("Passes/Shading/TsrAntiAlias");
        ID3D12PipelineState* updatePso = shaders.compute("Passes/Shading/TsrUpdate");
        const float exposureRatio = u.exposureRatio;
        // the guide's blend of a held history: 1 / (1 + 16 / (input / output size)^2) (the reference's TheoricBlendFactor)
        const float fraction = (float)w / (float)W;
        const float theoreticBlend = 1.0f / (1.0f + 16.0f / (fraction * fraction));
        g.addPass("m.tsr.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(scatter, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(scatter), w, h, 0 };
                      c.cmd->SetPipelineState(clearPso);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                  });
        g.addPass("m.tsr.dilate", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(depth, Use::SrvCompute);
                      b.use(motion, Use::SrvCompute);
                      b.use(previousDepth, Use::SrvCompute);
                      b.use(dilated, Use::UavCompute);
                      b.use(info, Use::UavCompute);
                      b.use(scatter, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[8] = { c.srv(depth), c.srv(motion), c.srv(previousDepth), c.uav(dilated), c.uav(info), c.uav(scatter), w, h };
                      c.cmd->SetPipelineState(dilatePso);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 8);
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
                  },
                  [=](PassContext& c) {
                      const uint32_t k[16] = { c.srv(dilated), c.srv(info), c.srv(scatter), c.srv(previousGuide), c.uav(reprojected), c.uav(decimateMask), w, h,
                                               asUint(jx), asUint(jy), asUint(exposureRatio), guideReset ? 1u : 0u,
                                               flickering ? c.srv(previousFlicker) : 0xFFFFFFFFu, flickering ? c.uav(reprojectedFlicker) : 0xFFFFFFFFu, frameIndex, 0 };
                      c.cmd->SetPipelineState(decimatePso);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                  });
        if (flickering)
            g.addPass("m.tsr.flicker", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(src, Use::SrvCompute);
                          b.use(reprojectedFlicker, Use::SrvCompute);
                          b.use(decimateMask, Use::SrvCompute);
                          b.use(info, Use::SrvCompute);
                          b.use(moireError, Use::UavCompute);
                          b.use(nextFlicker, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[12] = { c.srv(src), c.srv(reprojectedFlicker), c.srv(decimateMask), c.srv(info), c.uav(moireError), c.uav(nextFlicker), w, h,
                                                   guideReset ? 1u : 0u, 0, 0, 0 };
                          c.cmd->SetPipelineState(flickerPso);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 12);
                          c.cmd->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
                      });
        g.addPass("m.tsr.reject", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(src, Use::SrvCompute);
                      b.use(reprojected, Use::SrvCompute);
                      b.use(decimateMask, Use::SrvCompute);
                      b.use(rejection, Use::UavCompute);
                      b.use(nextGuide, Use::UavCompute);
                      b.use(aaInput, Use::UavCompute);
                      if (flickering) b.use(moireError, Use::SrvCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[12] = { c.srv(src), c.srv(reprojected), c.srv(decimateMask), c.uav(rejection), c.uav(nextGuide), c.uav(aaInput), w, h,
                                               asUint(theoreticBlend), flickering ? c.srv(moireError) : 0xFFFFFFFFu, 0, 0 };
                      c.cmd->SetPipelineState(rejectPso);
                      c.computeConstants(k, 12);
                      c.cmd->Dispatch((w + 15) / 16, (h + 15) / 16, 1);
                  });
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
                      b.use(dilated, Use::SrvCompute);
                      b.use(aa, Use::SrvCompute);
                      b.use(history, Use::SrvCompute);
                      b.use(output, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[16] = { c.srv(src), c.srv(rejection), c.srv(dilated), c.srv(history), c.uav(output), w, h, reset ? 1u : 0u,
                                               W, H, asUint(jx), asUint(jy), asUint(exposureRatio), c.srv(aa), 0, 0 };
                      c.cmd->SetPipelineState(updatePso);
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                  });
        return output;
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
