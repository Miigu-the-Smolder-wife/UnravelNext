#include "unx/metrics/Census.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace unx::metrics
{
namespace
{
constexpr char kMagic[8] = { 'U', 'N', 'X', 'I', 'D', 'S', '1', '\0' };

bool matches(uint64_t entry, uint64_t sub)
{
    if (entry == sub) return true;
    if (sub == kSkyId || entry == kSkyId) return false;
    return (uint32_t)entry == kAnyTriangle && (entry >> 32) == (sub >> 32);
}

CensusResult run(const std::vector<uint64_t>& sub, uint32_t stride, uint32_t W, uint32_t H,
                 const std::vector<uint64_t>& centre, const IdentityImage* engine)
{
    if (stride < 16) fail("census: stride %u < 16", stride);
    if (sub.size() != (size_t)W * H * stride) fail("census: %zu sub-samples for %ux%u x %u", sub.size(), W, H, stride);
    uint64_t d2 = 0, d3 = 0, d5 = 0, d9 = 0, m1 = 0, m4 = 0, m8 = 0;
    std::vector<uint64_t> set;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            set.clear();
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const uint32_t nx = (uint32_t)std::clamp((int)x + dx, 0, (int)W - 1), ny = (uint32_t)std::clamp((int)y + dy, 0, (int)H - 1);
                    set.push_back(centre[(size_t)ny * W + nx]);
                }
            const size_t p = (size_t)y * W + x;
            if (engine)
                for (uint32_t k = engine->offsets[p] + 1; k < engine->offsets[p + 1]; ++k) set.push_back(engine->ids[k]);
            const uint64_t* s = &sub[p * stride];
            uint64_t distinct[16];
            uint32_t nd = 0, missed = 0;
            for (uint32_t i = 0; i < 16; ++i)
            {
                if (std::find(distinct, distinct + nd, s[i]) == distinct + nd) distinct[nd++] = s[i];
                bool found = false;
                for (uint64_t e : set)
                    if (matches(e, s[i]))
                    {
                        found = true;
                        break;
                    }
                missed += found ? 0 : 1;
            }
            d2 += nd >= 2;
            d3 += nd >= 3;
            d5 += nd >= 5;
            d9 += nd >= 9;
            m1 += missed >= 1;
            m4 += missed >= 4;
            m8 += missed >= 8;
        }
    CensusResult r;
    r.pixels = (uint64_t)W * H;
    const double n = (double)r.pixels;
    r.distinct2 = d2 / n;
    r.distinct3 = d3 / n;
    r.distinct5 = d5 / n;
    r.distinct9 = d9 / n;
    r.missed1 = m1 / n;
    r.missed4 = m4 / n;
    r.missed8 = m8 / n;
    return r;
}
} // namespace

IdentityImage readIdentities(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("cannot read %s", path.string().c_str());
    char magic[8];
    f.read(magic, 8);
    if (!f || std::memcmp(magic, kMagic, 8) != 0) fail("%s: not a .unxids file", path.string().c_str());
    IdentityImage img;
    f.read(reinterpret_cast<char*>(&img.width), 4);
    f.read(reinterpret_cast<char*>(&img.height), 4);
    img.offsets.resize((size_t)img.width * img.height + 1);
    f.read(reinterpret_cast<char*>(img.offsets.data()), (std::streamsize)(img.offsets.size() * 4));
    if (!f) fail("%s: truncated offsets", path.string().c_str());
    for (size_t i = 1; i < img.offsets.size(); ++i)
        if (img.offsets[i] <= img.offsets[i - 1]) fail("%s: pixel %zu has no vis-buffer identity", path.string().c_str(), i - 1);
    img.ids.resize(img.offsets.back());
    f.read(reinterpret_cast<char*>(img.ids.data()), (std::streamsize)(img.ids.size() * 8));
    if (!f) fail("%s: truncated ids", path.string().c_str());
    return img;
}

void writeIdentities(const std::filesystem::path& path, const IdentityImage& img)
{
    if (img.offsets.size() != (size_t)img.width * img.height + 1 || img.ids.size() != img.offsets.back()) fail("census: inconsistent identity image");
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) fail("cannot write %s", path.string().c_str());
    f.write(kMagic, 8);
    f.write(reinterpret_cast<const char*>(&img.width), 4);
    f.write(reinterpret_cast<const char*>(&img.height), 4);
    f.write(reinterpret_cast<const char*>(img.offsets.data()), (std::streamsize)(img.offsets.size() * 4));
    f.write(reinterpret_cast<const char*>(img.ids.data()), (std::streamsize)(img.ids.size() * 8));
}

CensusResult census(const std::vector<uint64_t>& subsamples, uint32_t stride, const IdentityImage& engine)
{
    const size_t n = (size_t)engine.width * engine.height;
    std::vector<uint64_t> centre(n);
    for (size_t p = 0; p < n; ++p) centre[p] = engine.ids[engine.offsets[p]];
    return run(subsamples, stride, engine.width, engine.height, centre, &engine);
}

CensusResult selfCensus(const std::vector<uint64_t>& subsamples17, uint32_t W, uint32_t H)
{
    const size_t n = (size_t)W * H;
    std::vector<uint64_t> centre(n);
    for (size_t p = 0; p < n; ++p) centre[p] = subsamples17[p * 17 + 16];
    return run(subsamples17, 17, W, H, centre, nullptr);
}
} // namespace unx::metrics
