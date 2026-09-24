#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>

namespace unx::scene::model
{
namespace
{
float saturate(float x) { return std::clamp(x, 0.0f, 1.0f); }
float3 lerp3(float3 a, float3 b, float t) { return a + (b - a) * t; }

float radicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

std::vector<float> buildTable()
{
    const uint32_t n = kAlbedoTableSize, samples = 4096;
    std::vector<float> table(n * n);
    for (uint32_t ri = 0; ri < n; ++ri)
        for (uint32_t mi = 0; mi < n; ++mi)
        {
            const float r = (float)ri / (n - 1);
            const float mu = std::max((float)mi / (n - 1), 1e-4f);
            const float alpha = alphaFromRoughness(r);
            const float3 v{ std::sqrt(1 - mu * mu), 0, mu };  // n = +Z
            // Visible-normal sampling (Heitz 2018): every sample weighs G2(v,l) / G1(v) <= 1, so the estimate is
            // bounded by 1 and has low variance even at grazing angles with sharp lobes.
            const float3 vh = normalize(float3{ alpha * v.x, alpha * v.y, v.z });
            const float lensq = vh.x * vh.x + vh.y * vh.y;
            const float3 t1 = lensq > 0 ? float3{ -vh.y, vh.x, 0 } / std::sqrt(lensq) : float3{ 1, 0, 0 };
            const float3 t2 = cross(vh, t1);
            const float g1v = 2 * mu / (mu + std::sqrt(alpha * alpha + (1 - alpha * alpha) * mu * mu));
            double sum = 0;
            for (uint32_t s = 0; s < samples; ++s)
            {
                const float u1 = (s + 0.5f) / samples, u2 = radicalInverse(s);
                const float radius = std::sqrt(u1), phi = 2 * kPi * u2;
                const float p1 = radius * std::cos(phi);
                const float sBlend = 0.5f * (1 + vh.z);
                const float p2 = (1 - sBlend) * std::sqrt(std::max(0.0f, 1 - p1 * p1)) + sBlend * radius * std::sin(phi);
                const float3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1 - p1 * p1 - p2 * p2));
                const float3 h = normalize(float3{ alpha * nh.x, alpha * nh.y, std::max(0.0f, nh.z) });
                const float VoH = dot(v, h);
                const float3 l = h * (2 * VoH) - v;
                const float NoL = l.z;
                if (NoL <= 0) continue;
                // f * NoL / pdf(l) with F = 1 and pdf(l) = G1(v) VoH D / (NoV 4 VoH)  ->  4 V NoL NoV / G1(v)
                sum += 4.0 * visibilitySmithGgxCorrelated(mu, NoL, alpha) * NoL * mu / g1v;
            }
            table[ri * n + mi] = (float)(sum / samples);
        }
    return table;
}
} // namespace

float alphaFromRoughness(float roughness) { return std::max(roughness * roughness, kMinAlpha); }

float3 f0(const Surface& s)
{
    const float d = 0.08f * s.specular;
    return lerp3({ d, d, d }, s.baseColor, s.metallic);
}

float distributionGgx(float NoH, float sinSqNH, float alpha)
{
    const float a2 = alpha * alpha;
    const float t = sinSqNH + a2 * NoH * NoH;  // = NoH^2 (a2 - 1) + 1 without cancellation (C request, INTERFACES 8.1)
    return a2 / (kPi * t * t);
}

float visibilitySmithGgxCorrelated(float NoV, float NoL, float alpha)
{
    const float a2 = alpha * alpha;
    const float gv = NoL * std::sqrt(NoV * NoV * (1 - a2) + a2);
    const float gl = NoV * std::sqrt(NoL * NoL * (1 - a2) + a2);
    return 0.5f / (gv + gl);
}

float3 fresnelSchlick(float3 f, float VoH)
{
    const float w = std::pow(1 - saturate(VoH), 5.0f);
    return f + (float3{ 1, 1, 1 } - f) * w;
}

const std::vector<float>& directionalAlbedoTable()
{
    static const std::vector<float> table = buildTable();
    return table;
}

float directionalAlbedo(float NoV, float roughness)
{
    const auto& t = directionalAlbedoTable();
    const float last = (float)(kAlbedoTableSize - 1);
    const float x = std::clamp(NoV, 0.0f, 1.0f) * last, y = std::clamp(roughness, 0.0f, 1.0f) * last;
    const uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y;
    const uint32_t x1 = std::min(x0 + 1, kAlbedoTableSize - 1), y1 = std::min(y0 + 1, kAlbedoTableSize - 1);
    const float fx = x - x0, fy = y - y0;
    auto at = [&](uint32_t xi, uint32_t yi) { return t[yi * kAlbedoTableSize + xi]; };
    return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
}

float3 evaluate(const Surface& s, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    const float3 albedo = s.baseColor * ((1 - s.metallic) / kPi);
    if (s.cls == MaterialClass::Foliage && NoV * NoL < 0)
        return albedo * s.transmission;  // thin-leaf diffuse transmission to the other side
    if (NoV <= 0 || NoL <= 0) return {};
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), VoH = saturate(dot(v, h));
    const float alpha = alphaFromRoughness(s.roughness);
    const float3 f = f0(s);
    const float3 nxh = cross(n, h);
    const float3 single = fresnelSchlick(f, VoH) * (distributionGgx(NoH, dot(nxh, nxh), alpha) * visibilitySmithGgxCorrelated(NoV, NoL, alpha));
    const float e = directionalAlbedo(NoV, s.roughness);
    const float3 compensation = float3{ 1, 1, 1 } + f * (1 / e - 1);
    const float3 diffuse = s.cls == MaterialClass::Foliage ? albedo * (1 - s.transmission) : albedo;
    return diffuse + single * compensation;
}
} // namespace unx::scene::model
