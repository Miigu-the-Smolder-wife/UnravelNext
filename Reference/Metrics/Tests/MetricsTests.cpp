// Metrics self-checks: PFM round trip, relMSE of known perturbations, FLIP identity and monotonicity, temporal metric.
#include "unx/core/Log.h"
#include "unx/metrics/Metrics.h"

#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace unx;

namespace
{
metrics::Image gradient(uint32_t w, uint32_t h, float scale)
{
    metrics::Image img;
    img.width = w;
    img.height = h;
    img.rgb.resize((size_t)w * h * 3);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            float* p = img.pixel(x, y);
            p[0] = scale * (x + 1) / w;
            p[1] = scale * (y + 1) / h;
            p[2] = scale * 0.25f * (((x / 8 + y / 8) & 1) ? 1.f : 0.2f);
        }
    return img;
}
#define CHECK(c) do { if (!(c)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #c); } while (0)
} // namespace

int main()
{
    try
    {
        const metrics::Image a = gradient(128, 96, 2.0f);
        const auto path = std::filesystem::temp_directory_path() / "unx_metrics_roundtrip.pfm";
        metrics::writePfm(path, a);
        const metrics::Image b = metrics::readPfm(path);
        std::filesystem::remove(path);
        CHECK(b.width == a.width && b.height == a.height && b.rgb == a.rgb);  // exact round trip, rows oriented

        CHECK(metrics::relMse(a, a) == 0);
        metrics::Image c = a;
        for (float& v : c.rgb) v *= 1.1f;
        const double rel = metrics::relMse(a, c);
        CHECK(rel > 0 && rel < 0.01);

        const auto same = metrics::flipHdr(a, a);
        CHECK(same.mean == 0 && same.p99 == 0);
        metrics::Image d = a;
        for (float& v : d.rgb) v *= 1.5f;
        const auto small = metrics::flipHdr(a, c), large = metrics::flipHdr(a, d);
        CHECK(small.mean > 0 && large.mean > small.mean && large.p99 >= large.mean);

        const std::vector<metrics::Image> still = { a, a, a };
        for (double v : metrics::temporalInstability(still)) CHECK(v == 0);
        const std::vector<metrics::Image> flicker = { a, c, a };
        for (double v : metrics::temporalInstability(flicker)) CHECK(v > 0.05);
        logf("metrics tests passed (relMSE 10%% gain %.5f, HDR-FLIP mean 10%% %.4f / 50%% %.4f)\n", rel, small.mean, large.mean);
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL %s\n", e.what());
        return 1;
    }
}
