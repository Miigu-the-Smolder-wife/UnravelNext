#include "unx/cook/TextureCook.h"

#include "CookCache.h"
#include "CookSourceHash.generated.h"
#include "unx/core/Jobs.h"
#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <d3d11.h>  // before DirectXTex.h: it declares the D3D11 (GPU encoder) overloads only then
#include <DirectXTex.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>

namespace unx::cook
{
using Microsoft::WRL::ComPtr;
using render::material::MipChain;

namespace
{
constexpr uint32_t kMagic = 0x584B4F43u;  // "COKX"
constexpr uint32_t kFormat = 1;
constexpr size_t kMemoryCapBytes = size_t(1) << 30;  // bounded RAM cache (the disk cache is the real one)

std::mutex g_mutex;
std::string g_directory;
bool g_directorySet = false;
TextureCookStats g_stats;

// Memory cache, least recently used first.
struct MemoryEntry
{
    std::shared_ptr<const MipChain> chain;
    size_t bytes = 0;
    std::list<std::string>::iterator order;
};
std::unordered_map<std::string, MemoryEntry> g_memory;
std::list<std::string> g_order;
size_t g_memoryBytes = 0;

size_t chainBytes(const MipChain& c)
{
    size_t n = 0;
    for (const auto& l : c.levels) n += l.size();
    return n;
}

std::shared_ptr<const MipChain> memoryFind(const std::string& key)
{
    std::lock_guard lock(g_mutex);
    auto it = g_memory.find(key);
    if (it == g_memory.end()) return nullptr;
    g_order.splice(g_order.end(), g_order, it->second.order);
    return it->second.chain;
}

void memoryStore(const std::string& key, std::shared_ptr<const MipChain> chain)
{
    std::lock_guard lock(g_mutex);
    if (g_memory.count(key)) return;
    const size_t bytes = chainBytes(*chain);
    g_order.push_back(key);
    g_memory[key] = MemoryEntry{ std::move(chain), bytes, std::prev(g_order.end()) };
    g_memoryBytes += bytes;
    while (g_memoryBytes > kMemoryCapBytes && g_order.size() > 1)
    {
        auto oldest = g_memory.find(g_order.front());
        g_memoryBytes -= oldest->second.bytes;
        g_memory.erase(oldest);
        g_order.pop_front();
    }
}

// ---- D3D11 device for DirectXTex's GPU BC7 encoder ---------------------------------------------------------------------
std::mutex g_gpuMutex;
ComPtr<ID3D11Device> g_device;
bool g_deviceTried = false;

ID3D11Device* encoderDevice()
{
    if (!g_deviceTried)
    {
        g_deviceTried = true;
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
        const HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &g_device, nullptr, nullptr);
        if (FAILED(hr))
        {
            logf("UnravelNext cook: no D3D11 device for the BC7 encoder (0x%08x); colour textures are stored uncompressed\n", (unsigned)hr);
            g_device.Reset();
        }
    }
    return g_device.Get();
}

bool bc7Available()
{
    std::lock_guard lock(g_gpuMutex);
    return encoderDevice() != nullptr;
}

struct Use
{
    bool alphaTested = false;
    float cutoff = 0;
};

// Same rule as material::buildMipChain (TextureSystem.cpp usesOf): the first alpha-tested material's cutoff.
Use useOf(const scene::Scene& s, uint32_t index)
{
    Use u;
    for (const scene::Material& m : s.materials)
        if (m.baseColorTexture == index && m.alphaCutoff > 0 && !u.alphaTested)
        {
            u.alphaTested = true;
            u.cutoff = m.alphaCutoff;
        }
    return u;
}

Key keyOf(const scene::Scene& s, uint32_t index, uint32_t kind, bool bc7)
{
    const scene::Texture& t = s.textures[index];
    const Use u = useOf(s, index);
    Sha256 h;
    h.update(std::string_view("unx.cook.texture"));
    const uint32_t ints[7] = { kind, t.width, t.height, (uint32_t)t.format, u.alphaTested ? 1u : 0u, bc7 ? 1u : 0u, kMaxAbsError };
    const double reals[2] = { (double)u.cutoff, kMinPsnrDb };
    h.update(ints, sizeof ints);
    h.update(reals, sizeof reals);
    const uint64_t n = t.texels.size();
    h.update(&n, sizeof n);
    h.update(t.texels.data(), t.texels.size());
    return h.finish();
}

std::vector<uint8_t> serialize(const MipChain& c)
{
    std::vector<uint8_t> out;
    auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    const uint32_t head[5] = { (uint32_t)c.format, c.width, c.height, c.bytesPerTexel, (uint32_t)c.levels.size() };
    put(head, sizeof head);
    put(&c.slopeRange, sizeof c.slopeRange);
    for (const auto& l : c.levels)
    {
        const uint64_t n = l.size();
        put(&n, sizeof n);
        put(l.data(), l.size());
    }
    return out;
}

bool deserialize(const std::vector<uint8_t>& in, MipChain& c)
{
    size_t at = 0;
    auto get = [&](void* p, size_t n) {
        if (n > in.size() - at) return false;
        std::memcpy(p, in.data() + at, n);
        at += n;
        return true;
    };
    uint32_t head[5];
    if (!get(head, sizeof head) || !get(&c.slopeRange, sizeof c.slopeRange)) return false;
    c.format = (DXGI_FORMAT)head[0];
    c.width = head[1];
    c.height = head[2];
    c.bytesPerTexel = head[3];
    if (head[4] > 32) return false;
    c.levels.resize(head[4]);
    for (auto& l : c.levels)
    {
        uint64_t n = 0;
        if (!get(&n, sizeof n) || n > in.size() - at) return false;
        l.resize((size_t)n);
        if (!get(l.data(), (size_t)n)) return false;
    }
    return at == in.size();
}

DXGI_FORMAT blockFormatOf(DXGI_FORMAT stored)
{
    switch (stored)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R8G8_UNORM: return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_R8_UNORM: return DXGI_FORMAT_BC4_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

uint32_t channelsOf(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R8G8_UNORM ? 2 : f == DXGI_FORMAT_R8_UNORM ? 1 : 4;
}

MipChain cook(const scene::Scene& s, uint32_t index, uint32_t kind, bool bc7, TextureCookStats& st)
{
    MipChain base = kind == 0 ? render::material::buildMipChain(s, index) : render::material::buildCoverageChain(s, index);
    ++st.cooked;
    if (kind != 0 || base.levels.empty()) return base;
    const Use u = useOf(s, index);
    DXGI_FORMAT block = blockFormatOf(base.format);
    const bool colour = block == DXGI_FORMAT_BC7_UNORM || block == DXGI_FORMAT_BC7_UNORM_SRGB;
    if (block == DXGI_FORMAT_UNKNOWN || (base.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && u.alphaTested) || (colour && !bc7) ||
        base.width % 4 != 0 || base.height % 4 != 0)
    {
        ++st.keptExact;
        return base;
    }
    MipChain out;
    out.format = block;
    out.width = base.width;
    out.height = base.height;
    out.slopeRange = base.slopeRange;
    out.bytesPerTexel = 0;
    double worstPsnr = 1e9;
    uint32_t worstError = 0, failedLevel = 0;
    bool ok = true;
    for (uint32_t l = 0; l < (uint32_t)base.levels.size() && ok; ++l)
    {
        const uint32_t w = std::max(1u, base.width >> l), h = std::max(1u, base.height >> l);
        const DXGI_FORMAT source = base.format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : base.format;
        BlockResult r;
        if (!encodeLevel(source, w, h, base.levels[l].data(), r))
        {
            ok = false;
            break;
        }
        worstPsnr = std::min(worstPsnr, r.psnrDb);
        worstError = std::max(worstError, r.maxAbsError);
        if (r.psnrDb < kMinPsnrDb || r.maxAbsError > kMaxAbsError)
        {
            ok = false;
            failedLevel = l;
        }
        out.levels.push_back(std::move(r.blocks));
    }
    if (!ok)
    {
        ++st.failedBound;
        logf("UnravelNext cook: texture '%s' kept uncompressed (block error at level %u of %zu: PSNR %.1f dB, max %u of 255; bound %.1f dB, %u)\n",
             s.textures[index].name.c_str(), failedLevel, base.levels.size(), worstPsnr, worstError, kMinPsnrDb, kMaxAbsError);
        return base;
    }
    ++st.compressed;
    st.minPsnrDb = std::min(st.minPsnrDb, worstPsnr);
    st.maxAbsError = std::max(st.maxAbsError, worstError);
    return out;
}
} // namespace

bool encodeLevel(DXGI_FORMAT source, uint32_t width, uint32_t height, const uint8_t* texels, BlockResult& out)
{
    using namespace DirectX;
    const DXGI_FORMAT block = blockFormatOf(source);
    if (block == DXGI_FORMAT_UNKNOWN) return false;
    const uint32_t ch = channelsOf(source);
    // Encode whole blocks: a level whose size is not a multiple of 4 (mip tails, odd sizes) is padded by repeating its
    // edge texels. DirectXTex's GPU encoder would read zeros past the edge and fit the block's endpoints to them; the
    // padding texels are never sampled (the level's logical size excludes them), so only real texels shape the block.
    const uint32_t pw = (width + 3) & ~3u, ph = (height + 3) & ~3u;
    std::vector<uint8_t> padded((size_t)pw * ph * ch);
    for (uint32_t y = 0; y < ph; ++y)
        for (uint32_t x = 0; x < pw; ++x)
            std::memcpy(padded.data() + ((size_t)y * pw + x) * ch, texels + ((size_t)std::min(y, height - 1) * width + std::min(x, width - 1)) * ch, ch);
    Image img{};
    img.width = pw;
    img.height = ph;
    img.format = source;
    img.rowPitch = (size_t)pw * ch;
    img.slicePitch = img.rowPitch * ph;
    img.pixels = padded.data();
    ScratchImage compressed;
    const bool bc7 = block == DXGI_FORMAT_BC7_UNORM;
    if (bc7)
    {
        std::lock_guard lock(g_gpuMutex);
        ID3D11Device* device = encoderDevice();
        if (!device) return false;
        // Every BC7 mode (3-subset modes 0 and 2 included), uniform channel weights, alpha weighted like colour. DirectXTex
        // dispatches 64 blocks at a time, so each dispatch's time is bounded whatever the texture size.
        const HRESULT hr = Compress(device, img, block, TEX_COMPRESS_BC7_USE_3SUBSETS | TEX_COMPRESS_UNIFORM, 1.0f, compressed);
        if (FAILED(hr))
        {
            logf("UnravelNext cook: BC7 encode failed (0x%08x)\n", (unsigned)hr);
            return false;
        }
    }
    else
    {
        const HRESULT hr = Compress(img, block, TEX_COMPRESS_UNIFORM, TEX_THRESHOLD_DEFAULT, compressed);
        if (FAILED(hr))
        {
            logf("UnravelNext cook: %s encode failed (0x%08x)\n", block == DXGI_FORMAT_BC5_UNORM ? "BC5" : "BC4", (unsigned)hr);
            return false;
        }
    }
    const Image* c = compressed.GetImage(0, 0, 0);
    const size_t blocksX = (width + 3) / 4, blocksY = (height + 3) / 4, blockBytes = block == DXGI_FORMAT_BC4_UNORM ? 8 : 16;
    out.format = source == DXGI_FORMAT_R8G8B8A8_UNORM && block == DXGI_FORMAT_BC7_UNORM ? DXGI_FORMAT_BC7_UNORM : block;
    out.blocks.resize(blocksX * blocksY * blockBytes);
    for (size_t y = 0; y < blocksY; ++y) std::memcpy(out.blocks.data() + y * blocksX * blockBytes, c->pixels + y * c->rowPitch, blocksX * blockBytes);
    ScratchImage decoded;
    HRESULT hr = Decompress(*c, source, decoded);
    if (FAILED(hr))
    {
        logf("UnravelNext cook: decode check failed (0x%08x)\n", (unsigned)hr);
        return false;
    }
    const Image* d = decoded.GetImage(0, 0, 0);
    const size_t rowBytes = (size_t)width * ch;
    out.decoded.resize(rowBytes * height);
    double se = 0;
    uint32_t worst = 0;
    for (uint32_t y = 0; y < height; ++y)  // the level's own texels only
    {
        const uint8_t* a = texels + (size_t)y * rowBytes;
        const uint8_t* b = d->pixels + (size_t)y * d->rowPitch;
        std::memcpy(out.decoded.data() + (size_t)y * rowBytes, b, rowBytes);
        for (size_t i = 0; i < rowBytes; ++i)
        {
            const int e = (int)a[i] - (int)b[i];
            se += (double)e * e;
            worst = std::max(worst, (uint32_t)std::abs(e));
        }
    }
    const double mse = se / (double)(rowBytes * height);
    out.psnrDb = mse > 0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0;
    out.maxAbsError = worst;
    return true;
}

MipChain textureChain(const scene::Scene& s, uint32_t index, uint32_t kind)
{
    if (index >= s.textures.size()) fail("cook: texture %u of %zu", index, s.textures.size());
    // No alpha-tested consumer means no coverage chain. Do not hash/read the
    // entire source image a second time just to cache an empty result.
    if (kind == 1 && !useOf(s, index).alphaTested) return {};
    const auto t0 = std::chrono::steady_clock::now();
    const bool bc7 = bc7Available();
    const Key key = keyOf(s, index, kind, bc7);
    const std::string hex = hexOf(key);
    if (auto m = memoryFind(hex))
    {
        std::lock_guard lock(g_mutex);
        ++g_stats.fromMemory;
        return *m;
    }
    std::string dir;
    {
        std::lock_guard lock(g_mutex);
        dir = cacheDirectory(g_directory, g_directorySet);
    }
    std::vector<uint8_t> payload;
    auto chain = std::make_shared<MipChain>();
    bool fromDisk = false;
    if (!dir.empty() && readEntry(dir, "textures", key, kMagic, kFormat, UNX_COOK_SOURCE_HASH, payload) && deserialize(payload, *chain)) fromDisk = true;
    TextureCookStats local;
    if (!fromDisk)
    {
        *chain = cook(s, index, kind, bc7, local);
        if (!dir.empty()) writeEntry(dir, "textures", key, kMagic, kFormat, UNX_COOK_SOURCE_HASH, serialize(*chain));
    }
    memoryStore(hex, chain);
    std::lock_guard lock(g_mutex);
    if (fromDisk) ++g_stats.fromDisk;
    g_stats.cooked += local.cooked;
    g_stats.compressed += local.compressed;
    g_stats.keptExact += local.keptExact;
    g_stats.failedBound += local.failedBound;
    g_stats.minPsnrDb = std::min(g_stats.minPsnrDb, local.minPsnrDb);
    g_stats.maxAbsError = std::max(g_stats.maxAbsError, local.maxAbsError);
    g_stats.cookMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return *chain;
}

void setTextureCacheDirectory(const std::string& directory)
{
    std::lock_guard lock(g_mutex);
    g_directory = directory;
    g_directorySet = true;
}

void clearTextureMemoryCache()
{
    std::lock_guard lock(g_mutex);
    g_memory.clear();
    g_order.clear();
    g_memoryBytes = 0;
}

TextureCookStats textureCookStats()
{
    std::lock_guard lock(g_mutex);
    return g_stats;
}

void resetTextureCookStats()
{
    std::lock_guard lock(g_mutex);
    g_stats = {};
}
} // namespace unx::cook
