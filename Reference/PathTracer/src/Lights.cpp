#include "Lights.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>

namespace unx::reference
{
namespace
{
constexpr float kPi = 3.14159265358979323846f;
float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
float3 minf(float3 a, float3 b) { return { std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z) }; }
float3 maxf(float3 a, float3 b) { return { std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z) }; }

void orthonormal(float3 n, float3& t1, float3& t2)
{
    const float3 ref = std::fabs(n.y) < 0.99f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    t1 = normalize(cross(ref, n));
    t2 = cross(n, t1);
}

// First intersection of a ray with a capsule (segment pa-pb, radius r), -1 if none (Quilez).
float capsuleIntersect(float3 ro, float3 rd, float3 pa, float3 pb, float r)
{
    const float3 ba = pb - pa, oa = ro - pa;
    const float baba = dot(ba, ba), bard = dot(ba, rd), baoa = dot(ba, oa), rdoa = dot(rd, oa), oaoa = dot(oa, oa);
    const float a = baba - bard * bard, b = baba * rdoa - baoa * bard, c = baba * oaoa - baoa * baoa - r * r * baba;
    float h = b * b - a * c;
    if (h >= 0 && a > 1e-12f)
    {
        const float t = (-b - std::sqrt(h)) / a;
        const float y = baoa + t * bard;
        if (y > 0 && y < baba) return t;
        const float3 oc = y <= 0 ? oa : ro - pb;
        const float b2 = dot(rd, oc), c2 = dot(oc, oc) - r * r;
        h = b2 * b2 - c2;
        if (h > 0) return -b2 - std::sqrt(h);
        return -1;
    }
    // Ray parallel to the axis: only the end spheres can be hit.
    float best = -1;
    for (const float3& e : { pa, pb })
    {
        const float3 oc = ro - e;
        const float b2 = dot(rd, oc), c2 = dot(oc, oc) - r * r, h2 = b2 * b2 - c2;
        if (h2 > 0)
        {
            const float t = -b2 - std::sqrt(h2);
            if (t > 0 && (best < 0 || t < best)) best = t;
        }
    }
    return best;
}
} // namespace

LightSet::LightSet(const scene::Scene& s) : m_lights(s.lights)
{
    const size_t n = m_lights.size();
    m_up.resize(n);
    m_spotScale.resize(n);
    m_spotOffset.resize(n);
    if (n == 0) return;
    float3 lo{ INFINITY, INFINITY, INFINITY }, hi{ -INFINITY, -INFINITY, -INFINITY };
    std::vector<float> ranges;
    for (size_t i = 0; i < n; ++i)
    {
        scene::Light& l = m_lights[i];
        if (!(l.range > 0)) fail("reference: light %zu has range %g", i, l.range);
        l.forward = normalize(l.forward);
        l.right = normalize(l.right - l.forward * dot(l.right, l.forward));
        m_up[i] = cross(l.forward, l.right);
        const float ci = std::cos(l.spotInner), co = std::cos(l.spotOuter);
        m_spotScale[i] = 1.0f / std::max(ci - co, 1e-4f);
        m_spotOffset[i] = -co * m_spotScale[i];
        const float3 r{ l.range, l.range, l.range };
        lo = minf(lo, l.position - r);
        hi = maxf(hi, l.position + r);
        ranges.push_back(l.range);
    }
    std::nth_element(ranges.begin(), ranges.begin() + ranges.size() / 2, ranges.end());
    const float cellSize = std::clamp(ranges[ranges.size() / 2] * 0.5f, 0.5f, 16.0f);
    const float3 ext = hi - lo;
    m_min = lo;
    const float e[3] = { ext.x, ext.y, ext.z };
    float cs[3];
    for (int k = 0; k < 3; ++k)
    {
        m_dim[k] = std::clamp((uint32_t)std::ceil(e[k] / cellSize), 1u, 512u);
        cs[k] = e[k] / m_dim[k];
    }
    m_cell = { cs[0], cs[1], cs[2] };
    const size_t cells = (size_t)m_dim[0] * m_dim[1] * m_dim[2];
    std::vector<std::vector<uint32_t>> lists(cells);
    for (uint32_t i = 0; i < n; ++i)
    {
        const scene::Light& l = m_lights[i];
        const float3 a = l.position - float3{ l.range, l.range, l.range } - m_min, b = l.position + float3{ l.range, l.range, l.range } - m_min;
        const int x0 = std::max(0, (int)(a.x / m_cell.x)), x1 = std::min((int)m_dim[0] - 1, (int)(b.x / m_cell.x));
        const int y0 = std::max(0, (int)(a.y / m_cell.y)), y1 = std::min((int)m_dim[1] - 1, (int)(b.y / m_cell.y));
        const int z0 = std::max(0, (int)(a.z / m_cell.z)), z1 = std::min((int)m_dim[2] - 1, (int)(b.z / m_cell.z));
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                {
                    // Keep the light only if its range sphere touches the cell box.
                    const float3 cmin = m_min + float3{ x * m_cell.x, y * m_cell.y, z * m_cell.z }, cmax = cmin + m_cell;
                    const float3 q = maxf(cmin, minf(l.position, cmax));
                    const float3 dd = q - l.position;
                    if (dot(dd, dd) <= l.range * l.range) lists[((size_t)z * m_dim[1] + y) * m_dim[0] + x].push_back(i);
                }
    }
    m_cellStart.resize(cells + 1);
    for (size_t c = 0; c < cells; ++c)
    {
        m_cellStart[c] = (uint32_t)m_cellLights.size();
        m_cellLights.insert(m_cellLights.end(), lists[c].begin(), lists[c].end());
    }
    m_cellStart[cells] = (uint32_t)m_cellLights.size();
}

float LightSet::window(const scene::Light& l, float3 x) const
{
    const float3 d = x - l.position;
    const float r = std::sqrt(dot(d, d)) / l.range;
    const float r4 = r * r * r * r;
    const float w = saturate(1 - r4);
    return w * w;
}

float LightSet::importance(uint32_t i, float3 x) const
{
    const scene::Light& l = m_lights[i];
    const float3 d = x - l.position;
    const float d2 = dot(d, d);
    if (d2 >= l.range * l.range) return 0;
    const float w = window(l, x);
    if (w <= 0) return 0;
    const float lum = std::max(0.2126f * l.color.x + 0.7152f * l.color.y + 0.0722f * l.color.z, 1e-6f);
    switch (l.type)
    {
    case scene::LightType::Point: return l.intensity * lum * w / std::max(d2, 1e-4f);
    case scene::LightType::Spot:
    {
        const float c = dot(d, l.forward) / std::sqrt(std::max(d2, 1e-12f));
        const float sp = saturate(c * m_spotScale[i] + m_spotOffset[i]);
        return l.intensity * lum * w * sp * sp / std::max(d2, 1e-4f);
    }
    case scene::LightType::Rect:
    case scene::LightType::Disk:
    {
        if (dot(d, l.forward) <= 0) return 0;  // behind a one-sided emitter: every point of it is invisible
        const float area = l.type == scene::LightType::Rect ? l.size.x * l.size.y : kPi * l.size.x * l.size.x;
        return l.intensity * lum * area * w / std::max(d2, area);
    }
    case scene::LightType::Sphere:
    {
        const float a = kPi * l.size.x * l.size.x;
        return l.intensity * lum * a * w / std::max(d2, a);
    }
    case scene::LightType::Tube:
    {
        const float a = 2 * l.size.y * l.size.x + kPi * l.size.y * l.size.y;
        return l.intensity * lum * a * w / std::max(d2, a);
    }
    }
    return 0;
}

void LightSet::gather(float3 x, LightCandidates& out) const
{
    out.lights.clear();
    out.cumulative.clear();
    out.total = 0;
    if (m_lights.empty()) return;
    const float3 r = x - m_min;
    const int cx = (int)std::floor(r.x / m_cell.x), cy = (int)std::floor(r.y / m_cell.y), cz = (int)std::floor(r.z / m_cell.z);
    if (cx < 0 || cy < 0 || cz < 0 || cx >= (int)m_dim[0] || cy >= (int)m_dim[1] || cz >= (int)m_dim[2]) return;
    const size_t c = ((size_t)cz * m_dim[1] + cy) * m_dim[0] + cx;
    for (uint32_t k = m_cellStart[c]; k < m_cellStart[c + 1]; ++k)
    {
        const uint32_t i = m_cellLights[k];
        const float imp = importance(i, x);
        if (imp <= 0) continue;
        out.total += imp;
        out.lights.push_back(i);
        out.cumulative.push_back(out.total);
    }
}

uint32_t LightSet::choose(const LightCandidates& c, float u, float& probability)
{
    const float target = u * c.total;
    size_t k = std::upper_bound(c.cumulative.begin(), c.cumulative.end(), target) - c.cumulative.begin();
    k = std::min(k, c.lights.size() - 1);
    const float prev = k ? c.cumulative[k - 1] : 0.0f;
    probability = (c.cumulative[k] - prev) / c.total;
    return (uint32_t)k;
}

bool LightSet::sample(uint32_t i, float3 x, float u1, float u2, LightSample& s) const
{
    const scene::Light& l = m_lights[i];
    const float w = window(l, x);
    if (w <= 0) return false;
    const Rgb colour(l.color);
    switch (l.type)
    {
    case scene::LightType::Point:
    case scene::LightType::Spot:
    {
        const float3 d = l.position - x;
        const float d2 = dot(d, d);
        if (d2 <= 0) return false;
        s.distance = std::sqrt(d2);
        s.wi = d / s.distance;
        float f = l.intensity * w / d2;
        if (l.type == scene::LightType::Spot)
        {
            const float sp = saturate(dot(-s.wi, l.forward) * m_spotScale[i] + m_spotOffset[i]);
            f *= sp * sp;
        }
        if (f <= 0) return false;
        s.L = colour * f;
        s.pdf = 1;
        s.delta = true;
        return true;
    }
    case scene::LightType::Rect:
    case scene::LightType::Disk:
    {
        float3 p;
        float area;
        if (l.type == scene::LightType::Rect)
        {
            p = l.position + l.right * ((u1 - 0.5f) * l.size.x) + m_up[i] * ((u2 - 0.5f) * l.size.y);
            area = l.size.x * l.size.y;
        }
        else
        {
            // Concentric disk mapping (Shirley-Chiu).
            const float a = 2 * u1 - 1, b = 2 * u2 - 1;
            float rr = 0, phi = 0;
            if (a != 0 || b != 0)
            {
                if (std::fabs(a) > std::fabs(b)) { rr = a; phi = (kPi / 4) * (b / a); }
                else { rr = b; phi = kPi / 2 - (kPi / 4) * (a / b); }
            }
            p = l.position + l.right * (l.size.x * rr * std::cos(phi)) + m_up[i] * (l.size.x * rr * std::sin(phi));
            area = kPi * l.size.x * l.size.x;
        }
        const float3 d = p - x;
        const float d2 = dot(d, d);
        s.distance = std::sqrt(d2);
        s.wi = d / s.distance;
        const float cosL = -dot(s.wi, l.forward);
        if (cosL <= 0 || area <= 0) return false;
        s.pdf = d2 / (area * cosL);
        s.L = colour * (l.intensity * w);
        s.delta = false;
        return true;
    }
    case scene::LightType::Sphere:
    {
        const float3 dc = l.position - x;
        const float dist2 = dot(dc, dc), r = l.size.x;
        if (dist2 <= r * r) return false;  // inside the emitter
        const float dist = std::sqrt(dist2);
        const float sin2 = r * r / dist2, cosMax = std::sqrt(std::max(0.0f, 1 - sin2));
        const float oneMinus = sin2 / (1 + cosMax);  // 1 - cosMax without cancellation
        const float cosT = 1 - u1 * oneMinus, sinT = std::sqrt(std::max(0.0f, 1 - cosT * cosT)), phi = 2 * kPi * u2;
        const float3 axis = dc / dist;
        float3 t1, t2;
        orthonormal(axis, t1, t2);
        s.wi = normalize(axis * cosT + t1 * (sinT * std::cos(phi)) + t2 * (sinT * std::sin(phi)));
        // Distance to the sphere along wi (first intersection).
        const float b = dot(s.wi, dc), h = b * b - (dist2 - r * r);
        s.distance = b - std::sqrt(std::max(0.0f, h));
        s.pdf = 1.0f / (2 * kPi * oneMinus);
        s.L = colour * (l.intensity * w);
        s.delta = false;
        return true;
    }
    case scene::LightType::Tube:
    {
        const float len = l.size.x, r = l.size.y;
        const float3 a = l.position - l.right * (0.5f * len), bEnd = l.position + l.right * (0.5f * len);
        const float sideArea = 2 * kPi * r * len, capArea = 4 * kPi * r * r, area = sideArea + capArea;
        float3 p, n;
        float t1u = u1 * area;
        if (t1u < sideArea)
        {
            const float along = t1u / sideArea * len, phi = 2 * kPi * u2;
            float3 e1, e2;
            orthonormal(l.right, e1, e2);
            n = e1 * std::cos(phi) + e2 * std::sin(phi);
            p = a + l.right * along + n * r;
        }
        else
        {
            // Uniform direction on the sphere; the half facing away from the segment belongs to the nearer end cap.
            const float v = (t1u - sideArea) / capArea;
            const float z = 1 - 2 * v, sr = std::sqrt(std::max(0.0f, 1 - z * z)), phi = 2 * kPi * u2;
            float3 e1, e2;
            orthonormal(l.right, e1, e2);
            n = l.right * z + e1 * (sr * std::cos(phi)) + e2 * (sr * std::sin(phi));
            p = (z >= 0 ? bEnd : a) + n * r;
        }
        const float3 d = p - x;
        const float d2 = dot(d, d);
        s.distance = std::sqrt(d2);
        s.wi = d / s.distance;
        const float cosL = -dot(s.wi, n);
        if (cosL <= 0) return false;  // points facing away emit away from x (convex emitter)
        s.pdf = d2 / (area * cosL);
        s.L = colour * (l.intensity * w);
        s.delta = false;
        return true;
    }
    }
    return false;
}

bool LightSet::intersect(uint32_t i, float3 o, float3 d, float tmax, float& t, Rgb& L, float& pdf) const
{
    const scene::Light& l = m_lights[i];
    const float w = window(l, o);
    if (w <= 0) return false;
    switch (l.type)
    {
    case scene::LightType::Point:
    case scene::LightType::Spot: return false;
    case scene::LightType::Rect:
    case scene::LightType::Disk:
    {
        const float dn = dot(d, l.forward);
        if (dn >= 0) return false;  // approaching from behind: one-sided
        const float th = dot(l.position - o, l.forward) / dn;
        if (!(th > 0 && th < tmax)) return false;
        const float3 q = o + d * th - l.position;
        float area;
        if (l.type == scene::LightType::Rect)
        {
            if (std::fabs(dot(q, l.right)) > 0.5f * l.size.x || std::fabs(dot(q, m_up[i])) > 0.5f * l.size.y) return false;
            area = l.size.x * l.size.y;
        }
        else
        {
            if (dot(q, q) > l.size.x * l.size.x) return false;
            area = kPi * l.size.x * l.size.x;
        }
        t = th;
        pdf = th * th / (area * -dn);
        L = Rgb(l.color) * (l.intensity * w);
        return true;
    }
    case scene::LightType::Sphere:
    {
        const float3 dc = l.position - o;
        const float dist2 = dot(dc, dc), r = l.size.x;
        if (dist2 <= r * r) return false;
        const float b = dot(d, dc), h = b * b - (dist2 - r * r);
        if (h < 0) return false;
        const float th = b - std::sqrt(h);
        if (!(th > 0 && th < tmax)) return false;
        const float sin2 = r * r / dist2, cosMax = std::sqrt(std::max(0.0f, 1 - sin2));
        t = th;
        pdf = 1.0f / (2 * kPi * (sin2 / (1 + cosMax)));
        L = Rgb(l.color) * (l.intensity * w);
        return true;
    }
    case scene::LightType::Tube:
    {
        const float len = l.size.x, r = l.size.y;
        const float3 a = l.position - l.right * (0.5f * len), bEnd = l.position + l.right * (0.5f * len);
        const float th = capsuleIntersect(o, d, a, bEnd, r);
        if (!(th > 0 && th < tmax)) return false;
        const float3 p = o + d * th;
        const float3 ba = bEnd - a;
        const float s = std::clamp(dot(p - a, ba) / dot(ba, ba), 0.0f, 1.0f);
        const float3 n = normalize(p - (a + ba * s));
        const float cosL = -dot(d, n);
        if (cosL <= 0) return false;
        const float area = 2 * kPi * r * len + 4 * kPi * r * r;
        t = th;
        pdf = th * th / (area * cosL);
        L = Rgb(l.color) * (l.intensity * w);
        return true;
    }
    }
    return false;
}
} // namespace unx::reference
