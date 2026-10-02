#include "unx/material/TextureSystem.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::render::material
{
namespace
{
double srgbToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double linearToSrgb(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055; }

float halfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0)
    {
        if (mant == 0) bits = sign;
        else
        {
            const float v = std::ldexp((float)mant, -24);
            std::memcpy(&bits, &v, 4);
            bits |= sign;
        }
    }
    else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
    else bits = sign | ((exp + 112) << 23) | (mant << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

// Round to nearest even; values beyond the half range clamp to the largest finite half (65504).
uint16_t floatToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t e = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (e == 0xFF) return (uint16_t)(mant ? (sign | 0x7E00u) : (sign | 0x7BFFu));
    const int exp = (int)e - 127 + 15;
    if (exp >= 31) return (uint16_t)(sign | 0x7BFFu);
    if (exp <= 0)
    {
        if (exp < -10) return (uint16_t)sign;
        mant |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exp);
        uint32_t h = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (h & 1))) ++h;
        return (uint16_t)(sign | h);
    }
    uint32_t h = ((uint32_t)exp << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) ++h;
    if ((h & 0x7FFFu) >= 0x7C00u) h = (h & 0x8000u) | 0x7BFFu;
    return (uint16_t)(sign | h);
}

struct Image
{
    uint32_t w = 0, h = 0, c = 0;
    std::vector<double> v;
    void init(uint32_t width, uint32_t height, uint32_t channels)
    {
        w = width;
        h = height;
        c = channels;
        v.assign((size_t)w * h * c, 0.0);
    }
    double& at(uint32_t x, uint32_t y, uint32_t k) { return v[((size_t)y * w + x) * c + k]; }
    double at(uint32_t x, uint32_t y, uint32_t k) const { return v[((size_t)y * w + x) * c + k]; }
};

// Destination texel i of a dimension n -> max(1, n / 2) covers source [i f, (i + 1) f), f = n / dn: weights of the
// covered source texels (fractional at the ends when n is odd). Sums to 1.
std::vector<std::vector<std::pair<uint32_t, double>>> boxWeights(uint32_t n, uint32_t dn)
{
    std::vector<std::vector<std::pair<uint32_t, double>>> out(dn);
    const double f = (double)n / dn;
    for (uint32_t i = 0; i < dn; ++i)
    {
        const double lo = i * f, hi = (i + 1) * f;
        for (uint32_t j = (uint32_t)std::floor(lo); j < n && j < hi; ++j)
        {
            const double overlap = std::min(hi, j + 1.0) - std::max(lo, (double)j);
            if (overlap > 0) out[i].push_back({ j, overlap / f });
        }
    }
    return out;
}

Image downsample(const Image& s)
{
    const uint32_t dw = std::max(1u, s.w / 2), dh = std::max(1u, s.h / 2);
    const auto wx = boxWeights(s.w, dw), wy = boxWeights(s.h, dh);
    Image rows;
    rows.init(dw, s.h, s.c);
    for (uint32_t y = 0; y < s.h; ++y)
        for (uint32_t x = 0; x < dw; ++x)
            for (const auto& [sx, w] : wx[x])
                for (uint32_t k = 0; k < s.c; ++k) rows.at(x, y, k) += w * s.at(sx, y, k);
    Image d;
    d.init(dw, dh, s.c);
    for (uint32_t y = 0; y < dh; ++y)
        for (const auto& [sy, w] : wy[y])
            for (uint32_t x = 0; x < dw; ++x)
                for (uint32_t k = 0; k < s.c; ++k) d.at(x, y, k) += w * rows.at(x, sy, k);
    return d;
}

uint32_t mipCount(uint32_t w, uint32_t h)
{
    uint32_t n = 1;
    while (w > 1 || h > 1)
    {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        ++n;
    }
    return n;
}

uint8_t unorm8(double v) { return (uint8_t)std::lround(std::clamp(v, 0.0, 1.0) * 255.0); }
uint16_t unorm16(double v) { return (uint16_t)std::lround(std::clamp(v, 0.0, 1.0) * 65535.0); }

// Fraction of texels whose alpha (x scale, clamped to 1) passes the cutoff.
double coverage(const Image& img, uint32_t channel, double cutoff, double scale)
{
    size_t pass = 0;
    for (uint32_t y = 0; y < img.h; ++y)
        for (uint32_t x = 0; x < img.w; ++x)
            if (std::min(1.0, img.at(x, y, channel) * scale) >= cutoff) ++pass;
    return (double)pass / ((double)img.w * img.h);
}

// Alpha scale of a mip level that makes its coverage at the cutoff equal to the base level's (Castano 2010).
double coverageScale(const Image& level, uint32_t channel, double cutoff, double target)
{
    double lo = 0, hi = 1;
    while (coverage(level, channel, cutoff, hi) < target && hi < 1e6) hi *= 2;
    for (int i = 0; i < 40; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        if (coverage(level, channel, cutoff, mid) < target) lo = mid;
        else hi = mid;
    }
    // hi reaches the target coverage; lo is below it. Pick the closer of the two.
    const double ch = coverage(level, channel, cutoff, hi), cl = coverage(level, channel, cutoff, lo);
    return std::abs(cl - target) < std::abs(ch - target) ? lo : hi;
}

struct Use
{
    bool baseColor = false, normal = false, roughMetal = false, emissive = false;
    bool alphaTested = false;
    double cutoff = 0;
};

Use usesOf(const scene::Scene& s, uint32_t index)
{
    Use u;
    for (const scene::Material& m : s.materials)
    {
        if (m.baseColorTexture == index)
        {
            u.baseColor = true;
            if (m.alphaCutoff > 0)
            {
                if (u.alphaTested && u.cutoff != m.alphaCutoff)
                    logf("M textures: '%s' is alpha-tested at cutoffs %.3f and %.3f; mips keep coverage at %.3f\n", s.textures[index].name.c_str(), u.cutoff,
                         (double)m.alphaCutoff, u.cutoff);
                else u.cutoff = m.alphaCutoff;
                u.alphaTested = true;
            }
        }
        if (m.normalTexture == index) u.normal = true;
        if (m.roughMetalTexture == index) u.roughMetal = true;
        if (m.emissiveTexture == index) u.emissive = true;
    }
    return u;
}

uint64_t fnv1a(const void* data, size_t bytes, uint64_t h)
{
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
} // namespace

MipChain buildMipChain(const scene::Scene& s, uint32_t index)
{
    const scene::Texture& t = s.textures.at(index);
    const Use use = usesOf(s, index);
    MipChain out;
    out.width = t.width;
    out.height = t.height;
    const uint32_t levels = mipCount(t.width, t.height);
    const size_t n = (size_t)t.width * t.height;
    auto src = [&](size_t texel, uint32_t bpp, uint32_t k) { return t.texels[texel * bpp + k]; };

    switch (t.format)
    {
    case scene::TextureFormat::Rgba8Srgb:
    {
        // Internal chain: premultiplied linear colour (alpha-weighted when alpha-tested) and alpha, plus the plain
        // colour mean for texels whose alpha averages to zero.
        const bool weighted = use.alphaTested;
        Image img;
        img.init(t.width, t.height, 7);
        for (size_t i = 0; i < n; ++i)
        {
            const double a = src(i, 4, 3) / 255.0;
            for (uint32_t k = 0; k < 3; ++k)
            {
                const double c = srgbToLinear(src(i, 4, k) / 255.0);
                img.v[i * 7 + k] = weighted ? c * a : c;
                img.v[i * 7 + 4 + k] = c;
            }
            img.v[i * 7 + 3] = a;
        }
        out.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        out.bytesPerTexel = 4;
        const double target = use.alphaTested ? coverage(img, 3, use.cutoff, 1.0) : 0;
        for (uint32_t l = 0; l < levels; ++l)
        {
            if (l > 0) img = downsample(img);
            const double scale = (use.alphaTested && l > 0 && target > 0 && target < 1) ? coverageScale(img, 3, use.cutoff, target) : 1.0;
            std::vector<uint8_t> bytes((size_t)img.w * img.h * 4);
            for (size_t i = 0; i < (size_t)img.w * img.h; ++i)
            {
                const double a = img.v[i * 7 + 3];
                for (uint32_t k = 0; k < 3; ++k)
                {
                    const double c = !weighted ? img.v[i * 7 + k] : (a > 1e-9 ? img.v[i * 7 + k] / a : img.v[i * 7 + 4 + k]);
                    bytes[i * 4 + k] = unorm8(linearToSrgb(std::clamp(c, 0.0, 1.0)));
                }
                bytes[i * 4 + 3] = unorm8(std::min(1.0, a * scale));
            }
            out.levels.push_back(std::move(bytes));
        }
        break;
    }
    case scene::TextureFormat::Rg8Normal:
    {
        // Slopes of the tangent-space normals (x/z, y/z); z >= 1/64 bounds them (the map's normals within 89.1 deg).
        Image img;
        img.init(t.width, t.height, 3);
        double S = 1e-4;
        for (size_t i = 0; i < n; ++i)
        {
            const double x = src(i, 2, 0) / 255.0 * 2 - 1, y = src(i, 2, 1) / 255.0 * 2 - 1;
            const double z2 = 1 - x * x - y * y;
            double nx = x, ny = y, nz = std::sqrt(std::max(z2, 0.0));
            const double len = std::sqrt(nx * nx + ny * ny + nz * nz);
            nx /= len;
            ny /= len;
            nz = std::max(nz / len, 1.0 / 64);
            const double sx = nx / nz, sy = ny / nz;
            img.v[i * 3 + 0] = sx;
            img.v[i * 3 + 1] = sy;
            img.v[i * 3 + 2] = sx * sx + sy * sy;
            S = std::max(S, std::max(std::abs(sx), std::abs(sy)));
        }
        out.format = DXGI_FORMAT_R16G16B16A16_UNORM;
        out.bytesPerTexel = 8;
        out.slopeRange = (float)S;
        const double S2 = 2 * S * S;
        for (uint32_t l = 0; l < levels; ++l)
        {
            if (l > 0) img = downsample(img);
            std::vector<uint8_t> bytes((size_t)img.w * img.h * 8);
            for (size_t i = 0; i < (size_t)img.w * img.h; ++i)
            {
                const double mx = img.v[i * 3], my = img.v[i * 3 + 1], q = mx * mx + my * my;
                const double inner = std::max(img.v[i * 3 + 2] - q, 0.0);
                const uint16_t c[4] = { unorm16((mx / S + 1) * 0.5), unorm16((my / S + 1) * 0.5), unorm16(inner / S2), unorm16(q / S2) };
                std::memcpy(bytes.data() + i * 8, c, 8);
            }
            out.levels.push_back(std::move(bytes));
        }
        break;
    }
    case scene::TextureFormat::Rgba8Linear:
    case scene::TextureFormat::Rg8RoughMetal:
    case scene::TextureFormat::R8Linear:
    {
        const uint32_t ch = t.format == scene::TextureFormat::Rgba8Linear ? 4 : t.format == scene::TextureFormat::Rg8RoughMetal ? 2 : 1;
        out.format = ch == 4 ? DXGI_FORMAT_R8G8B8A8_UNORM : ch == 2 ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
        out.bytesPerTexel = ch;
        Image img;
        img.init(t.width, t.height, ch);
        for (size_t i = 0; i < n * ch; ++i) img.v[i] = t.texels[i] / 255.0;
        for (uint32_t l = 0; l < levels; ++l)
        {
            if (l > 0) img = downsample(img);
            std::vector<uint8_t> bytes(img.v.size());
            for (size_t i = 0; i < img.v.size(); ++i) bytes[i] = unorm8(img.v[i]);
            out.levels.push_back(std::move(bytes));
        }
        break;
    }
    case scene::TextureFormat::Rgba16Float:
    {
        out.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        out.bytesPerTexel = 8;
        Image img;
        img.init(t.width, t.height, 4);
        for (size_t i = 0; i < n * 4; ++i)
        {
            uint16_t h;
            std::memcpy(&h, t.texels.data() + i * 2, 2);
            img.v[i] = halfToFloat(h);
        }
        for (uint32_t l = 0; l < levels; ++l)
        {
            if (l > 0) img = downsample(img);
            std::vector<uint8_t> bytes(img.v.size() * 2);
            for (size_t i = 0; i < img.v.size(); ++i)
            {
                const uint16_t h = floatToHalf((float)img.v[i]);
                std::memcpy(bytes.data() + i * 2, &h, 2);
            }
            out.levels.push_back(std::move(bytes));
        }
        break;
    }
    default: fail("M textures: texture '%s' has an unknown format %u", t.name.c_str(), (unsigned)t.format);
    }
    return out;
}

namespace
{
ChainProvider g_chainProvider = nullptr;
}

void setChainProvider(ChainProvider provider)
{
    g_chainProvider = provider;
}

MipChain buildCoverageChain(const scene::Scene& s, uint32_t index)
{
    const scene::Texture& t = s.textures.at(index);
    const Use use = usesOf(s, index);
    MipChain out;
    if (!use.alphaTested || t.format != scene::TextureFormat::Rgba8Srgb) return out;
    out.width = t.width;
    out.height = t.height;
    out.format = DXGI_FORMAT_R8_UNORM;
    out.bytesPerTexel = 1;
    Image img;
    img.init(t.width, t.height, 1);
    for (size_t i = 0; i < (size_t)t.width * t.height; ++i) img.v[i] = t.texels[i * 4 + 3] / 255.0 >= use.cutoff ? 1.0 : 0.0;
    const uint32_t levels = mipCount(t.width, t.height);
    for (uint32_t l = 0; l < levels; ++l)
    {
        if (l > 0) img = downsample(img);
        std::vector<uint8_t> bytes(img.v.size());
        for (size_t i = 0; i < img.v.size(); ++i) bytes[i] = unorm8(img.v[i]);
        out.levels.push_back(std::move(bytes));
    }
    return out;
}

TextureSystem::~TextureSystem() { clear(); }

void TextureSystem::release(Resource& r)
{
    if (!m_device) return;
    if (r.resource) m_device->deferRelease(r.resource);
    if (r.srv != gpu::kNone)
    {
        DescriptorHeaps* h = &m_device->descriptors();
        const uint32_t srv = r.srv;
        m_device->deferCall([h, srv] { h->freeResource(srv); });
    }
    r = {};
}

void TextureSystem::clear()
{
    for (Resource& r : m_textures) release(r);
    for (Resource& r : m_coverage) release(r);
    m_textures.clear();
    m_coverage.clear();
    release(m_table);
    m_gpuBytes = 0;
}

void TextureSystem::sync(Device& device, const GpuScene& gpuScene)
{
    const scene::Scene* s = gpuScene.source();
    if (!s) fail("M textures: the GPU scene has no source scene");
    if (m_device == &device && m_source == s && m_revision == gpuScene.revision()) return;
    m_device = &device;
    m_source = s;
    m_revision = gpuScene.revision();

    // Texture content identity: re-upload only when it changed (cluster installs also bump the revision).
    uint64_t h = 0xCBF29CE484222325ull;
    for (const scene::Texture& t : s->textures)
    {
        const uint32_t head[4] = { t.width, t.height, (uint32_t)t.format, t.wrap ? 1u : 0u };
        h = fnv1a(head, sizeof head, h);
        h = fnv1a(t.texels.data(), t.texels.size(), h);
    }
    for (const scene::Material& m : s->materials)
    {
        const float cut = m.alphaCutoff;
        const uint32_t refs[6] = { m.baseColorTexture, m.normalTexture, m.roughMetalTexture, m.emissiveTexture, m.terrainSplat[0], m.terrainSplat[1] };
        h = fnv1a(&cut, sizeof cut, h);
        h = fnv1a(refs, sizeof refs, h);
    }
    const std::string fingerprint = format("%016llx/%zu", (unsigned long long)h, s->textures.size());

    DescriptorHeaps& heaps = device.descriptors();
    if (fingerprint != m_fingerprint)
    {
        clear();
        m_fingerprint = fingerprint;
        m_textures.resize(s->textures.size());
        m_coverage.resize(s->textures.size());
        m_slopeRange.assign(s->textures.size(), 0.0f);

        // Upload in batches of ~256 MB of staging memory.
        constexpr uint64_t kBatch = 256ull << 20;
        struct Pending
        {
            ComPtr<ID3D12Resource> texture;
            MipChain chain;
            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
            std::vector<UINT> rows;         // per level: texel rows, or block rows of a block-compressed chain
            std::vector<UINT64> rowBytes;   // per level: bytes of one (block) row, the chain's tight row pitch
            uint64_t offset = 0;
        };
        std::vector<Pending> batch;
        uint64_t batchBytes = 0;
        auto flush = [&]() {
            if (batch.empty()) return;
            D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC1 bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = batchBytes;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> staging;
            check(device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
                  "M texture staging");
            uint8_t* mapped = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(staging->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map M texture staging");
            CommandList cl = device.acquireCommandList(QueueType::Graphics);
            std::vector<D3D12_TEXTURE_BARRIER> barriers;
            for (Pending& p : batch)
            {
                for (uint32_t l = 0; l < (uint32_t)p.chain.levels.size(); ++l)
                {
                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp = p.footprints[l];
                    const size_t rowBytes = (size_t)p.rowBytes[l];
                    if (p.chain.levels[l].size() != rowBytes * p.rows[l]) fail("M textures: chain level %u has %zu bytes, expected %zu", l, p.chain.levels[l].size(), rowBytes * p.rows[l]);
                    for (uint32_t y = 0; y < p.rows[l]; ++y)
                        std::memcpy(mapped + p.offset + fp.Offset + (size_t)y * fp.Footprint.RowPitch, p.chain.levels[l].data() + (size_t)y * rowBytes, rowBytes);
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed = fp;
                    placed.Offset += p.offset;
                    D3D12_TEXTURE_COPY_LOCATION dst{ p.texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    dst.SubresourceIndex = l;
                    D3D12_TEXTURE_COPY_LOCATION srcLoc{ staging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                    srcLoc.PlacedFootprint = placed;
                    cl.list->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
                }
                D3D12_TEXTURE_BARRIER b{};
                b.SyncBefore = D3D12_BARRIER_SYNC_COPY;
                b.SyncAfter = D3D12_BARRIER_SYNC_NONE;
                b.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
                b.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
                b.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST;
                b.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
                b.pResource = p.texture.Get();
                b.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFFu;
                barriers.push_back(b);
            }
            D3D12_BARRIER_GROUP g{};
            g.Type = D3D12_BARRIER_TYPE_TEXTURE;
            g.NumBarriers = (UINT32)barriers.size();
            g.pTextureBarriers = barriers.data();
            cl.list->Barrier(1, &g);
            staging->Unmap(0, nullptr);
            const uint64_t fence = device.submit(cl);
            device.queue(QueueType::Graphics).waitCpu(fence);
            batch.clear();
            batchBytes = 0;
        };

        // Scene textures (kind 0) and the cut-out coverage of alpha-tested base colours (kind 1).
        for (uint32_t job = 0; job < 2 * (uint32_t)s->textures.size(); ++job)
        {
            const uint32_t i = job >> 1, kind = job & 1;
            Pending p;
            p.chain = g_chainProvider ? g_chainProvider(*s, i, kind) : kind == 0 ? buildMipChain(*s, i) : buildCoverageChain(*s, i);
            if (p.chain.levels.empty()) continue;
            if (kind == 0) m_slopeRange[i] = p.chain.slopeRange;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = p.chain.width;
            d.Height = p.chain.height;
            d.DepthOrArraySize = 1;
            d.MipLevels = (UINT16)p.chain.levels.size();
            d.Format = p.chain.format;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            p.footprints.resize(p.chain.levels.size());
            UINT64 total = 0;
            p.rows.resize(p.chain.levels.size());
            p.rowBytes.resize(p.chain.levels.size());
            device.d3d()->GetCopyableFootprints(&d, 0, (UINT)p.chain.levels.size(), 0, p.footprints.data(), p.rows.data(), p.rowBytes.data(), &total);
            const uint64_t aligned = (batchBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(uint64_t)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            if (aligned + total > kBatch && !batch.empty()) flush();
            p.offset = (batchBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(uint64_t)(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            batchBytes = p.offset + total;

            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC1 d1{};
            d1.Dimension = d.Dimension;
            d1.Width = d.Width;
            d1.Height = d.Height;
            d1.DepthOrArraySize = 1;
            d1.MipLevels = d.MipLevels;
            d1.Format = d.Format;
            d1.SampleDesc.Count = 1;
            d1.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d1, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&p.texture)),
                  "M texture");
            const std::string name = kind == 0 ? s->textures[i].name : s->textures[i].name + " (cut-out coverage)";
            const std::wstring wide(name.begin(), name.end());
            p.texture->SetName(wide.c_str());
            D3D12_RESOURCE_ALLOCATION_INFO info = device.d3d()->GetResourceAllocationInfo(0, 1, &d);
            m_gpuBytes += info.SizeInBytes;

            Resource& r = kind == 0 ? m_textures[i] : m_coverage[i];
            r.resource = p.texture;
            r.srv = heaps.allocateResource();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = p.chain.format;
            sd.Texture2D.MipLevels = d.MipLevels;
            device.d3d()->CreateShaderResourceView(p.texture.Get(), &sd, heaps.resourceCpu(r.srv));
            batch.push_back(std::move(p));
        }
        flush();
    }

    // Material table (always rebuilt: cheap, and material parameters may change with any upload).
    release(m_table);
    m_anyEmissive = false;
    std::vector<TextureSetGpu> table;
    auto srvOf = [&](uint32_t tex, scene::TextureFormat expected, scene::TextureFormat alt, const scene::Material& m, const char* slot) -> uint32_t {
        if (tex == scene::kNone) return gpu::kNone;
        if (tex >= s->textures.size()) fail("M textures: material '%s' %s texture %u out of range", m.name.c_str(), slot, tex);
        const scene::TextureFormat f = s->textures[tex].format;
        if (f != expected && f != alt)
            fail("M textures: material '%s' uses texture '%s' (format %u) as %s", m.name.c_str(), s->textures[tex].name.c_str(), (unsigned)f, slot);
        return m_textures[tex].srv;
    };
    m_published.clear();
    auto clampBit = [&](uint32_t tex, uint32_t bit) { return (tex != scene::kNone && tex < s->textures.size() && !s->textures[tex].wrap) ? bit : 0u; };
    for (const scene::Material& m : s->materials)
    {
        TextureSetGpu e{};
        e.baseColor = srvOf(m.baseColorTexture, scene::TextureFormat::Rgba8Srgb, scene::TextureFormat::Rgba8Srgb, m, "base colour");
        e.moments = srvOf(m.normalTexture, scene::TextureFormat::Rg8Normal, scene::TextureFormat::Rg8Normal, m, "normal");
        e.roughMetal = srvOf(m.roughMetalTexture, scene::TextureFormat::Rg8RoughMetal, scene::TextureFormat::Rg8RoughMetal, m, "roughness/metallic");
        e.emissive = srvOf(m.emissiveTexture, scene::TextureFormat::Rgba8Srgb, scene::TextureFormat::Rgba16Float, m, "emissive");
        // baked ambient occlusion (R8): the resolve puts it in the pixel's material word (materials without a layer
        // record; a layered material's word has no room for it), the shading kernel applies it to the indirect light
        e.occlusion = srvOf(m.occlusionTexture, scene::TextureFormat::R8Linear, scene::TextureFormat::R8Linear, m, "occlusion");
        e.slopeRange = m.normalTexture != scene::kNone ? m_slopeRange[m.normalTexture] : 0.0f;
        e.flags = clampBit(m.baseColorTexture, gpu::MaterialTextureBaseColor) | clampBit(m.normalTexture, gpu::MaterialTextureNormal) |
                  clampBit(m.roughMetalTexture, gpu::MaterialTextureRoughMetal) | clampBit(m.emissiveTexture, gpu::MaterialTextureEmissive) |
                  clampBit(m.occlusionTexture, gpu::MaterialTextureOcclusion);
        e.coverage = (m.alphaCutoff > 0 && m.baseColorTexture != scene::kNone && m.baseColorTexture < m_coverage.size()) ? m_coverage[m.baseColorTexture].srv : gpu::kNone;
        if (m.cls == scene::MaterialClass::Terrain)
        {
            // v1.74 Terrain class: the set carries the two splat maps in slots no other pass reads for it - occlusion =
            // splat 0, slopeRange = the bits of splat 1's SRV index (there is no normal map) - and no colour textures, so
            // paths without the class (band B until M's pre-shading, R hits) see the material's constants; the layers'
            // own sets are their materials' (MaterialTerrain.hlsli)
            e.baseColor = e.moments = e.roughMetal = e.emissive = e.coverage = gpu::kNone;
            e.occlusion = srvOf(m.terrainSplat[0], scene::TextureFormat::Rgba8Linear, scene::TextureFormat::Rgba8Linear, m, "terrain splat 0");
            const uint32_t splat1 = srvOf(m.terrainSplat[1], scene::TextureFormat::Rgba8Linear, scene::TextureFormat::Rgba8Linear, m, "terrain splat 1");
            std::memcpy(&e.slopeRange, &splat1, 4);
            e.flags = clampBit(m.terrainSplat[0], gpu::MaterialTextureBaseColor) | clampBit(m.terrainSplat[1], gpu::MaterialTextureEmissive);
        }
        // the material inputs' textures (scene::Material detail maps, height, emissive mask): to the material's record in
        // the GPU scene; a masked emission is per pixel like a textured one (the resolve's emissive texture)
        gpu::MaterialTextures pub;
        pub.detailColor = srvOf(m.detailColorTexture, scene::TextureFormat::Rgba8Srgb, scene::TextureFormat::Rgba8Srgb, m, "detail colour");
        pub.detailNormal = srvOf(m.detailNormalTexture, scene::TextureFormat::Rg8Normal, scene::TextureFormat::Rg8Normal, m, "detail normal");
        pub.detailSlopeRange = m.detailNormalTexture != scene::kNone ? m_slopeRange[m.detailNormalTexture] : 0.0f;
        pub.height = srvOf(m.heightTexture, scene::TextureFormat::R8Linear, scene::TextureFormat::R8Linear, m, "height");
        pub.emissiveMask = srvOf(m.emissiveMaskTexture, scene::TextureFormat::R8Linear, scene::TextureFormat::R8Linear, m, "emissive mask");
        pub.inputClamp = clampBit(m.detailColorTexture, 1u) | clampBit(m.detailNormalTexture, 2u) | clampBit(m.heightTexture, 4u) | clampBit(m.emissiveMaskTexture, 8u);
        if (pub.emissiveMask != gpu::kNone) e.flags |= gpu::MaterialTextureEmissiveMask;
        if (e.emissive != gpu::kNone || pub.emissiveMask != gpu::kNone) m_anyEmissive = true;
        table.push_back(e);
        pub.baseColor = e.baseColor;
        pub.normal = e.moments;
        pub.roughMetal = e.roughMetal;
        pub.emissive = e.emissive;
        pub.clamp = e.flags;
        m_published.push_back(pub);
    }
    if (table.empty()) table.push_back(TextureSetGpu{ gpu::kNone, gpu::kNone, gpu::kNone, gpu::kNone, gpu::kNone, 0, 0, gpu::kNone });

    const uint64_t bytes = table.size() * sizeof(TextureSetGpu);
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource3(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_table.resource)),
          "M material table");
    m_table.resource->SetName(L"M material texture table");
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "M material table staging");
    void* p = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &p), "map M material table");
    std::memcpy(p, table.data(), bytes);
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(m_table.resource.Get(), 0, staging.Get(), 0, bytes);
    const uint64_t fence = device.submit(cl);
    device.queue(QueueType::Graphics).waitCpu(fence);
    m_table.srv = heaps.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.Buffer.NumElements = (UINT)table.size();
    sd.Buffer.StructureByteStride = sizeof(TextureSetGpu);
    device.d3d()->CreateShaderResourceView(m_table.resource.Get(), &sd, heaps.resourceCpu(m_table.srv));
}
} // namespace unx::render::material
