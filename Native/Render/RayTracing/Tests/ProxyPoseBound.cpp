// CPU test of the posed proxy error bound (ProxyPoseBound.h): a skinned tube of two joints (smooth weights across the
// middle) and a coarse cut that keeps only the end rings, so its long triangles span the joint. For bends of 0 to 120
// degrees the bound must hold: it is at least the Hausdorff distance between the posed source and the posed cut
// (points sampled densely on each, their distances to the other's triangles by brute force). Also reports the ratio bound / measured.
//
//   unx_test_raytracing_proxyposebound
#include "unx/core/Log.h"
#include "unx/rt/ProxyPoseBound.h"
#include "unx/scene/SceneData.h"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
constexpr float kPi = 3.14159265358979f;

// Tube along +y from 0 to 2 m, radius 0.1 m, 'rings' rings of 'sides' vertices; joint 0 below, joint 1 above, weights
// blend linearly over y in [0.8, 1.2].
scene::Mesh tube(uint32_t rings, uint32_t sides)
{
    scene::Mesh m;
    m.name = "tube";
    for (uint32_t r = 0; r < rings; ++r)
        for (uint32_t s = 0; s < sides; ++s)
        {
            const float y = 2.0f * r / (rings - 1), a = 2 * kPi * s / sides;
            m.positions.push_back({ 0.1f * std::cos(a), y, 0.1f * std::sin(a) });
            m.normals.push_back({ std::cos(a), 0, std::sin(a) });
            m.uv0.push_back({ (float)s / sides, y });
            const float w1 = std::clamp((y - 0.8f) / 0.4f, 0.0f, 1.0f);
            m.skin.joints.insert(m.skin.joints.end(), { 0, 1, 0, 0 });
            m.skin.weights.insert(m.skin.weights.end(), { 1 - w1, w1, 0, 0 });
        }
    for (uint32_t r = 0; r + 1 < rings; ++r)
        for (uint32_t s = 0; s < sides; ++s)
        {
            const uint32_t a = r * sides + s, b = r * sides + (s + 1) % sides, c = a + sides, d = b + sides;
            m.indices.insert(m.indices.end(), { a, c, b, b, c, d });
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    m.skin.inverseBind.resize(2);
    return m;
}

// Joint 1 rotated by 'angle' about the x axis through (0, 1, 0); joint 0 at rest.
std::vector<float4> palette(float angle)
{
    const float c = std::cos(angle), s = std::sin(angle);
    // p' = R (p - o) + o, o = (0, 1, 0): rows of [R | o - R o].
    const float ty = 1 - c, tz = -s;
    return { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 1, 0, 0, 0 }, { 0, c, -s, ty }, { 0, s, c, tz } };
}

float3 skinPoint(const scene::Mesh& m, const std::vector<float4>& pal, uint32_t v)
{
    float3 out{};
    for (int k = 0; k < 4; ++k)
    {
        const float w = m.skin.weights[4 * v + k];
        if (w <= 0) continue;
        const uint32_t j = m.skin.joints[4 * v + k];
        const float3 p = m.positions[v];
        const float4 a = pal[3 * j], b = pal[3 * j + 1], c = pal[3 * j + 2];
        out = out + float3{ a.x * p.x + a.y * p.y + a.z * p.z + a.w, b.x * p.x + b.y * p.y + b.z * p.z + b.w, c.x * p.x + c.y * p.y + c.z * p.z + c.w } * w;
    }
    return out;
}

// Points on a posed triangle list: 36 barycentric samples per triangle.
std::vector<float3> samples(const std::vector<float3>& posed, const std::vector<uint32_t>& indices)
{
    std::vector<float3> out;
    for (size_t t = 0; t + 2 < indices.size(); t += 3)
        for (int i = 0; i <= 7; ++i)
            for (int j = 0; i + j <= 7; ++j)
            {
                const float u = i / 7.0f, v = j / 7.0f;
                out.push_back(posed[indices[t]] * (1 - u - v) + posed[indices[t + 1]] * u + posed[indices[t + 2]] * v);
            }
    return out;
}

// Distance from p to triangle abc (Ericson 5.1.5).
float pointTriangle(float3 p, float3 a, float3 b, float3 c)
{
    const float3 ab = b - a, ac = c - a, ap = p - a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return length(p - a);
    const float3 bp = p - b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return length(p - b);
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return length(p - (a + ab * (d1 / (d1 - d3))));
    const float3 cp = p - c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return length(p - c);
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return length(p - (a + ac * (d2 / (d2 - d6))));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) return length(p - (b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)))));
    const float denom = 1 / (va + vb + vc);
    return length(p - (a + ab * (vb * denom) + ac * (vc * denom)));
}

// Largest distance from the sample points to the posed triangle list.
float nearestMax(const std::vector<float3>& from, const std::vector<float3>& posed, const std::vector<uint32_t>& indices)
{
    float worst = 0;
    for (const float3& p : from)
    {
        float best = 1e30f;
        for (size_t t = 0; t + 2 < indices.size(); t += 3) best = std::min(best, pointTriangle(p, posed[indices[t]], posed[indices[t + 1]], posed[indices[t + 2]]));
        worst = std::max(worst, best);
    }
    return worst;
}
} // namespace

int main()
{
    const uint32_t rings = 41, sides = 12;
    const scene::Mesh m = tube(rings, sides);
    // The cut: the bottom and top rings joined directly (two rings, 2 x sides triangles).
    std::vector<uint32_t> cut;
    const uint32_t top = (rings - 1) * sides;
    for (uint32_t s = 0; s < sides; ++s)
    {
        const uint32_t a = s, b = (s + 1) % sides, c = top + s, d = top + (s + 1) % sides;
        cut.insert(cut.end(), { a, c, b, b, c, d });
    }
    rt::ProxyPoseSkeleton skeleton = rt::proxyPoseSkeleton(m);
    const rt::ProxyPoseCoefficients coefficients = rt::proxyPoseCoefficients(m, skeleton, cut);
    logf("tube %u triangles, cut %zu triangles: bind error %.5f m, %zu (joint, reference) terms\n", (uint32_t)(m.indices.size() / 3), cut.size() / 3,
         coefficients.bindError, coefficients.terms.size());
    bool pass = coefficients.bindError < 1e-4f;  // a cylinder's end rings span its straight sides exactly
    for (const float degrees : { 0.0f, 10.0f, 30.0f, 60.0f, 90.0f, 120.0f })
    {
        const std::vector<float4> pal = palette(degrees * kPi / 180);
        std::vector<float3> posed(m.positions.size());
        for (uint32_t v = 0; v < (uint32_t)posed.size(); ++v) posed[v] = skinPoint(m, pal, v);
        const std::vector<float3> source = samples(posed, m.indices), coarse = samples(posed, cut);
        const float measured = std::max(nearestMax(source, posed, cut), nearestMax(coarse, posed, m.indices));
        rt::ProxyPoseTerms terms;
        rt::proxyPoseTerms(skeleton, pal, terms);
        const float bound = rt::proxyPoseError(coefficients, terms);
        const bool ok = bound + 1e-4f >= measured;  // 0.1 mm: the float rounding of the measurement
        logf("  bend %5.1f deg: measured Hausdorff %.4f m, bound %.4f m (ratio %.2f) -> %s\n", degrees, measured, bound, measured > 0 ? bound / measured : 0.0f,
             ok ? "PASS" : "FAIL");
        pass = pass && ok;
    }
    logf("RESULT %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
