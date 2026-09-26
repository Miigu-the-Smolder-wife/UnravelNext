#include "unx/scene/MaterialModel.h"

#include "unx/core/Log.h"

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

struct Tables
{
    std::vector<float> e, ab;  // E (n * n), (A, B) (2 * n * n)
};

Tables buildTables()
{
    const uint32_t n = kAlbedoTableSize, samples = 4096;
    Tables out;
    out.e.resize(n * n);
    out.ab.resize(2 * n * n);
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
            double sum = 0, a = 0, b = 0;
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
                const double weight = 4.0 * visibilitySmithGgxCorrelated(mu, NoL, alpha) * NoL * mu / g1v;
                const double w = std::pow(1.0 - std::clamp((double)VoH, 0.0, 1.0), 5.0);  // Schlick weight
                sum += weight;
                a += weight * (1 - w);
                b += weight * w;
            }
            out.e[ri * n + mi] = (float)(sum / samples);
            out.ab[2 * (ri * n + mi)] = (float)(a / samples);
            out.ab[2 * (ri * n + mi) + 1] = (float)(b / samples);
        }
    return out;
}

const Tables& tables()
{
    static const Tables t = buildTables();
    return t;
}

namespace eta150
{
#include "CoatTables150.inc"
}
namespace eta133
{
#include "CoatTables133.inc"
}

std::vector<float> buildCoatTable()
{
    std::vector<float> t(2 * kCoatTableStride, 0.0f);
    auto put = [&](uint32_t k, const float* ec, const float* ems, const float* ax, const float* bx, const float* kms, const float* ab, const float* bb,
                   const float* ksingle) {
        float* o = t.data() + k * kCoatTableStride;
        std::copy(ec, ec + 1024, o);
        std::copy(ems, ems + 1024, o + 1024);
        std::copy(ax, ax + 1024, o + 2048);
        std::copy(bx, bx + 1024, o + 3072);
        std::copy(kms, kms + 32, o + 4096);
        std::copy(ab, ab + 32, o + 4128);
        std::copy(bb, bb + 32, o + 4160);
        o[4192] = ksingle[0];  // K(r = 0): the smooth interface
    };
    put(0, eta150::kCoatReflectance, eta150::kCoatReflectanceMs, eta150::kBaseEscapeA, eta150::kBaseEscapeB, eta150::kCoatInnerDiffuseReflectanceMs, eta150::kBaseMeanA,
        eta150::kBaseMeanB, eta150::kCoatInnerDiffuseReflectance);
    put(1, eta133::kCoatReflectance, eta133::kCoatReflectanceMs, eta133::kBaseEscapeA, eta133::kBaseEscapeB, eta133::kCoatInnerDiffuseReflectanceMs, eta133::kBaseMeanA,
        eta133::kBaseMeanB, eta133::kCoatInnerDiffuseReflectance);
    return t;
}

// Bilinear on the 32 x 32 (mu, r) grid of the albedo tables; 1D on r.
float coatLookup2(const float* t, float mu, float r)
{
    const float last = (float)(kAlbedoTableSize - 1);
    const float x = std::clamp(mu, 0.0f, 1.0f) * last, y = std::clamp(r, 0.0f, 1.0f) * last;
    const uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y;
    const uint32_t x1 = std::min(x0 + 1, kAlbedoTableSize - 1), y1 = std::min(y0 + 1, kAlbedoTableSize - 1);
    const float fx = x - x0, fy = y - y0;
    auto at = [&](uint32_t xi, uint32_t yi) { return t[yi * kAlbedoTableSize + xi]; };
    return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
}
float coatLookup1(const float* t, float r)
{
    const float y = std::clamp(r, 0.0f, 1.0f) * (float)(kAlbedoTableSize - 1);
    const uint32_t y0 = (uint32_t)y, y1 = std::min(y0 + 1, kAlbedoTableSize - 1);
    return t[y0] + (y - y0) * (t[y1] - t[y0]);
}
} // namespace

const std::vector<float>& coatTable()
{
    static const std::vector<float> t = buildCoatTable();
    return t;
}

uint32_t coatIndex(float eta)
{
    for (uint32_t k = 0; k < 2; ++k)
        if (eta == kCoatEtas[k]) return k;
    fail("clearcoat: eta %g is not a tabulated coat", eta);
}

float fresnelDielectric(float cosI, float eta)
{
    const float c = saturate(cosI);
    const float s2 = (1 - c * c) / (eta * eta);
    if (s2 >= 1) return 1;
    const float ct = std::sqrt(1 - s2);
    const float rs = (c - eta * ct) / (c + eta * ct), rp = (eta * c - ct) / (eta * c + ct);
    return 0.5f * (rs * rs + rp * rp);
}

float evaluateCoatLobe(const Coat& c, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return 0;
    const float* t = coatTable().data() + coatIndex(c.eta) * kCoatTableStride;
    const float rc = c.roughness, ac = alphaFromRoughness(rc);
    const float ecv = coatLookup2(t, NoV, rc), ecl = coatLookup2(t, NoL, rc), emv = coatLookup2(t + 1024, NoV, rc), eml = coatLookup2(t + 1024, NoL, rc);
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), VoH = saturate(dot(v, h));
    const float3 nxh = cross(n, h);
    const float scale = ecv > 0 && ecl > 0 ? std::sqrt(emv * eml / (ecv * ecl)) : 1.0f;
    return distributionGgx(NoH, dot(nxh, nxh), ac) * visibilitySmithGgxCorrelated(NoV, NoL, ac) * fresnelDielectric(VoH, c.eta) * scale;
}

namespace
{
// Charlie D (normalised: its projected area on n is 1).
float sheenD(float NoH, float alpha)
{
    const float inv = 1 / alpha, sin2 = std::max(0.0f, 1 - NoH * NoH);
    return (2 + inv) * std::pow(sin2, 0.5f * inv) / (2 * kPi);
}

// Sheen table cell (column i at sqrt(mu) = i / (M - 1), row j at sqrt((r - 0.1) / 0.9) = j / (R - 1)): its mu and r.
float sheenMu(uint32_t i) { const float x = (float)i / (kSheenTableMu - 1); return std::max(x * x, 1e-4f); }
float sheenRow(uint32_t j) { const float y = (float)j / (kSheenTableR - 1); return 0.1f + 0.9f * y * y; }

std::vector<float> buildSheenTable()
{
    const uint32_t nm = kSheenTableMu, nr = kSheenTableR, cells = nm * nr;
    std::vector<float> t(2 * cells);
    // A(mu, r) = integral of D(m) max(0, w.m) dm: per cosine c of m the azimuth integral is closed,
    // 2 (a phi0 + b sin phi0) with a = mu c, b = |w x n| |m x n|, phi0 = acos(-a / b) (midpoint in c, 4096 steps).
    const uint32_t nc = 4096;
    for (uint32_t j = 0; j < nr; ++j)
        for (uint32_t i = 0; i < nm; ++i)
        {
            const double alpha = alphaFromRoughness(sheenRow(j));
            const double mu = sheenMu(i), sw = std::sqrt(1 - mu * mu);
            double sum = 0;
            for (uint32_t k = 0; k < nc; ++k)
            {
                const double c = (k + 0.5) / nc, sm = std::sqrt(1 - c * c);
                const double D = (2 + 1 / alpha) * std::pow(sm * sm, 0.5 / alpha) / (2 * 3.14159265358979);
                const double aa = mu * c, bb = sw * sm;
                double az;
                if (bb <= aa) az = 2 * 3.14159265358979 * aa;
                else
                {
                    const double phi0 = std::acos(std::clamp(-aa / bb, -1.0, 1.0));
                    az = 2 * (aa * phi0 + bb * std::sin(phi0));
                }
                sum += D * az;
            }
            t[j * nm + i] = (float)(sum / nc);
        }
    // E_sh(mu, r) = integral of f_sh cos over the hemisphere with C = 1, over the microfacet normal h (dl = 4 v.h dh:
    // E = integral of D(h) G2 v.h / n.v over v.h > 0, n.l > 0), where D is smooth in (cos theta_h, phi) (midpoint, the
    // lobe's mirror symmetry about the plane of v and n: phi in [0, pi) twice).
    const uint32_t nt = 1024, np = 256;
    for (uint32_t j = 0; j < nr; ++j)
        for (uint32_t i = 0; i < nm; ++i)
        {
            const float r = sheenRow(j), alpha = alphaFromRoughness(r), mu = sheenMu(i);
            const float3 v{ std::sqrt(1 - mu * mu), 0, mu };
            const float Av = t[j * nm + i];
            double sum = 0;
            for (uint32_t a = 0; a < nt; ++a)
            {
                const float c = (a + 0.5f) / nt, sn = std::sqrt(1 - c * c), D = sheenD(c, alpha);
                for (uint32_t b = 0; b < np; ++b)
                {
                    const float ph = kPi * (b + 0.5f) / np;
                    const float3 h{ sn * std::cos(ph), sn * std::sin(ph), c };
                    const float VoH = dot(v, h);
                    if (VoH <= 0) continue;
                    const float NoL = 2 * VoH * c - mu;
                    if (NoL <= 0) continue;
                    const float G2 = 1 / std::max(Av / mu + sheenLookup(t.data(), NoL, r) / NoL - 1, 1.0f);
                    sum += D * G2 * VoH / mu;
                }
            }
            t[cells + j * nm + i] = (float)(sum * 2 * kPi / nt / np);
        }
    return t;
}
} // namespace

float sheenLookup(const float* t, float mu, float r)
{
    const float x = std::sqrt(std::clamp(mu, 0.0f, 1.0f)) * (kSheenTableMu - 1);
    const float y = std::sqrt(std::clamp((r - 0.1f) / 0.9f, 0.0f, 1.0f)) * (kSheenTableR - 1);
    const uint32_t x0 = std::min((uint32_t)x, kSheenTableMu - 2), y0 = std::min((uint32_t)y, kSheenTableR - 2);
    const float fx = x - x0, fy = y - y0;
    auto at = [&](uint32_t xi, uint32_t yi) { return t[yi * kSheenTableMu + xi]; };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

const std::vector<float>& sheenTable()
{
    static const std::vector<float> t = buildSheenTable();
    return t;
}

float sheenProjectedArea(float mu, float roughness) { return sheenLookup(sheenTable().data(), mu, roughness); }

float sheenAlbedo(float NoV, float roughness) { return sheenLookup(sheenTable().data() + kSheenTableMu * kSheenTableR, NoV, roughness); }

float evaluateSheenLobe(float roughness, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return 0;
    const float G2 = 1 / std::max(sheenProjectedArea(NoV, roughness) / NoV + sheenProjectedArea(NoL, roughness) / NoL - 1, 1.0f);
    return sheenD(saturate(dot(n, normalize(v + l))), alphaFromRoughness(roughness)) * G2 / (4 * NoV * NoL);
}

float3 evaluateSheen(const Surface& s, const Sheen& sh, float3 n, float3 v, float3 l)
{
    const float3 base = evaluate(s, n, v, l);
    const float cmax = std::max(sh.color.x, std::max(sh.color.y, sh.color.z));
    if (!(cmax > 0)) return base;
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return base;
    return sh.color * evaluateSheenLobe(sh.roughness, n, v, l) + base * (1 - cmax * sheenAlbedo(NoV, sh.roughness));
}

float3 evaluateCoated(const Surface& s, const Coat& c, float3 n, float3 v, float3 l)
{
    const float3 base = evaluate(s, n, v, l);
    if (!(c.cover > 0)) return base;
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return base * (1 - c.cover);
    const float* t = coatTable().data() + coatIndex(c.eta) * kCoatTableStride;
    const float eta = c.eta, rc = c.roughness, ac = alphaFromRoughness(rc);
    // coat reflection (A2)
    const float ecv = coatLookup2(t, NoV, rc), ecl = coatLookup2(t, NoL, rc), emv = coatLookup2(t + 1024, NoV, rc), eml = coatLookup2(t + 1024, NoL, rc);
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), VoH = saturate(dot(v, h));
    const float3 nxh = cross(n, h);
    const float scale = ecv > 0 && ecl > 0 ? std::sqrt(emv * eml / (ecv * ecl)) : 1.0f;
    const float fc = distributionGgx(NoH, dot(nxh, nxh), ac) * visibilitySmithGgxCorrelated(NoV, NoL, ac) * fresnelDielectric(VoH, eta) * scale;
    const float tv = 1 - emv, tl = 1 - eml;
    // first pass (S): the base lobe between the refracted directions, widened by the coat's roughness
    auto refract = [&](float3 w, float mu, float& muIn) {
        muIn = std::sqrt(std::max(0.0f, 1 - (1 - mu * mu) / (eta * eta)));
        return (w - n * mu) * (1 / eta) + n * muIn;
    };
    float mv, ml;
    const float3 pv = refract(v, NoV, mv), pl = refract(l, NoL, ml);
    const float sv = 1 - NoV / (eta * mv), sl = 1 - NoL / (eta * ml);
    const float ab = alphaFromRoughness(s.roughness);
    Surface lobe = s;
    lobe.roughness = std::sqrt(std::sqrt(ab * ab + 0.25f * (sv * sv + sl * sl) * ac * ac));
    const float3 f1 = evaluate(lobe, n, pv, pl) * (tv * tl / (eta * eta));
    // light returned by the coat's inside
    const float3 F = f0(s), rd = s.baseColor * (1 - s.metallic);
    const float2 abl = specularAlbedo(ml, s.roughness);
    const float axl = coatLookup2(t + 2048, ml, s.roughness), bxl = coatLookup2(t + 3072, ml, s.roughness);
    const float3 comp = float3{ 1, 1, 1 } + F * (1 / directionalAlbedo(ml, s.roughness) - 1);
    const float3 returned = rd * t[4192] + (F * (abl.x - axl) + float3{ 1, 1, 1 } * (abl.y - bxl)) * comp;
    const float abar = coatLookup1(t + 4128, s.roughness), bbar = coatLookup1(t + 4160, s.roughness);
    const float3 rho = rd + (F * abar + float3{ 1, 1, 1 } * bbar) * (float3{ 1, 1, 1 } + F * (1 / (abar + bbar) - 1));
    const float kms = coatLookup1(t + 4096, rc);
    const float3 fms = float3{ returned.x * rho.x / (1 - rho.x * kms), returned.y * rho.y / (1 - rho.y * kms), returned.z * rho.z / (1 - rho.z * kms) } *
                       (tl * tv / (kPi * eta * eta));
    return base * (1 - c.cover) + (float3{ fc, fc, fc } + f1 + fms) * c.cover;
}

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

const std::vector<float>& directionalAlbedoTable() { return tables().e; }

const std::vector<float>& specularAlbedoTable() { return tables().ab; }

float2 specularAlbedo(float NoV, float roughness)
{
    const auto& t = specularAlbedoTable();
    const float last = (float)(kAlbedoTableSize - 1);
    const float x = std::clamp(NoV, 0.0f, 1.0f) * last, y = std::clamp(roughness, 0.0f, 1.0f) * last;
    const uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y;
    const uint32_t x1 = std::min(x0 + 1, kAlbedoTableSize - 1), y1 = std::min(y0 + 1, kAlbedoTableSize - 1);
    const float fx = x - x0, fy = y - y0;
    auto at = [&](uint32_t xi, uint32_t yi) { return float2{ t[2 * (yi * kAlbedoTableSize + xi)], t[2 * (yi * kAlbedoTableSize + xi) + 1] }; };
    const float2 a = at(x0, y0), b = at(x1, y0), c = at(x0, y1), d = at(x1, y1);
    return { (a.x * (1 - fx) + b.x * fx) * (1 - fy) + (c.x * (1 - fx) + d.x * fx) * fy, (a.y * (1 - fx) + b.y * fx) * (1 - fy) + (c.y * (1 - fx) + d.y * fx) * fy };
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
float3 hairAbsorption(const Material& m)
{
    if (m.hairEumelanin + m.hairPheomelanin > 0)
        return float3{ 0.419f, 0.697f, 1.37f } * m.hairEumelanin + float3{ 0.187f, 0.4f, 1.05f } * m.hairPheomelanin;
    const float b = m.hairBetaN;
    const float d = 5.969f - 0.215f * b + 2.532f * b * b - 10.73f * b * b * b + 5.574f * b * b * b * b + 0.245f * b * b * b * b * b;
    auto channel = [&](float c) {
        const float l = std::log(std::clamp(c, 1e-4f, 1.0f)) / d;
        return l * l;
    };
    return float3{ channel(m.baseColor.x), channel(m.baseColor.y), channel(m.baseColor.z) };
}
} // namespace unx::scene::model
