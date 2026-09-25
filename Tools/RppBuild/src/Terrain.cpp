#include "Terrain.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>

namespace unx::rpp
{
float3 Anchor::rotate(float3 v) const
{
    const float c = std::cos(yaw), s = std::sin(yaw);
    return { c * v.x + s * v.z, v.y, -s * v.x + c * v.z };
}
float3 Anchor::toWorld(float3 p) const { return rotate(p) + translation; }
float3 Anchor::toLocal(float3 p) const
{
    const float3 d = p - translation;
    const float c = std::cos(yaw), s = std::sin(yaw);
    return { c * d.x - s * d.z, d.y, s * d.x + c * d.z };
}
float3x4 Anchor::matrix() const
{
    const float c = std::cos(yaw), s = std::sin(yaw);
    float3x4 m;
    m.m[0][0] = c;  m.m[0][1] = 0; m.m[0][2] = s; m.m[0][3] = translation.x;
    m.m[1][0] = 0;  m.m[1][1] = 1; m.m[1][2] = 0; m.m[1][3] = translation.y;
    m.m[2][0] = -s; m.m[2][1] = 0; m.m[2][2] = c; m.m[2][3] = translation.z;
    return m;
}
float4 Anchor::quaternion() const { return { 0, std::sin(0.5f * yaw), 0, std::cos(0.5f * yaw) }; }

namespace sg
{
float smoothstep(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
float rollingTerrain(float x, float z)
{
    return 6.f * std::sin(x / 70.f) * std::cos(z / 90.f) + 2.f * std::sin(x / 13.f + z / 17.f) + 0.7f * std::sin(x / 3.1f - z / 2.7f);
}
float cityTerrain(float x, float z)
{
    const float d = std::max(std::fabs(x), std::fabs(z));
    const float w = smoothstep(260.0f, 420.0f, d);
    return w * (8.f * std::sin(x / 110.f) * std::cos(z / 130.f) + 3.f * std::sin(x / 37.f + z / 53.f) + 4.f);
}
float lakeFloor(float x, float z)
{
    const float r = std::sqrt(x * x + z * z);
    const float lake = 1.0f - smoothstep(120.0f, 190.0f, r);
    const float bay = (1.0f - smoothstep(90.0f, 140.0f, std::fabs(z))) * smoothstep(60.0f, 120.0f, x) * (1.0f - smoothstep(430.0f, 470.0f, x));
    const float basin = std::max(lake, bay);
    const float land = 0.5f * rollingTerrain(x, z) + 3.0f;
    return land + (-4.0f - land) * basin;
}
} // namespace sg

float Terrain::cityWeight(float x, float z) const
{
    const float3 l = m_layout.city.toLocal({ x, 0, z });
    return 1.0f - sg::smoothstep(m_layout.cityCore, m_layout.cityRing, std::max(std::fabs(l.x), std::fabs(l.z)));
}

float Terrain::lakeWeight(float x, float z) const
{
    const float3 l = m_layout.lake.toLocal({ x, 0, z });
    const float m0 = m_layout.lakeMargin0, m1 = m_layout.lakeMargin1;
    const float r = std::sqrt(l.x * l.x + l.z * l.z);
    const float basin = 1.0f - sg::smoothstep(190.0f + m0, 190.0f + m1, r);
    const float bay = (1.0f - sg::smoothstep(140.0f + m0, 140.0f + m1, std::fabs(l.z))) * sg::smoothstep(60.0f - m1, 60.0f - m0, l.x) *
                      (1.0f - sg::smoothstep(470.0f + m0, 470.0f + m1, l.x));
    return std::max(basin, bay);
}

float Terrain::padWeight(float x, float z) const
{
    const float dx = x - m_layout.lodge.translation.x, dz = z - m_layout.lodge.translation.z;
    return 1.0f - sg::smoothstep(m_layout.padRadius, m_layout.padRing, std::sqrt(dx * dx + dz * dz));
}

float Terrain::sourceHeight(Source s, float x, float z) const
{
    switch (s)
    {
    case Source::Forest: return sg::rollingTerrain(x, z);
    case Source::City:
    {
        const float3 l = m_layout.city.toLocal({ x, 0, z });
        return m_layout.city.translation.y + sg::cityTerrain(l.x, l.z);
    }
    case Source::Lake:
    {
        const float3 l = m_layout.lake.toLocal({ x, 0, z });
        return m_layout.lake.translation.y + sg::lakeFloor(l.x, l.z);
    }
    case Source::Lodge: return m_layout.lodge.translation.y - 0.2f;  // C interior's outside ground
    }
    return 0;
}

float Terrain::height(float x, float z) const
{
    float h = sg::rollingTerrain(x, z);
    const float wc = cityWeight(x, z);
    if (wc > 0) h += (m_layout.city.translation.y - h) * wc;  // flat core: cityTerrain is 0 inside cityCore
    const float wl = lakeWeight(x, z);
    if (wl > 0) h += (sourceHeight(Source::Lake, x, z) - h) * wl;
    const float wp = padWeight(x, z);
    if (wp > 0) h += (sourceHeight(Source::Lodge, x, z) - h) * wp;
    return h;
}

bool Terrain::inCitySquare(float x, float z) const
{
    const float3 l = m_layout.city.toLocal({ x, 0, z });
    return std::fabs(l.x) <= m_layout.citySquare && std::fabs(l.z) <= m_layout.citySquare;
}

scene::Mesh Terrain::buildMesh(uint32_t material) const
{
    const int n = 1000;
    const float x0 = -1000, z0 = -1000, d = 2000.0f / n;
    scene::Mesh m;
    m.name = "rpp1_terrain";
    m.positions.reserve((size_t)(n + 1) * (n + 1));
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            const float x = x0 + i * d, z = z0 + j * d, e = 0.5f;
            const float hx = (height(x + e, z) - height(x - e, z)) / (2 * e), hz = (height(x, z + e) - height(x, z - e)) / (2 * e);
            m.positions.push_back({ x, height(x, z), z });
            m.normals.push_back(normalize(float3{ -hx, 1, -hz }));
            m.uv0.push_back({ x / 8, z / 8 });
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const float qx = x0 + i * d, qz = z0 + j * d;
            // Cells wholly inside the city street square are left out (the street plane covers them), as in C's city.
            if (inCitySquare(qx, qz) && inCitySquare(qx + d, qz) && inCitySquare(qx, qz + d) && inCitySquare(qx + d, qz + d)) continue;
            const uint32_t a = (uint32_t)(j * (n + 1) + i), b = a + 1, c = a + n + 1, dd = c + 1;
            // counter-clockwise seen from above (+Y): a, c, dd / a, dd, b (x right, z down the rows)
            m.indices.insert(m.indices.end(), { a, c, dd, a, dd, b });
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), material });
    return m;
}

namespace
{
const scene::Mesh& terrainMesh(const scene::Scene& s)
{
    for (const scene::Mesh& m : s.meshes)
        if (m.name == "terrain") return m;
    fail("rppbuild: %s has no mesh named 'terrain'", s.name.c_str());
}
template <class F>
void compare(const scene::Scene& s, F f)
{
    const scene::Mesh& m = terrainMesh(s);
    double worst = 0;
    for (const float3& p : m.positions) worst = std::max(worst, (double)std::fabs(p.y - f(p.x, p.z)));
    if (worst > 1e-3) fail("rppbuild: %s terrain differs from RppBuild's copy of C's formula by %.4f m (C changed SceneGen: update Tools/RppBuild/src/Terrain.cpp)", s.name.c_str(), worst);
    logf("terrain check %s: %zu vertices, worst %.2e m\n", s.name.c_str(), m.positions.size(), worst);
}
} // namespace

void checkAgainstSceneGen(const scene::Scene& forest, const scene::Scene& city, const scene::Scene& lake)
{
    compare(forest, sg::rollingTerrain);
    compare(city, sg::cityTerrain);
    compare(lake, sg::lakeFloor);
}
} // namespace unx::rpp
