#include "unx/metrics/Metrics.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

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

namespace
{
void putU32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }
void putF32(std::string& s, float v) { s.append(reinterpret_cast<const char*>(&v), 4); }
void attribute(std::string& h, const char* name, const char* type, const std::string& value)
{
    h += name;
    h += '\0';
    h += type;
    h += '\0';
    putU32(h, (uint32_t)value.size());
    h += value;
}
std::string exrHeader(uint32_t w, uint32_t h)
{
    std::string head;
    putU32(head, 20000630u);  // magic 76 2f 31 01
    putU32(head, 2u);         // version 2, single-part scanline
    std::string channels;
    for (const char* c : { "B", "G", "R" })  // alphabetical, as the format requires
    {
        channels += c;
        channels += '\0';
        putU32(channels, 2u);  // FLOAT
        channels += std::string(4, '\0');  // pLinear, reserved
        putU32(channels, 1u);
        putU32(channels, 1u);
    }
    channels += '\0';
    attribute(head, "channels", "chlist", channels);
    attribute(head, "compression", "compression", std::string(1, '\0'));
    std::string box;
    putU32(box, 0);
    putU32(box, 0);
    putU32(box, w - 1);
    putU32(box, h - 1);
    attribute(head, "dataWindow", "box2i", box);
    attribute(head, "displayWindow", "box2i", box);
    attribute(head, "lineOrder", "lineOrder", std::string(1, '\0'));
    std::string one;
    putF32(one, 1.0f);
    attribute(head, "pixelAspectRatio", "float", one);
    std::string centre;
    putF32(centre, 0.0f);
    putF32(centre, 0.0f);
    attribute(head, "screenWindowCenter", "v2f", centre);
    attribute(head, "screenWindowWidth", "float", one);
    head += '\0';
    return head;
}
} // namespace

void writeExr(const std::filesystem::path& path, const Image& img)
{
    if (img.width == 0 || img.height == 0 || img.rgb.size() != (size_t)img.width * img.height * 3) fail("writeExr: empty or malformed image");
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    const std::string head = exrHeader(img.width, img.height);
    const uint64_t lineBytes = 8 + (uint64_t)img.width * 12;
    std::ofstream f(path, std::ios::binary);
    if (!f) fail("cannot write %s", path.string().c_str());
    f.write(head.data(), (std::streamsize)head.size());
    const uint64_t first = head.size() + 8ull * img.height;
    for (uint32_t y = 0; y < img.height; ++y)
    {
        const uint64_t o = first + y * lineBytes;
        f.write(reinterpret_cast<const char*>(&o), 8);
    }
    std::vector<float> line((size_t)img.width * 3);
    for (uint32_t y = 0; y < img.height; ++y)
    {
        const int32_t yy = (int32_t)y;
        const uint32_t size = img.width * 12;
        f.write(reinterpret_cast<const char*>(&yy), 4);
        f.write(reinterpret_cast<const char*>(&size), 4);
        for (uint32_t c = 0; c < 3; ++c)  // B, G, R planes
            for (uint32_t x = 0; x < img.width; ++x) line[(size_t)c * img.width + x] = img.pixel(x, y)[2 - c];
        f.write(reinterpret_cast<const char*>(line.data()), (std::streamsize)size);
    }
    if (!f) fail("cannot write %s", path.string().c_str());
}

Image readExr(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("cannot read %s", path.string().c_str());
    const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // The layout writeExr produces: find the data window, then the offset table after the header's terminator.
    const size_t dw = all.find(std::string("dataWindow\0box2i\0", 17));
    if (all.size() < 8 || dw == std::string::npos) fail("%s: not an EXR written by writeExr", path.string().c_str());
    int32_t box[4];
    std::memcpy(box, all.data() + dw + 17 + 4, 16);
    Image img;
    img.width = (uint32_t)(box[2] - box[0] + 1);
    img.height = (uint32_t)(box[3] - box[1] + 1);
    const std::string head = exrHeader(img.width, img.height);
    if (all.compare(0, head.size(), head) != 0) fail("%s: EXR header differs from writeExr's layout", path.string().c_str());
    img.rgb.resize((size_t)img.width * img.height * 3);
    const uint64_t lineBytes = 8 + (uint64_t)img.width * 12;
    if (all.size() != head.size() + 8ull * img.height + lineBytes * img.height) fail("%s: truncated EXR", path.string().c_str());
    for (uint32_t y = 0; y < img.height; ++y)
    {
        const char* p = all.data() + head.size() + 8ull * img.height + y * lineBytes + 8;
        for (uint32_t c = 0; c < 3; ++c)
            for (uint32_t x = 0; x < img.width; ++x) std::memcpy(&img.pixel(x, y)[2 - c], p + ((size_t)c * img.width + x) * 4, 4);
    }
    return img;
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
