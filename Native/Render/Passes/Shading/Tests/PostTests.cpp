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
// Options: --levels N (6, test 1).
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/scene/SceneData.h"
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
// CPU references of ShadingCommon.hlsli: shPbrNeutralPeak (peak 1 = shPbrNeutral).
float pbrNeutralPeak(float c[3], float peak)
{
    const float startCompression = 0.8f * peak - 0.04f, desaturation = 0.15f;
    const float x = std::min(c[0], std::min(c[1], c[2]));
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    for (int i = 0; i < 3; ++i) c[i] -= offset;
    const float top = std::max(c[0], std::max(c[1], c[2]));
    if (top < startCompression) return top;
    const float d = peak - startCompression;
    const float newPeak = peak - d * d / (top + d - startCompression);
    for (int i = 0; i < 3; ++i) c[i] *= newPeak / top;
    const float g = 1 - 1 / (desaturation * (top - newPeak) / peak + 1);
    for (int i = 0; i < 3; ++i) c[i] = c[i] * (1 - g) + newPeak * g;
    return top;
}
float pbrNeutral(float c[3])
{
    const float startCompression = 0.8f - 0.04f, desaturation = 0.15f;
    const float x = std::min(c[0], std::min(c[1], c[2]));
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    for (int i = 0; i < 3; ++i) c[i] -= offset;
    const float peak = std::max(c[0], std::max(c[1], c[2]));
    if (peak < startCompression) return peak;
    const float d = 1 - startCompression;
    const float newPeak = 1 - d * d / (peak + d - startCompression);
    for (int i = 0; i < 3; ++i) c[i] *= newPeak / peak;
    const float g = 1 - 1 / (desaturation * (peak - newPeak) + 1);
    for (int i = 0; i < 3; ++i) c[i] = c[i] * (1 - g) + newPeak * g;
    return peak;
}
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
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc > 2 && std::strcmp(argv[1], "--levels") == 0) kLevels = (uint32_t)std::atoi(argv[2]);
        DeviceOptions o;
        o.debugLayer = true;
        Device device(o);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        auto loadQuality = [] { return QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality"); };
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
                    pbrNeutral(e);
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
                        if (peak == 1.0f) pbrNeutral(e);
                        else pbrNeutralPeak(e, peak);
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
