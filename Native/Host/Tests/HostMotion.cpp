// Motion blur through the host (render A item A5; COVERAGE 14.12 (2b), M MotionBlur.cpp), every renderer with
// gi.deterministic = true and a fixed exposure so frames compare exactly:
//   1. a static camera over a static scene: the 180 degree shutter changes nothing (bit identical to shutter 0: the
//      frame's image is already its integral);
//   2. the camera yawing delta per frame: a vertical edge of the box becomes a ramp as long as the streak, 0.5 x delta in
//      pixels at the image centre, x sec^2 of the angle off the axis elsewhere (velocity from the vis buffer's surfaces
//      and the previous view, then the gather): the ramp's 10-90 % width is 0.8 of the streak at the edge for a box blur,
//      within 12 % (one pixel of the count); the sharp frame's edge is a step (<= 2 px);
//   3. no D3D12 debug-layer errors.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 960, kHeight = 540, kFrames = 8;

// Frames of a camera turning 'yaw' radians per frame about +y (0: static); the last frame's pixels.
std::vector<uint32_t> renderTurn(float shutter, float yaw, uint32_t& errors)
{
    HostRendererOptions o;
    o.standalone = true;
    o.framesInFlight = 2;
    o.shaderDirectory = executableDirectory() / "shaders";
    o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
    o.qualityOverrides = { "gi.deterministic=true", "shading.motion_blur_shutter=" + std::to_string(shutter) };
    HostRenderer h(o);
    h.scene() = test::oneBox();
    h.commit();
    const scene::Camera base = h.scene().cameras[0];
    std::vector<uint32_t> pixels((size_t)kWidth * kHeight);
    for (uint32_t f = 0; f < kFrames; ++f)
    {
        FramePacket p;
        p.frameIndex = f;
        p.deltaTime = 1.0f / 60;
        p.width = kWidth;
        p.height = kHeight;
        p.camera = base;
        const float a = yaw * (float)f, c = std::cos(a), s = std::sin(a);
        const float3 fw = base.forward;
        p.camera.forward = normalize(float3{ c * fw.x + s * fw.z, fw.y, -s * fw.x + c * fw.z });
        const bool last = f + 1 == kFrames;
        h.renderStandalone(h.queueFrame(std::move(p)), last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
    }
    errors += h.debugErrors();
    return pixels;
}

float luma(uint32_t p) { return 0.2126f * (p & 1023u) + 0.7152f * ((p >> 10) & 1023u) + 0.0722f * ((p >> 20) & 1023u); }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const std::string out = argc > 2 && std::string(argv[1]) == "--out" ? argv[2] : std::string();
        uint32_t failures = 0, errors = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-86s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };

        // 1. static
        const std::vector<uint32_t> still = renderTurn(0.5f, 0.0f, errors), stillSharp = renderTurn(0.0f, 0.0f, errors);
        expect("static: the 180 degree shutter changes nothing (bit identical to shutter 0)", still == stillSharp);

        // 2. turning: the streak length of the camera's rotation at the image centre (pinhole: px per radian = (W/2) /
        //    tan(hfov/2); hfov from the vertical fov and the aspect)
        const scene::Camera camera = test::oneBox().cameras[0];
        const double tanV = std::tan(camera.verticalFov * 0.5), tanH = tanV * kWidth / kHeight;
        const float yaw = 0.05f;                                  // 2.9 degrees per frame
        const double pxPerRad = (kWidth * 0.5) / tanH;            // at the centre
        const double streak = 0.5 * yaw * pxPerRad;               // 180 degree shutter
        const std::vector<uint32_t> blurred = renderTurn(0.5f, yaw, errors), sharp = renderTurn(0.0f, yaw, errors);
        if (!out.empty())
            for (int k = 0; k < 2; ++k)
            {
                std::ofstream f(std::filesystem::path(out) / (k ? "motion_sharp.ppm" : "motion_blurred.ppm"), std::ios::binary);
                f << "P6\n" << kWidth << ' ' << kHeight << "\n255\n";
                for (uint32_t px : (k ? sharp : blurred))
                    for (int c = 0; c < 3; ++c) f.put((char)(((px >> (10 * c)) & 1023u) >> 2));
            }
        uint64_t differing = 0;
        for (size_t i = 0; i < blurred.size(); ++i) differing += blurred[i] != sharp[i];
        logf("  turning: %llu of %zu pixels differ between the 180 degree and the instantaneous shutter\n", (unsigned long long)differing, blurred.size());
        // per row through the box (y 300..400): the strongest step in the sharp frame (its box edge), then the 10-90 % width of
        // the transition between the plateaus 24 px either side, in both frames; the median over the rows
        auto width = [&](const uint32_t* row, int x) {
            const float lo = luma(row[x - 24]), hi = luma(row[x + 24]);
            if (std::abs(hi - lo) < 60) return -1.0;
            const float a = std::min(lo, hi) + 0.1f * std::abs(hi - lo), c = std::max(lo, hi) - 0.1f * std::abs(hi - lo);
            int count = 0;  // pixels strictly between the 10 % and 90 % levels
            for (int k = -24; k <= 24; ++k)
            {
                const float v = luma(row[x + k]);
                count += v > a && v < c;
            }
            return (double)count;
        };
        std::vector<double> widths, sharpWidths;
        for (uint32_t y = 300; y <= 400; y += 10)
        {
            const uint32_t* s = sharp.data() + (size_t)y * kWidth;
            const uint32_t* b = blurred.data() + (size_t)y * kWidth;
            float bestStep = 0;
            uint32_t edge = 0;
            for (uint32_t x = 32; x + 32 < kWidth; ++x)
            {
                const float step = std::abs(luma(s[x + 1]) - luma(s[x - 1]));
                if (step > bestStep) { bestStep = step; edge = x; }
            }
            if (bestStep < 100) continue;
            const double wb = width(b, (int)edge), ws = width(s, (int)edge);
            if (wb < 0 || ws < 0) continue;
            // the streak at this edge: a pinhole's pixels per radian at a pixel theta off the axis are (W/2) / tan(hfov/2)
            // x sec^2(theta)
            const double xn = ((double)edge + 0.5 - kWidth * 0.5) / (kWidth * 0.5), t = xn * tanH;
            const double streakHere = 0.5 * yaw * (kWidth * 0.5) / tanH * (1 + t * t);
            widths.push_back(wb / (0.8 * streakHere));  // measured over expected
            sharpWidths.push_back(ws);
        }
        auto median = [](std::vector<double> v) {
            if (v.empty()) return -1.0;
            std::sort(v.begin(), v.end());
            return v[v.size() / 2];
        };
        const double ratio = median(widths), sharpWidth = median(sharpWidths);
        logf("  yaw %.3f rad/frame: streak %.2f px at the centre; edge 10-90 %% width / (0.8 x the streak at the edge) %.3f, sharp edge %.1f px (median of %zu rows)\n",
             yaw, streak, ratio, sharpWidth, widths.size());
        expect("turning: the sharp frame has a step edge (<= 2 px)", sharpWidth >= 0 && sharpWidth <= 2);
        expect("turning: the blurred edge is a ramp of 0.8 x the streak there (within 12 %: one pixel of the count)", ratio > 0 && std::abs(ratio - 1) < 0.12);
        expect("D3D12 debug layer errors 0", errors == 0);
        logf(failures ? "HOST MOTION TEST FAILED (%u)\n" : "HOST MOTION TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
