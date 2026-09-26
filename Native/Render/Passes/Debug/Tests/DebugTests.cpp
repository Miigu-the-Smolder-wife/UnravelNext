// Track E debug drawing correctness (A15; no GPU lock needed after the first run of new kernels). Every check compares a
// pixel of the composited output with the value the drawing rules give (DebugDraw.hlsli, DebugCommon.hlsli):
//   1. screen primitives over an SDR (R10G10B10A2, display-encoded) output: a 3 px line's rows at its box-filtered
//      coverage saturate(1.5 + 0.5 - d), a round dot, a half-opaque triangle, a glyph inside its cell only;
//   2. world lines against a scene depth (a wall at 5 m on the left half, sky on the right): hidden behind the wall,
//      drawn in front of it and over the sky; XRay at a quarter of the opacity where hidden;
//   3. GPU appends from a kernel (debugLine, debugNumber 12.5 = "12.50", 5 glyphs) and the read-back counts;
//   4. overflow: 10 appends into a capacity of 4 lines: 10 counted, LinesFull set, the view unchanged past the 4 drawn;
//   5. a linear output (RGBA16F display light): the overlay's sRGB colour is decoded before blending;
//   6. debug.view = depth: Turbo of the log view depth, sky black; the probe prints the value;
//   7. hudLines of a synthetic timing (small passes summed per track).
//   unx_test_debug_debugtests [--no-debug-layer]
#include "../../Atmosphere/Tests/TestFrame.h"

#include "unx/debug/DebugDraw.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuProfiler.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace
{
constexpr uint32_t W = 256, H = 128;

float halfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
float srgbToLinear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float linearToSrgb(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f; }

struct Rgb
{
    float r, g, b;
};

// One test frame: debugBegin first (its UAV goes into the frame constants, as FrameRenderer does), then 'build', then
// debugOverlay over 'view'. The colour target is filled with 'background' first.
class DebugFrame
{
public:
    explicit DebugFrame(TestFrame& tf) : tf(tf)
    {
        ring = tf.makeBuffer(16 * 1024, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE none{ 0, 0 };
        check(ring->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map debug test constants");
    }
    ~DebugFrame() { ring->Unmap(0, nullptr); }

    struct Setup
    {
        DXGI_FORMAT format = DXGI_FORMAT_R10G10B10A2_UNORM;
        float background[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
        bool depth = false;      // left half at 5 m (device depth near / 5), right half sky (0)
        std::function<void(FramePassContext&, ViewResources&)> build;
    };
    std::shared_ptr<std::vector<uint8_t>> run(const Setup& s)
    {
        std::shared_ptr<std::vector<uint8_t>> out;
        tf.run([&](FramePassContext& base) {
            uint32_t slot = 0;
            uint32_t debugUav = 0xFFFFFFFFu;
            FramePassContext fc{ base.device, base.graph, base.shaders, base.quality, base.scene, base.frame, base.resources, base.services,
                                 [&](const ViewDesc& v) {
                                     gpu::FrameConstants c = FrameRenderer::frameConstants(tf.gpuScene, tf.frame, v);
                                     c.debugDraw = debugUav;
                                     std::memcpy(mapped + slot * 1024, &c, sizeof c);
                                     return ring->GetGPUVirtualAddress() + 1024 * (uint64_t)slot++;
                                 },
                                 base.trackState, 2 };
            debugUav = tracks::debugBegin(fc);
            ViewResources view;
            view.view = tf.frame.mainView;
            view.frameConstants = fc.frameConstantsFor(view.view);
            view.color = fc.graph.createTexture(TextureDesc{ "debug.test.colour", W, H, 1, 1, s.format });
            const TextureRef colour = view.color;
            uint32_t bg[4];
            std::memcpy(bg, s.background, sizeof bg);
            ID3D12PipelineState* fill = fc.shaders.compute("Passes/Debug/Tests/DebugTestKernels.MODE0");
            fc.graph.addPass("debug.test.fill", QueueType::Graphics, [&](PassBuilder& b) { b.use(colour, Use::UavCompute); },
                             [=](PassContext& c) {
                                 const uint32_t k[8] = { c.uav(colour), W, H, 0, bg[0], bg[1], bg[2], bg[3] };
                                 c.cmd->SetPipelineState(fill);
                                 c.computeConstants(k, 8);
                                 c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                             });
            if (s.depth)
            {
                view.depth = fc.graph.createTexture(TextureDesc{ "debug.test.depth", W, H, 1, 1, DXGI_FORMAT_D32_FLOAT });
                const TextureRef depth = view.depth;
                const float wall = tf.frame.mainView.nearPlane / 5.0f;
                fc.graph.addPass("debug.test.depth", QueueType::Graphics, [&](PassBuilder& b) { b.use(depth, Use::DepthWrite); },
                                 [=](PassContext& c) {
                                     c.cmd->ClearDepthStencilView(c.dsv(depth), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                                     const D3D12_RECT left{ 0, 0, (LONG)W / 2, (LONG)H };
                                     c.cmd->ClearDepthStencilView(c.dsv(depth), D3D12_CLEAR_FLAG_DEPTH, wall, 0, 1, &left);
                                 });
            }
            if (s.build) s.build(fc, view);
            tracks::debugOverlay(fc, view);
            out = tf.readback(fc, view.color);
        });
        return out;
    }

    // Pixel (x, y) of an SDR (R10G10B10A2) or RGBA16F readback.
    static Rgb pixel(const std::vector<uint8_t>& data, DXGI_FORMAT format, uint32_t x, uint32_t y)
    {
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM)
        {
            uint32_t v;
            std::memcpy(&v, data.data() + (size_t)y * TestFrame::rowPitch(W, 4) + x * 4, 4);
            return { (v & 1023) / 1023.0f, ((v >> 10) & 1023) / 1023.0f, ((v >> 20) & 1023) / 1023.0f };
        }
        uint16_t h[4];
        std::memcpy(h, data.data() + (size_t)y * TestFrame::rowPitch(W, 8) + x * 8, 8);
        return { halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2]) };
    }

    TestFrame& tf;
    ComPtr<ID3D12Resource> ring;
    uint8_t* mapped = nullptr;
};

Rgb blend(Rgb c, float a, const float* bg) { return { c.r * a + bg[0] * (1 - a), c.g * a + bg[1] * (1 - a), c.b * a + bg[2] * (1 - a) }; }
void expectPixel(const std::vector<uint8_t>& data, DXGI_FORMAT format, uint32_t x, uint32_t y, Rgb want, float tol, const char* what)
{
    const Rgb got = DebugFrame::pixel(data, format, x, y);
    const float e = std::max({ std::abs(got.r - want.r), std::abs(got.g - want.g), std::abs(got.b - want.b) });
    S_CHECK(e <= tol, "%s: pixel (%u, %u) = (%.4f, %.4f, %.4f), expected (%.4f, %.4f, %.4f)", what, x, y, got.r, got.g, got.b, want.r, want.g, want.b);
}
// Pixel row of a world point (the view's viewProj; y down).
float rowOf(const ViewDesc& v, float3 p)
{
    const float4x4& m = v.viewProj;
    const float y = m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3];
    const float w = m.m[3][0] * p.x + m.m[3][1] * p.y + m.m[3][2] * p.z + m.m[3][3];
    return (0.5f - 0.5f * y / w) * H;
}
float turboChannel(float t, const float* c4, const float* c2)
{
    const float v4[4] = { 1, t, t * t, t * t * t };
    const float v2[2] = { v4[2] * v4[2], v4[3] * v4[2] };
    return std::clamp(c4[0] * v4[0] + c4[1] * v4[1] + c4[2] * v4[2] + c4[3] * v4[3] + c2[0] * v2[0] + c2[1] * v2[1], 0.0f, 1.0f);
}
Rgb turbo(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    const float r4[4] = { 0.13572138f, 4.61539260f, -42.66032258f, 132.13108234f }, r2[2] = { -152.94239396f, 59.28637943f };
    const float g4[4] = { 0.09140261f, 2.19418839f, 4.84296658f, -14.18503333f }, g2[2] = { 4.27729857f, 2.82956604f };
    const float b4[4] = { 0.10667330f, 12.64194608f, -60.58204836f, 110.36276771f }, b2[2] = { -89.90310912f, 27.34824973f };
    return { turboChannel(t, r4, r2), turboChannel(t, g4, g2), turboChannel(t, b4, b2) };
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--no-debug-layer") debugLayer = false;
            else fail("unknown argument %s", a.c_str());
        }
        TestFrame tf(debugLayer);
        scene::Scene sc;
        sc.name = "debug test";
        scene::Camera cam;
        cam.name = "main";
        cam.position = { 0, 0, 0 };
        cam.forward = { 0, 0, -1 };
        sc.cameras.push_back(cam);
        tf.setScene(sc);
        const ViewDesc view = ViewDesc::fromCamera(cam, W, H, float4x4{});
        tf.frame.mainView = view;
        tf.frame.mainView.prevViewProj = view.viewProj;
        DebugFrame df(tf);
        debug::DrawList& list = debug::drawList(tf.trackState);
        const float tol = 2.5f / 1023.0f;  // 10-bit output + half-float overlay

        // ---- 1. screen primitives, SDR output
        {
            DebugFrame::Setup s;
            list.line({ 20, 20, 0 }, { 100, 20, 0 }, debug::rgba(uint8_t(255), uint8_t(0), uint8_t(0)), 3.0f, debug::Screen);
            list.point({ 200, 30, 0 }, debug::rgba(uint8_t(0), uint8_t(0), uint8_t(255)), 9.0f, debug::Screen);
            list.triangle({ 20, 60, 0 }, { 120, 60, 0 }, { 20, 120, 0 }, debug::rgba(uint8_t(0), uint8_t(255), uint8_t(0), uint8_t(128)), debug::Screen);
            list.text({ 150, 60, 0 }, "A", debug::rgba(uint8_t(255), uint8_t(255), uint8_t(255)), 32.0f, debug::Screen);
            const auto data = df.run(s);
            const float* bg = s.background;
            for (uint32_t y = 15; y <= 25; ++y)
            {
                const float cover = std::clamp(2.0f - std::abs(y + 0.5f - 20.0f), 0.0f, 1.0f);
                expectPixel(*data, s.format, 60, y, blend({ 1, 0, 0 }, cover, bg), tol, "3 px line row");
            }
            expectPixel(*data, s.format, 200, 30, { 0, 0, 1 }, tol, "dot centre");
            expectPixel(*data, s.format, 206, 30, blend({ 0, 0, 1 }, 0, bg), tol, "outside the dot");
            expectPixel(*data, s.format, 40, 70, blend({ 0, 1, 0 }, 128 / 255.0f, bg), tol, "half-opaque triangle");
            expectPixel(*data, s.format, 110, 110, blend({ 0, 1, 0 }, 0, bg), tol, "outside the triangle");
            float maxInside = 0;
            for (uint32_t y = 60; y < 92; ++y)
                for (uint32_t x = 150; x < 166; ++x) maxInside = std::max(maxInside, DebugFrame::pixel(*data, s.format, x, y).r);
            S_CHECK(maxInside > 0.9f, "glyph 'A' drew no opaque pixel in its 16 x 32 cell (max red %.3f)", maxInside);
            for (uint32_t y = 55; y < 97; ++y)
                for (uint32_t x = 145; x < 171; ++x)
                    if (x < 150 || x >= 166 || y < 60 || y >= 92) expectPixel(*data, s.format, x, y, blend({ 1, 1, 1 }, 0, bg), tol, "outside the glyph cell");
            std::printf("screen: line rows at coverage saturate(2 - d), dot, triangle a = 0.5, glyph in its cell only\n");
        }

        // ---- 2. world lines against the scene depth
        {
            DebugFrame::Setup s;
            s.depth = true;
            const float3 hiddenA{ -40, 0, -10 }, hiddenB{ 40, 0, -10 };        // 10 m: behind the 5 m wall on the left
            const float3 xrayA{ -40, -1.5f, -10 }, xrayB{ 40, -1.5f, -10 };
            const float3 frontA{ -8, 0.4f, -2 }, frontB{ 8, 0.4f, -2 };          // 2 m: in front of the wall
            list.line(hiddenA, hiddenB, debug::rgba(uint8_t(0), uint8_t(0), uint8_t(255)), 3.0f, debug::DepthTest);
            list.line(xrayA, xrayB, debug::rgba(uint8_t(255), uint8_t(0), uint8_t(0)), 3.0f, debug::DepthTest | debug::XRay);
            list.line(frontA, frontB, debug::rgba(uint8_t(0), uint8_t(255), uint8_t(0)), 3.0f, debug::DepthTest);
            const auto data = df.run(s);
            const float* bg = s.background;
            auto row = [&](float3 p) { return (uint32_t)std::floor(rowOf(view, p)); };  // the pixel whose centre is within 0.5 px
            expectPixel(*data, s.format, 64, row(hiddenA), blend({ 0, 0, 1 }, 0, bg), tol, "line behind the wall");
            expectPixel(*data, s.format, 192, row(hiddenA), { 0, 0, 1 }, tol, "line over the sky");
            expectPixel(*data, s.format, 64, row(xrayA), blend({ 1, 0, 0 }, 0.25f, bg), tol, "x-ray line behind the wall");
            expectPixel(*data, s.format, 192, row(xrayA), { 1, 0, 0 }, tol, "x-ray line over the sky");
            expectPixel(*data, s.format, 64, row(frontA), { 0, 1, 0 }, tol, "line in front of the wall");
            std::printf("world: hidden behind 5 m, drawn at 2 m and over the sky, x-ray 0.25 where hidden\n");
        }

        // ---- 3. GPU appends; 4. overflow (stats are read back two frames later)
        {
            tf.quality.applyOverride("debug.draw=true");
            DebugFrame::Setup s;
            s.build = [&](FramePassContext& fc, ViewResources& v) {
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Debug/Tests/DebugTestKernels.MODE1");
                const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
                fc.graph.addPass("debug.test.append", QueueType::Graphics, [](PassBuilder& b) { b.keep(); },
                                 [=](PassContext& c) {
                                     c.cmd->SetPipelineState(pso);
                                     c.bindFrameConstants(cb);
                                     c.cmd->Dispatch(1, 1, 1);
                                 });
            };
            const auto data = df.run(s);
            // width 1 at y = 100: rows 99 and 100 (centres 0.5 px away) at coverage 0.5, row 101 none
            expectPixel(*data, s.format, 30, 99, blend({ 1, 1, 1 }, 0.5f, s.background), tol, "GPU line (row 99, d = 0.5)");
            expectPixel(*data, s.format, 30, 100, blend({ 1, 1, 1 }, 0.5f, s.background), tol, "GPU line (row 100, d = 0.5)");
            expectPixel(*data, s.format, 30, 101, blend({ 1, 1, 1 }, 0, s.background), tol, "GPU line (row 101, d = 1.5)");
            float numberMax = 0;
            for (uint32_t y = 100; y < 116; ++y)
                for (uint32_t x = 80; x < 112; ++x) numberMax = std::max(numberMax, DebugFrame::pixel(*data, s.format, x, y).r);
            S_CHECK(numberMax > 0.9f, "GPU number: no opaque pixel in its cells (max %.3f)", numberMax);
            const uint64_t appendFrame = tf.frame.frameIndex - 1;

            tf.quality.applyOverride("debug.max_lines=4");
            DebugFrame::Setup o;
            o.build = [&](FramePassContext& fc, ViewResources& v) {
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Debug/Tests/DebugTestKernels.MODE2");
                const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
                fc.graph.addPass("debug.test.overflow", QueueType::Graphics, [](PassBuilder& b) { b.keep(); },
                                 [=](PassContext& c) {
                                     const uint32_t k[4] = { 10, 0, 0, 0 };
                                     c.cmd->SetPipelineState(pso);
                                     c.bindFrameConstants(cb);
                                     c.computeConstants(k, 4);
                                     c.cmd->Dispatch(2, 1, 1);
                                 });
            };
            const auto over = df.run(o);
            const uint64_t overflowFrame = tf.frame.frameIndex - 1;
            // Line k (width 1 at y = 5 + k) marks rows 4 + k and 5 + k; which 4 of the 10 appends fit is the atomics' order, so 4
            // lines mark 5 (adjacent) to 8 rows of 4..14, all 10 would mark 11.
            uint32_t drawn = 0;
            for (uint32_t y = 4; y <= 14; ++y)
            {
                const Rgb p = DebugFrame::pixel(*over, o.format, 12, y);
                drawn += p.r > 0.5f && p.b < 0.5f;
            }
            S_CHECK(drawn >= 5 && drawn <= 8, "overflow: %u rows marked, expected 5..8 for 4 drawn lines", drawn);
            debug::Stats st = debug::lastStats(tf.trackState);
            tf.quality.applyOverride("debug.max_lines=1048576");
            tf.quality.applyOverride("debug.draw=false");
            for (int k = 0; k < 2; ++k) df.run({});
            st = debug::lastStats(tf.trackState);
            S_CHECK(st.frame == overflowFrame, "stats frame %llu, expected %llu", (unsigned long long)st.frame, (unsigned long long)overflowFrame);
            S_CHECK(st.lines == 10 && (st.status & debug::LinesFull) && !(st.status & debug::GlyphsFull), "overflow stats: %u lines, status 0x%x", st.lines, st.status);
            (void)appendFrame;
            std::printf("gpu: appended line and number drawn; overflow: 10 counted, LinesFull, 4 lines drawn (%u rows)\n", drawn);
        }
        {
            // 3 (counts): the GPU append frame's counts, read back two frames later
            tf.quality.applyOverride("debug.draw=true");
            DebugFrame::Setup s;
            s.build = [&](FramePassContext& fc, ViewResources& v) {
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Debug/Tests/DebugTestKernels.MODE1");
                const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
                fc.graph.addPass("debug.test.append", QueueType::Graphics, [](PassBuilder& b) { b.keep(); },
                                 [=](PassContext& c) {
                                     c.cmd->SetPipelineState(pso);
                                     c.bindFrameConstants(cb);
                                     c.cmd->Dispatch(1, 1, 1);
                                 });
            };
            list.line({ 1, 1, 0 }, { 2, 2, 0 }, debug::rgba(uint8_t(255), uint8_t(255), uint8_t(255)), 1.0f, debug::Screen);
            df.run(s);
            const uint64_t frame = tf.frame.frameIndex - 1;
            tf.quality.applyOverride("debug.draw=false");
            for (int k = 0; k < 2; ++k) df.run({});
            const debug::Stats st = debug::lastStats(tf.trackState);
            S_CHECK(st.frame == frame && st.lines == 2 && st.glyphs == 5 && st.triangles == 0 && st.status == 0,
                    "append stats: frame %llu (want %llu), %u lines (1 CPU + 1 GPU), %u glyphs ('12.50': 4 significant digits), %u triangles, status 0x%x", (unsigned long long)st.frame,
                    (unsigned long long)frame, st.lines, st.glyphs, st.triangles, st.status);
            std::printf("counts: 1 CPU + 1 GPU line, 5 glyphs of '12.50'\n");
        }

        // ---- 5. linear output
        {
            DebugFrame::Setup s;
            s.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            s.background[0] = s.background[1] = s.background[2] = 0.5f;
            list.line({ 20, 40, 0 }, { 100, 40, 0 }, debug::rgba(uint8_t(128), uint8_t(128), uint8_t(128)), 3.0f, debug::Screen);
            list.line({ 20, 80, 0 }, { 100, 80, 0 }, debug::rgba(uint8_t(255), uint8_t(0), uint8_t(0), uint8_t(128)), 3.0f, debug::Screen);
            const auto data = df.run(s);
            const float grey = srgbToLinear(128 / 255.0f), a = 128 / 255.0f;
            expectPixel(*data, s.format, 60, 40, { grey, grey, grey }, 2e-3f, "linear output: opaque sRGB 128");
            expectPixel(*data, s.format, 60, 80, { 1 * a + 0.5f * (1 - a), 0.5f * (1 - a), 0.5f * (1 - a) }, 2e-3f, "linear output: red at a = 0.5");
            std::printf("linear output: sRGB overlay colour decoded before blending\n");
        }

        // ---- 6. debug.view = depth with a probe
        {
            tf.quality.applyOverride("debug.view=\"depth\"");
            tf.quality.applyOverride("debug.view_probe=[64, 64]");
            DebugFrame::Setup s;
            s.depth = true;
            const auto data = df.run(s);
            const uint64_t frame = tf.frame.frameIndex - 1;
            const float nearM = view.nearPlane;
            const Rgb t = turbo(std::log2(5.0f / nearM) / std::log2(10000.0f / nearM));
            expectPixel(*data, s.format, 20, 20, t, 3e-3f, "depth view: Turbo of log depth at 5 m");
            expectPixel(*data, s.format, 200, 20, { 0, 0, 0 }, tol, "depth view: sky black");
            tf.quality.applyOverride("debug.view=\"none\"");
            tf.quality.applyOverride("debug.view_probe=[-1, -1]");
            for (int k = 0; k < 2; ++k) df.run({});
            const debug::Stats st = debug::lastStats(tf.trackState);
            // the probe: a cross of 4 lines and the view depth "5" (1 glyph: an integer value prints exactly)
            S_CHECK(st.frame == frame && st.lines == 4 && st.glyphs >= 1, "probe stats: frame %llu, %u lines, %u glyphs", (unsigned long long)st.frame, st.lines, st.glyphs);
            std::printf("depth view: Turbo(log depth), sky black, probe cross + %u glyphs\n", st.glyphs);
        }

        // ---- 7. HUD text
        {
            FrameTiming t;
            t.frame = 7;
            t.gpuFrameMs = 10.0;
            t.passes = { { "v.vis", QueueType::Graphics, 0.0, 2.0 }, { "m.shade", QueueType::Graphics, 2.0, 7.0 }, { "s.a", QueueType::Graphics, 7.0, 7.05 },
                         { "s.b", QueueType::Graphics, 7.05, 7.1 } };
            debug::Stats st;
            st.frame = 5;
            st.lines = 3;
            st.status = debug::LinesFull;
            const std::vector<std::string> lines = debug::hudLines(&t, st);
            S_CHECK(lines.size() == 5, "hud: %zu lines", lines.size());
            S_CHECK(lines[0].find("10.000 ms") != std::string::npos && lines[1].find("v.vis") == 0 && lines[1].find("2.000") != std::string::npos &&
                        lines[2].find("m.shade") == 0 && lines[2].find("5.000") != std::string::npos && lines[3].find("s.* (2)") == 0 &&
                        lines[3].find("0.100") != std::string::npos && lines[4].find("LINES FULL") != std::string::npos,
                    "hud text:\n%s\n%s\n%s\n%s\n%s", lines[0].c_str(), lines[1].c_str(), lines[2].c_str(), lines[3].c_str(), lines[4].c_str());
            S_CHECK(debug::hudLines(nullptr, debug::Stats{}).size() == 1, "hud without timings");
            std::printf("hud: frame line, passes >= 1%%, small passes summed per track, status\n");
        }
        const uint32_t errors = tf.device.drainDebugMessages();
        S_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("debug tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
