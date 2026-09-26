// M's HDR post chain (Post.cpp, render A item A4), on synthetic inputs (exact: no scene, no temporal state):
//   1. the lens PSF's tail (bloom pyramid) conserves energy: an impulse's energy comes back in the tail (fine-pixel units:
//      half-resolution texels count 4) at every texel parity, within the unbiased half rounding of the pyramid's stores
//      (the symmetric impulse's rounding errors are correlated: |dE|/E <= 5e-4; a toward-zero store gave -2.2e-3 and a
//      stride-2 point-sampled down pass +/-100 %), and two runs are bit identical;
//   2. the final pass against a CPU reference on an HDR ramp over the curve's whole range (vignetting 0.7 of this view's
//      projection, PBR Neutral, an identity 33^3 LUT, sRGB, the triangular 10-bit dither of the same hash): every channel
//      within 1 10-bit step, at most 0.01 % of them off by one (float pow/exp at rounding boundaries; without the
//      shader's explicit rounding the hardware's UNORM conversion put 3 % one code low);
//   3. grain and bloom: two runs bit identical (hashes of pixel and frame index), grain zero-mean (|mean| < 0.1 step);
//   4. an HDR display (FrameContext::displayPeak): the RGBA16F output against the CPU reference of the curve generalised
//      to the peak (4 x paper white, vignetting 0.7) within half precision, never above the peak; at peak 1 the HDR
//      output equals the SDR curve before its encoding (half precision);
//   5. motion blur's gather (MotionBlur.hlsl) on a uniform velocity (40 px/frame, 180 degree shutter: a 20 px streak ahead
//      of each pixel) over sinusoids along it: against the exact exposure integral (1/L) int_0^L I(x + t) dt in closed form;
//   6. heat haze (Distortion.hlsl): D = (3, 0) px under a haze front at device depth 0.5; pixels behind it (0.1) read
//      I(x + D (1 - 0.1 / 0.5)) = I(x + 2.4) (bilinear of the sinusoids: within 1e-3), pixels in front (0.9) are unchanged;
//   7. motion blur's rotation stage (MotionRotation.hlsl) for a yaw of 0.2 rad per frame (a 31 px streak at the centre) over
//      an environment image L(direction): against the CPU mean of L over each pixel's arc Q^tau d, tau in [0, s] (pixels
//      whose arc stays on the image).
//   8. depth of field (DepthOfField.cpp, FEATURES_GAME 4.1) against the thin lens ray-cast on the CPU over the same scene:
//      each ray (4 x 4 jittered subsamples x 1,024 stratified lens points per pixel) solved exactly for the nearest surface's
//      pinhole position q (q + u rho(q) = p) and textured with the pinhole image there; pixels where a ray meets a surface
//      that the pinhole image does not hold (hidden behind a nearer one, or outside the frame) are counted and left out.
//      Scenes: a tilted plane (1/z linear across the view, |rho| <= 12 and <= 60 px) with HDR points, a blurred near card
//      over a sharp wall, a sharp card over a blurred wall with HDR points (no halo on the card), and a lens so small that
//      every pixel is in focus (output bit identical to the input).
// Options: --levels N (6, test 1), --dof (test 8 alone).
#include "FilmCurve.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuProfiler.h"
#include "unx/render/GpuScene.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/scene/SceneData.h"
#include "unx/shading/DepthOfField.h"
#include "unx/shading/MotionBlur.h"
#include "unx/shading/Post.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
// The tail's support radius: the down passes reach 3 source texels per level (3 (2^n - 1) px), the up passes 2 coarse
// texels (2^(n+1) - 4 px): about 250 px for 6 levels. The impulse's tail stays inside a 1024 x 1024 frame, where the
// pyramid conserves energy exactly; light whose tail crosses the frame border loses the part outside the frame.
constexpr uint32_t kSize = 1024;
uint32_t kLevels = 6;
constexpr float kEnergy = 1000.0f;
constexpr uint32_t kWidth = 640, kHeight = 360, kFrame = 7;

float halfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    const float f = e == 0 ? std::ldexp((float)m, -24) : std::ldexp(1.0f + m / 1024.0f, (int)e - 15);
    return s ? -f : f;
}
uint32_t asUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// Textures read back after the graph ran (rows at the copy pitch).
struct Readbacks
{
    Device& device;
    struct Item
    {
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        uint32_t rows = 0;
        std::vector<uint8_t>* out = nullptr;
        uint32_t* pitch = nullptr;
    };
    std::vector<std::shared_ptr<Item>> items;
    void texture(RenderGraph& g, TextureRef t, std::vector<uint8_t>& out, uint32_t& pitch)
    {
        auto item = std::make_shared<Item>();
        item->out = &out;
        item->pitch = &pitch;
        items.push_back(item);
        Device* dev = &device;
        g.addPass("post.test.readback", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(t, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& c) {
                      ID3D12Resource* src = c.resource(t);
                      D3D12_RESOURCE_DESC d = src->GetDesc();
                      UINT rows;
                      UINT64 rowBytes, total;
                      dev->d3d()->GetCopyableFootprints(&d, 0, 1, 0, &item->fp, &rows, &rowBytes, &total);
                      item->rows = rows;
                      D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
                      D3D12_RESOURCE_DESC bd{};
                      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                      bd.Width = total;
                      bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                      bd.SampleDesc.Count = 1;
                      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                      check(dev->d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&item->buffer)),
                            "readback");
                      D3D12_TEXTURE_COPY_LOCATION dst{ item->buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                      dst.PlacedFootprint = item->fp;
                      D3D12_TEXTURE_COPY_LOCATION s{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                      c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
                  });
    }
    // A buffer's first 'bytes' (read back as rows = 1, pitch = bytes).
    void buffer(RenderGraph& g, BufferRef buf, uint64_t bytes, std::vector<uint8_t>& out, uint32_t& pitch)
    {
        auto item = std::make_shared<Item>();
        item->out = &out;
        item->pitch = &pitch;
        item->rows = 1;
        item->fp.Footprint.RowPitch = (uint32_t)bytes;
        items.push_back(item);
        Device* dev = &device;
        g.addPass("post.test.readback buffer", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(buf, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& c) {
                      D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
                      D3D12_RESOURCE_DESC bd{};
                      bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                      bd.Width = bytes;
                      bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
                      bd.SampleDesc.Count = 1;
                      bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                      check(dev->d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&item->buffer)),
                            "readback");
                      c.cmd->CopyBufferRegion(item->buffer.Get(), 0, c.resource(buf), 0, bytes);
                  });
    }
    void finish()
    {
        device.waitIdle();
        for (auto& i : items)
        {
            uint8_t* p = nullptr;
            check(i->buffer->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
            i->out->assign(p, p + (size_t)i->fp.Footprint.RowPitch * i->rows);
            i->buffer->Unmap(0, nullptr);
            *i->pitch = i->fp.Footprint.RowPitch;
        }
        items.clear();
    }
};

struct Context
{
    GpuScene scene;
    RenderGraph graph;
    TrackState state;
    FrameContext frame;
    FrameResources resources;
    FrameServices services;
    FramePassContext fc;
    Context(Device& device, ShaderLibrary& shaders, const QualityConfig& quality)
        : scene(device), graph(device),
          fc{ device, graph, shaders, quality, scene, frame, resources, services, [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state }
    {
    }
};

void fill(Context& x, ShaderLibrary& shaders, TextureRef t, uint32_t mode, uint32_t px = 0, uint32_t py = 0, uint32_t value = 0,
          D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0)
{
    const TextureDesc d = x.graph.desc(t);
    ID3D12PipelineState* pso = shaders.compute("Passes/Shading/Tests/PostImpulse");
    x.graph.addPass("post.test.input", QueueType::Graphics, [&](PassBuilder& b) { b.use(t, Use::UavCompute); },
                    [=](PassContext& c) {
                        const uint32_t k[8] = { c.uav(t), px, py, mode == 0 ? asUint(kEnergy) : value, mode, 0, 0, 0 };
                        c.cmd->SetPipelineState(pso);
                        if (frameConstants) c.bindFrameConstants(frameConstants);
                        c.computeConstants(k, 8);
                        c.cmd->Dispatch((d.width + 7) / 8, (d.height + 7) / 8, 1);
                    });
}

std::vector<uint8_t> tail(Device& device, ShaderLibrary& shaders, const QualityConfig& quality, uint32_t px, uint32_t py, uint32_t& pitch, uint32_t& tw, uint32_t& th)
{
    Context x(device, shaders, quality);
    const TextureRef hdr = x.graph.createTexture(TextureDesc{ "post.test.hdr", kSize, kSize, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    fill(x, shaders, hdr, 0, px, py);
    const TextureRef t = shading::postBloomTail(x.fc, hdr, kLevels);
    const TextureDesc td = x.graph.desc(t);
    tw = td.width;
    th = td.height;
    std::vector<uint8_t> out;
    Readbacks rb{ device };
    rb.texture(x.graph, t, out, pitch);
    x.graph.execute(nullptr);
    rb.finish();
    if (device.drainDebugMessages() != 0) fail("D3D12 debug layer errors");
    return out;
}

// The view of tests 2 and 3 (the default camera's projection at 640 x 360) and its frame constants.
struct PostView
{
    ViewResources view;
    ComPtr<ID3D12Resource> constants;
};
PostView postView(Device& device)
{
    PostView v;
    v.view.view = ViewDesc::fromCamera(scene::Camera{}, kWidth, kHeight, float4x4{});
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 4096;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&v.constants)), "constants");
    gpu::FrameConstants c{};
    c.viewProj = v.view.view.viewProj;
    c.invViewProj = v.view.view.invViewProj;
    c.view = v.view.view.view;
    c.proj = v.view.view.proj;
    c.viewWidth = kWidth;
    c.viewHeight = kHeight;
    c.nearPlane = v.view.view.nearPlane;
    c.exposure = 1.0f;
    void* p = nullptr;
    check(v.constants->Map(0, nullptr, &p), "map constants");
    std::memcpy(p, &c, sizeof c);
    v.constants->Unmap(0, nullptr);
    v.view.frameConstants = v.constants->GetGPUVirtualAddress();
    return v;
}

// The chain over the ramp; the HDR input and the RGB10A2 output read back.
void chain(Device& device, ShaderLibrary& shaders, const QualityConfig& quality, std::vector<uint8_t>& hdrBytes, uint32_t& hdrPitch, std::vector<uint8_t>& outBytes,
           uint32_t& outPitch, float displayPeak = 0)
{
    Context x(device, shaders, quality);
    x.frame.frameIndex = kFrame;
    x.frame.displayPeak = displayPeak;
    PostView v = postView(device);
    const TextureRef hdr = x.graph.createTexture(TextureDesc{ "post.test.ramp", kWidth, kHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    v.view.color = x.graph.createTexture(
        TextureDesc{ "post.test.output", kWidth, kHeight, 1, 1, displayPeak > 0 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM });
    fill(x, shaders, hdr, 1);
    if (!shading::postActive(x.fc, v.view)) fail("the chain is not active with post terms set");
    shading::postChain(x.fc, v.view, hdr);
    Readbacks rb{ device };
    rb.texture(x.graph, hdr, hdrBytes, hdrPitch);
    rb.texture(x.graph, v.view.color, outBytes, outPitch);
    x.graph.execute(nullptr);
    rb.finish();
    if (device.drainDebugMessages() != 0) fail("D3D12 debug layer errors");
}

// CPU reference of PostFinal.hlsl without bloom and grain.
// (the tone curve's CPU reference: FilmCurve.h)
float srgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
float hashUnit(uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t v[3] = { x * 1664525u + 1013904223u, y * 1664525u + 1013904223u, z * 1664525u + 1013904223u };
    for (int round = 0; round < 2; ++round)
    {
        v[0] += v[1] * v[2];
        v[1] += v[2] * v[0];
        v[2] += v[0] * v[1];
        if (round == 0)
            for (uint32_t& a : v) a ^= a >> 16u;
    }
    return (float)(v[0] >> 8) * (1.0f / 16777216.0f);
}
float saturate(float v) { return std::min(1.0f, std::max(0.0f, v)); }

// ---------------------------------------------------------------- 8
// A texture filled from CPU rows (the staging buffer lives until the graph executed: 'keep').
TextureRef uploadTexture(Context& x, Device& device, const char* name, uint32_t w, uint32_t h, DXGI_FORMAT format, uint32_t texelBytes, const void* data,
                         std::vector<ComPtr<ID3D12Resource>>& keep)
{
    const TextureRef t = x.graph.createTexture(TextureDesc{ name, w, h, 1, 1, format });
    const uint32_t pitch = (w * texelBytes + 255) & ~255u;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (uint64_t)pitch * h;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&staging)), "staging");
    uint8_t* p = nullptr;
    check(staging->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map staging");
    for (uint32_t y = 0; y < h; ++y) std::memcpy(p + (size_t)y * pitch, static_cast<const uint8_t*>(data) + (size_t)y * w * texelBytes, (size_t)w * texelBytes);
    staging->Unmap(0, nullptr);
    ID3D12Resource* src = staging.Get();
    keep.push_back(staging);
    x.graph.addPass("post.test.upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(t, Use::CopyDst); },
                    [=](PassContext& c) {
                        D3D12_TEXTURE_COPY_LOCATION to{ c.resource(t), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, from{ src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                        from.PlacedFootprint.Footprint = { format, w, h, 1, pitch };
                        c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                    });
    return t;
}

// A test 8 scene: the pinhole image, its depth, the lens, and the scene's surfaces for the reference. hit(p, u, k0, k1, q):
// for the receiver point p (px) and the lens point u (unit disk) with rho = k0 - k1 x device depth, the pinhole position q
// of the nearest surface point the thin-lens ray meets (the solution of q + u rho(q) = p); false when that point is not in
// the pinhole image (hidden behind a nearer surface or outside the frame: no data).
struct DofScene
{
    const char* name;
    uint32_t w = 0, h = 0;
    std::vector<float> device;      // reversed-Z device depth near / z (the view's depth buffer)
    std::vector<float> colour;      // RGBA32F
    float aperture = 0, focus = 0;  // m
    std::function<bool(double px, double py, double ux, double uy, double k0, double k1, double& qx, double& qy)> hit;
};

// The thin lens over the scene's surfaces, textured by the pinhole image (pixel-constant): per receiver the mean over
// 4 x 4 subsamples and 32 x 32 stratified lens points (concentric map) of the colour at the ray's pinhole position.
// holes[i]: the receiver's (subsample, lens point) pairs without data (left out by the test).
void dofReference(const DofScene& s, double k0, double k1, std::vector<double>& out, std::vector<uint32_t>& holes)
{
    const uint32_t S = 4, N = 32;
    out.assign((size_t)s.w * s.h * 3, 0.0);
    holes.assign((size_t)s.w * s.h, 0);
    std::vector<double> lens;
    for (uint32_t a = 0; a < N; ++a)
        for (uint32_t b = 0; b < N; ++b)
        {
            const double sx = 2 * (a + 0.5) / N - 1, sy = 2 * (b + 0.5) / N - 1;
            double r, phi;
            if (std::abs(sx) > std::abs(sy)) r = sx, phi = 0.7853981633974483 * (sy / sx);
            else r = sy, phi = 1.5707963267948966 - 0.7853981633974483 * (sx / sy);
            lens.push_back(r * std::cos(phi));
            lens.push_back(r * std::sin(phi));
        }
    for (uint32_t y = 0; y < s.h; ++y)
        for (uint32_t x = 0; x < s.w; ++x)
        {
            const size_t rcv = (size_t)y * s.w + x;
            for (uint32_t j = 0; j < S; ++j)
                for (uint32_t i = 0; i < S; ++i)
                    for (size_t l = 0; l < lens.size(); l += 2)
                    {
                        // stratified subsample position, jittered per ray (a fixed grid cannot see sub-stratum shifts)
                        const double jx = hashUnit(x * 4 + i, y * 4 + j, (uint32_t)l * 2), jy = hashUnit(x * 4 + i, y * 4 + j, (uint32_t)l * 2 + 1);
                        double qx, qy;
                        if (!s.hit(x + (i + jx) / S, y + (j + jy) / S, lens[l], lens[l + 1], k0, k1, qx, qy) || qx < 0 || qy < 0 || qx >= s.w || qy >= s.h)
                        {
                            ++holes[rcv];
                            continue;
                        }
                        const float* c = &s.colour[((size_t)qy * s.w + (size_t)qx) * 4];
                        for (int ch = 0; ch < 3; ++ch) out[rcv * 3 + ch] += c[ch];
                    }
        }
    const double inv = 1.0 / ((double)S * S * N * N);
    for (double& v : out) v *= inv;
}

bool g_dofProfile = false;

// Test 8: returns the failures.
template <typename LoadQuality>
int testDepthOfField(Device& device, ShaderLibrary& shaders, const LoadQuality& loadQuality)
{
    int failures = 0;
    auto expect = [&](const char* what, bool ok) {
        logf("  %-76s %s\n", what, ok ? "ok" : "FAILED");
        if (!ok) ++failures;
    };
    const uint32_t w = 192, h = 108;
    const ViewDesc desc = ViewDesc::fromCamera(scene::Camera{}, w, h, float4x4{});
    const double zNear = desc.nearPlane, fpx = 0.5 * h * desc.proj.m[1][1];
    auto texture = [&](uint32_t x, uint32_t y, uint32_t seed) {  // a textured surface: smooth hues + hashed detail
        const float n = hashUnit(x, y, seed);
        return float3{ 0.25f + 0.2f * std::sin(0.21f * x + seed) + 0.15f * n, 0.3f + 0.2f * std::cos(0.17f * y + 2 * seed) + 0.1f * n,
                       0.35f + 0.15f * std::sin(0.11f * (x + y)) + 0.1f * n };
    };
    auto scene = [&](const char* name, float aperture, float focus, auto&& depthAt, auto&& colourAt, auto&& hit) {
        DofScene s;
        s.name = name;
        s.w = w;
        s.h = h;
        s.aperture = aperture;
        s.focus = focus;
        s.hit = hit;
        s.device.resize((size_t)w * h);
        s.colour.resize((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const double z = depthAt(x, y);
                s.device[(size_t)y * w + x] = (float)(zNear / z);
                const float3 c = colourAt(x, y);
                float* o = &s.colour[((size_t)y * w + x) * 4];
                o[0] = c.x, o[1] = c.y, o[2] = c.z, o[3] = 1;
            }
        return s;
    };
    // the aperture giving rho = r px at depth z for focus zf: r = fpx A (1/zf - 1/z) / 2
    auto apertureFor = [&](double r, double z, double zf) { return (float)(2 * r / (fpx * std::abs(1 / zf - 1 / z))); };
    auto points = [&](uint32_t x, uint32_t y, float3 c) {  // sparse HDR points (bokeh)
        return (x % 23 == 7 && y % 19 == 5) ? float3{ 40, 36, 30 } : c;
    };
    std::vector<DofScene> scenes;
    // tilted plane: 1/z linear across x (device depth zNear/z linear in the pinhole x), in focus at the middle
    const double inv0 = 1 / 1.5, inv1 = 1 / 12.0;
    auto plane = [&](uint32_t x, uint32_t) { return 1.0 / (inv0 + (inv1 - inv0) * (x + 0.5) / w); };
    auto planeHit = [&](double px, double py, double ux, double uy, double k0, double k1, double& qx, double& qy) {
        // rho(q) = k0 - k1 zNear (inv0 + (inv1 - inv0) qx / w) = alpha + beta qx; q + u rho(q) = p
        const double alpha = k0 - k1 * zNear * inv0, beta = -k1 * zNear * (inv1 - inv0) / w;
        qx = (px - ux * alpha) / (1 + ux * beta);
        qy = py - uy * (alpha + beta * qx);
        return true;
    };
    const double zMid = plane(w / 2, 0);
    auto planeColour = [&](uint32_t x, uint32_t y) { return points(x, y, texture(x, y, 1)); };
    scenes.push_back(scene("tilted plane, texture only, |rho| <= 12 px", apertureFor(12, 1.5, zMid), (float)zMid, plane, [&](uint32_t x, uint32_t y) { return texture(x, y, 1); }, planeHit));
    scenes.push_back(scene("tilted plane, |rho| <= 12 px", apertureFor(12, 1.5, zMid), (float)zMid, plane, planeColour, planeHit));
    scenes.push_back(scene("tilted plane, |rho| <= 60 px", apertureFor(60, 1.5, zMid), (float)zMid, plane, planeColour, planeHit));
    // a card 0.6 m away over a wall 10 m away
    auto card = [&](double x, double y) { return x >= 70 && x < 130 && y >= 30 && y < 80; };
    auto cardScene = [&](uint32_t x, uint32_t y) { return card(x + 0.5, y + 0.5) ? 0.6 : 10.0; };
    const float cardDevice = (float)(zNear / 0.6), wallDevice = (float)(zNear / 10.0);
    auto cardHit = [&](double px, double py, double ux, double uy, double k0, double k1, double& qx, double& qy) {
        const double rc = k0 - k1 * cardDevice, rw = k0 - k1 * wallDevice;
        qx = px - ux * rc, qy = py - uy * rc;
        if (card(qx, qy)) return true;
        qx = px - ux * rw, qy = py - uy * rw;
        return !card(qx, qy);  // (the wall behind the card: not in the pinhole image)
    };
    scenes.push_back(scene("blurred near card (rho -15 px) over a sharp wall", apertureFor(15, 0.6, 10), 10.0f, cardScene,
                           [&](uint32_t x, uint32_t y) { return card(x + 0.5, y + 0.5) ? texture(x, y, 3) * 1.5f : texture(x, y, 4); }, cardHit));
    scenes.push_back(scene("sharp card over a blurred wall (rho +14 px) with HDR points", apertureFor(14, 10, 0.6), 0.6f, cardScene,
                           [&](uint32_t x, uint32_t y) { return card(x + 0.5, y + 0.5) ? texture(x, y, 3) : points(x, y, texture(x, y, 4)); }, cardHit));
    scenes.push_back(scene("near focus (|rho| <= 0.4 px)", apertureFor(0.4, 1.5, zMid), (float)zMid, plane, planeColour, planeHit));
    // a wall at the focus distance: rho = 0 up to float rounding, the image unchanged
    scenes.push_back(scene("a wall at the focus distance", apertureFor(8, 1.5, 3.0), 3.0f, [&](uint32_t, uint32_t) { return 3.0; }, planeColour,
                           [&](double px, double py, double ux, double uy, double k0, double k1, double& qx, double& qy) {
                               const double rw = k0 - k1 * (double)(float)(zNear / 3.0);
                               qx = px - ux * rw, qy = py - uy * rw;
                               return true;
                           }));
    auto run = [&](const DofScene& s, std::vector<uint8_t>& out, uint32_t& op) {
        const QualityConfig q = loadQuality();
        Context x(device, shaders, q);
        x.frame.lensAperture = s.aperture;
        x.frame.lensFocus = s.focus;
        ViewResources v;
        v.view = desc;
        std::vector<ComPtr<ID3D12Resource>> keep;
        const TextureRef image = uploadTexture(x, device, "post.test.dof.image", w, h, DXGI_FORMAT_R32G32B32A32_FLOAT, 16, s.colour.data(), keep);
        v.depth = uploadTexture(x, device, "post.test.dof.depth", w, h, DXGI_FORMAT_R32_FLOAT, 4, s.device.data(), keep);
        const TextureRef focused = x.graph.createTexture(TextureDesc{ "post.test.dof.out", w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        if (!shading::depthOfFieldActive(x.fc, v)) fail("depth of field inactive with a lens set");
        shading::DepthOfFieldProducts products;
        shading::depthOfField(x.fc, v, image, focused, &products);
        Readbacks rb{ device };
        rb.texture(x.graph, focused, out, op);
        const uint32_t tilesX = (w + 31) / 32, tilesY = (h + 31) / 32;
        std::vector<uint8_t> maxima, reach;
        uint32_t mp = 0, rp = 0;
        std::vector<uint8_t> cocBytes, levels[16];
        uint32_t cp = 0, lp[16] = {};
        if (g_dofProfile)
        {
            rb.buffer(x.graph, products.maxima, (uint64_t)tilesX * tilesY * 32, maxima, mp);
            rb.buffer(x.graph, products.reach, (uint64_t)tilesX * tilesY * 32, reach, rp);
            rb.texture(x.graph, products.coc, cocBytes, cp);
            for (uint32_t c = 1; c < 8; ++c)
            {
                rb.texture(x.graph, products.colour[c], levels[2 * c], lp[2 * c]);
                rb.texture(x.graph, products.shape[c], levels[2 * c + 1], lp[2 * c + 1]);
            }
        }
        x.graph.execute(nullptr);
        rb.finish();
        if (device.drainDebugMessages() != 0) fail("D3D12 debug layer errors");
        if (g_dofProfile)
        {
            auto fnv = [](const std::vector<uint8_t>& v) {
                uint64_t hv = 1469598103934665603ull;
                for (uint8_t b8 : v) hv = (hv ^ b8) * 1099511628211ull;
                return (unsigned long long)hv;
            };
            logf("    intermediate hashes: maxima %016llx reach %016llx coc %016llx\n", fnv(maxima), fnv(reach), fnv(cocBytes));
            for (uint32_t c = 1; c < 8; ++c) logf("      level %u: colour %016llx shape %016llx\n", c, fnv(levels[2 * c]), fnv(levels[2 * c + 1]));
            for (uint32_t ty = 0; ty < tilesY; ++ty)
            {
                std::string row;
                for (uint32_t tx = 0; tx < tilesX; ++tx)
                {
                    uint32_t zeros = 0;
                    for (uint32_t y = ty * 32; y < std::min(h, ty * 32 + 32); ++y)
                        for (uint32_t xx = tx * 32; xx < tx * 32 + 32; ++xx)
                        {
                            float r;
                            std::memcpy(&r, cocBytes.data() + (size_t)y * cp + (size_t)xx * 4, 4);
                            zeros += r == 0.0f;
                        }
                    row += unx::format(" %4u", zeros);
                }
                logf("      zero radii per tile, row %u:%s\n", ty, row.c_str());
            }
            std::string cols;
            for (uint32_t xx = 0; xx < 64; ++xx)
            {
                uint32_t zeros = 0;
                for (uint32_t y = 0; y < 64; ++y)
                {
                    float r;
                    std::memcpy(&r, cocBytes.data() + (size_t)y * cp + (size_t)xx * 4, 4);
                    zeros += r == 0.0f;
                }
                cols += zeros == 64 ? '0' : zeros == 0 ? '.' : 'p';
            }
            logf("      zero columns x 0..63 (rows 0..63): %s\n", cols.c_str());
            for (uint32_t y = 0; y < 32; ++y)
            {
                std::string line;
                for (uint32_t xx = 0; xx < 32; ++xx)
                {
                    float r;
                    std::memcpy(&r, cocBytes.data() + (size_t)y * cp + (size_t)xx * 4, 4);
                    line += r == 0.0f ? '0' : '.';
                }
                logf("      tile 0 row %2u: %s\n", y, line.c_str());
            }
        }
    };
    std::vector<std::vector<uint8_t>> firstRuns;
    for (const DofScene& s : scenes)
    {
        std::vector<uint8_t> out, again;
        uint32_t op = 0, ap = 0;
        run(s, out, op);
        run(s, again, ap);
        expect(unx::format("depth of field [%s]: two runs bit identical", s.name).c_str(), out == again);
        firstRuns.push_back(out);
        if (g_dofProfile)
        {
            uint64_t hGpu = 1469598103934665603ull, hIn = 1469598103934665603ull;
            for (uint8_t v : out) hGpu = (hGpu ^ v) * 1099511628211ull;
            for (float v : s.colour) hIn = (hIn ^ asUint(v)) * 1099511628211ull;
            for (float v : s.device) hIn = (hIn ^ asUint(v)) * 1099511628211ull;
            logf("    hashes: input %016llx, GPU output %016llx\n", (unsigned long long)hIn, (unsigned long long)hGpu);
        }
        const double k0 = 0.5 * fpx * s.aperture / s.focus, k1 = 0.5 * fpx * s.aperture / zNear;
        std::vector<double> ref;
        std::vector<uint32_t> holes;
        dofReference(s, k0, k1, ref, holes);
        double se = 0, sr = 0, worst = 0, sum = 0;
        uint64_t judged = 0, holed = 0, bitSame = 0, worstX = 0, worstY = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t xx = 0; xx < w; ++xx)
                for (int c = 0; c < 3; ++c) sum += ref[((size_t)y * w + xx) * 3 + c];
        const double mean = sum / (3.0 * w * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t xx = 0; xx < w; ++xx)
            {
                const size_t i = (size_t)y * w + xx;
                float g[4];
                std::memcpy(g, out.data() + (size_t)y * op + (size_t)xx * 16, 16);
                bitSame += std::memcmp(g, &s.colour[i * 4], 16) == 0;
                if (holes[i])
                {
                    ++holed;
                    continue;
                }
                ++judged;
                for (int c = 0; c < 3; ++c)
                {
                    const double r = ref[i * 3 + c], e = g[c] - r;
                    se += e * e;
                    sr += r * r;
                    const double rel = std::abs(e) / std::max(r, 0.25 * mean);
                    if (rel > worst) worst = rel, worstX = xx, worstY = y;
                }
            }
        const double relMse = se / std::max(sr, 1e-30);
        if (g_dofProfile)  // (--dof-profile) GPU / reference (green) around the worst pixel
            for (int dy = -2; dy <= 2; ++dy)
            {
                std::string line;
                for (int dx = -2; dx <= 2; ++dx)
                {
                    const int xx = (int)worstX + dx, y = (int)worstY + dy;
                    if (xx < 0 || y < 0 || xx >= (int)w || y >= (int)h) continue;
                    float g[4];
                    std::memcpy(g, out.data() + (size_t)y * op + (size_t)xx * 16, 16);
                    line += unx::format(" %7.3f/%7.3f", g[1], ref[((size_t)y * w + xx) * 3 + 1]);
                }
                logf("    around the worst:%s\n", line.c_str());
            }
        if (g_dofProfile)  // (--dof-profile) mean GPU and reference per 16 px column band
            for (uint32_t b0 = 0; b0 < w; b0 += 16)
            {
                double g0 = 0, r0 = 0;
                uint32_t n0 = 0;
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t xx = b0; xx < b0 + 16; ++xx)
                    {
                        const size_t i = (size_t)y * w + xx;
                        if (holes[i]) continue;
                        float g[4];
                        std::memcpy(g, out.data() + (size_t)y * op + (size_t)xx * 16, 16);
                        g0 += g[1];
                        r0 += ref[i * 3 + 1];
                        ++n0;
                    }
                const double rho = 0.5 * fpx * s.aperture * (1 / s.focus - 1 / (s.w ? zNear / s.device[(size_t)(h / 2) * w + b0 + 8] : 1));
                logf("    columns %3u..%3u (rho %+.1f): gpu %.4f ref %.4f (%u px)\n", b0, b0 + 15, rho, n0 ? g0 / n0 : 0.0, n0 ? r0 / n0 : 0.0, n0);
            }
        logf("depth of field [%s]: aperture %.4f m, focus %.3f m; %llu pixels judged, %llu with hidden surfaces showing (left out), relMSE %.3e, worst %.3e at "
             "(%llu, %llu), %llu pixels bit identical to the input\n",
             s.name, s.aperture, s.focus, (unsigned long long)judged, (unsigned long long)holed, relMse, worst, (unsigned long long)worstX,
             (unsigned long long)worstY, (unsigned long long)bitSame);
        if (std::string(s.name).rfind("a wall at the focus", 0) == 0)
        {
            double change = 0;  // largest |output - input| relative to the input
            for (size_t i = 0; i < (size_t)w * h; ++i)
            {
                float g[4];
                std::memcpy(g, out.data() + (i / w) * op + (i % w) * 16, 16);
                for (int c = 0; c < 3; ++c) change = std::max(change, (double)std::abs(g[c] - s.colour[i * 4 + c]) / std::max(s.colour[i * 4 + c], 1e-3f));
            }
            logf("  a wall at the focus distance: largest change %.2e of the input\n", change);
            expect("depth of field: a wall at the focus distance keeps the image (float rounding of rho = 0: < 1e-4)", change < 1e-4);
        }
        else
        {
            // the coarse octaves' texels smooth a steep plane's bokeh edges (|rho| changes 0.625 px per px at 60 px)
            const double limit = std::string(s.name).find("60 px") != std::string::npos ? 1e-2 : 1e-3;
            expect(unx::format("depth of field [%s]: vs the CPU thin-lens ray cast (relMSE < %.0e)", s.name, limit).c_str(), relMse < limit);
        }
    }
    // every scene again after all the others (the transient memory holds other data now): the same image
    for (size_t k = 0; k < scenes.size(); ++k)
    {
        std::vector<uint8_t> later;
        uint32_t lp = 0;
        run(scenes[k], later, lp);
        expect(unx::format("depth of field [%s]: a later run (other memory contents) bit identical", scenes[k].name).c_str(), later == firstRuns[k]);
    }
    return failures;
}
} // namespace

// --dof-time (render C, A5 cost): the depth of field passes at 3840 x 2160 on synthetic depth, GPU time per pass
// (GpuProfiler, median of 8 frames after 2 warm-up frames):
//   aim     the aiming case: a weapon (15 % of the view, 0.35 m, rho about -30 px) over a scene at 15..40 m in focus at 20 m;
//   blur    the whole view blurred: a wall at 50 m, focus 1 m, rho about +20 px;
//   focus   the whole view near focus: |rho| <= 0.4 px.
// Input and output are RGBA16F as in the frame (ShadingSystem's intermediate).
template <typename LoadQuality>
void timeDepthOfField(Device& device, ShaderLibrary& shaders, const LoadQuality& loadQuality)
{
    const uint32_t w = 3840, h = 2160;
    const ViewDesc desc = ViewDesc::fromCamera(scene::Camera{}, w, h, float4x4{});
    const double zNear = desc.nearPlane, fpx = 0.5 * h * desc.proj.m[1][1];
    auto apertureFor = [&](double r, double z, double zf) { return (float)(2 * r / (fpx * std::abs(1 / zf - 1 / z))); };
    struct Case
    {
        const char* name;
        float aperture, focus;
        std::vector<float> depth, colour;
        std::vector<uint16_t> half;  // the colour as RGBA16F (the frame's intermediate format)
    };
    auto toHalf = [](float f) {  // round to nearest even, finite inputs
        uint32_t x;
        std::memcpy(&x, &f, 4);
        const uint32_t sign = (x >> 16) & 0x8000u;
        const int32_t e = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
        uint32_t m = x & 0x7FFFFFu;
        if (e <= 0) return (uint16_t)sign;
        if (e >= 31) return (uint16_t)(sign | 0x7C00u);
        uint32_t hv = sign | (uint32_t)e << 10 | m >> 13;
        const uint32_t rest = m & 0x1FFFu;
        if (rest > 0x1000u || (rest == 0x1000u && (hv & 1))) ++hv;
        return (uint16_t)hv;
    };
    auto make = [&](const char* name, float aperture, float focus, auto&& depthAt) {
        Case c{ name, aperture, focus, {}, {}, {} };
        c.depth.resize((size_t)w * h);
        c.colour.resize((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                c.depth[(size_t)y * w + x] = (float)(zNear / depthAt(x, y));
                const float n = hashUnit(x, y, 7);
                float* o = &c.colour[((size_t)y * w + x) * 4];
                o[0] = 0.3f + 0.2f * n, o[1] = 0.3f + 0.2f * std::sin(0.01f * x), o[2] = 0.3f + 0.2f * std::cos(0.013f * y), o[3] = 1;
                if (x % 97 == 13 && y % 89 == 7) o[0] = o[1] = o[2] = 40;  // sparse HDR points
            }
        c.half.resize(c.colour.size());
        for (size_t i = 0; i < c.colour.size(); ++i) c.half[i] = toHalf(c.colour[i]);
        return c;
    };
    std::vector<Case> cases;
    auto weapon = [&](uint32_t x, uint32_t y) { return x >= 0.55 * w && x < 0.95 * w && y >= 0.62 * h; };
    cases.push_back(make("aim", apertureFor(30, 0.35, 20), 20.0f, [&](uint32_t x, uint32_t y) { return weapon(x, y) ? 0.35 : 15.0 + 25.0 * y / h; }));
    cases.push_back(make("blur", apertureFor(20, 50, 1), 1.0f, [&](uint32_t, uint32_t) { return 50.0; }));
    const double zMid = 3.0;
    cases.push_back(make("focus", apertureFor(0.4, 1.5, zMid), (float)zMid, [&](uint32_t x, uint32_t) { return 1.0 / (1 / 1.5 + (1 / 6.0 - 1 / 1.5) * (x + 0.5) / w); }));
    GpuProfiler profiler(device, 2, 256);
    for (const Case& c : cases)
    {
        std::map<std::string, std::vector<double>> passMs;
        std::vector<double> totals;
        const uint32_t warm = 2, frames = 8;
        auto collect = [&](bool keep) {
            if (const FrameTiming* t = profiler.lastCompleted())
            {
                double sum = 0;
                for (const PassTiming& pt : t->passes)
                    if (pt.name.rfind("m.dof.", 0) == 0)
                    {
                        if (keep) passMs[pt.name].push_back(pt.durationMs());
                        sum += pt.durationMs();
                    }
                if (keep) totals.push_back(sum);
            }
        };
        static uint64_t frameIndex = 0;
        for (uint32_t i = 0; i < warm + frames + 2; ++i)
        {
            profiler.beginFrame(frameIndex++);
            collect(i >= warm + 2);
            if (i >= warm + frames) continue;  // the last two only read back
            const QualityConfig q = loadQuality();
            Context x(device, shaders, q);
            x.frame.lensAperture = c.aperture;
            x.frame.lensFocus = c.focus;
            ViewResources v;
            v.view = desc;
            std::vector<ComPtr<ID3D12Resource>> keep;
            const TextureRef image = uploadTexture(x, device, "post.time.dof.image", w, h, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, c.half.data(), keep);
            v.depth = uploadTexture(x, device, "post.time.dof.depth", w, h, DXGI_FORMAT_R32_FLOAT, 4, c.depth.data(), keep);
            const TextureRef focused = x.graph.createTexture(TextureDesc{ "post.time.dof.out", w, h, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });  // the frame's intermediate format
            shading::depthOfField(x.fc, v, image, focused, nullptr);
            x.graph.addPass("post.time.keep", QueueType::Graphics, [&](PassBuilder& b) {
                b.use(focused, Use::SrvCompute);
                b.keep();
            }, [](PassContext&) {});
            x.graph.execute(&profiler);
            device.waitIdle();
        }
        auto median = [](std::vector<double> v) {
            if (v.empty()) return 0.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        logf("  dof time [%s] 4K: %.3f ms (median of %zu frames)", c.name, median(totals), totals.size());
        for (const auto& [name, v] : passMs) logf(", %s %.3f", name.c_str() + 6, median(v));
        logf("\n");
    }
}

int main(int argc, char** argv)
{
    try
    {
        if (argc > 2 && std::strcmp(argv[1], "--levels") == 0) kLevels = (uint32_t)std::atoi(argv[2]);
        bool dofOnly = false, warp = false, dofTime = false;
        for (int i = 1; i < argc; ++i)
        {
            dofOnly = dofOnly || std::strcmp(argv[i], "--dof") == 0;
            dofTime = dofTime || std::strcmp(argv[i], "--dof-time") == 0;
            warp = warp || std::strcmp(argv[i], "--warp") == 0;
            g_dofProfile = g_dofProfile || std::strcmp(argv[i], "--dof-profile") == 0;
        }
        DeviceOptions o;
        o.debugLayer = true;
        ComPtr<ID3D12Device> warpDevice;
        if (warp)  // --warp: the software adapter (correctness while hardware runs are held)
        {
            ComPtr<IDXGIFactory6> factory;
            check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&warpDevice))))
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&warpDevice)), "WARP device");
            o.externalDevice = warpDevice.Get();
            o.debugLayer = false;
        }
        Device device(o);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        auto loadQuality = [] { return QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality"); };
        if (dofTime)
        {
            timeDepthOfField(device, shaders, loadQuality);
            return 0;
        }
        if (dofOnly)
        {
            const int f = testDepthOfField(device, shaders, loadQuality);
            logf(f ? "POST TESTS FAILED (%d)\n" : "POST TESTS PASSED\n", f);
            return f ? 1 : 0;
        }
        const QualityConfig quality = loadQuality();
        int failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-76s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };

        // 1. The bloom tail at every texel parity of the impulse (a stride-2 point-sampled pyramid gains or loses energy by
        //    parity).
        {
            uint32_t pitch = 0, tw = 0, th = 0;
            const uint32_t positions[4][2] = { { kSize / 2, kSize / 2 }, { kSize / 2 + 1, kSize / 2 }, { kSize / 2, kSize / 2 + 1 }, { kSize / 2 + 1, kSize / 2 + 1 } };
            std::vector<uint8_t> first;
            double worst = 0, other = 0;
            for (const auto& at : positions)
            {
                const std::vector<uint8_t> a = tail(device, shaders, quality, at[0], at[1], pitch, tw, th);
                if (first.empty()) first = a;
                double energy = 0;
                for (uint32_t y = 0; y < th; ++y)
                    for (uint32_t x = 0; x < tw; ++x)
                    {
                        uint16_t h[4];
                        std::memcpy(h, a.data() + (size_t)y * pitch + (size_t)x * 8, 8);
                        energy += halfToFloat(h[0]);
                        other += std::abs(halfToFloat(h[1])) + std::abs(halfToFloat(h[2]));
                    }
                energy *= 4;  // half-resolution texels: 4 fine pixels each
                const double error = std::abs(energy - kEnergy) / kEnergy;
                worst = std::max(worst, error);
                logf("bloom tail of a %g impulse at (%u, %u) of %ux%u, %u levels: energy %.4f (relative error %.2e)\n", kEnergy, at[0], at[1], kSize, kSize,
                     kLevels, energy, error);
            }
            const std::vector<uint8_t> again = tail(device, shaders, quality, positions[0][0], positions[0][1], pitch, tw, th);
            expect("bloom: the PSF tail conserves an impulse's energy at every parity (half)", worst < 5e-4);
            expect("bloom: no energy in the other channels", other == 0);
            expect("bloom: deterministic (two runs bit identical)", first == again);
        }

        // 2. The final pass against the CPU reference (identity LUT, vignetting 0.7).
        const std::filesystem::path cube = std::filesystem::temp_directory_path() / "unx_post_test_identity.cube";
        {
            std::ofstream out(cube);
            out << "LUT_3D_SIZE 33\n";
            for (int b = 0; b < 33; ++b)
                for (int g = 0; g < 33; ++g)
                    for (int r = 0; r < 33; ++r) out << r / 32.0 << ' ' << g / 32.0 << ' ' << b / 32.0 << '\n';
        }
        const std::string lut = "shading.post_lut=\"" + cube.generic_string() + "\"";
        {
            QualityConfig q = loadQuality();
            q.applyOverride(lut);
            q.applyOverride("shading.post_vignette=0.7");
            std::vector<uint8_t> hdr, out;
            uint32_t hp = 0, op = 0;
            chain(device, shaders, q, hdr, hp, out, op);
            const float4x4 proj = ViewDesc::fromCamera(scene::Camera{}, kWidth, kHeight, float4x4{}).proj;
            uint64_t offByOne = 0, total = 0;
            int64_t signedSum = 0;
            int worst = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    uint16_t h[4];
                    std::memcpy(h, hdr.data() + (size_t)y * hp + (size_t)x * 8, 8);
                    float e[3] = { halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2]) };
                    const float nx = (x + 0.5f) / kWidth * 2 - 1, ny = (y + 0.5f) / kHeight * 2 - 1;
                    const float tx = nx / proj.m[0][0], ty = ny / proj.m[1][1];
                    const float c2 = 1.0f / (1.0f + tx * tx + ty * ty);
                    const float vig = 1.0f + (c2 * c2 - 1.0f) * 0.7f;
                    for (float& c : e) c = std::max(c * vig, 0.0f);
                    unx::test::filmCurve(e, 1.0f);
                    const float n = hashUnit(x, y, kFrame ^ 0x5bd1e995u) + hashUnit(y, x, kFrame + 104729u) - 1.0f;
                    uint32_t word;
                    std::memcpy(&word, out.data() + (size_t)y * op + (size_t)x * 4, 4);
                    for (int c = 0; c < 3; ++c)
                    {
                        const float display = saturate(srgb(saturate(e[c])) + n / 1023.0f);
                        const int expected = (int)std::lround(display * 1023.0f), got = (int)((word >> (10 * c)) & 1023u);
                        const int d = std::abs(expected - got);
                        worst = std::max(worst, d);
                        offByOne += d == 1;
                        signedSum += got - expected;
                        ++total;
                    }
                }
            logf("final pass vs CPU reference: worst %d steps, %.3f %% of channels off by one, mean %+.5f steps\n", worst, 100.0 * offByOne / total,
                 (double)signedSum / total);
            expect("final pass: every channel within 1 10-bit step of the reference", worst <= 1);
            expect("final pass: at most 0.01 % of channels off by one", offByOne * 10000 <= total);
        }

        // 3. Grain and bloom: deterministic, grain zero-mean.
        {
            QualityConfig grainQ = loadQuality(), bloomQ = loadQuality();
            grainQ.applyOverride(lut);
            grainQ.applyOverride("shading.post_grain=0.01");
            grainQ.applyOverride("shading.post_bloom_strength=0.04");
            bloomQ.applyOverride(lut);
            bloomQ.applyOverride("shading.post_bloom_strength=0.04");
            std::vector<uint8_t> hdr, a, b, c;
            uint32_t hp = 0, pa = 0, pb = 0, pc = 0;
            chain(device, shaders, grainQ, hdr, hp, a, pa);
            chain(device, shaders, grainQ, hdr, hp, b, pb);
            chain(device, shaders, bloomQ, hdr, hp, c, pc);
            double sum = 0;
            uint64_t changed = 0, n = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t x = 0; x < kWidth; ++x)
                {
                    uint32_t wa, wc;
                    std::memcpy(&wa, a.data() + (size_t)y * pa + (size_t)x * 4, 4);
                    std::memcpy(&wc, c.data() + (size_t)y * pc + (size_t)x * 4, 4);
                    for (int k = 0; k < 3; ++k)
                    {
                        const int va = (int)((wa >> (10 * k)) & 1023u), vc = (int)((wc >> (10 * k)) & 1023u);
                        // channels the grain could clip (2.45 sigma relative: up to 26 codes below white) are not zero-mean;
                        // chosen by the reference value, so the selection does not depend on the noise
                        if (vc < 8 || vc > 1023 - 32) continue;
                        sum += va - vc;
                        changed += va != vc;
                        ++n;
                    }
                }
            logf("grain 0.01: %.1f %% of the channels it cannot clip changed, mean %+.4f steps\n", 100.0 * changed / n, sum / n);
            expect("grain and bloom: two runs bit identical", a == b);
            expect("grain: changes the image, zero-mean (|mean| < 0.1 step)", changed > n / 2 && std::abs(sum / n) < 0.1);
        }
        // 4. HDR display output: peak 4 against the generalised curve, peak 1 against the SDR curve.
        {
            const float4x4 proj = ViewDesc::fromCamera(scene::Camera{}, kWidth, kHeight, float4x4{}).proj;
            for (const float peak : { 4.0f, 1.0f })
            {
                QualityConfig q = loadQuality();
                q.applyOverride("shading.post_vignette=0.7");
                std::vector<uint8_t> hdr, out;
                uint32_t hp = 0, op = 0;
                chain(device, shaders, q, hdr, hp, out, op, peak);
                double worst = 0, top = 0;
                for (uint32_t y = 0; y < kHeight; ++y)
                    for (uint32_t x = 0; x < kWidth; ++x)
                    {
                        uint16_t h[4], ov[4];
                        std::memcpy(h, hdr.data() + (size_t)y * hp + (size_t)x * 8, 8);
                        std::memcpy(ov, out.data() + (size_t)y * op + (size_t)x * 8, 8);
                        float e[3] = { halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2]) };
                        const float nx = (x + 0.5f) / kWidth * 2 - 1, ny = (y + 0.5f) / kHeight * 2 - 1;
                        const float tx = nx / proj.m[0][0], ty = ny / proj.m[1][1];
                        const float c2 = 1.0f / (1.0f + tx * tx + ty * ty);
                        const float vig = 1.0f + (c2 * c2 - 1.0f) * 0.7f;
                        for (float& c : e) c = std::max(c * vig, 0.0f);
                        unx::test::filmCurve(e, peak);
                        for (int c = 0; c < 3; ++c)
                        {
                            const float expected = saturate(e[c] / peak) * peak, got = halfToFloat(ov[c]);
                            worst = std::max(worst, (double)std::abs(got - expected) / std::max(expected, 1e-3f));
                            top = std::max(top, (double)got);
                        }
                    }
                logf("HDR display peak %.0f: worst relative error %.2e vs the CPU curve, largest output %.4f\n", peak, worst, top);
                expect(peak == 1.0f ? "HDR peak 1: the SDR curve before encoding (half precision)" : "HDR peak 4: the generalised curve (half precision)", worst < 2e-3);
                expect("HDR: never above the peak", top <= peak * (1 + 1e-3));
            }
        }
        // 5. motion blur's gather against the exact exposure integral
        {
            const QualityConfig q = loadQuality();
            Context x(device, shaders, q);
            x.frame.frameIndex = kFrame;
            PostView v = postView(device);
            const TextureRef image = x.graph.createTexture(TextureDesc{ "post.test.motion.image", kWidth, kHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            const TextureRef velocity = x.graph.createTexture(TextureDesc{ "post.test.motion.velocity", kWidth, kHeight, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
            const TextureRef depthTexture = x.graph.createTexture(TextureDesc{ "post.test.motion.depth", kWidth, kHeight, 1, 1, DXGI_FORMAT_R32_FLOAT });
            const TextureRef blurred = x.graph.createTexture(TextureDesc{ "post.test.motion.blurred", kWidth, kHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            const float vx = 40.0f;
            fill(x, shaders, image, 3);
            fill(x, shaders, velocity, 2, asUint(vx), asUint(0.0f), 0);
            fill(x, shaders, depthTexture, 2, asUint(0.01f), 0, 0);
            v.view.depth = depthTexture;
            shading::motionBlurWithVelocity(x.fc, v.view, image, blurred, velocity);
            std::vector<uint8_t> out;
            uint32_t op = 0;
            Readbacks rb{ device };
            rb.texture(x.graph, blurred, out, op);
            x.graph.execute(nullptr);
            rb.finish();
            if (device.drainDebugMessages() != 0) fail("D3D12 debug layer errors");
            const double L = 0.5 * vx, pi2 = 6.283185307179586;
            double worst[2] = { 0, 0 }, mean[2] = { 0, 0 };
            uint64_t n = 0;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t xx = 8; xx + 48 < kWidth; ++xx)
                {
                    uint16_t h[4];
                    std::memcpy(h, out.data() + (size_t)y * op + (size_t)xx * 8, 8);
                    const double x0 = xx + 0.5;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const double lambda = ch == 0 ? 64.0 : 23.0, k = pi2 / lambda;
                        const double exact = 0.5 + 0.4 * (std::cos(k * x0) - std::cos(k * (x0 + L))) / (k * L);
                        const double e = std::abs(halfToFloat(h[ch]) - exact);
                        worst[ch] = std::max(worst[ch], e);
                        mean[ch] += e;
                    }
                    ++n;
                }
            logf("motion blur 20 px streak vs the exact integral: period 64 px mean %.4f worst %.4f, period 23 px mean %.4f worst %.4f\n", mean[0] / n, worst[0],
                 mean[1] / n, worst[1]);
            // the midpoint rule's error h^2 / 24 max|f''| with h = 2 px: 6.4e-4 (period 64) and 5.0e-3 (period 23), + half precision
            expect("motion blur: the gather matches the exposure integral (period 64: worst < 1.5e-3)", worst[0] < 1.5e-3);
            expect("motion blur: ... near a full period inside the streak (period 23: worst < 6e-3)", worst[1] < 6e-3);
        }
        // 6. heat haze
        {
            const QualityConfig q = loadQuality();
            Context x(device, shaders, q);
            PostView v = postView(device);
            const uint32_t qw = (kWidth + 3) / 4, qh = (kHeight + 3) / 4;
            const TextureRef image = x.graph.createTexture(TextureDesc{ "post.test.haze.image", kWidth, kHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            const TextureRef offset = x.graph.createTexture(TextureDesc{ "post.test.haze.offset", qw, qh, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
            const TextureRef haze = x.graph.createTexture(TextureDesc{ "post.test.haze.depth", qw, qh, 1, 1, DXGI_FORMAT_R16_FLOAT });
            const TextureRef behind = x.graph.createTexture(TextureDesc{ "post.test.haze.behind", kWidth, kHeight, 1, 1, DXGI_FORMAT_R32_FLOAT });
            const TextureRef front = x.graph.createTexture(TextureDesc{ "post.test.haze.front", kWidth, kHeight, 1, 1, DXGI_FORMAT_R32_FLOAT });
            const TextureRef outBehind = x.graph.createTexture(TextureDesc{ "post.test.haze.outBehind", kWidth, kHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            const TextureRef outFront = x.graph.createTexture(TextureDesc{ "post.test.haze.outFront", kWidth, kHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            fill(x, shaders, image, 3);
            fill(x, shaders, offset, 2, asUint(3.0f), asUint(0.0f), 0);
            fill(x, shaders, haze, 2, asUint(0.5f), 0, 0);
            fill(x, shaders, behind, 2, asUint(0.1f), 0, 0);
            fill(x, shaders, front, 2, asUint(0.9f), 0, 0);
            v.view.distortionOffset = offset;
            v.view.distortionDepth = haze;
            v.view.depth = behind;
            shading::distortion(x.fc, v.view, image, outBehind);
            v.view.depth = front;
            shading::distortion(x.fc, v.view, image, outFront);
            std::vector<uint8_t> in, ob, of;
            uint32_t ip = 0, bp = 0, fp = 0;
            Readbacks rb{ device };
            rb.texture(x.graph, image, in, ip);
            rb.texture(x.graph, outBehind, ob, bp);
            rb.texture(x.graph, outFront, of, fp);
            x.graph.execute(nullptr);
            rb.finish();
            if (device.drainDebugMessages() != 0) fail("D3D12 debug layer errors");
            double worst = 0;
            bool frontSame = true;
            for (uint32_t y = 0; y < kHeight; ++y)
                for (uint32_t xx = 0; xx < kWidth; ++xx)
                {
                    frontSame = frontSame && std::memcmp(in.data() + (size_t)y * ip + (size_t)xx * 8, of.data() + (size_t)y * fp + (size_t)xx * 8, 8) == 0;
                    if (xx < 4 || xx + 8 >= kWidth) continue;
                    uint16_t h[4];
                    std::memcpy(h, ob.data() + (size_t)y * bp + (size_t)xx * 8, 8);
                    // the bilinear image at x + 2.4: texel values at centres j + 0.5, linear between them
                    const double xs = xx + 0.5 + 2.4 - 0.5;
                    const int j = (int)std::floor(xs);
                    const double f = xs - j;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        const double lambda = ch == 0 ? 64.0 : 23.0;
                        auto texel = [&](int t) {
                            uint16_t v4[4];
                            std::memcpy(v4, in.data() + (size_t)y * ip + (size_t)t * 8, 8);
                            return (double)halfToFloat(v4[ch]);
                        };
                        (void)lambda;
                        const double expected = texel(j) * (1 - f) + texel(j + 1) * f;
                        worst = std::max(worst, std::abs(halfToFloat(h[ch]) - expected));
                    }
                }
            logf("heat haze: behind the front vs the image at x + 2.4 (bilinear): worst %.2e; in front unchanged: %s\n", worst, frontSame ? "yes" : "no");
            expect("heat haze: pixels behind read the image at p + D (1 - z_haze / z_pixel)", worst < 1e-3);
            expect("heat haze: pixels in front of the haze are unchanged (bit identical)", frontSame);
        }
        // 7. the rotation stage against the exposure integral over the arc
        {
            const QualityConfig q = loadQuality();
            Context x(device, shaders, q);
            PostView v = postView(device);
            const TextureRef image = x.graph.createTexture(TextureDesc{ "post.test.rotation.image", kWidth, kHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            const TextureRef blurred = x.graph.createTexture(TextureDesc{ "post.test.rotation.blurred", kWidth, kHeight, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            fill(x, shaders, image, 4, 0, 0, 0, v.view.frameConstants);
            const float phi = 0.2f, cp = std::cos(phi), sp = std::sin(phi);
            const float3 Q[3] = { float3{ cp, 0, sp }, float3{ 0, 1, 0 }, float3{ -sp, 0, cp } };  // rotation about +y by phi
            const bool engaged = shading::motionRotationBlur(x.fc, v.view, image, blurred, Q);
            std::vector<uint8_t> out;
            uint32_t op = 0;
            Readbacks rb{ device };
            rb.texture(x.graph, blurred, out, op);
            x.graph.execute(nullptr);
            rb.finish();
            if (device.drainDebugMessages() != 0) fail("D3D12 debug layer errors");
            const double shutter = q.number("shading.motion_blur_shutter");
            const double tanV = 1.0 / v.view.view.proj.m[1][1], tanH = 1.0 / v.view.view.proj.m[0][0];
            auto env = [](double dx, double dy, double dz, int ch) {
                const double lon = std::atan2(dx, -dz);
                return ch == 0 ? 0.5 + 0.4 * std::sin(8 * lon) : ch == 1 ? 0.5 + 0.3 * dy : 0.5 + 0.2 * std::sin(3 * lon + 2 * dy);
            };
            double worst = 0, mean = 0;
            uint32_t worstX = 0, worstY = 0;
            int worstCh = 0;
            double worstGpu = 0, worstCpu = 0;
            uint64_t n = 0;
            for (uint32_t y = 0; y < kHeight; y += 3)
                for (uint32_t xx = 0; xx < kWidth; xx += 3)
                {
                    const double nx = (xx + 0.5) / kWidth * 2 - 1, ny = 1 - (y + 0.5) / kHeight * 2;
                    double dx = nx * tanH, dy = ny * tanV, dz = -1;
                    const double l = std::sqrt(dx * dx + dy * dy + dz * dz);
                    dx /= l; dy /= l; dz /= l;
                    // the arc's end on the image?
                    const double a1 = shutter * phi, ex = std::cos(a1) * dx + std::sin(a1) * dz, ez = -std::sin(a1) * dx + std::cos(a1) * dz;
                    // (a latitude circle bends on the image: the arc and its bilinear rows must stay inside both ways, 4 px from the
                    // border; outside it the frame has no information)
                    if (std::abs(nx) > 1 - 8.0 / kWidth || std::abs(ny) > 1 - 8.0 / kHeight || !(ez < 0) || std::abs(ex / -ez) > tanH * (1 - 8.0 / kWidth) || std::abs(dy / -ez) > tanV * (1 - 8.0 / kHeight)) continue;
                    double acc[3] = { 0, 0, 0 };
                    const int samples = 256;
                    for (int k = 0; k < samples; ++k)
                    {
                        const double a = shutter * phi * (k + 0.5) / samples, c = std::cos(a), s = std::sin(a);
                        const double rx = c * dx + s * dz, rz = -s * dx + c * dz;
                        for (int ch = 0; ch < 3; ++ch) acc[ch] += env(rx, dy, rz, ch) / samples;
                    }
                    uint16_t h[4];
                    std::memcpy(h, out.data() + (size_t)y * op + (size_t)xx * 8, 8);
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        const double e = std::abs(halfToFloat(h[ch]) - acc[ch]);
                        if (e > worst) { worstX = xx; worstY = y; worstCh = ch; worstGpu = halfToFloat(h[ch]); worstCpu = acc[ch]; }
                        worst = std::max(worst, e);
                        mean += e;
                        ++n;
                    }
                }
            logf("rotation stage (yaw 0.2 rad/frame, arc %.3f rad): %s, %llu channels, mean %.2e worst %.2e at (%u, %u) vs the arc integral\n", shutter * phi,
                 engaged ? "engaged" : "not engaged", (unsigned long long)n, n ? mean / n : 0.0, worst, worstX, worstY);
            logf("  worst channel %d: GPU %.5f, CPU %.5f\n", worstCh, worstGpu, worstCpu);
            expect("motion blur rotation: engaged for a 31 px streak with the axis outside the view", engaged);
            expect("motion blur rotation: the arc mean of the image (worst < 2e-3: half precision)", n > 1000 && worst < 2e-3);
        }
        failures += testDepthOfField(device, shaders, loadQuality);
        std::filesystem::remove(cube);
        logf(failures ? "POST TESTS FAILED (%d)\n" : "POST TESTS PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
