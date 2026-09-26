// Light functions (track E, A8). See include/unx/lights/LightFunctions.h and LightFunction.hlsli.
#include "unx/lights/LightFunctions.h"

#include "unx/core/Log.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace unx::lights
{
using namespace unx::render;

namespace
{
std::vector<std::string_view> tokens(std::string_view s)
{
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < s.size())
    {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' || s[i] == ',')) ++i;
        const size_t b = i;
        while (i < s.size() && !(s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n' || s[i] == ',')) ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}
std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
bool startsWithNoCase(std::string_view s, std::string_view p)
{
    if (s.size() < p.size()) return false;
    for (size_t i = 0; i < p.size(); ++i)
        if (std::toupper((unsigned char)s[i]) != std::toupper((unsigned char)p[i])) return false;
    return true;
}
} // namespace

IesProfile parseIes(std::string_view text)
{
    // the TILT line ends the header; everything after it is numbers
    size_t at = 0, numbers = std::string_view::npos;
    while (at < text.size())
    {
        size_t end = text.find('\n', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view line = trim(text.substr(at, end - at));
        if (startsWithNoCase(line, "TILT="))
        {
            if (!(line.size() == 9 && startsWithNoCase(line, "TILT=NONE")))
                fail("IES: TILT=%.*s - only TILT=NONE photometry is accepted (bake lamp tilt into the file)", (int)(line.size() - 5), line.data() + 5);
            numbers = end;
            break;
        }
        at = end + 1;
    }
    if (numbers == std::string_view::npos) fail("IES: no TILT line (not an LM-63 file)");
    const std::vector<std::string_view> t = tokens(text.substr(numbers));
    size_t k = 0;
    auto number = [&]() {
        if (k >= t.size()) fail("IES: truncated photometry");
        const std::string s(t[k++]);
        char* e = nullptr;
        const double v = std::strtod(s.c_str(), &e);
        if (e != s.c_str() + s.size() || !std::isfinite(v)) fail("IES: '%s' is not a number", s.c_str());
        return v;
    };
    auto count = [&]() {
        const double v = number();
        if (!(v >= 1 && v <= 1e6 && v == std::floor(v))) fail("IES: invalid count %g", v);
        return (uint32_t)v;
    };
    count();   // lamps
    number();  // lumens per lamp
    const double multiplier = number();
    const uint32_t V = count(), H = count(), type = count(), units = count();
    for (int i = 0; i < 3; ++i) number();  // width, length, height
    const double ballast = number();
    number();  // ballast-lamp photometric factor (future use)
    number();  // input watts
    if (type != 1) fail("IES: photometric type %u - only type C (1) is accepted", type);
    if (units < 1 || units > 2) fail("IES: invalid units %u", units);
    if (!(multiplier > 0)) fail("IES: candela multiplier %g must be positive", multiplier);
    if (!(ballast > 0)) fail("IES: ballast factor %g must be positive", ballast);
    if ((uint64_t)V * H + V + H != t.size() - k) fail("IES: %zu numbers after the header, %llu expected", t.size() - k, (unsigned long long)((uint64_t)V * H + V + H));
    IesProfile p;
    p.vertical.resize(V);
    p.horizontal.resize(H);
    for (float& a : p.vertical) a = (float)number();
    for (float& a : p.horizontal) a = (float)number();
    for (uint32_t i = 0; i < V; ++i)
    {
        if (p.vertical[i] < 0 || p.vertical[i] > 180) fail("IES: vertical angle %g outside [0, 180]", p.vertical[i]);
        if (i && !(p.vertical[i] > p.vertical[i - 1])) fail("IES: vertical angles not ascending");
    }
    for (uint32_t i = 0; i < H; ++i)
        if (i && !(p.horizontal[i] > p.horizontal[i - 1])) fail("IES: horizontal angles not ascending");
    const float h0 = p.horizontal.front(), h1 = p.horizontal.back();
    const bool ok = (h0 == 0 && (h1 == 0 || h1 == 90 || h1 == 180 || h1 == 360)) || (h0 == 90 && h1 == 270);
    if (!ok) fail("IES: horizontal angles %g..%g - type C needs 0..{0, 90, 180, 360} or 90..270", h0, h1);
    // the file lists, per horizontal angle, the candela over every vertical angle; stored [v * H + h]
    p.values.resize((size_t)V * H);
    for (uint32_t h = 0; h < H; ++h)
        for (uint32_t v = 0; v < V; ++v)
        {
            const double c = number() * multiplier * ballast;
            if (!(c >= 0)) fail("IES: negative candela %g", c);
            p.values[(size_t)v * H + h] = (float)c;
        }
    for (float c : p.values) p.peak = std::max(p.peak, c);
    if (!(p.peak > 0)) fail("IES: all candela values are 0");
    for (float& c : p.values) c /= p.peak;
    return p;
}

void LightFunctions::set(uint32_t light, const LightFunction& f)
{
    if (f.profile == Profile::Ies && (f.ies.vertical.empty() || f.ies.horizontal.empty() || f.ies.values.size() != f.ies.vertical.size() * f.ies.horizontal.size()))
        fail("light function %u: IES profile without data", light);
    if ((f.profile == Profile::Cookie || f.profile == Profile::Gobo) &&
        (f.image.width == 0 || f.image.height == 0 || f.image.width > 16384 || f.image.height > 16384 || f.image.rgb.size() != (size_t)f.image.width * f.image.height * 3))
        fail("light function %u: image %ux%u with %zu values", light, f.image.width, f.image.height, f.image.rgb.size());
    if (f.profile == Profile::Cookie && !(f.tanX > 0 && f.tanY > 0 && std::isfinite(f.tanX) && std::isfinite(f.tanY)))
        fail("light function %u: cookie half-angle tangents must be positive", light);
    for (size_t i = 1; i < f.intensityKeys.size(); ++i)
        if (!(f.intensityKeys[i].x >= f.intensityKeys[i - 1].x)) fail("light function %u: intensity key times not ascending", light);
    for (size_t i = 1; i < f.colorKeys.size(); ++i)
        if (!(f.colorKeys[i].x >= f.colorKeys[i - 1].x)) fail("light function %u: colour key times not ascending", light);
    if (!(f.intensityPeriod >= 0) || !(f.colorPeriod >= 0) || !(f.flickerDepth >= 0) || !(f.flickerFrequency >= 0) || f.flickerOctaves > 8)
        fail("light function %u: periods, flicker depth and frequency must be >= 0, octaves <= 8", light);
    if (m_functions.size() <= light) m_functions.resize((size_t)light + 1);
    const bool sameImage = m_functions[light] && m_functions[light]->profile == f.profile && m_functions[light]->image.width == f.image.width &&
                           m_functions[light]->image.height == f.image.height && m_functions[light]->image.rgb == f.image.rgb;
    m_functions[light] = std::make_unique<LightFunction>(f);
    if (m_imageRevision.size() < m_functions.size()) m_imageRevision.resize(m_functions.size(), 0);
    if (!sameImage) m_imageRevision[light] = ++m_revision;
    ++m_revision;
}
void LightFunctions::clear(uint32_t light)
{
    if (light < m_functions.size() && m_functions[light])
    {
        m_functions[light].reset();
        m_imageRevision[light] = ++m_revision;
    }
}
const LightFunction* LightFunctions::get(uint32_t light) const { return light < m_functions.size() ? m_functions[light].get() : nullptr; }
uint64_t LightFunctions::imageRevision(uint32_t light) const { return light < m_imageRevision.size() ? m_imageRevision[light] : 0; }

LightFunctions& lightFunctions(TrackState& state) { return state.get<LightFunctions>("lights.functions"); }

// ---- CPU reference (LightFunction.hlsli) ----

namespace
{
uint32_t lfHash(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    return v ^ (v >> 16);
}
float lerpf(float a, float b, float t) { return a + (b - a) * t; }
float keys1(const std::vector<float2>& k, float period, float t)
{
    if (k.empty()) return 1;
    if (period > 0) t -= period * std::floor(t / period);
    if (t <= k[0].x) return k[0].y;
    for (size_t i = 1; i < k.size(); ++i)
        if (t <= k[i].x) return lerpf(k[i - 1].y, k[i].y, (t - k[i - 1].x) / std::max(k[i].x - k[i - 1].x, 1e-20f));
    return k.back().y;
}
float3 keys3(const std::vector<float4>& k, float period, float t)
{
    if (k.empty()) return { 1, 1, 1 };
    if (period > 0) t -= period * std::floor(t / period);
    if (t <= k[0].x) return { k[0].y, k[0].z, k[0].w };
    for (size_t i = 1; i < k.size(); ++i)
        if (t <= k[i].x)
        {
            const float s = (t - k[i - 1].x) / std::max(k[i].x - k[i - 1].x, 1e-20f);
            return { lerpf(k[i - 1].y, k[i].y, s), lerpf(k[i - 1].z, k[i].z, s), lerpf(k[i - 1].w, k[i].w, s) };
        }
    return { k.back().y, k.back().z, k.back().w };
}
float noise(uint32_t seed, float x)
{
    const float i = std::floor(x), f = x - i;
    const uint32_t k = (uint32_t)i;
    const float a = (float)lfHash(seed ^ lfHash(k)) * (2.0f / 4294967295.0f) - 1, b = (float)lfHash(seed ^ lfHash(k + 1)) * (2.0f / 4294967295.0f) - 1;
    const float s = f * f * (3 - 2 * f);
    return lerpf(a, b, s);
}
float flicker(float depth, float frequency, uint32_t octaves, uint32_t seed, float t)
{
    if (!(depth > 0) || octaves == 0) return 1;
    float sum = 0, weight = 0, amplitude = 1, x = frequency * t;
    for (uint32_t o = 0; o < octaves; ++o)
    {
        sum += amplitude * noise(lfHash(seed + o * 0x9E3779B9u), x);
        weight += amplitude;
        amplitude *= 0.5f;
        x *= 2;
    }
    return std::max(0.0f, 1 + depth * sum / weight);
}
std::pair<uint32_t, float> interval(const std::vector<float>& x, float a)
{
    if (x.size() == 1) return { 0, 0.0f };
    uint32_t lo = 0, hi = (uint32_t)x.size() - 1;
    while (hi - lo > 1)
    {
        const uint32_t mid = (lo + hi) / 2;
        if (x[mid] <= a) lo = mid;
        else hi = mid;
    }
    return { lo, std::clamp((a - x[lo]) / std::max(x[hi] - x[lo], 1e-20f), 0.0f, 1.0f) };
}
float iesFold(const IesProfile& p, float phi)
{
    const float first = p.horizontal.front(), last = p.horizontal.back();
    if (last <= 0) return 0;  // rotationally symmetric
    if (first >= 90)  // 90..270: symmetric about the 90-270 plane
    {
        if (phi < 90) return 180 - phi;
        if (phi > 270) return 540 - phi;
        return phi;
    }
    if (last <= 90)  // quadrant symmetric
    {
        phi = std::fmod(phi, 180.0f);
        return phi > 90 ? 180 - phi : phi;
    }
    if (last <= 180) return phi > 180 ? 360 - phi : phi;  // bilateral about the 0-180 plane
    return phi;
}
float ies(const IesProfile& p, float phiDeg, float thetaDeg)
{
    phiDeg = iesFold(p, phiDeg);
    if (thetaDeg < p.vertical.front() || thetaDeg > p.vertical.back()) return 0;
    const uint32_t H = (uint32_t)p.horizontal.size(), V = (uint32_t)p.vertical.size();
    const auto [h0, hf] = interval(p.horizontal, phiDeg);
    const auto [r0, vf] = interval(p.vertical, thetaDeg);
    const uint32_t h1 = std::min(h0 + 1, H - 1), r1 = std::min(r0 + 1, V - 1);
    const float a = p.values[(size_t)r0 * H + h0], c = p.values[(size_t)r0 * H + h1];
    const float d = p.values[(size_t)r1 * H + h0], e = p.values[(size_t)r1 * H + h1];
    return lerpf(lerpf(a, c, hf), lerpf(d, e, hf), vf);
}
// Bilinear at mip 0 as a sampler does it (texel centres at +0.5), clamp or wrap addressing.
float3 bilinear(const Image& im, float u, float v, bool wrap)
{
    const float x = u * im.width - 0.5f, y = v * im.height - 0.5f;
    const float fx0 = std::floor(x), fy0 = std::floor(y);
    const float fx = x - fx0, fy = y - fy0;
    auto at = [&](int64_t i, int64_t j) {
        if (wrap)
        {
            i = ((i % im.width) + im.width) % im.width;
            j = ((j % im.height) + im.height) % im.height;
        }
        else
        {
            i = std::clamp<int64_t>(i, 0, im.width - 1);
            j = std::clamp<int64_t>(j, 0, im.height - 1);
        }
        const float* p = im.rgb.data() + ((size_t)j * im.width + (size_t)i) * 3;
        return float3{ p[0], p[1], p[2] };
    };
    const int64_t i0 = (int64_t)fx0, j0 = (int64_t)fy0;
    const float3 a = at(i0, j0), b = at(i0 + 1, j0), c = at(i0, j0 + 1), d = at(i0 + 1, j0 + 1);
    auto mix = [&](float p, float q, float r, float s) { return lerpf(lerpf(p, q, fx), lerpf(r, s, fx), fy); };
    return { mix(a.x, b.x, c.x, d.x), mix(a.y, b.y, c.y, d.y), mix(a.z, b.z, c.z, d.z) };
}
float3 normalize3(float3 a)
{
    const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return { a.x / l, a.y / l, a.z / l };
}
float dot3(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float3 cross3(float3 a, float3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
constexpr float kPi = 3.14159265358979f;
} // namespace

float3 evaluate(const LightFunction& lf, float3 forward, float3 right, float3 dir, float time)
{
    float3 f{ 1, 1, 1 };
    if (lf.profile != Profile::None)
    {
        const float3 z = normalize3(forward);
        const float rz = dot3(right, z);
        const float3 x = normalize3(float3{ right.x - z.x * rz, right.y - z.y * rz, right.z - z.z * rz });
        const float3 y = cross3(z, x);
        const float angle = lf.rotationPhase + lf.rotationSpeed * time;
        const float sa = std::sin(angle), ca = std::cos(angle);
        const float dx = dot3(dir, x), dy = dot3(dir, y), dz = dot3(dir, z);
        const float rx = dx * ca + dy * sa, ry = -dx * sa + dy * ca;
        if (lf.profile == Profile::Ies)
        {
            float phi = std::atan2(ry, rx) * (180 / kPi);
            if (phi < 0) phi += 360;
            const float theta = std::acos(std::clamp(dz, -1.0f, 1.0f)) * (180 / kPi);
            const float v = ies(lf.ies, phi, theta);
            f = { v, v, v };
        }
        else if (lf.profile == Profile::Cookie)
        {
            if (!(dz > 0)) return { 0, 0, 0 };
            const float u = rx / (dz * lf.tanX), v = ry / (dz * lf.tanY);
            if (std::abs(u) > 1 || std::abs(v) > 1) return { 0, 0, 0 };
            f = bilinear(lf.image, 0.5f + 0.5f * u, 0.5f - 0.5f * v, false);
        }
        else
        {
            float phi = std::atan2(ry, rx);
            if (phi < 0) phi += 2 * kPi;
            const float theta = std::acos(std::clamp(dz, -1.0f, 1.0f));
            const float h = (float)lf.image.height;
            f = bilinear(lf.image, phi / (2 * kPi), std::clamp(theta / kPi, 0.5f / h, 1 - 0.5f / h), true);
        }
    }
    const float s = keys1(lf.intensityKeys, lf.intensityPeriod, time) * flicker(lf.flickerDepth, lf.flickerFrequency, lf.flickerOctaves, lf.flickerSeed, time);
    const float3 c = keys3(lf.colorKeys, lf.colorPeriod, time);
    return { f.x * s * c.x, f.y * s * c.y, f.z * s * c.z };
}

// ---- mips and the GPU table ----

std::vector<Image> mipChain(const Image& base)
{
    std::vector<Image> chain{ base };
    // exact box filter: each coarser texel is the area mean of the finer texels its footprint covers (fractional
    // coverage at odd sizes), separable
    auto down1 = [](const std::vector<float>& src, uint32_t n, uint32_t stride, uint32_t lines, uint32_t m, bool rows) {
        std::vector<float> dst((size_t)m * lines * 3);
        const double scale = (double)n / m;
        for (uint32_t l = 0; l < lines; ++l)
            for (uint32_t i = 0; i < m; ++i)
            {
                const double a = i * scale, b = (i + 1) * scale;
                double acc[3] = {};
                for (uint32_t s = (uint32_t)std::floor(a); s < std::min<uint32_t>(n, (uint32_t)std::ceil(b)); ++s)
                {
                    const double w = std::min<double>(b, s + 1) - std::max<double>(a, s);
                    if (w <= 0) continue;
                    const size_t idx = rows ? ((size_t)s * stride + l) * 3 : ((size_t)l * stride + s) * 3;
                    for (int c = 0; c < 3; ++c) acc[c] += w * src[idx + c];
                }
                const size_t o = rows ? ((size_t)i * lines + l) * 3 : ((size_t)l * m + i) * 3;
                for (int c = 0; c < 3; ++c) dst[o + c] = (float)(acc[c] / scale);
            }
        return dst;
    };
    while (chain.back().width > 1 || chain.back().height > 1)
    {
        const Image& p = chain.back();
        const uint32_t w = std::max(1u, p.width / 2), h = std::max(1u, p.height / 2);
        Image n;
        n.width = w;
        n.height = h;
        const std::vector<float> horizontal = down1(p.rgb, p.width, p.width, p.height, w, false);  // w x p.height
        n.rgb = down1(horizontal, p.height, w, w, h, true);                                          // w x h
        chain.push_back(std::move(n));
    }
    return chain;
}

namespace
{
uint16_t toHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t absx = x & 0x7FFFFFFFu;
    if (absx >= 0x47800000u) return (uint16_t)(sign | (absx > 0x7F800000u ? 0x7E00u : 0x7BFFu));  // NaN; clamp to max finite
    if (absx < 0x38800000u)  // subnormal half (round to nearest even)
    {
        const uint32_t mant = (absx & 0x7FFFFFu) | 0x800000u;
        const int shift = 113 - (int)(absx >> 23);
        if (shift > 24) return (uint16_t)sign;
        const uint32_t v = mant >> shift, rem = mant & ((1u << shift) - 1), halfway = 1u << (shift - 1);
        return (uint16_t)(sign | (v + ((rem > halfway || (rem == halfway && (v & 1))) ? 1 : 0)));
    }
    const uint32_t v = absx - 0x38000000u;
    const uint32_t r = (v >> 13) + (((v & 0x1FFFu) > 0x1000u || ((v & 0x1FFFu) == 0x1000u && ((v >> 13) & 1))) ? 1 : 0);
    return (uint16_t)(sign | std::min<uint32_t>(r, 0x7BFFu));
}
uint32_t asUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "light function buffer");
    r->SetName(name);
    return r;
}

class LightFunctionPass
{
public:
    explicit LightFunctionPass(Device& device) : m_device(device) {}
    ~LightFunctionPass()
    {
        m_device.waitIdle();
        for (Texture& t : m_textures)
            if (t.srv != kNone) m_device.descriptors().freeResource(t.srv);
    }
    void record(FramePassContext& fc, const LightFunctions& lf)
    {
        // last frame's staging buffers: that frame is submitted now, so its fences cover their copies
        for (ComPtr<ID3D12Resource>& u : m_uploads) m_device.deferRelease(u);
        m_uploads.clear();
        const uint32_t lights = lf.lights();
        bool any = false;
        for (uint32_t i = 0; i < lights && !any; ++i) any = lf.get(i) != nullptr;
        if (!any)
        {
            releaseTextures(0);
            m_table.Reset();
            m_tableRevision = 0;
            return;  // FrameResources::lightFunctions stays invalid: consumers pass LIGHT_FUNCTION_NONE
        }
        RenderGraph& g = fc.graph;
        // images: one RGBA16F texture with a full box-filtered mip chain per cookie / gobo light
        if (m_textures.size() < lights) m_textures.resize(lights);
        releaseTextures(lights);
        std::vector<TextureRef> uploaded;
        for (uint32_t i = 0; i < lights; ++i)
        {
            const LightFunction* f = lf.get(i);
            const bool image = f && (f->profile == Profile::Cookie || f->profile == Profile::Gobo);
            Texture& t = m_textures[i];
            if (!image)
            {
                if (t.texture) releaseTexture(t);
                continue;
            }
            if (t.texture && t.revision == lf.imageRevision(i)) continue;
            if (t.texture) releaseTexture(t);
            uploaded.push_back(createTexture(g, t, *f, lf.imageRevision(i), i));
        }
        if (!uploaded.empty())
        {
            // the readers find the images through the table (bindless indices), so they cannot declare them: this pass
            // moves the fresh images to the shader-resource layout for the rest of the frame (sync after: all shading);
            // at frame end the graph returns them to COMMON, where later frames read them.
            g.addPass("lights.images.publish", QueueType::Graphics,
                      [=](PassBuilder& b) {
                          for (TextureRef t : uploaded) b.use(t, Use::SrvGraphics);
                          b.keep();
                      },
                      [](PassContext&) {});
        }
        // the table: rebuilt when anything changed (a new buffer, so frames in flight keep reading theirs)
        if (!m_table || m_tableRevision != lf.revision())
        {
            const std::vector<uint32_t> words = buildTable(lf);
            const uint64_t bytes = words.size() * 4;
            if (m_table) m_device.deferRelease(m_table);
            m_table = makeBuffer(m_device, bytes, D3D12_HEAP_TYPE_DEFAULT, L"light function table");
            m_tableBytes = bytes;
            ComPtr<ID3D12Resource> upload = makeBuffer(m_device, bytes, D3D12_HEAP_TYPE_UPLOAD, L"light function table upload");
            uint8_t* p = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(upload->Map(0, &none, reinterpret_cast<void**>(&p)), "map light function table upload");
            std::memcpy(p, words.data(), bytes);
            upload->Unmap(0, nullptr);
            const BufferRef table = g.importBuffer(m_table.Get(), BufferDesc{ "lights.functions", bytes, 0 });
            ID3D12Resource* src = upload.Get();
            g.addPass("lights.table.upload", QueueType::Graphics, [=](PassBuilder& b) { b.use(table, Use::CopyDst); },
                      [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(table), 0, src, 0, bytes); });
            m_uploads.push_back(upload);
            m_tableRevision = lf.revision();
            fc.resources.lightFunctions = table;
            return;
        }
        fc.resources.lightFunctions = g.importBuffer(m_table.Get(), BufferDesc{ "lights.functions", m_tableBytes, 0 });
    }

private:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    struct Texture
    {
        ComPtr<ID3D12Resource> texture;
        uint32_t srv = kNone, width = 0, height = 0, mips = 0;
        uint64_t revision = 0;
    };

    void releaseTexture(Texture& t)
    {
        m_device.deferRelease(t.texture);
        const uint32_t srv = t.srv;
        DescriptorHeaps* heaps = &m_device.descriptors();
        if (srv != kNone) m_device.deferCall([heaps, srv] { heaps->freeResource(srv); });
        t = Texture{};
    }
    void releaseTextures(uint32_t from)
    {
        for (size_t i = from; i < m_textures.size(); ++i)
            if (m_textures[i].texture) releaseTexture(m_textures[i]);
        m_textures.resize(std::min<size_t>(m_textures.size(), from));
    }
    TextureRef createTexture(RenderGraph& g, Texture& t, const LightFunction& f, uint64_t revision, uint32_t light)
    {
        const std::vector<Image> chain = mipChain(f.image);
        const uint32_t mips = (uint32_t)chain.size();
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = f.image.width;
        d.Height = f.image.height;
        d.DepthOrArraySize = 1;
        d.MipLevels = (UINT16)mips;
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        d.SampleDesc.Count = 1;
        check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&t.texture)),
              "light function image");
        const std::wstring name = L"light function image " + std::to_wstring(light);
        t.texture->SetName(name.c_str());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(mips);
        std::vector<UINT> rows(mips);
        std::vector<UINT64> rowBytes(mips);
        UINT64 total = 0;
        const D3D12_RESOURCE_DESC legacy = t.texture->GetDesc();
        m_device.d3d()->GetCopyableFootprints(&legacy, 0, mips, 0, fp.data(), rows.data(), rowBytes.data(), &total);
        ComPtr<ID3D12Resource> upload = makeBuffer(m_device, total, D3D12_HEAP_TYPE_UPLOAD, L"light function image upload");
        uint8_t* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(upload->Map(0, &none, reinterpret_cast<void**>(&p)), "map light function image upload");
        for (uint32_t m = 0; m < mips; ++m)
        {
            const Image& im = chain[m];
            for (uint32_t y = 0; y < im.height; ++y)
            {
                uint16_t* row = reinterpret_cast<uint16_t*>(p + fp[m].Offset + (uint64_t)y * fp[m].Footprint.RowPitch);
                for (uint32_t x = 0; x < im.width; ++x)
                {
                    const float* s = im.rgb.data() + ((size_t)y * im.width + x) * 3;
                    row[4 * x + 0] = toHalf(s[0]);
                    row[4 * x + 1] = toHalf(s[1]);
                    row[4 * x + 2] = toHalf(s[2]);
                    row[4 * x + 3] = toHalf(1.0f);
                }
            }
        }
        upload->Unmap(0, nullptr);
        t.srv = m_device.descriptors().allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.Texture2D.MipLevels = mips;
        m_device.d3d()->CreateShaderResourceView(t.texture.Get(), &sd, m_device.descriptors().resourceCpu(t.srv));
        t.width = f.image.width;
        t.height = f.image.height;
        t.mips = mips;
        t.revision = revision;
        const TextureRef ref = g.importTexture(t.texture.Get(), TextureDesc{ "lights.image", t.width, t.height, 1, (uint16_t)mips, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                               D3D12_BARRIER_LAYOUT_COMMON);
        ID3D12Resource* src = upload.Get();
        g.addPass("lights.image.upload", QueueType::Graphics, [=](PassBuilder& b) { b.use(ref, Use::CopyDst); },
                  [=](PassContext& c) {
                      for (uint32_t m = 0; m < mips; ++m)
                      {
                          D3D12_TEXTURE_COPY_LOCATION dst{ c.resource(ref), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                          dst.SubresourceIndex = m;
                          D3D12_TEXTURE_COPY_LOCATION from{ src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                          from.PlacedFootprint = fp[m];
                          c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
                      }
                  });
        m_uploads.push_back(upload);
        return ref;
    }
    std::vector<uint32_t> buildTable(const LightFunctions& lf) const
    {
        const uint32_t lights = lf.lights();
        std::vector<uint32_t> w(4 + ((lights + 3) & ~3u), 0);
        w[0] = lights;
        for (uint32_t i = 0; i < lights; ++i)
        {
            const LightFunction* f = lf.get(i);
            if (!f) continue;
            const uint32_t record = (uint32_t)w.size();
            w[4 + i] = record * 4;
            w.resize(w.size() + 24, 0);
            auto put = [&](uint32_t k, uint32_t v) { w[record + k] = v; };
            auto putf = [&](uint32_t k, float v) { w[record + k] = asUint(v); };
            put(0, (uint32_t)f->profile);
            if (f->profile == Profile::Cookie || f->profile == Profile::Gobo)
            {
                const Texture& t = m_textures[i];
                put(1, t.srv);
                put(2, t.width);
                put(3, t.height);
                put(4, t.mips);
            }
            putf(8, f->tanX);
            putf(9, f->tanY);
            putf(10, f->rotationSpeed);
            putf(11, f->rotationPhase);
            putf(18, f->flickerDepth);
            putf(19, f->flickerFrequency);
            put(20, f->flickerOctaves);
            put(21, f->flickerSeed);
            if (f->profile == Profile::Ies)
            {
                const IesProfile& p = f->ies;
                put(5, (uint32_t)p.horizontal.size());
                put(6, (uint32_t)p.vertical.size());
                put(7, (uint32_t)w.size() * 4);
                for (float a : p.horizontal) w.push_back(asUint(a));
                for (float a : p.vertical) w.push_back(asUint(a));
                for (float v : p.values) w.push_back(asUint(v));
            }
            put(12, (uint32_t)f->intensityKeys.size());
            putf(13, f->intensityPeriod);
            put(14, (uint32_t)w.size() * 4);
            for (const float2& k : f->intensityKeys)
            {
                w.push_back(asUint(k.x));
                w.push_back(asUint(k.y));
            }
            while (w.size() & 3) w.push_back(0);  // float4 keys on 16 B
            put(15, (uint32_t)f->colorKeys.size());
            putf(16, f->colorPeriod);
            put(17, (uint32_t)w.size() * 4);
            for (const float4& k : f->colorKeys)
            {
                w.push_back(asUint(k.x));
                w.push_back(asUint(k.y));
                w.push_back(asUint(k.z));
                w.push_back(asUint(k.w));
            }
            while (w.size() & 3) w.push_back(0);
        }
        return w;
    }

    Device& m_device;
    std::vector<Texture> m_textures;  // per light index
    ComPtr<ID3D12Resource> m_table;
    std::vector<ComPtr<ID3D12Resource>> m_uploads;  // staging of copies recorded this frame (released at the next record)
    uint64_t m_tableBytes = 0, m_tableRevision = 0;
};
} // namespace
} // namespace unx::lights

namespace unx::render::tracks
{
void lightFunctions(FramePassContext& fc)
{
    if (!fc.trackState) return;
    auto& p = fc.trackState->get<std::unique_ptr<lights::LightFunctionPass>>("lights.pass");
    if (!p) p = std::make_unique<lights::LightFunctionPass>(fc.device);
    p->record(fc, lights::lightFunctions(*fc.trackState));
}
} // namespace unx::render::tracks
