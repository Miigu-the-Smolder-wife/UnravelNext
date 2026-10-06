// Short-range AO and bent normal (lumen.short_range_ao; LumenShortRangeAO.hlsli states the passes; owner A, for R's gi.lumen
// final gather). Persistent state: the filtered result and the view depth of the previous frame (ping-pong); the history
// is dropped on a new scene revision, a history discontinuity, an origin shift or a size change.
#include "unx/gi/LumenShortRangeAO.h"
#include "unx/render/HistoryResize.h"

#include "unx/core/Config.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <cstring>
#include <memory>

namespace unx::render::gi
{
namespace
{
uint32_t bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct ShortRangeAoState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> value[2], depth[2];
    uint32_t width = 0, height = 0, parity = 0, revision = 0xFFFFFFFFu;
    bool fresh = true;
    ~ShortRangeAoState() { release(); }
    void release()
    {
        if (!device) return;
        for (int k = 0; k < 2; ++k)
            for (ComPtr<ID3D12Resource>* t : { std::addressof(value[k]), std::addressof(depth[k]) })
                if (*t) device->deferRelease(*t);
    }
    void ensure(FramePassContext& fc, uint32_t w, uint32_t h)
    {
        if (value[0] && width == w && height == h) return;
        Device& d = fc.device;
        device = &d;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        auto texture = [&](ComPtr<ID3D12Resource>& out, DXGI_FORMAT format, const wchar_t* name) {
            const auto old = out;
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
                  "R short-range AO history");
            if (old)
            {
                if (!fresh) preserveHistoryResize(fc, old, out.Get());
                else d.deferRelease(old);
            }
            out->SetName(name);
        };
        for (int k = 0; k < 2; ++k)
        {
            texture(value[k], DXGI_FORMAT_R16G16B16A16_FLOAT, k ? L"R sao 1" : L"R sao 0");
            texture(depth[k], DXGI_FORMAT_R32_FLOAT, k ? L"R sao depth 1" : L"R sao depth 0");
        }
        width = w;
        height = h;
    }
};
} // namespace

TextureRef lumenShortRangeAO(FramePassContext& fc, const ViewResources& view)
{
    const QualityConfig& q = fc.quality;
    if (!q.has("lumen.short_range_ao") || !q.boolean("lumen.short_range_ao")) return {};
    if (view.view.kind != gpu::ViewKind::Main || !fc.trackState || !view.depth.valid() || !view.gbuffer.valid()) return {};
    const uint32_t factor = (uint32_t)q.integer("lumen.short_range_ao_downsample");
    if (factor != 1 && factor != 2) fail("lumen.short_range_ao_downsample must be 1 or 2");
    const uint32_t slices = (uint32_t)q.integer("lumen.short_range_ao_slices"), steps = (uint32_t)q.integer("lumen.short_range_ao_steps");
    if (slices < 1 || slices > 16 || steps < 1 || steps > 32) fail("lumen.short_range_ao_slices must be in [1, 16], steps in [1, 32]");
    const float radius = (float)q.number("lumen.short_range_ao_radius_px"), foreground = (float)q.number("lumen.short_range_ao_foreground_fraction");
    const float power = (float)q.number("lumen.short_range_ao_foreground_power"), clampScale = (float)q.number("lumen.short_range_ao_clamp_scale");
    const float maxFrames = (float)q.number("lumen.short_range_ao_max_frames"), threshold = (float)q.number("lumen.short_range_ao_history_distance");
    const bool temporal = q.boolean("lumen.short_range_ao_temporal"), hzb = q.boolean("lumen.short_range_ao_hzb") && view.hiz.valid();
    if (!(radius > 0) || !(foreground > 0)) fail("lumen.short_range_ao_radius_px and _foreground_fraction must be positive");

    RenderGraph& g = fc.graph;
    const uint32_t W = view.view.width, H = view.view.height, w = (W + factor - 1) / factor, h = (H + factor - 1) / factor;
    ShortRangeAoState& st = fc.state<ShortRangeAoState>("R.lumenShortRangeAO");
    st.ensure(fc, W, H);
    const float3 shift = fc.frame.originShift;
    const bool valid = !st.fresh && st.revision == fc.scene.revision() && fc.frame.discontinuity == 0 && shift.x == 0 && shift.y == 0 && shift.z == 0;
    st.fresh = false;
    st.revision = fc.scene.revision();
    const uint32_t prev = st.parity, next = prev ^ 1u;
    st.parity = next;

    const TextureRef depth = view.depth, gbuffer = view.gbuffer, hiz = view.hiz, visId = view.visId;
    const BufferRef clusters = view.visibleClusters;
    const bool hasVis = visId.valid() && clusters.valid();
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const TextureRef half = g.createTexture({ "r.gi.sao half", w, h, 1, 1, DXGI_FORMAT_R32_UINT });
    ID3D12PipelineState* search = fc.shaders.compute("Passes/GI/LumenShortRangeAO");
    g.addPass("r.gi.sao", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  if (hzb) b.use(hiz, Use::SrvCompute);
                  b.use(half, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[16] = { c.srv(depth), c.srv(gbuffer), 0, c.uav(half), w, h, factor, hzb ? c.srv(hiz) : gpu::kNone,
                                           slices, steps, bits(radius), bits(foreground), bits(power), 0, 0, 0 };
                  c.cmd->SetPipelineState(search);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 16);
                  gpuDispatch(c.cmd, (w + 7) / 8, (h + 7) / 8, 1);
              });

    auto import = [&](ComPtr<ID3D12Resource>& t, const char* name, DXGI_FORMAT format) {
        return g.importTexture(t.Get(), { name, W, H, 1, 1, format }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    };
    const bool history = valid && temporal;
    const TextureRef prevValue = history ? import(st.value[prev], "r.gi.sao (previous)", DXGI_FORMAT_R16G16B16A16_FLOAT) : TextureRef{};
    const TextureRef prevDepth = history ? import(st.depth[prev], "r.gi.sao depth (previous)", DXGI_FORMAT_R32_FLOAT) : TextureRef{};
    const TextureRef output = import(st.value[next], "r.gi.sao", DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef outDepth = import(st.depth[next], "r.gi.sao depth", DXGI_FORMAT_R32_FLOAT);
    ID3D12PipelineState* filter = fc.shaders.compute("Passes/GI/LumenShortRangeAOTemporal");
    g.addPass("r.gi.sao.temporal", QueueType::Compute,
              [&](PassBuilder& b) {
                  for (TextureRef t : { half, depth, gbuffer }) b.use(t, Use::SrvCompute);
                  if (history)
                  {
                      b.use(prevValue, Use::SrvCompute);
                      b.use(prevDepth, Use::SrvCompute);
                  }
                  if (hasVis)
                  {
                      b.use(visId, Use::SrvCompute);
                      b.use(clusters, Use::SrvCompute);
                  }
                  b.use(output, Use::UavCompute);
                  b.use(outDepth, Use::UavCompute);
                  b.keep();  // persistent state: the next frame's history
              },
              [=](PassContext& c) {
                  const uint32_t none = gpu::kNone;
                  const uint32_t k[16] = { c.srv(half), c.srv(depth), c.srv(gbuffer), 0,
                                           W, H, factor | (((history ? 1u : 0u) | (temporal ? 2u : 0u)) << 8), bits(clampScale),
                                           history ? c.srv(prevValue) : none, history ? c.srv(prevDepth) : none, hasVis ? c.srv(visId) : none, hasVis ? c.srv(clusters) : none,
                                           c.uav(output), c.uav(outDepth), bits(maxFrames), bits(threshold) };
                  c.cmd->SetPipelineState(filter);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 16);
                  gpuDispatch(c.cmd, (W + 7) / 8, (H + 7) / 8, 1);
              });
    return output;
}
} // namespace unx::render::gi
