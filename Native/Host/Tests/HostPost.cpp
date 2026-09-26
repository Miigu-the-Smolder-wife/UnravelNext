// HDR post chain through the host (render A item A4; FEATURES_GAME 4 and 6, M Post.cpp), quality overrides before commit
// (HostRendererOptions::qualityOverrides / overrideQuality, ABI UnxRendererQualityOverride):
// Every renderer runs with gi.deterministic = true (the GI's update selection by key priority instead of atomic order;
// the default false makes two renderers differ in about 4 % of the channels by up to 7 10-bit steps [실측]), so frames of
// different renderers compare exactly; the chain's exact terms are checked on synthetic inputs by Render's PostTests.
//   0. baseline: two plain renderers bit identical;
//   1. identity grading LUT only: the chain path (writers' linear variant into the RGBA16F target -> PostFinal's curve,
//      LUT and 10-bit dither) matches the direct writers' encoding: every channel within 2 10-bit steps (the dither is
//      one triangular step), the mean difference within 0.1 step;
//   2. vignetting only (1.0): the centre block within 1 step (mean; cos^4 is not flat there), the corners much darker;
//   3. every term on (bloom 0.04, vignette 0.5, grain 0.01, the LUT): two renderers bit identical, the frame differs
//      from the plain one;
//   4. an override after commit is refused (the render threads read the config);
//   5. an HDR display (FramePacket::displayPeak 4): the RGBA16F output is finite, within [0, peak], and where the SDR
//      frame is below the curve's shoulder the two agree (the HDR curve's linear part through sRGB within 2 steps).
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 640, kHeight = 360, kFrames = 6;

// The frames' last output: RGB10A2 words (SDR) or RGBA16F halves (displayPeak > 0; 4 per pixel).
std::vector<uint32_t> renderFrames(const std::vector<std::string>& overrides, uint32_t& debugErrors, float displayPeak = 0)
{
    HostRendererOptions o;
    o.standalone = true;
    o.framesInFlight = 2;
    o.shaderDirectory = executableDirectory() / "shaders";
    o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
    o.qualityOverrides = overrides;
    o.qualityOverrides.insert(o.qualityOverrides.begin(), "gi.deterministic=true");
    HostRenderer h(o);
    h.scene() = test::oneBox();
    h.commit();
    const scene::Camera camera = h.scene().cameras[0];
    std::vector<uint32_t> pixels((size_t)kWidth * kHeight * (displayPeak > 0 ? 2 : 1));
    for (uint32_t f = 0; f < kFrames; ++f)
    {
        FramePacket p;
        p.frameIndex = f;
        p.deltaTime = 1.0f / 60;
        p.width = kWidth;
        p.height = kHeight;
        p.camera = camera;
        p.displayPeak = displayPeak;
        const bool last = f + 1 == kFrames;
        h.renderStandalone(h.queueFrame(std::move(p)), last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
    }
    debugErrors += h.debugErrors();
    return pixels;
}

int channel(uint32_t p, int c) { return (int)((p >> (10 * c)) & 1023u); }

struct Diff
{
    int maxAbs = 0;
    double mean = 0;
};
// Channel differences b - a over the pixels [x0, x1) x [y0, y1) (10-bit steps).
Diff diff(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    Diff d;
    double sum = 0;
    uint64_t n = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            for (int c = 0; c < 3; ++c)
            {
                const int v = channel(b[(size_t)y * kWidth + x], c) - channel(a[(size_t)y * kWidth + x], c);
                d.maxAbs = std::max(d.maxAbs, std::abs(v));
                sum += v;
                ++n;
            }
    d.mean = sum / (double)n;
    return d;
}
// Fraction of channels whose difference exceeds 'steps'.
double beyond(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b, int steps)
{
    uint64_t n = 0;
    for (size_t i = 0; i < a.size(); ++i)
        for (int c = 0; c < 3; ++c) n += std::abs(channel(a[i], c) - channel(b[i], c)) > steps;
    return (double)n / (3.0 * (double)a.size());
}
double meanValue(const std::vector<uint32_t>& a, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    double sum = 0;
    for (uint32_t y = y0; y < y1; ++y)
        for (uint32_t x = x0; x < x1; ++x)
            for (int c = 0; c < 3; ++c) sum += channel(a[(size_t)y * kWidth + x], c);
    return sum / (3.0 * (x1 - x0) * (y1 - y0));
}
} // namespace

int main()
{
    try
    {
        uint32_t failures = 0, errors = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-86s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        // an identity 33^3 .cube (R fastest)
        const std::filesystem::path cube = std::filesystem::temp_directory_path() / "unx_post_identity.cube";
        {
            std::ofstream out(cube);
            out << "TITLE \"identity\"\nLUT_3D_SIZE 33\n";
            for (int b = 0; b < 33; ++b)
                for (int g = 0; g < 33; ++g)
                    for (int r = 0; r < 33; ++r) out << r / 32.0 << ' ' << g / 32.0 << ' ' << b / 32.0 << '\n';
        }
        const std::string lut = "shading.post_lut=\"" + std::filesystem::path(cube).generic_string() + "\"";

        const std::vector<uint32_t> plain = renderFrames({}, errors);
        const std::vector<uint32_t> plainAgain = renderFrames({}, errors);
        const Diff base = diff(plain, plainAgain, 0, 0, kWidth, kHeight);
        const double baseDiffering = beyond(plain, plainAgain, 0);
        logf("  baseline, two plain renderers: max %d steps, mean %+.4f, %.3f %% of channels differ\n", base.maxAbs, base.mean, 100.0 * baseDiffering);
        expect("baseline: two plain renderers bit identical (gi.deterministic)", plain == plainAgain);
        const std::vector<uint32_t> graded = renderFrames({ lut }, errors);
        const std::vector<uint32_t> vignetted = renderFrames({ "shading.post_vignette=1.0" }, errors);
        const std::vector<std::string> all = { "shading.post_bloom_strength=0.04", "shading.post_vignette=0.5", "shading.post_grain=0.01", lut };
        const std::vector<uint32_t> full = renderFrames(all, errors), fullAgain = renderFrames(all, errors);

        // 1. the chain path with an identity LUT against the direct writers
        const Diff g = diff(plain, graded, 0, 0, kWidth, kHeight);
        const double gBeyond = beyond(plain, graded, 2);
        logf("  identity LUT vs direct: max %d steps, mean %+.4f steps, %.3f %% of channels beyond 2 steps (plain mean value %.1f)\n", g.maxAbs, g.mean,
             100.0 * gBeyond, meanValue(plain, 0, 0, kWidth, kHeight));
        expect("identity LUT: every channel within 2 10-bit steps of the direct writers (dither 1 step)", gBeyond == 0);
        expect("identity LUT: no bias (mean difference within 0.1 step)", std::abs(g.mean) < 0.1);

        // 2. vignetting: the centre block unchanged, the corners darker
        const Diff centre = diff(graded, vignetted, kWidth / 2 - 8, kHeight / 2 - 8, kWidth / 2 + 8, kHeight / 2 + 8);
        const double cornerPlain = meanValue(plain, 0, 0, 24, 24) + meanValue(plain, kWidth - 24, kHeight - 24, kWidth, kHeight);
        const double cornerVignetted = meanValue(vignetted, 0, 0, 24, 24) + meanValue(vignetted, kWidth - 24, kHeight - 24, kWidth, kHeight);
        logf("  vignette: centre mean %+.4f steps (max %d), corners %.1f -> %.1f\n", centre.mean, centre.maxAbs, cornerPlain / 2, cornerVignetted / 2);
        expect("vignette: the centre block unchanged (mean within 1 step)", std::abs(centre.mean) < 1.0);
        expect("vignette: the corners much darker (more than 100 steps)", cornerVignetted < cornerPlain - 200);

        // 3. every term: deterministic, visibly applied
        const Diff f = diff(plain, full, 0, 0, kWidth, kHeight);
        logf("  every term vs plain: max %d steps, mean %+.3f\n", f.maxAbs, f.mean);
        expect("every term: two renderers bit identical", full == fullAgain);
        expect("every term: the frame differs from the plain one", f.maxAbs > 4);

        // 4. overrides after commit are refused
        {
            HostRendererOptions o;
            o.standalone = true;
            o.shaderDirectory = executableDirectory() / "shaders";
            o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
            HostRenderer h(o);
            h.scene() = test::oneBox();
            h.overrideQuality("shading.post_grain=0.02");
            h.commit();
            bool refused = false;
            try
            {
                h.overrideQuality("shading.post_grain=0.03");
            }
            catch (const std::exception&)
            {
                refused = true;
            }
            expect("an override after commit is refused", refused);
            errors += h.debugErrors();
        }
        // 5. HDR display output
        {
            const float peak = 4.0f;
            const std::vector<uint32_t> hdr = renderFrames({}, errors, peak);
            auto half = [&](size_t pixel, int c) {
                uint16_t h;
                std::memcpy(&h, reinterpret_cast<const uint8_t*>(hdr.data()) + pixel * 8 + c * 2, 2);
                const uint32_t sign = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
                if (e == 31) return m ? std::numeric_limits<float>::quiet_NaN() : (sign ? -INFINITY : INFINITY);
                const float f = e == 0 ? std::ldexp((float)m, -24) : std::ldexp(1.0f + m / 1024.0f, (int)e - 15);
                return sign ? -f : f;
            };
            bool finite = true;
            float top = 0, bottom = 0;
            int worst = 0;
            uint64_t compared = 0;
            for (size_t i = 0; i < plain.size(); ++i)
                for (int c = 0; c < 3; ++c)
                {
                    const float v = half(i, c);
                    finite = finite && std::isfinite(v);
                    top = std::max(top, v);
                    bottom = std::min(bottom, v);
                    // below the SDR shoulder (display 0.76 -> 10-bit 900) both curves are the same toe and linear part
                    const int sdr = channel(plain[i], c);
                    if (sdr > 16 && sdr < 880 && v < 0.7f)
                    {
                        const float o = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
                        worst = std::max(worst, std::abs((int)std::lround(o * 1023.0f) - sdr));
                        ++compared;
                    }
                }
            logf("  HDR peak %.0f: output in [%.4f, %.4f], %llu channels below the shoulder vs SDR: worst %d steps\n", peak, bottom, top, (unsigned long long)compared, worst);
            expect("HDR: finite output within [0, peak]", finite && bottom >= 0 && top <= peak * 1.001f);
            expect("HDR: below the shoulder the same image as SDR (2 steps: dither + half)", compared > 1000 && worst <= 2);
        }
        expect("D3D12 debug layer errors 0", errors == 0);
        std::filesystem::remove(cube);
        logf(failures ? "HOST POST TEST FAILED (%u)\n" : "HOST POST TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
