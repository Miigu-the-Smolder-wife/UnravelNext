#include "unx/scene/MaterialModel.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <iterator>
#include <thread>

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

// The table as generated by unx_test_scene_sheen --write-table (SheenTable.inc, kSheenTableSize floats printed with
// %.9e float literals: the exact floats of buildSheenTable); without the file (bootstrap) it is built here (about 25 s).
#if __has_include("SheenTable.inc")
namespace
{
const float kSheenTableData[] = {
#include "SheenTable.inc"
};
static_assert(sizeof(kSheenTableData) == sizeof(float) * kSheenTableSize);
} // namespace
const std::vector<float>& sheenTable()
{
    static const std::vector<float> t(std::begin(kSheenTableData), std::end(kSheenTableData));
    return t;
}
bool sheenTableGenerated() { return true; }
#else
const std::vector<float>& sheenTable()
{
    static const std::vector<float> t = buildSheenTable();
    return t;
}
bool sheenTableGenerated() { return false; }
#endif

std::vector<float> sheenTableReference() { return buildSheenTable(); }

float sheenProjectedArea(float mu, float roughness) { return sheenLookup(sheenTable().data(), mu, roughness); }

float sheenAlbedo(float NoV, float roughness) { return sheenLookup(sheenTable().data() + kSheenTableMu * kSheenTableR, NoV, roughness); }

float evaluateSheenLobe(float roughness, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return 0;
    const float G2 = 1 / std::max(sheenProjectedArea(NoV, roughness) / NoV + sheenProjectedArea(NoL, roughness) / NoL - 1, 1.0f);
    return sheenD(saturate(dot(n, normalize(v + l))), alphaFromRoughness(roughness)) * G2 / (4 * NoV * NoL);
}

float sheenSunRule(float roughness, float3 n, float3 v, float3 l0, float rho)
{
    const float3 du = normalize(cross(std::fabs(l0.z) < 0.9f ? float3{ 0, 0, 1 } : float3{ 1, 0, 0 }, l0)), dw = cross(l0, du);
    const float NoL0 = dot(n, l0), gu = dot(n, du), gw = dot(n, dw), k = std::sqrt(gu * gu + gw * gw);
    if (NoL0 <= -k * rho) return 0;
    float sum = 0;
    if (NoL0 >= k * rho)
    {
        const float q = rho * 0.70710678f;
        for (int i = 0; i < 4; ++i)
        {
            const float3 l = normalize(l0 + ((i & 2) ? dw : du) * ((i & 1) ? -q : q));
            sum += 0.25f * evaluateSheenLobe(roughness, n, v, l) * std::max(dot(n, l), 0.0f);
        }
        return sum;
    }
    const float eu = gu / std::max(k, 1e-8f), ew = gw / std::max(k, 1e-8f);
    const float phi0 = std::asin(std::clamp(-NoL0 / (k * rho), -1.0f, 1.0f)), half = 0.5f * (0.5f * kPi - phi0), mid = 0.5f * (0.5f * kPi + phi0);
    const float x4[4] = { -0.86113631f, -0.33998104f, 0.33998104f, 0.86113631f }, w4[4] = { 0.34785485f, 0.65214515f, 0.65214515f, 0.34785485f };
    const float x3[3] = { -0.77459667f, 0, 0.77459667f }, w3[3] = { 0.55555556f, 0.88888889f, 0.55555556f };
    for (int i = 0; i < 4; ++i)
    {
        const float phi = mid + half * x4[i], t = rho * std::sin(phi), w = rho * std::cos(phi);
        for (int j = 0; j < 3; ++j)
        {
            const float s = w * x3[j], ou = eu * t - ew * s, ow = ew * t + eu * s;
            const float3 l = normalize(l0 + du * ou + dw * ow);
            sum += w4[i] * w3[j] * evaluateSheenLobe(roughness, n, v, l) * std::max(dot(n, l), 0.0f) * w * w;
        }
    }
    return sum * half / (kPi * rho * rho);
}

float3 evaluateSheen(const Surface& s, const Sheen& sh, float3 n, float3 v, float3 l)
{
    const float3 base = evaluate(s, n, v, l);
    const float cmax = std::max(sh.color.x, std::max(sh.color.y, sh.color.z));
    if (!(cmax > 0)) return base;
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return base;
    // the cloth blend: the base's specular lobe x (1 - cloth) (f_d of a Standard surface is its whole albedo)
    const float3 specular = base - s.baseColor * ((1 - s.metallic) / kPi);
    return sh.color * evaluateSheenLobe(sh.roughness, n, v, l) + (base - specular * sh.cloth) * (1 - cmax * sheenAlbedo(NoV, sh.roughness));
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

// ---- Subsurface class (stage A: two specular lobes, light through thin parts)
Subsurface subsurfaceOf(const Material& m)
{
    Subsurface k;
    k.lobeMix = m.subsurfaceLobeMix;
    k.lobeRoughness = m.subsurfaceLobeRoughness;
    return k;
}

float3 subsurfaceRoughness(const Subsurface& k, float roughness)
{
    const float r0 = saturate(roughness * k.lobeRoughness.x), r1 = saturate(roughness * k.lobeRoughness.y);
    return { r0, r1, k.lobeMix * r0 + (1 - k.lobeMix) * r1 };
}

float subsurfaceThin(float c, float3 v, float3 l)
{
    const float x = saturate(-dot(l, v)), x2 = x * x, x4 = x2 * x2;
    const float inScatter = x4 * x4 * x4;  // S
    return c + (std::sqrt(c) - c) * inScatter;
}

float3 evaluateSubsurface(const Surface& s, const Subsurface& k, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    const float3 albedo = s.baseColor * ((1 - s.metallic) / kPi);
    if (s.transmission > 0 && NoV * NoL < 0)
    {
        const float c = std::fabs(NoL);
        return albedo * (s.transmission * subsurfaceThin(c, v, l) / c);  // light through a thin part
    }
    if (NoV <= 0 || NoL <= 0) return {};
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), VoH = saturate(dot(v, h));
    const float3 r = subsurfaceRoughness(k, s.roughness);
    const float3 f = f0(s);
    const float3 nxh = cross(n, h);
    const float sinSqNH = dot(nxh, nxh);
    const float d = k.lobeMix * distributionGgx(NoH, sinSqNH, alphaFromRoughness(r.x)) + (1 - k.lobeMix) * distributionGgx(NoH, sinSqNH, alphaFromRoughness(r.y));
    const float3 single = fresnelSchlick(f, VoH) * (d * visibilitySmithGgxCorrelated(NoV, NoL, alphaFromRoughness(r.z)));
    const float e = directionalAlbedo(NoV, r.z);
    const float3 compensation = float3{ 1, 1, 1 } + f * (1 / e - 1);
    return albedo + single * compensation;
}

// ---- Subsurface class (stage B: the diffusion profile)
float3 subsurfaceScaling(float3 albedo)
{
    auto s = [](float a) {
        a = saturate(a);
        return 1.9f - a + 3.5f * (a - 0.8f) * (a - 0.8f);
    };
    return { s(albedo.x), s(albedo.y), s(albedo.z) };
}

float3 subsurfaceDistance(float3 meanFreePath, float3 albedo)
{
    const float3 s = subsurfaceScaling(albedo);
    return { std::max(meanFreePath.x / s.x, 1e-6f), std::max(meanFreePath.y / s.y, 1e-6f), std::max(meanFreePath.z / s.z, 1e-6f) };
}

float subsurfaceProfile(float d, float r) { return subsurfaceRadialPdf(d, r) / (2 * kPi * r); }

float subsurfaceRadialPdf(float d, float r)
{
    const float y = std::exp(-r / (3 * d));
    return (y * y * y + y) / (4 * d);
}

float subsurfaceRadialCdf(float d, float r)
{
    const float y = std::exp(-r / (3 * d));
    return 1 - 0.25f * (y * y * y) - 0.75f * y;
}

float subsurfaceRadius(float d, float xi)
{
    const float u = std::max(1 - xi, 1e-6f);
    const float c = std::cbrt(2 * u + std::sqrt(1 + 4 * u * u)), c2 = c * c;
    const float y = 4 * u / (c2 + 1 + 1 / c2);
    return -3 * d * std::log(std::min(y, 1.0f));
}

float subsurfaceSampleRadius(float d, float centreCdf, uint32_t k, uint32_t pairs, float u)
{
    return subsurfaceRadius(d, centreCdf + (1 - centreCdf) * (((float)k + ((k & 1u) ? 1 - u : u)) / (float)pairs));
}

float subsurfaceSampleAngle(uint32_t k, float u)
{
    const float turns = u + (float)k * 0.61803398875f;
    return 6.28318530718f * (turns - std::floor(turns));
}

float3 subsurfaceSampleWeight(float3 d, float r, float h, float pdf)
{
    const float rr = std::sqrt(r * r + h * h);
    return { subsurfaceRadialPdf(d.x, rr) / pdf, subsurfaceRadialPdf(d.y, rr) / pdf, subsurfaceRadialPdf(d.z, rr) / pdf };
}

// ---- Anisotropy (A9, MATERIAL_LAYERS 1.5)
float2 anisoAlphas(float roughness, float strength)
{
    const float a = alphaFromRoughness(roughness), s = saturate(strength);
    return { a + (1 - a) * s * s, a };
}

float distributionGgxAniso(float ht, float hb, float hn, float alphaT, float alphaB)
{
    const float x = ht / alphaT, y = hb / alphaB, d = x * x + y * y + hn * hn;
    return 1 / (kPi * alphaT * alphaB * d * d);
}

float visibilitySmithGgxAniso(float3 v, float3 l, float alphaT, float alphaB)
{
    const float gv = l.z * std::sqrt(alphaT * alphaT * v.x * v.x + alphaB * alphaB * v.y * v.y + v.z * v.z);
    const float gl = v.z * std::sqrt(alphaT * alphaT * l.x * l.x + alphaB * alphaB * l.y * l.y + l.z * l.z);
    return 0.5f / (gv + gl);
}

namespace
{
float anisoTableMu(uint32_t i) { const float x = (float)i / (kAnisoTableMu - 1); return std::max(x * x, 1e-4f); }
float anisoTableAlpha(uint32_t j) { const float x = (float)j / (kAnisoTableR - 1); return std::max(x * x, kMinAlpha); }

// (A, B) of one view (n = +Z, v = (sin cos phi, sin sin phi, mu)), 4096 Hammersley visible-normal samples of the stretched
// lobe: weight = f n.l / pdf(l) with F = 1 = G2 / G1(v) (as buildTables, anisotropic).
void anisoPoint(float mu, float phi, float at, float ab, float& outA, float& outB)
{
    const uint32_t samples = 4096;
    const float st = std::sqrt(1 - mu * mu);
    const float3 v{ st * std::cos(phi), st * std::sin(phi), mu };
    const float3 vh = normalize(float3{ at * v.x, ab * v.y, v.z });
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0 ? float3{ -vh.y, vh.x, 0 } / std::sqrt(lensq) : float3{ 1, 0, 0 };
    const float3 t2 = cross(vh, t1);
    const float lv = std::sqrt(at * at * v.x * v.x + ab * ab * v.y * v.y + v.z * v.z);  // 2 mu (1 + Lambda(v)) / 2
    const double g1v = 2 * mu / (mu + lv);
    double a = 0, b = 0;
    for (uint32_t s = 0; s < samples; ++s)
    {
        const float u1 = (s + 0.5f) / samples, u2 = radicalInverse(s);
        const float radius = std::sqrt(u1), ph = 2 * kPi * u2;
        const float p1 = radius * std::cos(ph);
        const float sBlend = 0.5f * (1 + vh.z);
        const float p2 = (1 - sBlend) * std::sqrt(std::max(0.0f, 1 - p1 * p1)) + sBlend * radius * std::sin(ph);
        const float3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1 - p1 * p1 - p2 * p2));
        const float3 h = normalize(float3{ at * nh.x, ab * nh.y, std::max(0.0f, nh.z) });
        const float VoH = dot(v, h);
        const float3 l = h * (2 * VoH) - v;
        if (l.z <= 0) continue;
        const double weight = 4.0 * visibilitySmithGgxAniso(v, l, at, ab) * l.z * mu / g1v;
        const double w = std::pow(1.0 - std::clamp((double)VoH, 0.0, 1.0), 5.0);
        a += weight * (1 - w);
        b += weight * w;
    }
    outA = (float)(a / samples);
    outB = (float)(b / samples);
}

std::vector<float> buildAnisoTable()
{
    const uint32_t M = kAnisoTableMu, P = kAnisoTablePhi, R = kAnisoTableR;
    std::vector<float> t(kAnisoTableSize);
    auto row = [&](uint32_t jb) {
        for (uint32_t jt = 0; jt < R; ++jt)
            for (uint32_t k = 0; k < P; ++k)
                for (uint32_t i = 0; i < M; ++i)
                {
                    const size_t at = 2 * ((((size_t)jb * R + jt) * P + k) * M + i);
                    anisoPoint(anisoTableMu(i), 0.5f * kPi * k / (P - 1), anisoTableAlpha(jt), anisoTableAlpha(jb), t[at], t[at + 1]);
                }
    };
    std::vector<std::thread> threads;
    for (uint32_t jb = 0; jb < R; ++jb) threads.emplace_back(row, jb);  // each point depends on its own indices only
    for (std::thread& th : threads) th.join();
    return t;
}
} // namespace

const std::vector<float>& anisoAlbedoTable()
{
    static const std::vector<float> t = buildAnisoTable();
    return t;
}

float2 anisoSpecularAlbedo(float3 v, float alphaT, float alphaB)
{
    const std::vector<float>& t = anisoAlbedoTable();
    const uint32_t M = kAnisoTableMu, P = kAnisoTablePhi, R = kAnisoTableR;
    const float x = std::sqrt(saturate(v.z)) * (M - 1);
    const float y = std::atan2(std::fabs(v.y), std::fabs(v.x)) / (0.5f * kPi) * (P - 1);  // atan2(0, 0) = 0 at the pole
    const float zt = std::sqrt(saturate(alphaT)) * (R - 1), zb = std::sqrt(saturate(alphaB)) * (R - 1);
    const uint32_t x0 = std::min((uint32_t)x, M - 2), y0 = std::min((uint32_t)y, P - 2);
    const uint32_t t0 = std::min((uint32_t)zt, R - 2), b0 = std::min((uint32_t)zb, R - 2);
    const float fx = x - x0, fy = y - y0, ft = zt - t0, fb = zb - b0;
    float2 out{ 0, 0 };
    for (uint32_t c = 0; c < 16; ++c)
    {
        const uint32_t dx = c & 1, dy = (c >> 1) & 1, dt = (c >> 2) & 1, db = c >> 3;
        const float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dt ? ft : 1 - ft) * (db ? fb : 1 - fb);
        const size_t at = 2 * ((((size_t)(b0 + db) * R + (t0 + dt)) * P + (y0 + dy)) * M + (x0 + dx));
        out.x += w * t[at];
        out.y += w * t[at + 1];
    }
    return out;
}

bool anisoFrame(float3 T, float sign, float3 N, float theta, float3 n, float3& t, float3& b)
{
    const float nl = length(N);
    if (!(nl > 0)) return false;
    const float3 Nh = N / nl;
    float3 Tp = T - Nh * dot(Nh, T);
    const float tl = length(Tp);
    if (!(tl > 1e-12f)) return false;
    Tp = Tp / tl;
    const float3 Bp = cross(Nh, Tp) * (sign < 0 ? -1.0f : 1.0f);
    const float3 d = Tp * std::cos(theta) + Bp * std::sin(theta);
    const float3 dp = d - n * dot(n, d);
    const float dl = length(dp);
    if (!(dl > 1e-12f)) return false;
    t = dp / dl;
    b = cross(n, t);
    return true;
}

float3 evaluateAnisotropic(const Surface& s, const Anisotropy& a, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    const float3 albedo = s.baseColor * ((1 - s.metallic) / kPi);
    if (NoV <= 0 || NoL <= 0) return {};
    const float3 h = normalize(v + l);
    const float VoH = saturate(dot(v, h));
    const float2 al = anisoAlphas(s.roughness, a.strength);
    const float3 vl{ dot(v, a.t), dot(v, a.b), NoV }, ll{ dot(l, a.t), dot(l, a.b), NoL };
    const float3 f = f0(s);
    const float dv = distributionGgxAniso(dot(h, a.t), dot(h, a.b), std::max(dot(h, n), 0.0f), al.x, al.y) * visibilitySmithGgxAniso(vl, ll, al.x, al.y);
    const float2 ab = anisoSpecularAlbedo(vl, al.x, al.y);
    const float3 compensation = float3{ 1, 1, 1 } + f * (1 / (ab.x + ab.y) - 1);
    return albedo + fresnelSchlick(f, VoH) * dv * compensation;
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
// ---- Thin film (A9, MATERIAL_LAYERS 1.2, method (c); Reference/Studies/ThinFilm.cpp filmC is the study's form)
namespace
{
#include "ThinFilmTables.inc"
using cd = std::complex<double>;
constexpr double kPiD = 3.14159265358979323846;

cd sqrtUpper(cd z)
{
    const cd q = std::sqrt(z);
    return q.imag() < 0 ? -q : q;
}

// Interface amplitude coefficients (s, p) between media with normal components qa, qb and permittivities ea, eb.
void filmCoefficients(cd qa, cd qb, cd ea, cd eb, cd r[2])
{
    r[0] = (qa - qb) / (qa + qb);
    r[1] = (eb * qa - ea * qb) / (eb * qa + ea * qb);
}

// Airy reflectance of one polarisation averaged over wavenumbers [1/hi, 1/lo] (Poisson kernel): first = r01 (real),
// second = r12, path = the optical path difference 2 d q1 (nm).
double filmBandReflectance(double first, cd second, double path, double lo, double hi)
{
    const double r = std::norm(second), a = first * first, den = (1 - a) + a * (1 - r);
    if (den == 0) return 1;
    const double transfer = (1 - a) * (1 - r) / den, baseline = 1 - transfer;
    const cd z = second * first;
    const double q = std::abs(z);
    if (q == 0) return baseline;
    const double phase = std::arg(z), width = 2 * kPiD * path * (1 / lo - 1 / hi), mid = kPiD * path * (1 / lo + 1 / hi) + phase;
    if (width == 0)
    {
        const double inter = 2 * q * std::cos(mid);
        return (a + r + inter) / (1 + a * r + inter);
    }
    const double sh = std::sin(width * 0.5), ch = std::cos(width * 0.5);
    const double num = 2 * q * sh * (std::cos(mid) + q * ch), div = 1 + 2 * q * std::cos(mid) * ch + q * q * std::cos(width);
    return baseline + 2 * transfer * std::atan2(num, div) / width;
}

cd filmSubstrateAt(const Film& f, uint32_t bin)
{
    if (f.substrate == FilmSubstrate::Constant || f.substrate >= FilmSubstrate::Count) return cd(f.substrateIor, f.substrateExtinction);
    const float* nk = kFilmPresetNk[(uint32_t)f.substrate - 1][bin];
    return cd(nk[0], nk[1]);
}

// One bin's polarisation-averaged reflectance.
double filmBin(const Film& f, double n0, double cos0, uint32_t bin)
{
    const double lo = kFilmBinEdges[bin], hi = kFilmBinEdges[bin + 1];
    const double s2 = n0 * n0 * (1 - cos0 * cos0), nf = f.ior;
    const cd sub = filmSubstrateAt(f, bin), e2 = sub * sub, q2 = sqrtUpper(e2 - s2);
    const cd q0 = n0 * cos0;
    if (f.thickness <= 0)
    {
        cd r[2];
        filmCoefficients(q0, q2, n0 * n0, e2, r);
        return 0.5 * (std::norm(r[0]) + std::norm(r[1]));
    }
    const cd q1 = sqrtUpper(cd(nf * nf - s2, 0));
    cd r01[2], r12[2];
    filmCoefficients(q0, q1, n0 * n0, nf * nf, r01);
    filmCoefficients(q1, q2, nf * nf, e2, r12);
    if (nf * nf - s2 <= 0)
    {
        // evanescent film: no interference; exact Airy at the bin centre (complex phase = decay)
        const double lambda = 0.5 * (lo + hi);
        const cd ph = std::exp(cd(0, 1) * (4 * kPiD * f.thickness / lambda) * q1);
        double R = 0;
        for (int p = 0; p < 2; ++p) R += 0.5 * std::norm((r01[p] + r12[p] * ph) / (1.0 + r01[p] * r12[p] * ph));
        return R;
    }
    const double path = 2 * f.thickness * q1.real();
    return 0.5 * (filmBandReflectance(r01[0].real(), r12[0], path, lo, hi) + filmBandReflectance(r01[1].real(), r12[1], path, lo, hi));
}
} // namespace

Film filmOf(const Material& m)
{
    Film f;
    f.thickness = m.thinFilmThickness;
    f.ior = m.thinFilmIor;
    f.coverage = m.thinFilmCoverage;
    f.substrate = (FilmSubstrate)m.thinFilmSubstrate;
    f.substrateIor = m.substrateIor;
    f.substrateExtinction = m.substrateExtinction;
    return f;
}

float3 filmReflectance(const Film& f, float outerEta, float cosOuter)
{
    const double c = std::clamp((double)cosOuter, 0.0, 1.0);
    double rgb[3] = { 0, 0, 0 };
    for (uint32_t b = 0; b < kFilmTableBins; ++b)
    {
        const double R = filmBin(f, outerEta, c, b);
        for (int k = 0; k < 3; ++k) rgb[k] += R * kFilmBinWeights[b][k];
    }
    return float3{ (float)std::clamp(rgb[0], 0.0, 1.0), (float)std::clamp(rgb[1], 0.0, 1.0), (float)std::clamp(rgb[2], 0.0, 1.0) };
}

std::vector<float> filmTable(const Film& f, float outerEta)
{
    std::vector<float> t(3 * kFilmTableMu);
    for (uint32_t i = 0; i < kFilmTableMu; ++i)
    {
        const float3 r = filmReflectance(f, outerEta, (float)i / (kFilmTableMu - 1));
        t[3 * i] = r.x, t[3 * i + 1] = r.y, t[3 * i + 2] = r.z;
    }
    return t;
}

float3 filmFresnel(const Film& f, float3 f0, float VoH, float outerEta)
{
    const float3 schlick = fresnelSchlick(f0, VoH);
    if (!(f.coverage > 0)) return schlick;
    return lerp3(schlick, filmReflectance(f, outerEta, VoH), f.coverage);
}

float3 evaluateFilm(const Surface& s, const Film& film, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return {};
    const float3 albedo = s.baseColor * ((1 - s.metallic) / kPi);
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), VoH = saturate(dot(v, h));
    const float alpha = alphaFromRoughness(s.roughness);
    const float3 f = f0(s), fn = filmFresnel(film, f, 1);
    const float3 nxh = cross(n, h);
    const float3 single = filmFresnel(film, f, VoH) * (distributionGgx(NoH, dot(nxh, nxh), alpha) * visibilitySmithGgxCorrelated(NoV, NoL, alpha));
    const float e = directionalAlbedo(NoV, s.roughness);
    return albedo + single * (float3{ 1, 1, 1 } + fn * (1 / e - 1));
}
} // namespace unx::scene::model
