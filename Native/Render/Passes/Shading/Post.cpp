// Post chain of M in HDR (FEATURES_GAME 4 "order" and 6; render A item A4). When a post term is on, the main view is
// shaded into an exposed-linear RGBA16F target (the shading writers' linear variant) and this chain writes the display
// output: lens PSF (bloom: a shift-invariant, energy-conserving pyramid kernel, the PSF's tail holding the fraction
// shading.post_bloom_strength of the energy) -> natural vignetting (cos^4 of the field angle) -> tone curve (PBR Neutral,
// INTERFACES 8.4) -> grading LUT (33^3 .cube, after the curve) -> film grain (deterministic hash, after the curve) ->
// triangular dither of the 10-bit output -> sRGB. With every term off the chain is not recorded and the writers encode
// directly (gates and reference comparisons are unchanged: the quality keys default to off). An HDR display
// (FrameContext::displayPeak = peak / paper white) always runs the chain: the curve generalised to that peak (at 1 the
// SDR curve exactly), the LUT on the curve's output over the peak, grain, then display-referred linear light
// (1 = paper white) into the RGBA16F output without OETF or dither; the host encodes it (scRGB or PQ).
#include "unx/shading/Post.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

namespace unx::render::shading
{
namespace
{
struct PostParams
{
    float bloom = 0, vignette = 0, grain = 0;
    uint32_t levels = 6;
    std::string lut;
};

PostParams params(const QualityConfig& q)
{
    PostParams p;
    auto num = [&](const char* k, float d) { return q.has(k) ? (float)q.number(k) : d; };
    p.bloom = num("shading.post_bloom_strength", 0);
    p.vignette = num("shading.post_vignette", 0);
    p.grain = num("shading.post_grain", 0);
    p.levels = q.has("shading.post_bloom_levels") ? (uint32_t)q.integer("shading.post_bloom_levels") : 6u;
    p.lut = q.has("shading.post_lut") ? q.string("shading.post_lut") : std::string();
    if (p.bloom < 0 || p.bloom > 1 || p.vignette < 0 || p.vignette > 1 || p.grain < 0 || p.grain >= 0.4f || p.levels < 1 || p.levels > 10)
        fail("shading.post_*: bloom strength and vignette in [0, 1], 0 <= grain < 0.4, 1 <= bloom levels <= 10");
    return p;
}

// A 33^3 grading LUT (.cube, RGB in [0, 1]), uploaded once as an RGBA16F 3D texture.
struct LutState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> texture;
    uint32_t srv = 0xFFFFFFFFu;
    std::string path;
    ~LutState()
    {
        if (!device) return;
        device->deferRelease(texture);
        DescriptorHeaps* h = &device->descriptors();
        const uint32_t s = srv;
        if (s != 0xFFFFFFFFu) device->deferCall([h, s] { h->freeResource(s); });
    }
    void load(Device& d, const std::string& file)
    {
        if (file == path && texture) return;
        device = &d;
        std::ifstream in(file);
        if (!in) fail("shading.post_lut: cannot read '%s'", file.c_str());
        uint32_t size = 0;
        std::vector<float> rgb;
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream s(line);
            std::string word;
            s >> word;
            if (word == "LUT_3D_SIZE") s >> size;
            else if (word == "TITLE" || word == "DOMAIN_MIN" || word == "DOMAIN_MAX" || word == "LUT_1D_SIZE") continue;
            else
            {
                std::istringstream v(line);
                float r, g, b;
                if (v >> r >> g >> b) rgb.insert(rgb.end(), { r, g, b });
            }
        }
        if (size < 2 || rgb.size() != (size_t)size * size * size * 3) fail("shading.post_lut: '%s' is not a 3D .cube LUT (size %u, %zu values)", file.c_str(), size, rgb.size() / 3);
        std::vector<uint16_t> texels((size_t)size * size * size * 4);
        auto half = [](float f) {
            uint32_t x;
            std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000, e = (x >> 23) & 0xFF;
            int32_t exp = (int32_t)e - 127 + 15;
            const uint32_t m = x & 0x7FFFFF;
            if (e == 0 || exp < -10) return (uint16_t)sign;
            if (exp >= 31) return (uint16_t)(sign | 0x7C00);
            if (exp <= 0)  // half subnormal: the mantissa with its implicit bit, shifted, rounded to nearest
            {
                const uint32_t full = m | 0x800000, shift = (uint32_t)(14 - exp);
                return (uint16_t)(sign | ((full + (1u << (shift - 1))) >> shift));
            }
            // rounded to nearest; a mantissa carry adds into the exponent (an OR dropped it for even exponents)
            return (uint16_t)(sign | std::min<uint32_t>(((uint32_t)exp << 10) + ((m + 0x1000) >> 13), 0x7C00));
        };
        for (size_t i = 0; i < (size_t)size * size * size; ++i)
        {
            texels[i * 4 + 0] = half(rgb[i * 3 + 0]);
            texels[i * 4 + 1] = half(rgb[i * 3 + 1]);
            texels[i * 4 + 2] = half(rgb[i * 3 + 2]);
            texels[i * 4 + 3] = half(1.0f);
        }
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        td.Width = td.Height = size;
        td.DepthOrArraySize = (UINT16)size;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (texture) d.deferRelease(texture);
        check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&texture)),
              "M grading LUT");
        texture->SetName(L"M grading LUT");
        // upload: rows at the copy pitch alignment, one copy, waited for (load time)
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        const D3D12_RESOURCE_DESC legacy = texture->GetDesc();
        d.d3d()->GetCopyableFootprints(&legacy, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        ComPtr<ID3D12Resource> staging;
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
              "M grading LUT staging");
        uint8_t* mapped = nullptr;
        check(staging->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map M grading LUT staging");
        for (uint32_t z = 0; z < size; ++z)
            for (uint32_t y = 0; y < size; ++y)
                std::memcpy(mapped + fp.Offset + (size_t)z * fp.Footprint.RowPitch * size + (size_t)y * fp.Footprint.RowPitch,
                            texels.data() + ((size_t)z * size + y) * size * 4, (size_t)size * 8);
        staging->Unmap(0, nullptr);
        CommandList list = d.acquireCommandList(QueueType::Graphics);
        D3D12_TEXTURE_COPY_LOCATION dst{ texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        D3D12_TEXTURE_COPY_LOCATION src{ staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        src.PlacedFootprint = fp;
        list.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        d.queue(QueueType::Graphics).waitCpu(d.submit(list));
        if (srv == 0xFFFFFFFFu) srv = d.descriptors().allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.Texture3D.MipLevels = 1;
        d.d3d()->CreateShaderResourceView(texture.Get(), &sd, d.descriptors().resourceCpu(srv));
        path = file;
    }
};

uint32_t asUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
} // namespace

bool postActive(FramePassContext& fc, const ViewResources& view)
{
    if (view.view.kind != gpu::ViewKind::Main || fc.frame.outputLinearHdr) return false;
    if (fc.frame.displayPeak > 0) return true;  // an HDR display: the chain writes its encoding
    const PostParams p = params(fc.quality);
    return p.bloom > 0 || p.vignette > 0 || p.grain > 0 || !p.lut.empty();
}

TextureRef postTarget(FramePassContext& fc, const ViewResources& view)
{
    return fc.graph.createTexture(TextureDesc{ "m.post.hdr", view.view.width, view.view.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
}

TextureRef postBloomTail(FramePassContext& fc, TextureRef hdr, uint32_t levelCount)
{
    RenderGraph& g = fc.graph;
    const TextureDesc hd = g.desc(hdr);
    const uint32_t w = hd.width, h = hd.height;
    const PostParams p{ 1.0f, 0, 0, levelCount, {} };
    // Bloom: downsample the exposed HDR into a pyramid (13-tap, energy-normalised, levels 1/2 .. 1/2^n), then add the
    // levels back up with a tent filter; the result at 1/2 resolution is the PSF tail convolved with the image (its kernel
    // sums to 1). Final = (1 - s) x image + s x tail: the chain moves energy, it never adds it.
    {
        std::vector<TextureRef> levels;
        uint32_t lw = w, lh = h;
        for (uint32_t l = 0; l < p.levels && lw > 1 && lh > 1; ++l)
        {
            lw = std::max(1u, (lw + 1) / 2);
            lh = std::max(1u, (lh + 1) / 2);
            levels.push_back(g.createTexture(TextureDesc{ "m.post.bloom", lw, lh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT }));
        }
        ID3D12PipelineState* down = fc.shaders.compute("Passes/Shading/PostDownsample");
        ID3D12PipelineState* up = fc.shaders.compute("Passes/Shading/PostUpsample");
        for (size_t l = 0; l < levels.size(); ++l)
        {
            const TextureRef src = l == 0 ? hdr : levels[l - 1], dst = levels[l];
            const TextureDesc dd = g.desc(dst);
            g.addPass("m.post.down", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(src, Use::SrvCompute);
                          b.use(dst, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[4] = { c.srv(src), c.uav(dst), dd.width, dd.height };
                          c.cmd->SetPipelineState(down);
                          c.computeConstants(k, 4);
                          c.cmd->Dispatch((dd.width + 7) / 8, (dd.height + 7) / 8, 1);
                      });
        }
        // Up the pyramid: level l += tent(level l + 1), equal weights per level (each level a Gaussian-like octave).
        for (size_t l = levels.size() - 1; l > 0; --l)
        {
            const TextureRef src = levels[l], dst = levels[l - 1];
            const TextureDesc dd = g.desc(dst);
            // levels[l] holds the mean of the n - l coarsest levels: merging it into one more level keeps equal weights
            const float weight = (float)(levels.size() - l) / (float)(levels.size() - l + 1);
            g.addPass("m.post.up", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(src, Use::SrvCompute);
                          b.use(dst, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[8] = { c.srv(src), c.uav(dst), dd.width, dd.height, asUint(weight), 0, 0, 0 };
                          c.cmd->SetPipelineState(up);
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch((dd.width + 7) / 8, (dd.height + 7) / 8, 1);
                      });
        }
        return levels.front();
    }
}

void postChain(FramePassContext& fc, const ViewResources& view, TextureRef hdr)
{
    const PostParams p = params(fc.quality);
    const uint32_t w = view.view.width, h = view.view.height;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    RenderGraph& g = fc.graph;
    TextureRef bloom;
    if (p.bloom > 0) bloom = postBloomTail(fc, hdr, p.levels);
    LutState* lut = nullptr;
    if (!p.lut.empty())
    {
        lut = &fc.state<LutState>("M.post.lut");
        lut->load(fc.device, p.lut);
    }
    ID3D12PipelineState* final = fc.shaders.compute("Passes/Shading/PostFinal");
    const TextureRef output = view.color;
    const uint32_t lutSrv = lut ? lut->srv : 0xFFFFFFFFu, frame = (uint32_t)fc.frame.frameIndex;
    const float peak = fc.frame.displayPeak;  // 0: SDR
    g.addPass("m.post.final", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(hdr, Use::SrvCompute);
                  if (bloom.valid()) b.use(bloom, Use::SrvCompute);
                  b.use(output, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[12] = { c.srv(hdr), bloom.valid() ? c.srv(bloom) : 0xFFFFFFFFu, c.uav(output), lutSrv,
                                           asUint(p.bloom), asUint(p.vignette), asUint(p.grain), frame, w, h, asUint(peak), 0 };
                  c.cmd->SetPipelineState(final);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 12);
                  c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
              });
}
} // namespace unx::render::shading
