#include "unx/metrics/Metrics.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#pragma warning(push, 0)
#include "FLIP.h"
#pragma warning(pop)

namespace unx::metrics
{
namespace
{
void requireSameSize(const Image& a, const Image& b)
{
    if (a.width != b.width || a.height != b.height || a.rgb.size() != b.rgb.size())
        fail("metrics: image sizes differ (%ux%u vs %ux%u)", a.width, a.height, b.width, b.height);
}

FlipResult runFlip(const Image& reference, const Image& test, bool hdr)
{
    requireSameSize(reference, test);
    FLIP::Parameters parameters;  // defaults: 0.7 m viewing distance, 3840 px over 0.7 m (ARCHITECTURE 3)
    FLIP::image<FLIP::color3> ref, tst;
    ref.setPixels(reference.rgb.data(), (int)reference.width, (int)reference.height);
    tst.setPixels(test.rgb.data(), (int)test.width, (int)test.height);
    FLIP::image<float> error((int)reference.width, (int)reference.height, 0.0f);
    FLIP::evaluate(ref, tst, hdr, parameters, error);
    FlipResult r;
    r.errorMap.resize((size_t)reference.width * reference.height);
    double sum = 0;
    for (uint32_t y = 0; y < reference.height; ++y)
        for (uint32_t x = 0; x < reference.width; ++x)
        {
            const float e = error.get((int)x, (int)y);
            r.errorMap[(size_t)y * reference.width + x] = e;
            sum += e;
        }
    r.mean = sum / (double)r.errorMap.size();
    std::vector<float> sorted = r.errorMap;
    const size_t k = std::min(sorted.size() - 1, (size_t)std::ceil(0.99 * (double)sorted.size()) - 1);
    std::nth_element(sorted.begin(), sorted.begin() + (ptrdiff_t)k, sorted.end());
    r.p99 = sorted[k];
    return r;
}
} // namespace

Image readPfm(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("cannot read %s", path.string().c_str());
    std::string magic;
    uint32_t w = 0, h = 0;
    double scale = 0;
    f >> magic >> w >> h >> scale;
    f.get();  // single whitespace before the raster
    if (magic != "PF") fail("%s: only 3-channel PFM (PF) is supported", path.string().c_str());
    if (scale > 0) fail("%s: big-endian PFM is not supported", path.string().c_str());
    Image img;
    img.width = w;
    img.height = h;
    img.rgb.resize((size_t)w * h * 3);
    for (uint32_t row = 0; row < h; ++row)  // file rows are bottom to top
        f.read(reinterpret_cast<char*>(img.pixel(0, h - 1 - row)), (std::streamsize)w * 3 * sizeof(float));
    if (!f) fail("%s: truncated", path.string().c_str());
    return img;
}

void writePfm(const std::filesystem::path& path, const Image& img)
{
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) fail("cannot write %s", path.string().c_str());
    f << "PF\n" << img.width << " " << img.height << "\n-1.0\n";
    for (uint32_t row = 0; row < img.height; ++row)
        f.write(reinterpret_cast<const char*>(img.pixel(0, img.height - 1 - row)), (std::streamsize)img.width * 3 * sizeof(float));
}

double relMse(const Image& reference, const Image& test)
{
    requireSameSize(reference, test);
    double sum = 0;
    for (size_t i = 0; i < reference.rgb.size(); ++i)
    {
        const double r = reference.rgb[i], t = test.rgb[i];
        sum += (t - r) * (t - r) / (r * r + 0.01);
    }
    return sum / (double)reference.rgb.size();
}

FlipResult flipHdr(const Image& reference, const Image& test) { return runFlip(reference, test, true); }
FlipResult flipLdr(const Image& reference, const Image& test) { return runFlip(reference, test, false); }

std::vector<double> temporalInstability(const std::vector<Image>& frames)
{
    std::vector<double> out;
    auto lum = [](const float* p) { return 0.2126 * p[0] + 0.7152 * p[1] + 0.0722 * p[2]; };
    for (size_t i = 1; i < frames.size(); ++i)
    {
        requireSameSize(frames[i - 1], frames[i]);
        double diff = 0, mean = 0;
        const size_t n = (size_t)frames[i].width * frames[i].height;
        for (size_t p = 0; p < n; ++p)
        {
            const double a = lum(&frames[i - 1].rgb[3 * p]), b = lum(&frames[i].rgb[3 * p]);
            diff += std::fabs(b - a);
            mean += b;
        }
        out.push_back((diff / n) / (mean / n + 1e-6));
    }
    return out;
}
} // namespace unx::metrics
