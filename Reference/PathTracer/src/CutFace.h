#pragma once
// Cut face material class in the CPU reference (A11; the definition of Native/Render/Passes/Material/CutFace.hlsli,
// INTERFACES 8.1 Cut, v1.66 fields). A Cut surface is the Standard model whose texture values come from three object-space
// projections instead of uv0:
//   uv_k = cutScale (s_k p_b, p_c) for axis k with (b, c) = ((k + 1) % 3, (k + 2) % 3), s_k = sign of the normal's k
//          component; weights w_k = |n_k|^4 / sum (n = unit object-space geometric normal);
//   base colour, roughness factor, metallic factor = sum w_k (tap k);
//   shading normal = normalize(sum w_k r_k), r_k = whiteout(texture normal of tap k, projection k, unit object-space
//          interpolated normal n) in projection k's frame (tangent s_k e_b, bitangent e_c, normal s_k e_k): a flat texel
//          gives exactly n;
//   edge damage (cutDamageWidth > 0): coverage of the noisy band along the triangle's boundary edges (the reference
//          integrates the pixel by its samples, so the band is a hard threshold here), applied as base x (1 - 0.45 d)
//          and roughness toward 1 by 0.6 d.
#include "unx/core/Math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace unx::reference::cutface
{
struct Projection
{
    float2 uv;
    float3 tangent, bitangent, normal;
    float weight;
};

inline float component(float3 v, uint32_t k) { return k == 0 ? v.x : (k == 1 ? v.y : v.z); }
inline void setComponent(float3& v, uint32_t k, float x) { (k == 0 ? v.x : (k == 1 ? v.y : v.z)) = x; }

inline Projection projection(uint32_t k, float3 p, float3 n, float scale)
{
    const uint32_t b = (k + 1) % 3, c = (k + 2) % 3;
    const float s = component(n, k) < 0 ? -1.0f : 1.0f;
    Projection o{};
    o.uv = { scale * s * component(p, b), scale * component(p, c) };
    setComponent(o.tangent, b, s);
    setComponent(o.bitangent, c, 1);
    setComponent(o.normal, k, s);
    const float ax = std::pow(std::fabs(n.x), 4.0f), ay = std::pow(std::fabs(n.y), 4.0f), az = std::pow(std::fabs(n.z), 4.0f);
    o.weight = component({ ax, ay, az }, k) / std::max(ax + ay + az, 1e-30f);
    return o;
}

inline float3 whiteout(float3 tn, const Projection& pr, float3 n)
{
    return pr.tangent * (tn.x + dot(n, pr.tangent)) + pr.bitangent * (tn.y + dot(n, pr.bitangent)) + pr.normal * (tn.z * dot(n, pr.normal));
}

// Distance from barycentrics (b0, b1, b2) of triangle (p0, p1, p2) to its nearest boundary edge (bit i: edge opposite
// corner i); +inf without one.
inline float edgeDistance(float3 bary, float3 p0, float3 p1, float3 p2, uint32_t mask)
{
    const float area2 = length(cross(p1 - p0, p2 - p0));
    const float lengths[3] = { length(p2 - p1), length(p0 - p2), length(p1 - p0) };
    const float b[3] = { bary.x, bary.y, bary.z };
    float d = INFINITY;
    for (uint32_t i = 0; i < 3; ++i)
        if ((mask >> i) & 1u) d = std::min(d, std::max(b[i], 0.0f) * area2 / std::max(lengths[i], 1e-30f));
    return d;
}

inline float hash(int32_t x, int32_t y, int32_t z)
{
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)y * 0xd8163841u ^ (uint32_t)z * 0xcb1ab31fu;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFFu) / 16777215.0f;
}

inline float noise(float3 q)
{
    const float fx = std::floor(q.x), fy = std::floor(q.y), fz = std::floor(q.z);
    auto fade = [](float t) { return t * t * t * (t * (t * 6 - 15) + 10); };
    const float ux = fade(q.x - fx), uy = fade(q.y - fy), uz = fade(q.z - fz);
    const int32_t cx = (int32_t)fx, cy = (int32_t)fy, cz = (int32_t)fz;
    float v = 0;
    for (uint32_t k = 0; k < 8; ++k)
    {
        const int32_t ox = k & 1, oy = (k >> 1) & 1, oz = (k >> 2) & 1;
        const float w = (ox ? ux : 1 - ux) * (oy ? uy : 1 - uy) * (oz ? uz : 1 - uz);
        v += w * hash(cx + ox, cy + oy, cz + oz);
    }
    return v;
}

// Damage at object position p and boundary distance d (footprint 0: the band's indicator).
inline float damage(float3 p, float distance, float width)
{
    if (!(width > 0) || !(distance < INFINITY)) return 0;
    const float3 q = p * (1.0f / width);
    const float n = 0.5f * noise(q) + 0.3f * noise(q * 2.03f + float3{ 17.1f, 17.1f, 17.1f }) + 0.2f * noise(q * 4.01f + float3{ 41.7f, 41.7f, 41.7f });
    const float threshold = width * (0.35f + 0.65f * n);
    return distance <= threshold ? 1.0f : 0.0f;
}
} // namespace unx::reference::cutface
