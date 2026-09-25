#include "Meshes.h"

#include <cmath>

namespace unx::rpp
{
scene::Mesh boxMesh(const std::string& name, float3 lo, float3 hi, uint32_t material)
{
    scene::Mesh m;
    m.name = name;
    const float3 c = (lo + hi) * 0.5f, e = (hi - lo) * 0.5f;
    const float3 axes[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const float3& a : axes)
    {
        const float3 u = a.x != 0 ? float3{ 0, 0, a.x } : float3{ 1, 0, 0 };
        const float3 wv = cross(a, u);
        const uint32_t base = (uint32_t)m.positions.size();
        for (int k = 0; k < 4; ++k)
        {
            const float su = (k == 1 || k == 2) ? 1.0f : -1.0f, sw = k >= 2 ? 1.0f : -1.0f;
            const float3 p = c + float3{ (a.x + u.x * su + wv.x * sw) * e.x, (a.y + u.y * su + wv.y * sw) * e.y, (a.z + u.z * su + wv.z * sw) * e.z };
            m.positions.push_back(p);
            m.normals.push_back(a);
            m.tangents.push_back({ u.x, u.y, u.z, 1.0f });
            m.uv0.push_back({ dot(p, u) * 0.5f, dot(p, wv) * 0.5f });
        }
        // u x w = a: counter-clockwise seen from outside
        m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), material });
    return m;
}

scene::Mesh cylinderMesh(const std::string& name, float radius, float halfHeight, uint32_t sides, uint32_t material)
{
    scene::Mesh m;
    m.name = name;
    const float kTwoPi = 6.28318530718f;
    // Side: sides + 1 columns (seam duplicated for UVs), two rings.
    for (uint32_t k = 0; k <= sides; ++k)
    {
        const float a = kTwoPi * (float)k / (float)sides, c = std::cos(a), s = std::sin(a);
        for (int ring = 0; ring < 2; ++ring)
        {
            const float y = ring ? halfHeight : -halfHeight;
            m.positions.push_back({ radius * c, y, radius * s });
            m.normals.push_back({ c, 0, s });
            m.tangents.push_back({ -s, 0, c, 1.0f });  // direction of increasing arc length (u)
            m.uv0.push_back({ radius * a, y });
        }
    }
    for (uint32_t k = 0; k < sides; ++k)
    {
        const uint32_t a0 = 2 * k, a1 = a0 + 1, b0 = a0 + 2, b1 = a0 + 3;
        // outward normal: tangent (u, increasing k) x up points inward, so the order is (a0, a1, b0) / (b0, a1, b1)
        m.indices.insert(m.indices.end(), { a0, a1, b0, b0, a1, b1 });
    }
    // Caps: centre + rim, normals +-Y.
    for (int cap = 0; cap < 2; ++cap)
    {
        const float y = cap ? halfHeight : -halfHeight, ny = cap ? 1.0f : -1.0f;
        const uint32_t centre = (uint32_t)m.positions.size();
        m.positions.push_back({ 0, y, 0 });
        m.normals.push_back({ 0, ny, 0 });
        m.tangents.push_back({ 1, 0, 0, 1.0f });
        m.uv0.push_back({ 0, 0 });
        for (uint32_t k = 0; k <= sides; ++k)
        {
            const float a = kTwoPi * (float)k / (float)sides, c = std::cos(a), s = std::sin(a);
            m.positions.push_back({ radius * c, y, radius * s });
            m.normals.push_back({ 0, ny, 0 });
            m.tangents.push_back({ 1, 0, 0, 1.0f });
            m.uv0.push_back({ radius * c * 0.5f, radius * s * 0.5f });
        }
        for (uint32_t k = 0; k < sides; ++k)
        {
            const uint32_t r0 = centre + 1 + k, r1 = r0 + 1;
            if (cap) m.indices.insert(m.indices.end(), { centre, r1, r0 });  // seen from +Y: counter-clockwise
            else m.indices.insert(m.indices.end(), { centre, r0, r1 });
        }
    }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), material });
    return m;
}
} // namespace unx::rpp
