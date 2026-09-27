// Temporal upscale of M (user decision 2026-09-27: internal resolution + temporal upscale for the 4K budget;
// output.render_height_max, FrameContext::Upscale set by FrameRenderer::setupUpscale). Above render_height_max the main
// view renders at that height with a Halton (2, 3) sub-pixel jitter of its projection; every system of the frame sees
// that internal view. After the shading chain (haze, motion blur, depth of field at the internal resolution) two passes
// reconstruct the output resolution:
//   m.upscale.motion  per internal sample, the output-UV motion to the previous frame's unjittered view
//                     (UpscaleMotion.hlsl: the vis buffer's triangle in its previous-tick vertices);
//   m.upscale         per output pixel, the jittered samples around it (a Gaussian window), the reprojected previous
//                     output clipped to their YCoCg box, blended by how well this frame's samples cover the pixel
//                     against the history's weight (Upscale.hlsl).
// The output (RGBA16F at the output resolution, persistent ping-pong) is the post chain's input and the next frame's
// history. Cost per output pixel is fixed (9 colour + 9 depth loads, 1 motion load, 5 bilinear history taps); the
// shading, reflections, GI screen passes, shadows' projections and resolve run on 0.44 of the 4K pixels (1440p).
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
    ~UpscaleState()
    {
        if (!device) return;
        for (auto& t : history)
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

TextureRef temporalUpscale(FramePassContext& fc, const ViewResources& view, TextureRef src)
{
    if (!view.depth.valid()) fail("M.upscale: the main view has no depth");
    const FrameContext::Upscale& u = fc.frame.upscale;
    RenderGraph& g = fc.graph;
    const uint32_t w = view.view.width, h = view.view.height, W = u.outputWidth, H = u.outputHeight;
    const int64_t frames = fc.quality.has("output.upscale_history_frames") ? fc.quality.integer("output.upscale_history_frames") : 24;
    if (frames < 1 || frames > 256) fail("output.upscale_history_frames must be in [1, 256]");
    UpscaleState& s = fc.state<UpscaleState>("M.upscale");
    s.ensure(fc.device, W, H);
    const bool reset = u.reset || s.fresh;
    s.fresh = false;
    const uint32_t prev = s.parity, next = prev ^ 1u;
    s.parity = next;
    const TextureRef history = g.importTexture(s.history[prev].Get(), { "m.upscale.history (previous)", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                               D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const TextureRef output = g.importTexture(s.history[next].Get(), { "m.upscale.history", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                              D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const TextureRef motion = g.createTexture(TextureDesc{ "m.upscale.motion", w, h, 1, 1, DXGI_FORMAT_R32G32_FLOAT });
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
              },
              [=](PassContext& c) {
                  uint32_t k[24] = { hasVis ? c.srv(vis) : 0xFFFFFFFFu, hasVis ? c.srv(clusters) : 0xFFFFFFFFu, c.srv(depth), c.uav(motion), w, h, asUint(jx), asUint(jy) };
                  for (int r = 0; r < 4; ++r)
                      for (int col = 0; col < 4; ++col) k[8 + 4 * r + col] = asUint(prevViewProj.m[r][col]);
                  c.cmd->SetPipelineState(motionPso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 24);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
    const float ratio = u.exposureRatio, maxWeight = (float)frames;
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
                                           W, H, asUint(jx), asUint(jy), asUint(ratio), asUint(maxWeight), 0, 0 };
                  c.cmd->SetPipelineState(pso);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 16);
                  c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
              });
    return output;
}
} // namespace unx::render::shading
