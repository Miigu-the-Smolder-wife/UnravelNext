// Cook checks (C1). CPU parts run anywhere; the BC7 encoder is a GPU (D3D11) kernel: first run under the GPU lock.
#include "unx/cook/TextureCook.h"
#include "unx/core/Log.h"

#include <DirectXTex.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <exception>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include <windows.h>

using namespace unx;

namespace
{
struct TestCase
{
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}
struct Register
{
    Register(const char* name, std::function<void()> fn) { registry().push_back({ name, std::move(fn) }); }
};
#define UNX_TEST(name) \
    static void name(); \
    static Register reg_##name(#name, name); \
    static void name()
#define CHECK(cond) \
    do { if (!(cond)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #cond); } while (0)

// A texture with smooth gradients and mild noise (compressible content; the white-noise texture covers the other side).
std::vector<uint8_t> synthetic(uint32_t w, uint32_t h, uint32_t ch, uint32_t seed, float sigma = 0.5f, float amplitude = 1.0f)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.0f, sigma);
    std::vector<uint8_t> t((size_t)w * h * ch);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
            for (uint32_t c = 0; c < ch; ++c)
            {
                const float u = x / (float)w, v = y / (float)h;
                // One shared shape per texel scaled per channel: colours on a line inside each block (as in most material
                // textures), so the content is compressible at every level.
                const float shape = std::sin(6.2831853f * (u + 0.5f * v)) + 0.4f * std::cos(12.566f * v);
                float val = 128 + amplitude * (60 + 10 * c) * shape;
                val += noise(rng);
                t[((size_t)y * w + x) * ch + c] = (uint8_t)std::clamp(val, 0.0f, 255.0f);
            }
    if (ch == 4)
        for (size_t i = 0; i < (size_t)w * h; ++i) t[i * 4 + 3] = 255;
    return t;
}

scene::Texture texture(const char* name, scene::TextureFormat f, uint32_t w, uint32_t h, uint32_t seed, float amplitude = 1.0f)
{
    const uint32_t ch = f == scene::TextureFormat::Rgba8Srgb || f == scene::TextureFormat::Rgba8Linear ? 4
                        : f == scene::TextureFormat::Rg8Normal || f == scene::TextureFormat::Rg8RoughMetal ? 2 : 1;
    scene::Texture t;
    t.name = name;
    t.width = w;
    t.height = h;
    t.format = f;
    t.texels = synthetic(w, h, ch, seed, 0.5f, amplitude);
    return t;
}

// Five textures: base colour, alpha-tested base colour, normal, rough/metal, occlusion; two materials use them.
scene::Scene cookScene()
{
    scene::Scene s;
    s.textures.push_back(texture("base", scene::TextureFormat::Rgba8Srgb, 256, 128, 1));
    scene::Texture leaf = texture("leaf", scene::TextureFormat::Rgba8Srgb, 128, 128, 2);
    for (size_t i = 0; i < 128 * 128; ++i) leaf.texels[i * 4 + 3] = (uint8_t)((i % 128 < 64) ? 255 : 0);
    s.textures.push_back(leaf);
    s.textures.push_back(texture("normal", scene::TextureFormat::Rg8Normal, 128, 128, 3));
    s.textures.push_back(texture("roughmetal", scene::TextureFormat::Rg8RoughMetal, 128, 64, 4, 0.25f));  // low-contrast masks, as real roughness and AO maps
    s.textures.push_back(texture("occlusion", scene::TextureFormat::R8Linear, 64, 64, 5, 0.25f));
    scene::Texture noise = texture("noise", scene::TextureFormat::Rgba8Srgb, 64, 64, 6);
    std::mt19937 rng(99);
    for (size_t i = 0; i < noise.texels.size(); ++i) noise.texels[i] = (i % 4 == 3) ? 255 : (uint8_t)(rng() & 255);  // white noise
    s.textures.push_back(noise);
    scene::Material a;
    a.name = "a";
    a.baseColorTexture = 0;
    a.normalTexture = 2;
    a.roughMetalTexture = 3;
    a.occlusionTexture = 4;
    scene::Material b;
    b.name = "b";
    b.baseColorTexture = 1;
    b.alphaCutoff = 0.5f;
    scene::Material c;
    c.name = "c";
    c.baseColorTexture = 5;
    s.materials = { a, b, c };
    return s;
}

bool sameChain(const render::material::MipChain& a, const render::material::MipChain& b)
{
    return a.format == b.format && a.width == b.width && a.height == b.height && a.bytesPerTexel == b.bytesPerTexel && a.slopeRange == b.slopeRange &&
           a.levels == b.levels;
}

// An independent decode of a block chain level (DirectXTex from the stored blocks) must equal the encoder's decode check.
std::vector<uint8_t> decodeBlocks(DXGI_FORMAT block, DXGI_FORMAT to, uint32_t w, uint32_t h, const std::vector<uint8_t>& blocks)
{
    using namespace DirectX;
    Image img{};
    img.width = w;
    img.height = h;
    img.format = block;
    const size_t bx = (w + 3) / 4, by = (h + 3) / 4, bb = block == DXGI_FORMAT_BC4_UNORM ? 8 : 16;
    img.rowPitch = bx * bb;
    img.slicePitch = img.rowPitch * by;
    img.pixels = const_cast<uint8_t*>(blocks.data());
    ScratchImage out;
    CHECK(SUCCEEDED(Decompress(img, to, out)));
    const Image* d = out.GetImage(0, 0, 0);
    const uint32_t ch = to == DXGI_FORMAT_R8G8_UNORM ? 2 : to == DXGI_FORMAT_R8_UNORM ? 1 : 4;
    std::vector<uint8_t> t((size_t)w * h * ch);
    for (uint32_t y = 0; y < h; ++y) std::memcpy(t.data() + (size_t)y * w * ch, d->pixels + y * d->rowPitch, (size_t)w * ch);
    return t;
}
} // namespace

UNX_TEST(block_encoders_round_trip)
{
    // BC7 (GPU), BC5, BC4 of synthetic images: blocks decode (independently) to the encoder's own decode check, and the
    // error meets the cook bound on natural-image-like content.
    struct Case
    {
        DXGI_FORMAT source;
        uint32_t ch;
    } cases[] = { { DXGI_FORMAT_R8G8B8A8_UNORM, 4 }, { DXGI_FORMAT_R8G8_UNORM, 2 }, { DXGI_FORMAT_R8_UNORM, 1 } };
    for (const Case& c : cases)
    {
        const uint32_t w = 256, h = 192;
        const std::vector<uint8_t> t = synthetic(w, h, c.ch, 11 + c.ch);
        cook::BlockResult r;
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(cook::encodeLevel(c.source, w, h, t.data(), r));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        logf("    %u channels -> format %u: PSNR %.2f dB, max error %u, %.1f ms\n", c.ch, (unsigned)r.format, r.psnrDb, r.maxAbsError, ms);
        CHECK(r.blocks.size() == (size_t)(w / 4) * (h / 4) * (c.ch == 1 ? 8 : 16));
        CHECK(decodeBlocks(r.format, c.source, w, h, r.blocks) == r.decoded);
        CHECK(r.psnrDb >= cook::kMinPsnrDb && r.maxAbsError <= cook::kMaxAbsError);  // smooth content: within the bound
    }
    // A 1x1 and 2x2 level (the tail of a mip chain) pad to one block.
    const std::vector<uint8_t> tiny = synthetic(2, 2, 4, 3);
    cook::BlockResult r;
    CHECK(cook::encodeLevel(DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, tiny.data(), r) && r.blocks.size() == 16);
    CHECK(decodeBlocks(r.format, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 2, r.blocks) == r.decoded);
}

UNX_TEST(chain_formats_and_levels)
{
    // Each texture's stored form: block formats where they keep the definition, exact chains otherwise; block chains
    // have the exact chain's level count and decode within the bound at every level.
    cook::setTextureCacheDirectory("");
    cook::clearTextureMemoryCache();
    const scene::Scene s = cookScene();
    // The white-noise base colour misses the bound and is stored exactly (failedBound).
    const DXGI_FORMAT expected[6] = { DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_BC5_UNORM,
                                      DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB };
    cook::resetTextureCookStats();
    for (uint32_t i = 0; i < 6; ++i)
    {
        const render::material::MipChain exact = render::material::buildMipChain(s, i);
        const render::material::MipChain c = cook::textureChain(s, i, 0);
        logf("    %-10s format %u, %zu levels, %zu -> %zu bytes at level 0\n", s.textures[i].name.c_str(), (unsigned)c.format, c.levels.size(),
             exact.levels[0].size(), c.levels[0].size());
        CHECK(c.format == expected[i]);
        CHECK(c.levels.size() == exact.levels.size() && c.width == exact.width && c.height == exact.height);
        if (c.bytesPerTexel == 0)
        {
            const DXGI_FORMAT plain = exact.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : exact.format;
            for (uint32_t l = 0; l < c.levels.size(); ++l)
            {
                const uint32_t w = std::max(1u, c.width >> l), h = std::max(1u, c.height >> l);
                const DXGI_FORMAT block = c.format == DXGI_FORMAT_BC7_UNORM_SRGB ? DXGI_FORMAT_BC7_UNORM : c.format;
                const std::vector<uint8_t> d = decodeBlocks(block, plain, w, h, c.levels[l]);
                uint32_t worst = 0;
                for (size_t k = 0; k < d.size(); ++k) worst = std::max(worst, (uint32_t)std::abs((int)d[k] - (int)exact.levels[l][k]));
                CHECK(worst <= cook::kMaxAbsError);
            }
        }
        else CHECK(sameChain(c, exact));
    }
    const cook::TextureCookStats st = cook::textureCookStats();
    logf("    compressed %u (worst level PSNR %.2f dB, max error %u), kept exact %u, missed the bound %u\n", st.compressed, st.minPsnrDb, st.maxAbsError, st.keptExact,
         st.failedBound);
    CHECK(st.compressed == 3 && st.keptExact == 2 && st.failedBound == 1);
    // The cut-out coverage chain is exact.
    CHECK(sameChain(cook::textureChain(s, 1, 1), render::material::buildCoverageChain(s, 1)));
    CHECK(cook::textureChain(s, 0, 1).levels.empty());
}

UNX_TEST(texture_disk_cache_round_trip)
{
    // A fresh process (memory cache cleared) reads every chain from disk, identical to the cooked one; an edit of one
    // texel re-cooks only that texture; a corrupted entry is cooked again, never used.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("unx_cook_test_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    cook::setTextureCacheDirectory(dir.string());
    cook::clearTextureMemoryCache();
    cook::resetTextureCookStats();
    scene::Scene s = cookScene();
    std::vector<render::material::MipChain> first;
    for (uint32_t i = 0; i < 6; ++i) first.push_back(cook::textureChain(s, i, 0));
    cook::TextureCookStats st = cook::textureCookStats();
    CHECK(st.cooked == 6 && st.fromDisk == 0);
    cook::clearTextureMemoryCache();
    cook::resetTextureCookStats();
    for (uint32_t i = 0; i < 6; ++i) CHECK(sameChain(cook::textureChain(s, i, 0), first[i]));
    st = cook::textureCookStats();
    logf("    from disk: %u of 6, %.1f ms total\n", st.fromDisk, st.cookMs);
    CHECK(st.fromDisk == 6 && st.cooked == 0);
    for (uint32_t i = 0; i < 6; ++i) cook::textureChain(s, i, 0);
    CHECK(cook::textureCookStats().fromMemory == 6);
    s.textures[3].texels[17] ^= 0x40;
    cook::resetTextureCookStats();
    for (uint32_t i = 0; i < 6; ++i) cook::textureChain(s, i, 0);
    st = cook::textureCookStats();
    CHECK(st.cooked == 1 && st.fromMemory == 5);
    for (const auto& e : std::filesystem::directory_iterator(dir / "textures"))
    {
        std::vector<char> bytes;
        {
            std::ifstream f(e.path(), std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        }
        bytes[bytes.size() / 2] ^= 0x21;
        std::ofstream f(e.path(), std::ios::binary | std::ios::trunc);
        f.write(bytes.data(), (std::streamsize)bytes.size());
    }
    cook::clearTextureMemoryCache();
    cook::resetTextureCookStats();
    s.textures[3].texels[17] ^= 0x40;
    for (uint32_t i = 0; i < 6; ++i) CHECK(sameChain(cook::textureChain(s, i, 0), first[i]));
    st = cook::textureCookStats();
    CHECK(st.fromDisk == 0 && st.cooked == 6);
    cook::setTextureCacheDirectory("");
    cook::clearTextureMemoryCache();
    std::filesystem::remove_all(dir, ec);
}

UNX_TEST(approved_asset_images)
{
    // Measurement on real material textures (user-approved BlenderKit materials; their packed images extracted to the
    // folder UNX_COOK_TEST_IMAGES): each texture goes through the cook as the renderer would store it (base colour ->
    // sRGB chain -> BC7, roughness / AO / height -> R8 chain -> BC4, normal maps -> slope moments, kept exact), and the
    // worst level's PSNR and max error are reported. Report only; a texture outside the bound is stored exactly.
    wchar_t buffer[1024];
    const DWORD n = GetEnvironmentVariableW(L"UNX_COOK_TEST_IMAGES", buffer, 1024);
    if (n == 0 || n >= 1024)
    {
        logf("    (UNX_COOK_TEST_IMAGES not set: skipped)\n");
        return;
    }
    const std::filesystem::path root(std::wstring(buffer, n));
    (void)CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    cook::setTextureCacheDirectory("");
    struct Row
    {
        std::string name, kind;
        uint32_t w, h;
        DXGI_FORMAT stored;
        double psnr;
        uint32_t maxErr;
    };
    std::vector<Row> rows;
    for (const auto& e : std::filesystem::directory_iterator(root))
    {
        std::string name = e.path().filename().string(), lower = name;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        const std::string ext = e.path().extension().string();
        DirectX::ScratchImage loaded, conv;
        HRESULT hr = E_FAIL;
        if (lower.ends_with(".tga")) hr = DirectX::LoadFromTGAFile(e.path().c_str(), DirectX::TGA_FLAGS_IGNORE_SRGB, nullptr, loaded);
        else hr = DirectX::LoadFromWICFile(e.path().c_str(), DirectX::WIC_FLAGS_IGNORE_SRGB, nullptr, loaded);
        if (FAILED(hr))
        {
            logf("    (cannot load %s: 0x%08x)\n", name.c_str(), (unsigned)hr);
            continue;
        }
        const bool normal = lower.find("normal") != std::string::npos || lower.find("_nor") != std::string::npos;
        const bool colour = lower.find("color") != std::string::npos || lower.find("diff") != std::string::npos || lower.find("albedo") != std::string::npos;
        const bool gray = !normal && !colour &&
                          (lower.find("rough") != std::string::npos || lower.find("ao") != std::string::npos || lower.find("gloss") != std::string::npos ||
                           lower.find("height") != std::string::npos || lower.find("disp") != std::string::npos);
        if (!normal && !colour && !gray) continue;
        const DirectX::Image* src = loaded.GetImage(0, 0, 0);
        if (src->width > 4096 || src->height > 4096)
        {
            logf("    (%s: %zu x %zu, larger than 4096: skipped)\n", name.c_str(), src->width, src->height);
            continue;
        }
        const DXGI_FORMAT want = colour ? DXGI_FORMAT_R8G8B8A8_UNORM : normal ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;
        // Convert rejects a conversion to the same format.
        if (src->format != want && FAILED(DirectX::Convert(*src, want, DirectX::TEX_FILTER_DEFAULT, DirectX::TEX_THRESHOLD_DEFAULT, conv)))
        {
            logf("    (cannot convert %s from format %u)\n", name.c_str(), (unsigned)src->format);
            continue;
        }
        const DirectX::Image* im = src->format == want ? src : conv.GetImage(0, 0, 0);
        scene::Scene s;
        scene::Texture t;
        t.name = name;
        t.width = (uint32_t)im->width;
        t.height = (uint32_t)im->height;
        t.format = colour ? scene::TextureFormat::Rgba8Srgb : normal ? scene::TextureFormat::Rg8Normal : scene::TextureFormat::R8Linear;
        const uint32_t ch = colour ? 4 : normal ? 2 : 1;
        t.texels.resize((size_t)t.width * t.height * ch);
        for (uint32_t y = 0; y < t.height; ++y)
            for (uint32_t x = 0; x < t.width; ++x)
                for (uint32_t c = 0; c < ch; ++c)
                    t.texels[((size_t)y * t.width + x) * ch + c] = im->pixels[y * im->rowPitch + (size_t)x * (want == DXGI_FORMAT_R8_UNORM ? 1 : 4) + c];
        if (colour)
            for (size_t i = 0; i < (size_t)t.width * t.height; ++i) t.texels[i * 4 + 3] = 255;  // opaque material
        s.textures.push_back(t);
        scene::Material m;
        m.name = "m";
        if (colour) m.baseColorTexture = 0;
        else if (normal) m.normalTexture = 0;
        else m.occlusionTexture = 0;
        s.materials.push_back(m);
        const render::material::MipChain exact = render::material::buildMipChain(s, 0);
        Row r{ name, colour ? "colour" : normal ? "normal" : "gray", t.width, t.height, exact.format, 999.0, 0 };
        const DXGI_FORMAT plain = exact.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : exact.format;
        if (!normal)
            for (uint32_t l = 0; l < exact.levels.size(); ++l)
            {
                const uint32_t w = std::max(1u, t.width >> l), h = std::max(1u, t.height >> l);
                cook::BlockResult b;
                CHECK(cook::encodeLevel(plain, w, h, exact.levels[l].data(), b));
                r.psnr = std::min(r.psnr, b.psnrDb);
                r.maxErr = std::max(r.maxErr, b.maxAbsError);
                if (l == 0) r.stored = b.format;
            }
        rows.push_back(r);
        logf("    %-6s %-48s %4u x %4u: worst level PSNR %6.2f dB, max error %3u\n", r.kind.c_str(), name.c_str(), r.w, r.h, r.psnr, r.maxErr);
    }
    for (const char* kind : { "colour", "gray" })
    {
        std::vector<double> p;
        std::vector<uint32_t> m;
        for (const Row& r : rows)
            if (r.kind == kind)
            {
                p.push_back(r.psnr);
                m.push_back(r.maxErr);
            }
        if (p.empty()) continue;
        std::sort(p.begin(), p.end());
        std::sort(m.begin(), m.end());
        uint32_t within = 0;
        for (const Row& r : rows)
            if (r.kind == kind && r.psnr >= cook::kMinPsnrDb && r.maxErr <= cook::kMaxAbsError) ++within;
        logf("    %s: %zu textures, worst-level PSNR min / median %.2f / %.2f dB, max error median / max %u / %u; %u within the bound\n", kind, p.size(), p.front(),
             p[p.size() / 2], m[m.size() / 2], m.back(), within);
    }
}

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    uint32_t passed = 0, run = 0;
    for (const TestCase& t : registry())
    {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++run;
        try
        {
            t.fn();
        }
        catch (const std::exception& e)
        {
            logf("FAIL %s: %s\n", t.name, e.what());
            continue;
        }
        logf("PASS %s\n", t.name);
        ++passed;
    }
    logf("%u/%u passed\n", passed, run);
    return passed == run ? 0 : 1;
}
