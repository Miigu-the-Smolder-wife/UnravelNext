#pragma once
// renderergate --gi-cache-stats (RENDERER_REDESIGN_V2 10.6 / 10.8): a CPU pass over a readback of R's GI cache after the
// run (GiCache.hlsli layout; the functions below are that header's, ported). Two quantities the redesign's first-frame
// target rests on:
//   - parent against child: for every live entry of the current lighting epoch with at least 16 updates (phase of the
//     history word), its parent at level l + 1 (else l + 2) - the cell containing it, same normal class - when that parent
//     also has 16: |E_parent(n) - E_child(n)| / max(E_child(n), E_parent(n)) in luminance, n = the child's anchor normal,
//     E from each entry's own 9 x 9 irradiance map (giIrrMapAt). The distribution is what "the parent's converged value as
//     the child's prior" (1.1b) would start from.
//   - the entries read in the last frame (meta stamp = the header's frame): their updates in this epoch (young < 4, < 16).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace gate_gi
{
struct V3
{
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 operator*(V3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 normalize(V3 a)
{
    const float l = std::sqrt(dot(a, a));
    return l > 0 ? a * (1.0f / l) : V3{ 0, 0, 1 };
}

struct Cache
{
    const uint8_t* b = nullptr;
    size_t size = 0;
    uint32_t u(size_t off) const
    {
        uint32_t v = 0;
        if (off + 4 <= size) std::memcpy(&v, b + off, 4);
        return v;
    }
    float f(size_t off) const
    {
        const uint32_t v = u(off);
        float r;
        std::memcpy(&r, &v, 4);
        return r;
    }
};

inline void basis(V3 n, V3& t, V3& bt)
{
    const float s = n.z >= 0 ? 1.0f : -1.0f;
    const float a = -1.0f / (s + n.z);
    const float c = n.x * n.y * a;
    t = { 1 + s * n.x * n.x * a, s * c, -s * n.x };
    bt = { c, s + n.y * n.y * a, -n.y };
}
inline void hemiOctEncode(V3 d, float& u, float& v)
{
    const float k = 1.0f / (std::fabs(d.x) + std::fabs(d.y) + std::max(d.z, 0.0f));
    const float x = d.x * k, y = d.y * k;
    u = (x + y) * 0.5f + 0.5f;
    v = (x - y) * 0.5f + 0.5f;
}
inline V3 irrUnpack(uint32_t w)
{
    const uint32_t eb = ((w >> 27) + 103u) << 23;
    float scale;
    std::memcpy(&scale, &eb, 4);
    return V3{ (float)(w & 0x1FFu), (float)((w >> 9) & 0x1FFu), (float)((w >> 18) & 0x1FFu) } * scale;
}
inline void catmullRom(float t, float w[4])
{
    const float t2 = t * t, t3 = t2 * t;
    w[0] = -0.5f * t3 + t2 - 0.5f * t;
    w[1] = 1.5f * t3 - 2.5f * t2 + 1;
    w[2] = -1.5f * t3 + 2 * t2 + 0.5f * t;
    w[3] = 0.5f * t3 - 0.5f * t2;
}
inline V3 unpackNormal(uint32_t packed)
{
    const float ex = (float)(int16_t)(packed & 0xFFFFu) / 32767.0f, ey = (float)(int16_t)(packed >> 16) / 32767.0f;
    V3 n{ ex, ey, 1.0f - std::fabs(ex) - std::fabs(ey) };
    if (n.z < 0)
    {
        const float nx = (1.0f - std::fabs(n.y)) * (n.x >= 0 ? 1.0f : -1.0f), ny = (1.0f - std::fabs(n.x)) * (n.y >= 0 ? 1.0f : -1.0f);
        n.x = nx;
        n.y = ny;
    }
    return normalize(n);
}

// giIrrMapAt: irradiance (x 1) of 'entry' for normal n, na = its anchor normal.
inline V3 irrMapAt(const Cache& c, uint32_t offIrr, uint32_t entry, V3 na, V3 n)
{
    constexpr int N = 9;
    V3 t, bt;
    basis(na, t, bt);
    V3 local{ dot(n, t), dot(n, bt), std::max(dot(n, na), 0.0f) };
    local = dot(local, local) > 1e-12f ? normalize(local) : V3{ 0, 0, 1 };
    float u, v;
    hemiOctEncode(local, u, v);
    const float ex = u * N - 0.5f, ey = v * N - 0.5f;
    const int ix = (int)std::floor(ex), iy = (int)std::floor(ey);
    float wx[4], wy[4];
    catmullRom(ex - ix, wx);
    catmullRom(ey - iy, wy);
    const size_t base = (size_t)offIrr + (size_t)entry * 336u;
    V3 sum{};
    for (int jy = 0; jy < 4; ++jy)
    {
        const int ry = std::clamp(iy - 1 + jy, 0, N - 1);
        V3 row{};
        for (int jx = 0; jx < 4; ++jx)
        {
            const int rx = std::clamp(ix - 1 + jx, 0, N - 1);
            row = row + irrUnpack(c.u(base + (size_t)(ry * N + rx) * 4)) * wx[jx];
        }
        sum = sum + row * wy[jy];
    }
    return V3{ std::max(sum.x, 0.0f), std::max(sum.y, 0.0f), std::max(sum.z, 0.0f) } * 64.0f;
}

inline int signExtend18(uint32_t v) { return (int)(v << 14) >> 14; }
inline uint64_t parentKey(uint64_t key, uint32_t steps)
{
    const uint32_t level = (uint32_t)(key & 31u), cls = (uint32_t)((key >> 5) & 7u);
    int cx = signExtend18((uint32_t)(key >> 8) & 0x3FFFFu), cy = signExtend18((uint32_t)(key >> 26) & 0x3FFFFu), cz = signExtend18((uint32_t)(key >> 44) & 0x3FFFFu);
    cx >>= steps;  // floor(c / 2^steps): the cell containing this one, s_{l+k} = 2^k s_l
    cy >>= steps;
    cz >>= steps;
    const uint64_t k = (uint64_t)(level + steps) | ((uint64_t)cls << 5) | ((uint64_t)((uint32_t)cx & 0x3FFFFu) << 8) | ((uint64_t)((uint32_t)cy & 0x3FFFFu) << 26) |
                       ((uint64_t)((uint32_t)cz & 0x3FFFFu) << 44);
    return k | (1ull << 63);
}

inline double percentile(std::vector<float>& v, double p)
{
    if (v.empty()) return 0;
    const size_t k = std::min(v.size() - 1, (size_t)std::llround(p * (double)(v.size() - 1)));
    std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
    return v[k];
}

// JSON members ("key": value, ...) of the analysis.
inline std::string analyse(const uint8_t* bytes, size_t size)
{
    Cache c{ bytes, size };
    const uint32_t capacity = c.u(0), offMeta = c.u(24), offAnchor = c.u(28), offSh = c.u(32), frame = c.u(60), epoch = c.u(72), offIrr = c.u(248);
    std::unordered_map<uint64_t, uint32_t> byKey;
    byKey.reserve(capacity);
    std::vector<uint32_t> live;
    auto keyOf = [&](uint32_t e) { return (uint64_t)c.u((size_t)offMeta + e * 16u) | ((uint64_t)c.u((size_t)offMeta + e * 16u + 4) << 32); };
    auto phaseOf = [&](uint32_t e) -> uint32_t {
        const size_t a = (size_t)offSh + (size_t)e * 80u;
        return c.u(a + 72) == epoch ? (c.u(a + 68) & 0xFFFu) : 0u;
    };
    for (uint32_t e = 0; e < capacity; ++e)
    {
        const uint64_t k = keyOf(e);
        if ((k >> 63) == 0) continue;
        byKey.emplace(k, e);
        live.push_back(e);
    }
    const float lw[3] = { 0.2126f, 0.7152f, 0.0722f };
    auto lum = [&](V3 v) { return v.x * lw[0] + v.y * lw[1] + v.z * lw[2]; };
    std::vector<float> rel1, rel2;  // parent at l+1, at l+2 (when l+1 is missing or young)
    uint32_t converged = 0, noParent = 0, youngParent = 0, readLast = 0, read4 = 0, read16 = 0, readZero = 0;
    uint32_t levelHist[24] = {};
    for (uint32_t e : live)
    {
        const uint32_t n = phaseOf(e);
        if (c.u((size_t)offMeta + e * 16u + 8) == frame)
        {
            ++readLast;
            if (n == 0) ++readZero;
            if (n < 4) ++read4;
            if (n < 16) ++read16;
        }
        if (n < 16) continue;
        ++converged;
        const uint64_t k = keyOf(e);
        ++levelHist[std::min<uint32_t>((uint32_t)(k & 31u), 23u)];
        const V3 na = unpackNormal(c.u((size_t)offAnchor + e * 16u + 12));
        const V3 ec = irrMapAt(c, offIrr, e, na, na);
        bool done = false, sawYoung = false;
        for (uint32_t steps = 1; steps <= 2 && !done; ++steps)
        {
            const auto it = byKey.find(parentKey(k, steps));
            if (it == byKey.end()) continue;
            if (phaseOf(it->second) < 16)
            {
                sawYoung = true;
                continue;
            }
            const V3 pn = unpackNormal(c.u((size_t)offAnchor + it->second * 16u + 12));
            const V3 ep = irrMapAt(c, offIrr, it->second, pn, na);
            const float a = lum(ec), b = lum(ep);
            const float d = std::fabs(a - b) / std::max(std::max(a, b), 1e-6f);
            (steps == 1 ? rel1 : rel2).push_back(d);
            done = true;
        }
        if (!done) (sawYoung ? youngParent : noParent)++;
    }
    auto dist = [&](const char* name, std::vector<float>& v) {
        const size_t le3 = (size_t)std::count_if(v.begin(), v.end(), [](float x) { return x <= 0.03f; });
        const size_t le5 = (size_t)std::count_if(v.begin(), v.end(), [](float x) { return x <= 0.05f; });
        char s[512];
        std::snprintf(s, sizeof s,
                      "\"%s\": {\"pairs\": %zu, \"p50\": %.4f, \"p90\": %.4f, \"p95\": %.4f, \"p99\": %.4f, \"share_le_3pct\": %.4f, \"share_le_5pct\": %.4f}", name,
                      v.size(), percentile(v, 0.5), percentile(v, 0.9), percentile(v, 0.95), percentile(v, 0.99), v.empty() ? 0.0 : (double)le3 / v.size(),
                      v.empty() ? 0.0 : (double)le5 / v.size());
        return std::string(s);
    };
    std::string levels;
    for (uint32_t l = 0; l < 24; ++l)
        if (levelHist[l]) levels += (levels.empty() ? "" : ", ") + std::string("\"") + std::to_string(l) + "\": " + std::to_string(levelHist[l]);
    char head[512];
    std::snprintf(head, sizeof head,
                  "\"gi_cache_frame\": %u, \"gi_cache_live\": %zu, \"gi_cache_converged16\": %u, \"gi_cache_read_last_frame\": %u, "
                  "\"gi_cache_read_young0\": %u, \"gi_cache_read_young4\": %u, \"gi_cache_read_young16\": %u, "
                  "\"gi_parent_missing\": %u, \"gi_parent_young\": %u, ",
                  frame, live.size(), converged, readLast, readZero, read4, read16, noParent, youngParent);
    return std::string(head) + dist("gi_parent_l1", rel1) + ", " + dist("gi_parent_l2", rel2) + ", \"gi_converged_by_level\": {" + levels + "}";
}
} // namespace gate_gi
