// Clearcoat R1 (Docs/Design/MATERIAL_LAYERS_KO.md 1.1, cf84dd5) against the physical layer model, with the criteria of
// section 3: directional albedo relative error <= 2 % (or absolute <= 0.005), angular L1 <= 0.05, sphere renders
// (white furnace, sun + sky) dE76 mean <= 1.0 and P99 <= 2.3.
//
// Physical model (position-free random walk, RGB): rough dielectric top interface (GGX alpha_c, exact Fresnel n = 1.5,
// refraction, total internal reflection, single-scattering microfacets with height-correlated masking for reflection
// and separable masking for transmission), base BRDF evaluated with the in-medium directions (v1 model; with a film,
// its specular Fresnel is the exact spectral film reflectance), zero thickness, no absorption.
// Definitions evaluated exactly as written, with every table term replaced by its exact integral (E_c = R_c, K, a, e,
// rho-bar computed per configuration), so the measured difference is the formula's, not a table's.
// Both BSDFs are tabulated identically: incidence theta_i at 1 deg bin centres, exit (theta_o 1 deg) x (|dphi| 2 deg),
// energy per bin. Albedo and L1 (on 5 deg x 10 deg aggregates) come from the tables; renders look the tables up.
#include "Studies.h"
#include "ThinFilm.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/scene/MaterialModel.h"

#include "Bsdf.h"
#include "Sampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace unx::study
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kEta = 1.5;
constexpr int NI = 90, NT = 90, NP = 90;   // theta_i, theta_o (1 deg), |dphi| (2 deg)
constexpr int CT = 18, CP = 18;            // L1 aggregates: 5 deg x 10 deg
using reference::Rgb;

// ---------------------------------------------------------------------------------------------- microfacet helpers
double lambdaGgx(double mu, double a)
{
    if (mu >= 1) return 0;
    const double t2 = (1 - mu * mu) / (mu * mu);
    return 0.5 * (-1 + std::sqrt(1 + a * a * t2));
}
double g1(double mu, double a) { return 1 / (1 + lambdaGgx(mu, a)); }
double g2(double mi, double mo, double a) { return 1 / (1 + lambdaGgx(mi, a) + lambdaGgx(mo, a)); }
double ggxD(float3 n, float3 h, double a)  // cancellation-free
{
    const double noh = dot(n, h);
    if (noh <= 0) return 0;
    const float3 c = cross(n, h);
    const double sin2 = (double)c.x * c.x + (double)c.y * c.y + (double)c.z * c.z;
    const double t = sin2 + a * a * noh * noh;
    return a * a / (kPi * t * t);
}
float3 vndf(float3 w, double alpha, double u1, double u2)
{
    const float a = (float)alpha;
    const float3 vh = normalize(float3{ a * w.x, a * w.y, w.z });
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0 ? float3{ -vh.y, vh.x, 0 } / std::sqrt(lensq) : float3{ 1, 0, 0 };
    const float3 t2 = cross(vh, t1);
    const float r = (float)std::sqrt(u1), phi = (float)(2 * kPi * u2);
    const float p1 = r * std::cos(phi), s = 0.5f * (1 + vh.z);
    const float p2 = (1 - s) * std::sqrt(std::max(0.0f, 1 - p1 * p1)) + s * r * std::sin(phi);
    const float3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1 - p1 * p1 - p2 * p2));
    return normalize(float3{ a * nh.x, a * nh.y, std::max(0.0f, nh.z) });
}
double fresnelExact(double c, double eta)  // eta = n_t / n_i
{
    const double s2 = (1 - c * c) / (eta * eta);
    if (s2 >= 1) return 1;
    const double ct = std::sqrt(1 - s2);
    const double rs = (c - eta * ct) / (c + eta * ct), rp = (eta * c - ct) / (eta * c + ct);
    return 0.5 * (rs * rs + rp * rp);
}
float3 cosineDir(double u1, double u2)
{
    const double r = std::sqrt(u1), p = 2 * kPi * u2;
    return { (float)(r * std::cos(p)), (float)(r * std::sin(p)), (float)std::sqrt(std::max(0.0, 1 - u1)) };
}
// Smooth refraction about the macro normal (+z): outside direction (z > 0) <-> inside direction (z > 0), same azimuth.
float3 refractIn(float3 w)
{
    const double s = std::sqrt(std::max(0.0, 1.0 - (double)w.z * w.z)) / kEta, c = std::sqrt(std::max(0.0, 1 - s * s));
    const double h = std::sqrt((double)w.x * w.x + (double)w.y * w.y);
    return h > 0 ? float3{ (float)(w.x / h * s), (float)(w.y / h * s), (float)c } : float3{ 0, 0, 1 };
}
bool refractOut(float3 w, float3& out)
{
    const double s = std::sqrt(std::max(0.0, 1.0 - (double)w.z * w.z)) * kEta;
    if (s >= 1) return false;
    const double h = std::sqrt((double)w.x * w.x + (double)w.y * w.y);
    out = h > 0 ? float3{ (float)(w.x / h * s), (float)(w.y / h * s), (float)std::sqrt(1 - s * s) } : float3{ 0, 0, 1 };
    return true;
}

// Rough dielectric interface with microsurface multiple scattering (Heitz, Hanika, d'Eon, Dachsbacher 2016: Smith GGX,
// uniform height distribution, stochastic walk over reflections and refractions on both sides). The medium (index
// kEta) is below z = 0. wr is the propagation direction on arrival (from outside: wr.z < 0; from inside: wr.z > 0).
// Returns with wr the propagation direction on leaving and outside the side (true: into air, wr.z > 0). Energy
// conserving (no weights). False for a walk that did not leave within the order cap (not counted).
namespace ms
{
double c1(double h) { return std::clamp(0.5 * (h + 1), 0.0, 1.0); }
double invC1(double u) { return std::clamp(2 * u - 1, -1.0, 1.0); }
double lambda(float3 w, double a)  // signed: -1 - Lambda(-w) for w.z < 0
{
    if (w.z > 0.9999f) return 0;
    if (w.z < -0.9999f) return -1;
    const double s = std::sqrt(std::max(1e-30, 1.0 - (double)w.z * w.z)), x = w.z / (s * a);
    return 0.5 * (-1 + (x > 0 ? 1 : -1) * std::sqrt(1 + 1 / (x * x)));
}
double sampleHeight(float3 wr, double hr, double u, double a)
{
    if (wr.z > 0.9999f) return HUGE_VAL;
    if (wr.z < -0.9999f) return invC1(u * c1(hr));
    if (std::fabs(wr.z) < 0.0001f) return hr;
    const double L = lambda(wr, a);
    const double G1 = wr.z <= 0 ? 0.0 : std::pow(c1(hr), L);
    if (u > 1 - G1) return HUGE_VAL;
    return invC1(c1(hr) / std::pow(1 - u, 1 / L));
}
// Visible normal for a viewer direction wi on either side of the horizon (Dupuy & Benyoub 2023 spherical caps).
float3 vndfAny(float3 wi, double a, double u1, double u2)
{
    const float3 v = normalize(float3{ (float)(a * wi.x), (float)(a * wi.y), wi.z });
    const double phi = 2 * kPi * u1, z = (1 - u2) * (1 + v.z) - v.z, st = std::sqrt(std::clamp(1 - z * z, 0.0, 1.0));
    const float3 h{ (float)(st * std::cos(phi) + v.x), (float)(st * std::sin(phi) + v.y), (float)(z + v.z) };
    return normalize(float3{ (float)(a * h.x), (float)(a * h.y), std::max(h.z, 0.0f) });
}
// One phase-function step: wi points away from the microsurface on the side given by wiOutside.
float3 phase(float3 wi, bool wiOutside, bool& woOutside, double a, reference::Pcg32& rng)
{
    const double eta = wiOutside ? kEta : 1 / kEta;
    const double u1 = rng.uniform(), u2 = rng.uniform();
    const float3 m = wiOutside ? vndfAny(wi, a, u1, u2) : -vndfAny(-wi, a, u1, u2);
    const double c = std::max(0.0, (double)dot(wi, m));
    woOutside = wiOutside;
    if (rng.uniform() < fresnelExact(c, eta)) return m * (float)(2 * c) - wi;
    woOutside = !wiOutside;
    const double ct = -std::sqrt(std::max(0.0, 1 - (1 - c * c) / (eta * eta)));
    return normalize(m * (float)(c / eta + ct) - wi / (float)eta);
}
bool interact(float3& wr, bool& outside, double a, reference::Pcg32& rng)
{
    double hr = outside ? 1 + invC1(0.999) : -1 - invC1(0.999);
    for (int order = 0; order < 4096; ++order)
    {
        const double u = rng.uniform();
        hr = outside ? sampleHeight(wr, hr, u, a) : -sampleHeight(-wr, -hr, u, a);
        if (std::isinf(hr)) return (wr.z > 0) == outside;
        const bool wiOutside = outside;
        wr = phase(-wr, wiOutside, outside, a, rng);
        if (!(wr.z == wr.z)) return false;
    }
    return false;
}
} // namespace ms

// ---------------------------------------------------------------------------------------------- configuration
struct FilmTable  // RGB Fresnel of the base specular as a function of v.h (1024 samples in cos)
{
    std::vector<Rgb3> F;
    Rgb3 at(double c) const
    {
        const double x = std::clamp(c, 0.0, 1.0) * (F.size() - 1);
        const size_t i = std::min((size_t)x, F.size() - 2);
        const double t = x - i;
        return { F[i][0] + t * (F[i + 1][0] - F[i][0]), F[i][1] + t * (F[i + 1][1] - F[i][1]), F[i][2] + t * (F[i + 1][2] - F[i][2]) };
    }
};

struct Config
{
    std::string name;
    scene::model::Surface base;
    double rc = 0.12;
    const FilmTable* filmPhys = nullptr;  // exact spectral film (physical model)
    const FilmTable* filmDef = nullptr;   // method (a) film (definition)
    bool msCoat = false;                  // physical coat with microsurface multiple scattering (ms::interact)
};

// Base BRDF value f(v, l) in the base's frame (n = +z). Without a film: the v1 model; with a film: v1 with the
// specular Fresnel replaced by the film's RGB reflectance at v.h and F'(1) in the multiple-scattering compensation.
Rgb baseBrdf(const scene::model::Surface& s, const FilmTable* film, float3 v, float3 l)
{
    if (!film) return reference::evaluateModel(s, { 0, 0, 1 }, v, l);
    if (v.z <= 0 || l.z <= 0) return {};
    const float3 h = normalize(v + l);
    const double a = scene::model::alphaFromRoughness(s.roughness);
    const double dv = ggxD({ 0, 0, 1 }, h, a) * scene::model::visibilitySmithGgxCorrelated(v.z, l.z, (float)a);
    const Rgb3 F = film->at(dot(v, h)), F0 = film->at(1.0);
    const double e = scene::model::directionalAlbedo(v.z, s.roughness);
    const double kd = (1 - s.metallic) / kPi;
    const double base[3] = { s.baseColor.x, s.baseColor.y, s.baseColor.z };
    Rgb out;
    float* o = &out.r;
    for (int c = 0; c < 3; ++c) o[c] = (float)(base[c] * kd + F[c] * dv * (1 + F0[c] * (1 / e - 1)));
    return out;
}

// Per-configuration integrals the definition needs (exact, no tables).
struct Integrals
{
    std::vector<double> Rc;              // coat directional reflectance (definition f_c) on mu = (i + 0.5) / 1024
    double K = 0, Kout = 0;              // inner diffuse reflectance / transmittance of the rough coat
    std::vector<Rgb> a, e;               // base albedo and first-escape part on mu' = (i + 0.5) / 256
    Rgb rhoBar;                          // cosine-weighted hemisphere mean of a
    std::vector<double> RcOld;           // original definition: Schlick coat reflectance R_c
    // Candidate A (energy-conserving coat): directional reflectance of the multiple-scattering coat on the Rc grid, its
    // excess over f_c (Delta = RcMs - Rc) and cosine-weighted mean Delta-bar, inner diffuse reflectance.
    std::vector<double> RcMs;
    double DeltaBar = 0, KMs = 0;
    double at(const std::vector<double>& t, double mu) const
    {
        const double x = std::clamp(mu * t.size() - 0.5, 0.0, (double)t.size() - 1.0001);
        const size_t i = (size_t)x;
        return t[i] + (x - i) * (t[std::min(i + 1, t.size() - 1)] - t[i]);
    }
    Rgb atRgb(const std::vector<Rgb>& t, double mu) const
    {
        const double x = std::clamp(mu * t.size() - 0.5, 0.0, (double)t.size() - 1.0001);
        const size_t i = (size_t)x;
        const Rgb& p = t[i];
        const Rgb& q = t[std::min(i + 1, t.size() - 1)];
        return p + (q - p) * (float)(x - i);
    }
};

// ---------------------------------------------------------------------------------------------- definitions
float3 incident(int i)
{
    const double th = (i + 0.5) * (kPi / 2) / NI;
    return { (float)std::sin(th), 0, (float)std::cos(th) };
}
constexpr int kSplit = 16;
// Candidate B inputs per configuration (exact integrals, per incidence bin centre theta = (i + 0.5) deg): the lossless
// base-path census S_k (physical walk, base colour 1, same roughness, MS coat) and the energy of R1's f_1 (with
// candidate A's transmission), per channel.
struct SpecPath
{
    std::vector<std::array<double, kSplit>> S;
    std::vector<Rgb> f1E;
    std::vector<Rgb> g;  // B2: symmetric per-angle scale (Sinkhorn), f_path = f_1 g(mu_i) g(mu_o); empty = B (sqrt Gamma)
    // B3: g matches only the single-hit energy S_1 rho; the >= 2-hit energy E2 = sum_{k>=2} S_k rho^k is a separable
    // term E2(mu_i) E2(mu_o) / (pi E2-bar) (exact per incidence, reciprocal; by reciprocity E2 is also its exit profile).
    std::vector<Rgb> E2;
    Rgb E2bar;
};

struct Definitions
{
    const Config& cfg;
    Integrals in;
    const SpecPath* sp = nullptr;
    int coatForm = 0;  // candidate A's multiple-scattering coat term: 0 = Kulla-Conty additive (A), 1 = scaled f_c (A2)
    // Candidate S: angle-dependent refraction spread of the base lobe through the rough coat,
    // alpha'^2 = alpha_b^2 + (s(mu_i)^2 + s(mu_o)^2) alpha_c^2 / 4, s(mu) = 1 - cos(theta) / (eta cos(theta')) (the
    // sensitivity of the refracted angle to the microfacet tilt; at normal incidence this is R1's formula).
    bool spread = false;
    scene::model::Surface baseR1;  // base with alpha'_b
    double ac, ab;
    explicit Definitions(const Config& c) : cfg(c)
    {
        ac = scene::model::alphaFromRoughness((float)c.rc);
        ab = scene::model::alphaFromRoughness(c.base.roughness);
        const double apb2 = ab * ab + 0.5 * (1 - 1 / kEta) * (1 - 1 / kEta) * ac * ac;
        baseR1 = c.base;
        baseR1.roughness = (float)std::sqrt(std::sqrt(apb2));  // alpha = r^2
    }
    // R1: f = f_c + f_1 + f_ms (c = 1). parts (optional): f_c, f_1, f_ms.
    // candA (C-track candidate A, energy-conserving coat): f_c gains the coat's multiple-scattering reflection
    // Delta(mu_i) Delta(mu_o) / (pi Delta-bar) (Kulla-Conty form, Delta = E_ms - E_c), transmission uses 1 - E_ms, and
    // the inner diffuse reflectance is the multiple-scattering K_ms.
    // candB (C-track candidate B, metallic bases; includes A): the base path f_1 + f_ms is replaced by R1's f_1 lobe
    // scaled by sqrt(Gamma(mu_i) Gamma(mu_o)) per channel, Gamma = sum_k S_k rho^k / E_f1 (exact base-path energy over
    // the f_1 lobe's own energy), rho = base albedo a(mu').
    double gammaAt(double mu, int ch) const
    {
        const double th = std::acos(std::clamp(mu, 0.0, 1.0)) * 180 / kPi - 0.5;
        const int i0 = std::clamp((int)std::floor(th), 0, NI - 1), i1 = std::min(i0 + 1, NI - 1);
        const double t = std::clamp(th - i0, 0.0, 1.0);
        auto g = [&](int i) {
            const double muIn = refractIn(incident(i)).z;
            const Rgb a = in.atRgb(in.a, muIn);
            const double rho = (&a.r)[ch];
            double e = 0, rk = 1;
            for (int k = 1; k < kSplit; ++k)
            {
                rk *= rho;
                e += sp->S[i][k] * rk;
            }
            const double f1 = (&sp->f1E[i].r)[ch];
            return f1 > 1e-6 ? e / f1 : 1.0;
        };
        return g(i0) + t * (g(i1) - g(i0));
    }
    static double binLerp(const std::vector<Rgb>& v, double mu, int ch)  // linear in theta between incidence bin centres
    {
        const double th = std::acos(std::clamp(mu, 0.0, 1.0)) * 180 / kPi - 0.5;
        const int i0 = std::clamp((int)std::floor(th), 0, NI - 1), i1 = std::min(i0 + 1, NI - 1);
        const double t = std::clamp(th - i0, 0.0, 1.0);
        const double a = (&v[i0].r)[ch], b = (&v[i1].r)[ch];
        return a + t * (b - a);
    }
    double gAt(double mu, int ch) const { return binLerp(sp->g, mu, ch); }
    Rgb r1(float3 wo, float3 wi, Rgb* parts = nullptr, bool candA = false, bool candB = false) const
    {
        if (wo.z <= 0 || wi.z <= 0) return {};
        const float3 h = normalize(wo + wi);
        double fc = ggxD({ 0, 0, 1 }, h, ac) * g2(wi.z, wo.z, ac) / (4.0 * wi.z * wo.z) * fresnelExact(dot(wo, h), kEta);
        double Ti = 1 - in.at(in.Rc, wi.z), To = 1 - in.at(in.Rc, wo.z), K = in.K;
        if (candA)
        {
            const double Ei = in.at(in.RcMs, wi.z), Eo = in.at(in.RcMs, wo.z);
            const double Ci = in.at(in.Rc, wi.z), Co = in.at(in.Rc, wo.z);
            if (coatForm == 1)
            {
                // A2: the single-scattering lobe scaled reciprocally to the multiple-scattering energy (the coat's
                // multiply scattered reflection stays near the specular direction at grazing).
                if (Ci > 0 && Co > 0) fc *= std::sqrt(Ei * Eo / (Ci * Co));
            }
            else
            {
                const double di = std::max(0.0, Ei - Ci), dout = std::max(0.0, Eo - Co);
                if (in.DeltaBar > 0) fc += di * dout / (kPi * in.DeltaBar);
            }
            Ti = 1 - Ei;
            To = 1 - Eo;
            K = in.KMs;
        }
        const float3 pi = refractIn(wi), po = refractIn(wo);
        scene::model::Surface lobe = baseR1;
        if (spread)
        {
            const double si = 1 - wi.z / (kEta * pi.z), so = 1 - wo.z / (kEta * po.z);
            const double a2 = ab * ab + 0.25 * (si * si + so * so) * ac * ac;
            lobe.roughness = (float)std::sqrt(std::sqrt(a2));
        }
        const Rgb f1 = baseBrdf(lobe, cfg.filmDef, po, pi) * (float)(Ti * To / (kEta * kEta));
        const Rgb ret = in.atRgb(in.a, pi.z) - in.atRgb(in.e, pi.z);
        Rgb fms;
        const float* rb = &in.rhoBar.r;
        const float* rt = &ret.r;
        float* o = &fms.r;
        for (int c = 0; c < 3; ++c) o[c] = (float)(Ti * rt[c] * rb[c] / (1 - rb[c] * K) * To / (kPi * kEta * kEta));
        if (candB && sp && cfg.base.metallic > 0)
        {
            Rgb path;
            for (int c = 0; c < 3; ++c)
            {
                (&path.r)[c] = (float)((&f1.r)[c] * (sp->g.empty() ? std::sqrt(gammaAt(wi.z, c) * gammaAt(wo.z, c)) : gAt(wi.z, c) * gAt(wo.z, c)));
                const double e2b = (&sp->E2bar.r)[c];
                if (!sp->E2.empty() && e2b > 0) (&path.r)[c] += (float)(binLerp(sp->E2, wi.z, c) * binLerp(sp->E2, wo.z, c) / (kPi * e2b));
            }
            if (parts)
            {
                parts[0] = Rgb((float)fc);
                parts[1] = path;
                parts[2] = Rgb();
            }
            return Rgb((float)fc) + path;
        }
        if (parts)
        {
            parts[0] = Rgb((float)fc);
            parts[1] = f1;
            parts[2] = fms;
        }
        return Rgb((float)fc) + f1 + fms;
    }
    // Original 1.1 (for comparison): Schlick coat with multiple-scattering compensation, T_c T_c f_base, no refraction.
    Rgb old(float3 wo, float3 wi) const
    {
        if (wo.z <= 0 || wi.z <= 0) return {};
        const float3 h = normalize(wo + wi);
        const double eo = scene::model::directionalAlbedo(wo.z, (float)cfg.rc);
        const double fc = ggxD({ 0, 0, 1 }, h, ac) * scene::model::visibilitySmithGgxCorrelated(wo.z, wi.z, (float)ac) * (0.04 + 0.96 * std::pow(1 - dot(wo, h), 5.0)) *
                          (1 + 0.04 * (1 / eo - 1));
        const double T = (1 - in.at(in.RcOld, wo.z)) * (1 - in.at(in.RcOld, wi.z));
        return Rgb((float)fc) + baseBrdf(cfg.base, cfg.filmDef, wo, wi) * (float)T;
    }
};

void computeIntegrals(const Config& c, Definitions& d)
{
    Integrals& in = d.in;
    in.Rc.assign(1024, 0);
    in.RcOld.assign(1024, 0);
    parallelFor(1024, [&](uint32_t i) {
        const double mu = (i + 0.5) / 1024;
        const float3 v{ (float)std::sqrt(1 - mu * mu), 0, (float)mu };
        double r = 0, ro = 0;
        const uint32_t n = 1 << 15;
        const double eo = scene::model::directionalAlbedo((float)mu, (float)c.rc);
        for (uint32_t k = 0; k < n; ++k)
        {
            const float3 h = vndf(v, d.ac, (k + 0.5) / n, reference::toUnitFloat(reference::reverseBits(k)));
            const float3 l = h * (2 * dot(v, h)) - v;
            if (l.z <= 0) continue;
            const double w = g2(mu, l.z, d.ac) / g1(mu, d.ac);
            r += w * fresnelExact(dot(v, h), kEta);
            ro += w * (0.04 + 0.96 * std::pow(1 - dot(v, h), 5.0)) * (1 + 0.04 * (1 / eo - 1));
        }
        in.Rc[i] = r / n;
        in.RcOld[i] = ro / n;
    });
    // K: Lambertian radiance inside meeting the rough coat (mirrored frame: normal into the medium).
    {
        double sumR[4] = {}, sumT[4] = {};
        parallelFor(4, [&](uint32_t t) {
            reference::Pcg32 rng(77 + t, 3);
            double R = 0, T = 0;
            const uint32_t n = 1 << 20;
            for (uint32_t k = 0; k < n; ++k)
            {
                const float3 wo = cosineDir(rng.uniform(), rng.uniform());  // away from the interface, into the medium
                const float3 m = vndf(wo, d.ac, rng.uniform(), rng.uniform());
                const double cc = dot(wo, m), F = fresnelExact(cc, 1 / kEta);
                if (rng.uniform() < F)
                {
                    const float3 r = m * (float)(2 * cc) - wo;
                    if (r.z > 0) R += g2(wo.z, r.z, d.ac) / g1(wo.z, d.ac);
                }
                else
                {
                    const double eta = kEta, ct = std::sqrt(std::max(0.0, 1 - eta * eta * (1 - cc * cc)));
                    const float3 tt = normalize(m * (float)(eta * cc - ct) - wo * (float)eta);
                    if (tt.z < 0) T += g1(-tt.z, d.ac);
                }
            }
            sumR[t] = R / n;
            sumT[t] = T / n;
        });
        in.K = (sumR[0] + sumR[1] + sumR[2] + sumR[3]) / 4;
        in.Kout = (sumT[0] + sumT[1] + sumT[2] + sumT[3]) / 4;
    }
    // Multiple-scattering coat (candidate A): E_ms on the Rc grid, Delta-bar, K_ms.
    {
        in.RcMs.assign(1024, 0);
        parallelFor(1024, [&](uint32_t i) {
            const double mu = (i + 0.5) / 1024;
            reference::Pcg32 rng(9000 + i, 21);
            const uint32_t n = 1 << 15;
            double hits = 0;
            for (uint32_t k = 0; k < n; ++k)
            {
                float3 w{ -(float)std::sqrt(1 - mu * mu), 0, -(float)mu };
                bool outside = true;
                if (ms::interact(w, outside, d.ac, rng) && outside) hits += 1;
            }
            in.RcMs[i] = hits / n;
        });
        double db = 0;
        for (int i = 0; i < 1024; ++i) db += std::max(0.0, in.RcMs[i] - in.Rc[i]) * 2 * ((i + 0.5) / 1024) / 1024;
        in.DeltaBar = db;
        double sumK[4] = {};
        parallelFor(4, [&](uint32_t t) {
            reference::Pcg32 rng(7000 + t, 23);
            const uint32_t n = 1 << 19;
            double r = 0;
            for (uint32_t k = 0; k < n; ++k)
            {
                float3 w = cosineDir(rng.uniform(), rng.uniform());
                bool outside = false;
                if (ms::interact(w, outside, d.ac, rng) && !outside) r += 1;
            }
            sumK[t] = r / n;
        });
        in.KMs = (sumK[0] + sumK[1] + sumK[2] + sumK[3]) / 4;
    }
    // Base albedo a(mu'), first-escape part e(mu') (smooth-interface inner transmission), rho-bar.
    in.a.assign(256, Rgb());
    in.e.assign(256, Rgb());
    reference::Surface surf;
    surf.ng = surf.ns = { 0, 0, 1 };
    surf.bsdf = c.base;
    parallelFor(256, [&](uint32_t i) {
        const double mu = (i + 0.5) / 256;
        const float3 l{ (float)std::sqrt(1 - mu * mu), 0, (float)mu };
        const reference::Bsdf b(surf, l);
        reference::Pcg32 rng(1000 + i, 5);
        double A[3] = {}, E[3] = {};
        const uint32_t n = 1 << 16;
        for (uint32_t k = 0; k < n; ++k)
        {
            reference::BsdfSample bs;
            if (!b.sample(rng.uniform(), rng.uniform(), rng.uniform(), bs)) continue;
            const Rgb f = baseBrdf(c.base, c.filmDef, bs.wi, l);
            const double w = bs.wi.z / bs.pdf, tin = 1 - fresnelExact(bs.wi.z, 1 / kEta);
            const float* fp = &f.r;
            for (int ch = 0; ch < 3; ++ch)
            {
                A[ch] += fp[ch] * w;
                E[ch] += fp[ch] * w * tin;
            }
        }
        in.a[i] = Rgb((float)(A[0] / n), (float)(A[1] / n), (float)(A[2] / n));
        in.e[i] = Rgb((float)(E[0] / n), (float)(E[1] / n), (float)(E[2] / n));
    });
    Rgb rb;
    for (int i = 0; i < 256; ++i) rb += in.a[i] * (float)(2 * ((i + 0.5) / 256) / 256);
    in.rhoBar = rb;
}

// ---------------------------------------------------------------------------------------------- tables
struct Table  // energy per (theta_i, theta_o, |dphi|) bin, RGB
{
    std::vector<Rgb> E = std::vector<Rgb>((size_t)NI * NT * NP);
    Rgb& at(int i, int t, int p) { return E[((size_t)i * NT + t) * NP + p]; }
    const Rgb& at(int i, int t, int p) const { return E[((size_t)i * NT + t) * NP + p]; }
};
int binOf(float3 w, int& t, int& p)
{
    const double th = std::acos(std::clamp((double)w.z, 0.0, 1.0)), ph = std::fabs(std::atan2((double)w.y, (double)w.x));
    t = std::min(NT - 1, (int)(th / (kPi / 2) * NT));
    p = std::min(NP - 1, (int)(ph / kPi * NP));
    return 0;
}
double binCosSolidAngle(int t)  // integral of cos over the bin, both signs of dphi
{
    const double t0 = t * (kPi / 2) / NT, t1 = (t + 1) * (kPi / 2) / NT;
    return 0.5 * (std::sin(t1) * std::sin(t1) - std::sin(t0) * std::sin(t0)) * 2 * (kPi / NP);
}

// Energy (luminance) leaving per incidence bin, split by the number of base interactions: 0 (coat only), 1, ..., 14,
// >= 15 (last entry).
using Split = std::vector<std::array<double, kSplit>>;

// mask (optional): incidence bins to trace (others stay zero). split (optional): see Split.
void physicalTable(const Config& c, const Definitions& d, uint32_t photons, Table& T, Table& Thalf, const std::vector<uint8_t>* mask = nullptr, Split* split = nullptr)
{
    reference::Surface surf;
    surf.ng = surf.ns = { 0, 0, 1 };
    surf.bsdf = c.base;
    const double ac = d.ac;
    if (split) split->assign(NI, std::array<double, kSplit>{});
    parallelFor(NI, [&](uint32_t ii) {
        if (mask && !(*mask)[ii]) return;
        const float3 wi = incident((int)ii);
        reference::Pcg32 rng(0xC0A7 + ii, 11 + (uint64_t)(c.rc * 1000) + (c.msCoat ? 5000 : 0));
        std::array<double, kSplit> sp{};
        for (uint32_t s = 0; s < photons; ++s)
        {
            float3 dir = -wi;
            double w = 1;
            Rgb wrgb(1.0f);
            bool inside = false;
            int baseHits = 0;
            for (int bounce = 0; bounce < 256; ++bounce)
            {
                float3 exitDir{};
                bool exited = false;
                if (c.msCoat && (!inside || dir.z > 0))
                {
                    // Coat interface with microsurface multiple scattering, from either side.
                    bool outside = !inside;
                    if (!ms::interact(dir, outside, ac, rng)) break;
                    if (outside)
                    {
                        exitDir = dir;
                        exited = true;
                    }
                    else inside = true;
                }
                else if (!inside)
                {
                    const float3 wo = -dir;
                    const float3 m = vndf(wo, ac, rng.uniform(), rng.uniform());
                    const double cc = dot(wo, m);
                    if (rng.uniform() < fresnelExact(cc, kEta))
                    {
                        const float3 r = m * (float)(2 * cc) - wo;
                        if (r.z <= 0) break;
                        w *= g2(wo.z, r.z, ac) / g1(wo.z, ac);
                        exitDir = r;
                        exited = true;
                    }
                    else
                    {
                        const double eta = 1 / kEta, ct = std::sqrt(std::max(0.0, 1 - eta * eta * (1 - cc * cc)));
                        const float3 t = normalize(m * (float)(eta * cc - ct) - wo * (float)eta);
                        if (t.z >= 0) break;
                        w *= g1(-t.z, ac);
                        dir = t;
                        inside = true;
                    }
                }
                else if (dir.z < 0)
                {
                    const float3 l = -dir;
                    const reference::Bsdf b(surf, l);
                    reference::BsdfSample bs;
                    if (!b.sample(rng.uniform(), rng.uniform(), rng.uniform(), bs)) break;
                    wrgb *= baseBrdf(c.base, c.filmPhys, bs.wi, l) * (bs.wi.z / bs.pdf);
                    if (wrgb.isZero()) break;
                    dir = bs.wi;
                    ++baseHits;
                }
                else
                {
                    const float3 wof{ -dir.x, -dir.y, dir.z };  // mirrored frame (normal into the medium)
                    const float3 m = vndf(wof, ac, rng.uniform(), rng.uniform());
                    const double cc = dot(wof, m);
                    if (rng.uniform() < fresnelExact(cc, 1 / kEta))
                    {
                        const float3 r = m * (float)(2 * cc) - wof;
                        if (r.z <= 0) break;
                        w *= g2(wof.z, r.z, ac) / g1(wof.z, ac);
                        dir = { r.x, r.y, -r.z };
                    }
                    else
                    {
                        const double eta = kEta, ct = std::sqrt(std::max(0.0, 1 - eta * eta * (1 - cc * cc)));
                        const float3 t = normalize(m * (float)(eta * cc - ct) - wof * (float)eta);
                        if (t.z >= 0) break;
                        w *= g1(-t.z, ac);
                        exitDir = { t.x, t.y, -t.z };
                        exited = true;
                    }
                }
                if (exited)
                {
                    int bt, bp;
                    binOf(exitDir, bt, bp);
                    const Rgb add = wrgb * (float)(w / photons);
                    T.at((int)ii, bt, bp) += add;
                    if (s & 1) Thalf.at((int)ii, bt, bp) += add * 2.0f;  // odd half, for the noise floor
                    sp[std::min(baseHits, kSplit - 1)] += add.luminance();
                    break;
                }
                if (bounce > 8)
                {
                    const double q = std::min(1.0, w * wrgb.max());
                    if (rng.uniform() >= q) break;
                    w /= q;
                }
            }
        }
        if (split) (*split)[ii] = sp;
    });
}

enum class Which { R1, Old, R1A, R1B };
// parts (R1 only, optional): energy (luminance) of f_c, f_1, f_ms per incidence bin.
using Parts = std::vector<std::array<double, 3>>;
// f1E (optional): energy of the f_1 part per incidence bin, per channel.
// f1Only: T receives only the f_1 part (the transfer table B2 solves its scale on).
void definitionTable(const Config& c, const Definitions& d, Which which, uint32_t samples, Table& T, const std::vector<uint8_t>* mask = nullptr, Parts* parts = nullptr,
                     std::vector<Rgb>* f1E = nullptr, bool f1Only = false)
{
    if (parts) parts->assign(NI, { 0, 0, 0 });
    if (f1E) f1E->assign(NI, Rgb());
    reference::Surface surfR1;
    surfR1.ng = surfR1.ns = { 0, 0, 1 };
    surfR1.bsdf = which != Which::Old ? d.baseR1 : c.base;
    parallelFor(NI, [&](uint32_t ii) {
        if (mask && !(*mask)[ii]) return;
        const float3 wi = incident((int)ii);
        const float3 pi = which != Which::Old ? refractIn(wi) : wi;
        std::array<double, 3> pp = { 0, 0, 0 };
        Rgb f1Sum;
        const reference::Bsdf base(surfR1, pi);
        reference::Pcg32 rng(0xDEF0 + ii, 13 + (uint64_t)(c.rc * 1000) + (which == Which::R1 ? 0 : which == Which::Old ? 7 : which == Which::R1A ? 3 : 5));
        auto baseLobePdf = [&](float3 wo) {
            if (which == Which::Old) return (double)base.pdf(wo);
            const float3 po = refractIn(wo);
            return (double)base.pdf(po) * wo.z / (kEta * kEta * po.z);
        };
        for (uint32_t s = 0; s < samples; ++s)
        {
            const double u = rng.uniform();
            float3 wo{ 0, 0, -1 };
            if (u < 0.35)
            {
                const float3 h = vndf(wi, d.ac, rng.uniform(), rng.uniform());
                wo = h * (2 * dot(wi, h)) - wi;
            }
            else if (u < 0.8)
            {
                reference::BsdfSample bs;
                if (base.sample(rng.uniform(), rng.uniform(), rng.uniform(), bs))
                {
                    if (which == Which::Old) wo = bs.wi;
                    else if (!refractOut(bs.wi, wo)) wo = { 0, 0, -1 };
                }
            }
            else wo = cosineDir(rng.uniform(), rng.uniform());
            if (wo.z <= 1e-6f) continue;
            const float3 h = normalize(wo + wi);
            const double pc = g1(wi.z, d.ac) * ggxD({ 0, 0, 1 }, h, d.ac) / (4 * wi.z);
            const double pdf = 0.35 * pc + 0.45 * baseLobePdf(wo) + 0.2 * wo.z / kPi;
            if (!(pdf > 0)) continue;
            Rgb pr[3];
            const Rgb f = which != Which::Old ? d.r1(wo, wi, pr, which == Which::R1A || which == Which::R1B, which == Which::R1B) : d.old(wo, wi);
            int bt, bp;
            binOf(wo, bt, bp);
            const double wgt = wo.z / pdf / samples;
            T.at((int)ii, bt, bp) += (f1Only ? pr[1] : f) * (float)wgt;
            if (which != Which::Old)
            {
                for (int k = 0; k < 3; ++k) pp[k] += pr[k].luminance() * wgt;
                f1Sum += pr[1] * (float)wgt;
            }
        }
        if (parts) (*parts)[ii] = pp;
        if (f1E) (*f1E)[ii] = f1Sum;
    });
}

// ---------------------------------------------------------------------------------------------- metrics
struct Metrics
{
    double albedoRelMax = 0, albedoAbsAtRelMax = 0, albedoWorstTheta = 0;  // worst over theta_i (rel, when abs > 0.005)
    double albedoRel[5] = {};                                                // at 0/30/60/75/85 deg
    double l1Max = 0, l1WorstTheta = 0, l1Noise = 0;
    double furnaceMean = 0, furnaceP99 = 0, skyMean = 0, skyP99 = 0;
    double furnaceNoiseP99 = 0, skyNoiseP99 = 0;  // physical half-table render vs full (MC floor of one table)
    bool pass() const { return albedoRelMax <= 0.02 && l1Max <= 0.05 && furnaceMean <= 1.0 && furnaceP99 <= 2.3 && skyMean <= 1.0 && skyP99 <= 2.3; }
};

double albedoOf(const Table& T, int i)
{
    double s = 0;
    for (int t = 0; t < NT; ++t)
        for (int p = 0; p < NP; ++p) s += T.at(i, t, p).luminance();
    return s;
}
double l1Of(const Table& A, const Table& B, int i)
{
    double coarseA[CT][CP] = {}, coarseB[CT][CP] = {}, tot = 0;
    for (int t = 0; t < NT; ++t)
        for (int p = 0; p < NP; ++p)
        {
            coarseA[t * CT / NT][p * CP / NP] += A.at(i, t, p).luminance();
            coarseB[t * CT / NT][p * CP / NP] += B.at(i, t, p).luminance();
        }
    double l1 = 0;
    for (int t = 0; t < CT; ++t)
        for (int p = 0; p < CP; ++p)
        {
            l1 += std::fabs(coarseA[t][p] - coarseB[t][p]);
            tot += coarseB[t][p];
        }
    return tot > 0 ? l1 / tot : 0;
}

// Environment for the sphere renders (camera looks down -z; view direction +z). Sun: disk of 2.5 deg radius,
// illuminance 1 (normal incidence) with colour (1, 0.95, 0.85); sky: blue gradient; ground: dark grey.
struct EnvSample
{
    float3 dir;
    Rgb L;
    double dw;
};
std::vector<EnvSample> environment(bool furnace)
{
    std::vector<EnvSample> env;
    const int nth = 180, nph = 360;
    for (int a = 0; a < nth; ++a)
        for (int b = 0; b < nph; ++b)
        {
            const double th = (a + 0.5) * kPi / nth, ph = (b + 0.5) * 2 * kPi / nph;
            const float3 d{ (float)(std::sin(th) * std::cos(ph)), (float)std::cos(th), (float)(std::sin(th) * std::sin(ph)) };  // y up
            const double dw = std::sin(th) * (kPi / nth) * (2 * kPi / nph);
            Rgb L;
            if (furnace) L = Rgb(1.0f);
            else L = d.y > 0 ? Rgb(0.10f, 0.16f, 0.30f) * (float)(0.5 + 0.5 * d.y) : Rgb(0.02f, 0.02f, 0.02f);
            env.push_back({ d, L, dw });
        }
    if (!furnace)
    {
        const float3 s = normalize(float3{ 0.45f, 0.62f, 0.64f });
        const double radius = 2.5 * kPi / 180, omega = 2 * kPi * (1 - std::cos(radius));
        const float3 t1 = normalize(cross(std::fabs(s.y) < 0.9f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 }, s)), t2 = cross(s, t1);
        const int n = 400;
        for (int k = 0; k < n; ++k)
        {
            const double u1 = (k + 0.5) / n, u2 = reference::toUnitFloat(reference::reverseBits((uint32_t)k));
            const double c = 1 - u1 * (1 - std::cos(radius)), sn = std::sqrt(1 - c * c), ph = 2 * kPi * u2;
            const float3 d = normalize(s * (float)c + t1 * (float)(sn * std::cos(ph)) + t2 * (float)(sn * std::sin(ph)));
            env.push_back({ d, Rgb(1.0f, 0.95f, 0.85f) * (float)(1.0 / omega), omega / n });
        }
    }
    return env;
}

// Sphere render from a table: per pixel sum over the environment of BRDF(bin) * L * cos * dw.
std::vector<Rgb3> renderSphere(const Table& T, const std::vector<EnvSample>& env, int N)
{
    std::vector<Rgb3> img((size_t)N * N, Rgb3{ -1, -1, -1 });
    parallelFor((uint32_t)N, [&](uint32_t y) {
        for (int x = 0; x < N; ++x)
        {
            const double px = (x + 0.5) / N * 2 - 1, py = 1 - (y + 0.5) / N * 2;
            if (px * px + py * py >= 0.98) continue;
            const float3 n{ (float)px, (float)py, (float)std::sqrt(1 - px * px - py * py) };
            const float3 v{ 0, 0, 1 };
            // Local frame: z = n, x = projection of v (so v has phi = 0).
            float3 xa = v - n * dot(v, n);
            xa = length(xa) > 1e-6f ? normalize(xa) : normalize(cross(n, float3{ 0, 1, 0 }));
            const float3 ya = cross(n, xa);
            const double muo = dot(v, n);
            const int bt = std::min(NT - 1, (int)(std::acos(std::clamp(muo, 0.0, 1.0)) / (kPi / 2) * NT));
            const double norm = binCosSolidAngle(bt);
            double acc[3] = { 0, 0, 0 };
            for (const EnvSample& e : env)
            {
                const double mui = dot(e.dir, n);
                if (mui <= 0) continue;
                const int bi = std::min(NI - 1, (int)(std::acos(mui) / (kPi / 2) * NI));
                const double lx = dot(e.dir, xa), ly = dot(e.dir, ya);
                const int bp = std::min(NP - 1, (int)(std::fabs(std::atan2(ly, lx)) / kPi * NP));
                const Rgb f = T.at(bi, bt, bp) * (float)(1.0 / norm);  // bin-average f(v, l): exit bin of v, incidence bin of l
                const double w = mui * e.dw;
                acc[0] += f.r * e.L.r * w;
                acc[1] += f.g * e.L.g * w;
                acc[2] += f.b * e.L.b * w;
            }
            img[(size_t)y * N + x] = { acc[0], acc[1], acc[2] };
        }
    });
    return img;
}
void renderError(const std::vector<Rgb3>& a, const std::vector<Rgb3>& b, double whiteY, double& mean, double& p99)
{
    std::vector<double> de;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i][0] >= 0) de.push_back(deltaE76(a[i], b[i], whiteY));
    double s = 0;
    for (double v : de) s += v;
    mean = de.empty() ? 0 : s / de.size();
    std::sort(de.begin(), de.end());
    p99 = de.empty() ? 0 : de[(size_t)(0.99 * (de.size() - 1))];
}

Metrics compare(const Table& def, const Table& phys, const Table& physHalf, const std::vector<EnvSample>& furnace, const std::vector<EnvSample>& sky, double skyWhite,
                const std::vector<Rgb3>* physFurnace, const std::vector<Rgb3>* physSky)
{
    Metrics m;
    const int probe[5] = { 0, 30, 60, 75, 85 };
    for (int i = 0; i < NI; ++i)
    {
        const double ad = albedoOf(def, i), ap = albedoOf(phys, i);
        const double abs = std::fabs(ad - ap), rel = ap > 0 ? abs / ap : 0;
        const double score = abs <= 0.005 ? 0 : rel;  // the criterion accepts either bound
        if (score > m.albedoRelMax)
        {
            m.albedoRelMax = score;
            m.albedoAbsAtRelMax = ad - ap;
            m.albedoWorstTheta = i + 0.5;
        }
        for (int k = 0; k < 5; ++k)
            if (i == std::min(probe[k], 89)) m.albedoRel[k] = ap > 0 ? (ad - ap) / ap : 0;
        const double l1 = l1Of(def, phys, i);
        if (l1 > m.l1Max)
        {
            m.l1Max = l1;
            m.l1WorstTheta = i + 0.5;
        }
        m.l1Noise = std::max(m.l1Noise, l1Of(physHalf, phys, i));
    }
    const std::vector<Rgb3> fd = renderSphere(def, furnace, 64), sd = renderSphere(def, sky, 64);
    renderError(fd, *physFurnace, 1.0, m.furnaceMean, m.furnaceP99);
    renderError(sd, *physSky, skyWhite, m.skyMean, m.skyP99);
    double unused = 0;
    renderError(renderSphere(physHalf, furnace, 64), *physFurnace, 1.0, unused, m.furnaceNoiseP99);
    renderError(renderSphere(physHalf, sky, 64), *physSky, skyWhite, unused, m.skyNoiseP99);
    return m;
}

FilmTable makeFilm(bool reference, double n0, double nf, double d, const Substrate& s)
{
    FilmTable t;
    t.F.resize(1024);
    parallelFor(1024, [&](uint32_t i) {
        const double c = (double)i / 1023;
        t.F[i] = reference ? filmReference(n0, std::max(c, 1e-4), nf, d, s) : filmA(n0, std::max(c, 1e-4), nf, d, s);
    });
    return t;
}

std::string row(const std::string& name, double rc, const char* def, const Metrics& m)
{
    char b[512];
    std::snprintf(b, sizeof b, "| %s | %.2f | %s | %.1f%% @%.0f° (%+.3f) | %+.1f / %+.1f / %+.1f / %+.1f / %+.1f %% | %.3f @%.0f° | %.3f | %.2f / %.2f | %.2f / %.2f | %.2f / %.2f | %s |\n",
                  name.c_str(), rc, def, 100 * m.albedoRelMax, m.albedoWorstTheta, m.albedoAbsAtRelMax, 100 * m.albedoRel[0], 100 * m.albedoRel[1], 100 * m.albedoRel[2],
                  100 * m.albedoRel[3], 100 * m.albedoRel[4], m.l1Max, m.l1WorstTheta, m.l1Noise, m.furnaceMean, m.furnaceP99, m.skyMean, m.skyP99, m.furnaceNoiseP99, m.skyNoiseP99, m.pass() ? "PASS" : "FAIL");
    return b;
}
} // namespace

void clearcoatR1Study(const std::string& out, uint32_t photons, bool msCoat, bool candA, bool candB, bool candC, bool candD, bool candE)
{
    struct BaseDef
    {
        const char* name;
        scene::model::Surface s;
    };
    auto mk = [](float3 c, float r, float m) {
        scene::model::Surface s;
        s.baseColor = c;
        s.roughness = r;
        s.metallic = m;
        return s;
    };
    const BaseDef bases[] = { { "white diffuse 0.8", mk({ 0.8f, 0.8f, 0.8f }, 0.9f, 0) }, { "red paint r 0.5", mk({ 0.6f, 0.05f, 0.05f }, 0.5f, 0) },
                              { "metal flake r 0.3", mk({ 0.9f, 0.6f, 0.3f }, 0.3f, 1) }, { "chrome r 0.1", mk({ 0.9f, 0.9f, 0.9f }, 0.1f, 1) },
                              { "black glossy r 0.2", mk({ 0.02f, 0.02f, 0.02f }, 0.2f, 0) } };
    const double coats[] = { 0.05, 0.12, 0.30 };
    const std::vector<EnvSample> furnace = environment(true), sky = environment(false);
    // White for the sky render: a white Lambertian facing the sun.
    double skyWhite = 0;
    {
        const float3 s = normalize(float3{ 0.45f, 0.62f, 0.64f });
        for (const EnvSample& e : sky)
            if (dot(e.dir, s) > 0) skyWhite += e.L.luminance() * dot(e.dir, s) * e.dw / kPi;
    }
    std::ostringstream md, eq;
    md << "# Clearcoat R1 vs physical layer model [measured]\n\n"
          "`unx_study_material_layers " << (candE ? "clearcoat_r1e" : candD ? "clearcoat_r1d" : candC ? "clearcoat_r1c" : candB ? "clearcoat_r1b" : candA ? "clearcoat_r1a" : msCoat ? "clearcoat_r1_ms" : "clearcoat_r1") << "`. Physical coat: "
       << (msCoat ? "microsurface multiple scattering (Heitz et al. 2016, energy conserving)" : "single-scattering microfacets (energy lost at grazing)")
       << ". Criteria (MATERIAL_LAYERS 3): albedo rel <= 2 % (or abs <= 0.005), L1 <= 0.05, "
          "render dE76 mean <= 1.0 and P99 <= 2.3 (white furnace / sun + sky sphere, 64 x 64). Photons per incidence bin: "
          << photons << " (physical), " << photons << " (definition). 'L1 noise' = physical half vs full (MC floor).\n\n"
          "| base | r_c | definition | worst albedo (rel @theta, abs) | albedo rel at 0/30/60/75/85° | worst L1 | L1 noise | furnace dE mean / P99 | sun+sky dE mean / P99 | render noise P99 furnace / sky | criteria |\n"
          "|---|---|---|---|---|---|---|---|---|---|---|\n";
    eq << "\n## Equivalent roughness of the base lobe outside (alpha_eq ~ eta alpha'_b)\n\n| base | r_c | alpha'_b | eta alpha'_b | physical 75 % half-angle | GGX(eta alpha'_b) 75 % half-angle |\n|---|---|---|---|---|---|\n";
    // Optional filters (rerunning part of the grid): UNX_STUDY_RC = one r_c value, UNX_STUDY_BASE = base name substring.
    auto env = [](const char* name) {
        char* v = nullptr;
        size_t n = 0;
        std::string r;
        if (_dupenv_s(&v, &n, name) == 0 && v) r = v;
        free(v);
        return r;
    };
    const std::string onlyRc = env("UNX_STUDY_RC"), onlyBase = env("UNX_STUDY_BASE");
    for (double rc : coats)
        for (const BaseDef& b : bases)
        {
            if (!onlyRc.empty() && std::fabs(std::stod(onlyRc) - rc) > 1e-6) continue;
            if (!onlyBase.empty() && std::string(b.name).find(onlyBase) == std::string::npos) continue;
            Config c;
            c.name = b.name;
            c.base = b.s;
            c.rc = rc;
            c.msCoat = msCoat;
            Definitions d(c);
            computeIntegrals(c, d);
            SpecPath spd;
            if (candE) d.spread = true;
            if (candC || candD || candE)
            {
                // A2 + B2: scaled coat term; base path scaled by g(mu_i) g(mu_o) so that every incidence bin's base-path
                // energy equals the census value sum_k S_k rho^k (rho = a(mu') per channel), solved by symmetric
                // Sinkhorn iteration on R1+A2's f_1 transfer table (energy from incidence bin to exit theta bin).
                d.coatForm = 1;
                Config lossless = c;
                lossless.base.baseColor = { 1, 1, 1 };
                lossless.base.metallic = 1;
                Table t0, t1, tf;
                physicalTable(lossless, d, photons / 2, t0, t1, nullptr, &spd.S);
                definitionTable(c, d, Which::R1A, photons / 2, tf, nullptr, nullptr, nullptr, true);
                spd.g.assign(NI, Rgb(1.0f));
                if (candD) spd.E2.assign(NI, Rgb());
                for (int ch = 0; ch < 3; ++ch)
                {
                    std::vector<double> M((size_t)NI * NT, 0.0), E(NI, 0.0), g(NI, 1.0), E2(NI, 0.0);
                    for (int i = 0; i < NI; ++i)
                    {
                        for (int t = 0; t < NT; ++t)
                            for (int q = 0; q < NP; ++q) M[(size_t)i * NT + t] += (&tf.at(i, t, q).r)[ch];
                        const Rgb a = d.in.atRgb(d.in.a, refractIn(incident(i)).z);
                        const double rho = (&a.r)[ch];
                        double rk = 1;
                        for (int k = 1; k < kSplit; ++k)
                        {
                            rk *= rho;
                            (candD && k >= 2 ? E2[i] : E[i]) += spd.S[i][k] * rk;
                        }
                    }
                    if (candD)
                    {
                        double bar = 0;  // E2-bar = 2 int E2 mu dmu over the incidence bins (1 deg in theta)
                        for (int i = 0; i < NI; ++i)
                        {
                            const double th0 = i * kPi / 180, th1 = (i + 1) * kPi / 180;
                            bar += E2[i] * (std::sin(th1) * std::sin(th1) - std::sin(th0) * std::sin(th0));
                            (&spd.E2[i].r)[ch] = (float)E2[i];
                        }
                        (&spd.E2bar.r)[ch] = (float)bar;
                    }
                    for (int it = 0; it < 500; ++it)
                        for (int i = 0; i < NI; ++i)
                        {
                            double m = 0;
                            for (int t = 0; t < NT; ++t) m += M[(size_t)i * NT + t] * g[t];
                            if (m > 1e-9 && E[i] > 0) g[i] = std::sqrt(g[i] * E[i] / m);
                        }
                    for (int i = 0; i < NI; ++i) (&spd.g[i].r)[ch] = (float)g[i];
                }
                d.sp = &spd;
            }
            else if (candB)
            {
                // Candidate B inputs: lossless base-path census and R1+A's f_1 energy per incidence bin.
                Config lossless = c;
                lossless.base.baseColor = { 1, 1, 1 };
                lossless.base.metallic = 1;
                Table t0, t1, t2;
                physicalTable(lossless, d, photons / 2, t0, t1, nullptr, &spd.S);
                definitionTable(c, d, Which::R1A, photons / 2, t2, nullptr, nullptr, &spd.f1E);
                d.sp = &spd;
            }
            Table phys, half, r1, old;
            physicalTable(c, d, photons, phys, half);
            definitionTable(c, d, (candC || candD || candE || candB) ? Which::R1A : Which::R1, photons, r1);
            definitionTable(c, d, (candB || candC || candD || candE) ? Which::R1B : candA ? Which::R1A : Which::Old, photons, old);
            const std::vector<Rgb3> pf = renderSphere(phys, furnace, 64), ps = renderSphere(phys, sky, 64);
            const Metrics m1 = compare(r1, phys, half, furnace, sky, skyWhite, &pf, &ps), m0 = compare(old, phys, half, furnace, sky, skyWhite, &pf, &ps);
            const char* first = candE ? "R1 + A2 + S" : (candC || candD) ? "R1 + A2" : candB ? "R1 + A" : "R1";
            const char* second = candE ? "R1 + A2 + B2 + S" : candD ? "R1 + A2 + B3" : candC ? "R1 + A2 + B2" : candB ? "R1 + A + B" : candA ? "R1 + A (coat MS)" : "1.1 original";
            md << row(b.name, rc, first, m1) << row(b.name, rc, second, m0);
            logf("%s%s", row(b.name, rc, first, m1).c_str(), row(b.name, rc, second, m0).c_str());
            logf("   K %.4f (out %.4f), rho-bar (%.3f %.3f %.3f)\n", d.in.K, d.in.Kout, d.in.rhoBar.r, d.in.rhoBar.g, d.in.rhoBar.b);
            // Lobe width check at normal incidence (metal bases): angular energy distribution excluding the coat lobe
            // (the definition's f_c energy per theta_o bin is subtracted from both).
            if (b.s.metallic > 0)
            {
                auto halfAngle = [&](const std::function<double(int)>& energyAt) {
                    double tot = 0;
                    for (int t = 0; t < NT; ++t) tot += std::max(0.0, energyAt(t));
                    double acc = 0;
                    for (int t = 0; t < NT; ++t)
                    {
                        acc += std::max(0.0, energyAt(t));
                        if (acc >= 0.75 * tot) return (t + 1.0) * 90.0 / NT;
                    }
                    return 90.0;
                };
                // Coat-only energy per theta_o at normal incidence from the definition's f_c (computed directly).
                std::vector<double> coat(NT, 0.0);
                {
                    const float3 wi = incident(0);
                    const int n = 200000;
                    for (int k = 0; k < n; ++k)
                    {
                        const float3 h = vndf(wi, d.ac, (k + 0.5) / n, reference::toUnitFloat(reference::reverseBits((uint32_t)k)));
                        const float3 wo = h * (2 * dot(wi, h)) - wi;
                        if (wo.z <= 0) continue;
                        int bt, bp;
                        binOf(wo, bt, bp);
                        coat[bt] += g2(wi.z, wo.z, d.ac) / g1(wi.z, d.ac) * fresnelExact(dot(wo, h), kEta) / n;
                    }
                }
                const double physHalf = halfAngle([&](int t) {
                    double s = 0;
                    for (int p = 0; p < NP; ++p) s += phys.at(0, t, p).luminance();
                    return s - coat[t];
                });
                const double aeq = std::min(1.0, kEta * scene::model::alphaFromRoughness(d.baseR1.roughness));
                // GGX(aeq) reflection lobe at normal incidence: energy per theta_o bin by VNDF sampling.
                std::vector<double> gg(NT, 0.0);
                const float3 wi = incident(0);
                const int n = 400000;
                for (int k = 0; k < n; ++k)
                {
                    const float3 h = vndf(wi, aeq, (k + 0.5) / n, reference::toUnitFloat(reference::reverseBits((uint32_t)k)));
                    const float3 wo = h * (2 * dot(wi, h)) - wi;
                    if (wo.z <= 0) continue;
                    int bt, bp;
                    binOf(wo, bt, bp);
                    gg[bt] += 1.0 / n;
                }
                const double ggHalf = halfAngle([&](int t) { return gg[t]; });
                char e[256];
                std::snprintf(e, sizeof e, "| %s | %.2f | %.4f | %.4f | %.1f° | %.1f° |\n", b.name, rc, scene::model::alphaFromRoughness(d.baseR1.roughness), aeq, physHalf, ggHalf);
                eq << e;
            }
        }
    writeTextFile(out, md.str() + eq.str());
}

// Diagnosis of the R1 failures: energy leaving per incidence angle, split by the number of base interactions in the
// physical walk (single-scattering coat and multiple-scattering coat), against R1's own split (f_c, f_1, f_ms).
void clearcoatDiag(const std::string& out, uint32_t photons)
{
    auto mk = [](float3 col, float r, float m) {
        scene::model::Surface s;
        s.baseColor = col;
        s.roughness = r;
        s.metallic = m;
        return s;
    };
    struct Case
    {
        const char* name;
        scene::model::Surface base;
        double rc;
    };
    const Case cases[] = { { "chrome r 0.1", mk({ 0.9f, 0.9f, 0.9f }, 0.1f, 1), 0.05 },   { "metal flake r 0.3", mk({ 0.9f, 0.6f, 0.3f }, 0.3f, 1), 0.05 },
                           { "white diffuse 0.8", mk({ 0.8f, 0.8f, 0.8f }, 0.9f, 0), 0.05 }, { "white diffuse 0.8", mk({ 0.8f, 0.8f, 0.8f }, 0.9f, 0), 0.12 },
                           { "white diffuse 0.8", mk({ 0.8f, 0.8f, 0.8f }, 0.9f, 0), 0.30 }, { "chrome r 0.1", mk({ 0.9f, 0.9f, 0.9f }, 0.1f, 1), 0.30 } };
    const int probe[] = { 0, 30, 60, 70, 75, 80, 84, 87, 89 };
    std::vector<uint8_t> mask(NI, 0);
    for (int i : probe) mask[i] = 1;
    std::ostringstream md;
    md << "# Clearcoat R1 failure diagnosis [measured]\n\n"
          "`unx_study_material_layers clearcoat_diag`. Directional albedo (luminance) and its split by the number of base "
          "interactions before leaving: 0 (coat only) / 1 / 2 / >= 3. SS: physical walk with single-scattering coat microfacets "
          "(energy lost at grazing); MS: microsurface multiple scattering (Heitz et al. 2016, energy conserving). R1 split: f_c / f_1 / "
          "f_ms. Photons per angle: "
       << photons << ".\n";
    for (const Case& k : cases)
    {
        Config c;
        c.name = k.name;
        c.base = k.base;
        c.rc = k.rc;
        Definitions d(c);
        computeIntegrals(c, d);
        Table ss, ssHalf, msT, msHalf, r1;
        Split sss, mss;
        Parts pr;
        physicalTable(c, d, photons, ss, ssHalf, &mask, &sss);
        Config cm = c;
        cm.msCoat = true;
        physicalTable(cm, d, photons, msT, msHalf, &mask, &mss);
        definitionTable(c, d, Which::R1, photons, r1, &mask, &pr);
        const size_t mark = md.str().size();
        md << format("\n## %s, r_c %.2f (alpha_c %.4f, alpha_b %.4f, alpha'_b %.4f)\n\n", k.name, k.rc, d.ac, d.ab, scene::model::alphaFromRoughness(d.baseR1.roughness))
           << "| theta_i | albedo SS | albedo MS | albedo R1 | SS 0 / 1 / 2 / >=3 | MS 0 / 1 / 2 / >=3 | R1 f_c / f_1 / f_ms |\n|---|---|---|---|---|---|---|\n";
        for (int i : probe)
        {
            auto agg = [](const std::array<double, kSplit>& x) {
                std::array<double, 4> y{ x[0], x[1], x[2], 0 };
                for (int k = 3; k < kSplit; ++k) y[3] += x[k];
                return y;
            };
            const auto a = agg(sss[i]);
            const auto b = agg(mss[i]);
            const auto& r = pr[i];
            md << format("| %.1f° | %.4f | %.4f | %.4f | %.4f / %.4f / %.4f / %.4f | %.4f / %.4f / %.4f / %.4f | %.4f / %.4f / %.4f |\n", i + 0.5, a[0] + a[1] + a[2] + a[3],
                         b[0] + b[1] + b[2] + b[3], r[0] + r[1] + r[2], a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3], r[0], r[1], r[2]);
        }
        logf("%s", md.str().substr(mark).c_str());
        writeTextFile(out, md.str());
    }
}

// Specular-path census for the glossy/metal base redesign: a lossless GGX base (v1 metal, base colour 1, so its
// directional albedo is 1 at every angle) under the multiple-scattering coat. S_k(theta_i) = energy leaving after
// exactly k base interactions; for a base of constant albedo rho the layer's base-path energy is sum_k S_k rho^k.
// Also q_k = S_{k+1} / S_k (how far the tail is from geometric) and the energy still trapped at the walk cap.
void clearcoatSpecPath(const std::string& out, uint32_t photons)
{
    const double coats[] = { 0.05, 0.12, 0.30 };
    const float bases[] = { 0.1f, 0.2f, 0.3f, 0.45f, 0.6f };  // roughness (alpha = r^2: 0.01, 0.04, 0.09, 0.2, 0.36)
    const int probe[] = { 0, 30, 45, 60, 65, 70, 75, 80, 85, 89 };
    std::vector<uint8_t> mask(NI, 0);
    for (int i : probe) mask[i] = 1;
    std::ostringstream md;
    md << "# Specular-path census: lossless GGX base under the coat [measured]\n\n"
          "`unx_study_material_layers clearcoat_specpath`. Base: v1 metal, base colour 1 (albedo 1 at every angle), roughness r_b; "
          "coat: n 1.5, microsurface multiple scattering, roughness r_c. S_k = energy leaving after exactly k base interactions "
          "(S_0 = coat reflection). A base of constant albedo rho gives sum_k S_k rho^k. Photons per angle: "
       << photons << ".\n";
    for (double rc : coats)
        for (float rb : bases)
        {
            Config c;
            c.name = "lossless GGX";
            c.base.baseColor = { 1, 1, 1 };
            c.base.roughness = rb;
            c.base.metallic = 1;
            c.rc = rc;
            c.msCoat = true;
            Definitions d(c);
            Table T, Th;
            Split sp;
            physicalTable(c, d, photons, T, Th, &mask, &sp);
            const size_t mark = md.str().size();
            md << format("\n## r_c %.2f, r_b %.2f (alpha_c %.4f, alpha_b %.4f)\n\n", rc, rb, d.ac, d.ab)
               << "| theta_i | S_0 | S_1 | S_2 | S_3 | S_4 | S_5 | S_6..14 | S_>=15 | q_2 = S_3/S_2 | q_4 = S_5/S_4 | lost at walk cap | E(rho 0.9) | E(rho 0.6) |\n"
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
            for (int i : probe)
            {
                const auto& x = sp[i];
                double mid = 0, e9 = 0, e6 = 0, tot = 0;
                for (int k = 0; k < kSplit; ++k) tot += x[k];
                for (int k = 6; k < kSplit - 1; ++k) mid += x[k];
                for (int k = 1; k < kSplit; ++k)
                {
                    e9 += x[k] * std::pow(0.9, k);
                    e6 += x[k] * std::pow(0.6, k);
                }
                md << format("| %.1f° | %.4f | %.4f | %.4f | %.4f | %.4f | %.4f | %.4f | %.4f | %.3f | %.3f | %.4f | %.4f | %.4f |\n", i + 0.5, x[0], x[1], x[2], x[3], x[4], x[5], mid,
                             x[kSplit - 1], x[2] > 0 ? x[3] / x[2] : 0.0, x[4] > 0 ? x[5] / x[4] : 0.0, 1 - tot, e9, e6);
            }
            logf("%s", md.str().substr(mark).c_str());
            writeTextFile(out, md.str());
        }
}

// v1 metal white furnace: directional albedo of the v1 model with base colour 1, metallic 1 (F = 1, so the albedo is
// E_true(mu, r) / E_table(mu, r) with E_true the integral of the model's own D V). Reported per roughness and angle next
// to the core table value directionalAlbedo(mu, r) and E_true = albedo * E_table.
void v1Albedo(const std::string& out)
{
    const float rs[] = { 0.05f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f, 1.0f };
    const double mus[] = { 0.02, 0.05, 0.1, 0.2, 0.35, 0.5, 0.7, 0.85, 1.0 };
    constexpr int NR = 11, NM = 9;
    std::vector<double> alb(NR * NM, 0.0), sd(NR * NM, 0.0), adj(NR * NM, 0.0);
    parallelFor(NR * NM, [&](uint32_t idx) {
        const int ri = (int)idx / NM, mi = (int)idx % NM;
        reference::Surface surf;
        surf.ng = surf.ns = { 0, 0, 1 };
        surf.bsdf.baseColor = { 1, 1, 1 };
        surf.bsdf.metallic = 1;
        surf.bsdf.roughness = rs[ri];
        const double mu = mus[mi];
        const float3 v{ (float)std::sqrt(1 - mu * mu), 0, (float)mu };
        const reference::Bsdf b(surf, v);
        reference::Pcg32 rng(3100 + idx, 7);
        const uint32_t n = 1 << 22;
        double s1 = 0, s2 = 0, sa = 0;
        for (uint32_t k = 0; k < n; ++k)
        {
            reference::BsdfSample bs;
            double x = 0, y = 0;
            if (b.sample(rng.uniform(), rng.uniform(), rng.uniform(), bs) && bs.wi.z > 0)
            {
                x = reference::evaluateModel(surf.bsdf, { 0, 0, 1 }, v, bs.wi).g * bs.wi.z / bs.pdf;
                // Adjoint: the fixed direction is the light, the integral runs over the view direction.
                y = reference::evaluateModel(surf.bsdf, { 0, 0, 1 }, bs.wi, v).g * bs.wi.z / bs.pdf;
            }
            s1 += x;
            s2 += x * x;
            sa += y;
        }
        adj[idx] = sa / n;
        alb[idx] = s1 / n;
        sd[idx] = std::sqrt(std::max(0.0, s2 / n - alb[idx] * alb[idx]) / n);
    });
    std::ostringstream md;
    md << "# v1 metal white furnace (base colour 1, metallic 1) [measured]\n\n"
          "`unx_study_material_layers v1albedo`. Directional albedo of `reference::evaluateModel` (the v1 model: F D V (1 + F0 (1/E - 1)), "
          "E = scene::model::directionalAlbedo) with F0 = 1, 4M importance samples per cell (standard error in parentheses, x1e-4). "
          "F = 1 makes the albedo E_true / E_table, so an albedo above 1 means the table E is below the integral of the model's own "
          "D V. Adjoint albedo: the same integral with the roles swapped (fixed light direction, integral over the view direction), "
          "which light tracing and the layer walk see; the compensation factor depends on the view angle only, so the model is not "
          "reciprocal and the adjoint albedo is not 1. Cells: albedo (se) / adjoint albedo / E_table.\n\n| roughness \\\\ mu |";
    for (double mu : mus) md << format(" %.2f |", mu);
    md << "\n|---|";
    for (int m = 0; m < NM; ++m) md << "---|";
    md << "\n";
    for (int r = 0; r < NR; ++r)
    {
        md << format("| %.2f |", rs[r]);
        for (int m = 0; m < NM; ++m)
            md << format(" %.4f (%.0f) / %.4f / %.4f |", alb[r * NM + m], sd[r * NM + m] * 1e4, adj[r * NM + m], scene::model::directionalAlbedo((float)mus[m], rs[r]));
        md << "\n";
    }
    writeTextFile(out, md.str());
    logf("%s", md.str().c_str());
}

// v1 metal multiple-scattering compensation candidates (core request after 289ac6a): uncoated GGX conductor with the v1
// Schlick Fresnel, measured against the microsurface multiple-scattering conductor (Heitz et al. 2016 random walk, the
// same Fresnel at every microfacet bounce). Definitions:
//   v1  F D V (1 + F0 (1/E(mu_v) - 1))                                  (current, not reciprocal)
//   K   F D V + F_ms (1 - E(mu_i)) (1 - E(mu_o)) / (pi (1 - E_avg)),     F_ms = F_avg^2 E_avg / (1 - F_avg (1 - E_avg)),
//       F_avg = F0 + (1 - F0) / 21 (cosine-weighted Schlick mean)       (additive Kulla-Conty)
//   G   F D V g(mu_i) g(mu_o), g solved per channel by symmetric Sinkhorn so every incidence bin's energy equals the
//       reference albedo                                                (multiplicative symmetric scale)
// E is the core single-scattering table (scene::model::directionalAlbedo). Metrics: adjoint albedo (fixed light,
// from the tables), forward albedo (fixed view, direct integration), lobe L1, white furnace / sun + sky sphere dE.
namespace cond
{
Rgb schlick(const Rgb& f0, double c)
{
    const float w = (float)std::pow(1 - std::clamp(c, 0.0, 1.0), 5.0);
    return f0 + (Rgb(1.0f) - f0) * w;
}
// Heitz conductor walk: arrival propagation direction wr (z < 0); returns the leaving direction (z > 0) and the
// Fresnel throughput. False when the order cap is hit.
bool walk(float3 wr, double a, const Rgb& f0, reference::Pcg32& rng, float3& out, Rgb& w)
{
    double hr = 1 + ms::invC1(0.999);
    w = Rgb(1.0f);
    for (int order = 0; order < 4096; ++order)
    {
        hr = ms::sampleHeight(wr, hr, rng.uniform(), a);
        if (std::isinf(hr))
        {
            out = wr;
            return wr.z > 0;
        }
        const float3 wi = -wr;
        const float3 m = ms::vndfAny(wi, a, rng.uniform(), rng.uniform());
        const double c = std::max(0.0, (double)dot(wi, m));
        w *= schlick(f0, c);
        wr = m * (float)(2 * c) - wi;
    }
    return false;
}

struct Def
{
    double a = 0, r = 0, Eavg = 0;
    Rgb f0, Fms;
    const std::vector<Rgb>* g = nullptr;
    int kind = 0;  // 0 v1, 1 K, 2 G, 3 single scattering only (f_1), 4 mix (1 - w) G + w K
    double w = 0;  // kind 4
    double E(double mu) const { return scene::model::directionalAlbedo((float)mu, (float)r); }
    // f(v = wo, l = wi)
    Rgb eval(float3 wo, float3 wi) const
    {
        if (wo.z <= 0 || wi.z <= 0) return {};
        if (kind == 0)
        {
            scene::model::Surface s;
            s.baseColor = { f0.r, f0.g, f0.b };
            s.metallic = 1;
            s.roughness = (float)r;
            return reference::evaluateModel(s, { 0, 0, 1 }, wo, wi);
        }
        const float3 h = normalize(wo + wi);
        const double dv = ggxD({ 0, 0, 1 }, h, a) * g2(wi.z, wo.z, a) / (4.0 * wi.z * wo.z);
        Rgb f = schlick(f0, dot(wo, h)) * (float)dv;
        if (kind == 1 && Eavg < 1)
            f += Fms * (float)((1 - E(wi.z)) * (1 - E(wo.z)) / (kPi * (1 - Eavg)));
        if (kind == 2 && g)
            for (int c = 0; c < 3; ++c) (&f.r)[c] *= (float)(Definitions::binLerp(*g, wi.z, c) * Definitions::binLerp(*g, wo.z, c));
        if (kind == 4 && g)
        {
            // Both parts put the reference energy on every incidence (G exactly, K up to the E table), so their convex
            // mix does too and stays reciprocal; w moves the multiply scattered light from the lobe to the diffuse shape.
            Rgb out;
            const double kd = Eavg < 1 ? (1 - E(wi.z)) * (1 - E(wo.z)) / (kPi * (1 - Eavg)) : 0.0;
            for (int c = 0; c < 3; ++c)
            {
                const double fs = (&f.r)[c];
                const double fg = fs * Definitions::binLerp(*g, wi.z, c) * Definitions::binLerp(*g, wo.z, c);
                const double fk = fs + (&Fms.r)[c] * kd;
                (&out.r)[c] = (float)((1 - w) * fg + w * fk);
            }
            return out;
        }
        return f;
    }
};

// Table of f(v = exit, l = incidence) cos dω per (incidence bin, exit bin), by mixture sampling of the exit direction.
void defTable(const Def& d, uint32_t samples, Table& T)
{
    parallelFor(NI, [&](uint32_t ii) {
        const float3 wi = incident((int)ii);
        reference::Pcg32 rng(0x3E7A + ii, 17 + (uint64_t)d.kind);
        for (uint32_t s = 0; s < samples; ++s)
        {
            float3 wo;
            if (rng.uniform() < 0.75)
            {
                const float3 h = vndf(wi, d.a, rng.uniform(), rng.uniform());
                wo = h * (2 * dot(wi, h)) - wi;
            }
            else wo = cosineDir(rng.uniform(), rng.uniform());
            if (wo.z <= 1e-6f) continue;
            const float3 h = normalize(wo + wi);
            const double pdf = 0.75 * g1(wi.z, d.a) * ggxD({ 0, 0, 1 }, h, d.a) / (4 * wi.z) + 0.25 * wo.z / kPi;
            if (!(pdf > 0)) continue;
            int bt, bp;
            binOf(wo, bt, bp);
            T.at((int)ii, bt, bp) += d.eval(wo, wi) * (float)(wo.z / pdf / samples);
        }
    });
}

// Forward albedo: fixed view v = incident(i), integral over the light direction.
std::vector<Rgb> forwardAlbedo(const Def& d, uint32_t samples)
{
    std::vector<Rgb> out(NI);
    parallelFor(NI, [&](uint32_t ii) {
        const float3 v = incident((int)ii);
        reference::Pcg32 rng(0x4F0D + ii, 19 + (uint64_t)d.kind);
        Rgb sum;
        for (uint32_t s = 0; s < samples; ++s)
        {
            float3 l;
            if (rng.uniform() < 0.75)
            {
                const float3 h = vndf(v, d.a, rng.uniform(), rng.uniform());
                l = h * (2 * dot(v, h)) - v;
            }
            else l = cosineDir(rng.uniform(), rng.uniform());
            if (l.z <= 1e-6f) continue;
            const float3 h = normalize(l + v);
            const double pdf = 0.75 * g1(v.z, d.a) * ggxD({ 0, 0, 1 }, h, d.a) / (4 * v.z) + 0.25 * l.z / kPi;
            if (!(pdf > 0)) continue;
            sum += d.eval(v, l) * (float)(l.z / pdf / samples);
        }
        out[ii] = sum;
    });
    return out;
}
} // namespace cond

void metalMsStudy(const std::string& out, uint32_t photons)
{
    struct Col
    {
        const char* name;
        Rgb f0;
    };
    // Gold and copper F0 from the fitted presets (metal_presets.md), normal-incidence reflectance in air.
    const Col cols[] = { { "white (F0 1)", Rgb(1.0f) }, { "gold F0 (0.996, 0.733, 0.359)", Rgb(0.996f, 0.733f, 0.359f) }, { "copper F0 (0.912, 0.623, 0.518)", Rgb(0.912f, 0.623f, 0.518f) } };
    const double rs[] = { 0.05, 0.1, 0.2, 0.3, 0.45, 0.6, 0.8, 1.0 };
    const std::vector<EnvSample> furnace = environment(true), sky = environment(false);
    double skyWhite = 0;
    {
        const float3 s = normalize(float3{ 0.45f, 0.62f, 0.64f });
        for (const EnvSample& e : sky)
            if (dot(e.dir, s) > 0) skyWhite += e.L.luminance() * dot(e.dir, s) * e.dw / kPi;
    }
    std::ostringstream md;
    md << "# v1 metal multiple-scattering compensation: candidates K and G vs the MS conductor [measured]\n\n"
          "`unx_study_material_layers metalms`. Uncoated GGX conductor, Schlick Fresnel with the given F0 in every definition and in "
          "the reference (so only the multiple-scattering handling differs). Reference: microsurface multiple-scattering walk (Heitz "
          "2016), reciprocal, forward = adjoint. Photons per incidence bin: "
       << photons
       << ". Adjoint albedo = fixed light, energy over all exit directions (from the tables); forward albedo = fixed view, "
          "integral over the light (direct integration). Worst relative error over theta 0-89.5 deg (@theta); '(<=85°)' = worst "
          "over theta <= 85 deg, excluding the grazing bins where the E table resolution limits. Criteria as the layer study: "
          "albedo rel <= 1 % (core, this request), L1 <= 0.05, dE mean <= 1.0 / P99 <= 2.3.\n\n"
          "| F0 | r | def | adjoint albedo err (all / <=85°) | forward albedo err (all / <=85°) | worst L1 | L1 noise | furnace dE mean / P99 | sun+sky dE mean / P99 | render noise P99 furnace / sky | reference albedo 0 / 60 / 85 / 89.5° |\n"
          "|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const Col& col : cols)
        for (double r : rs)
        {
            const double a = scene::model::alphaFromRoughness((float)r);
            // Reference tables.
            Table phys, half;
            parallelFor(NI, [&](uint32_t ii) {
                const float3 wi = incident((int)ii);
                reference::Pcg32 rng(0x7A11 + ii, 23 + (uint64_t)(r * 1000));
                for (uint32_t s = 0; s < photons; ++s)
                {
                    float3 o;
                    Rgb w;
                    if (!cond::walk(-wi, a, col.f0, rng, o, w)) continue;
                    int bt, bp;
                    binOf(o, bt, bp);
                    const Rgb add = w * (1.0f / photons);
                    phys.at((int)ii, bt, bp) += add;
                    if (s & 1) half.at((int)ii, bt, bp) += add * 2.0f;
                }
            });
            std::vector<Rgb> refAlb(NI);
            for (int i = 0; i < NI; ++i)
                for (int t = 0; t < NT; ++t)
                    for (int p = 0; p < NP; ++p) refAlb[i] += phys.at(i, t, p);
            const std::vector<Rgb3> pf = renderSphere(phys, furnace, 64), ps = renderSphere(phys, sky, 64);
            // Definitions.
            cond::Def base;
            base.a = a;
            base.r = r;
            base.f0 = col.f0;
            {
                double e = 0;
                for (int i = 0; i < 1024; ++i)
                {
                    const double mu = (i + 0.5) / 1024;
                    e += base.E(mu) * 2 * mu / 1024;
                }
                base.Eavg = e;
                const Rgb favg = col.f0 + (Rgb(1.0f) - col.f0) * (1.0f / 21);
                for (int c = 0; c < 3; ++c)
                {
                    const double f = (&favg.r)[c];
                    (&base.Fms.r)[c] = (float)(f * f * e / (1 - f * (1 - e)));
                }
            }
            std::vector<Rgb> g(NI, Rgb(1.0f));
            {
                cond::Def ss = base;
                ss.kind = 3;
                Table tf;
                cond::defTable(ss, photons / 2, tf);
                for (int c = 0; c < 3; ++c)
                {
                    std::vector<double> M((size_t)NI * NT, 0.0), gg(NI, 1.0);
                    for (int i = 0; i < NI; ++i)
                        for (int t = 0; t < NT; ++t)
                            for (int p = 0; p < NP; ++p) M[(size_t)i * NT + t] += (&tf.at(i, t, p).r)[c];
                    for (int it = 0; it < 500; ++it)
                        for (int i = 0; i < NI; ++i)
                        {
                            double m = 0;
                            for (int t = 0; t < NT; ++t) m += M[(size_t)i * NT + t] * gg[t];
                            const double target = (&refAlb[i].r)[c];
                            if (m > 1e-9 && target > 0) gg[i] = std::sqrt(gg[i] * target / m);
                        }
                    for (int i = 0; i < NI; ++i) (&g[i].r)[c] = (float)gg[i];
                }
            }
            const char* names[3] = { "v1", "K", "G" };
            for (int kind = 0; kind < 3; ++kind)
            {
                cond::Def d = base;
                d.kind = kind;
                d.g = &g;
                Table T;
                cond::defTable(d, photons, T);
                const Metrics m = compare(T, phys, half, furnace, sky, skyWhite, &pf, &ps);
                const std::vector<Rgb> fwd = cond::forwardAlbedo(d, photons);
                double adjAll = 0, adjIn = 0, fwdAll = 0, fwdIn = 0;
                int adjAt = 0, fwdAt = 0;
                for (int i = 0; i < NI; ++i)
                {
                    double adjSum = 0;
                    for (int t = 0; t < NT; ++t)
                        for (int p = 0; p < NP; ++p) adjSum += T.at(i, t, p).luminance();
                    const double ref = refAlb[i].luminance();
                    const double ea = std::fabs(adjSum - ref) / ref, ef = std::fabs(fwd[i].luminance() - ref) / ref;
                    if (ea > adjAll)
                    {
                        adjAll = ea;
                        adjAt = i;
                    }
                    if (ef > fwdAll)
                    {
                        fwdAll = ef;
                        fwdAt = i;
                    }
                    if (i + 0.5 <= 85)
                    {
                        adjIn = std::max(adjIn, ea);
                        fwdIn = std::max(fwdIn, ef);
                    }
                }
                char b[640];
                std::snprintf(b, sizeof b,
                              "| %s | %.2f | %s | %.1f%% @%.0f° / %.1f%% | %.1f%% @%.0f° / %.1f%% | %.3f @%.0f° | %.3f | %.2f / %.2f | %.2f / %.2f | %.2f / %.2f | %.3f / %.3f / %.3f / %.3f |\n",
                              col.name, r, names[kind], 100 * adjAll, adjAt + 0.5, 100 * adjIn, 100 * fwdAll, fwdAt + 0.5, 100 * fwdIn, m.l1Max, m.l1WorstTheta, m.l1Noise, m.furnaceMean,
                              m.furnaceP99, m.skyMean, m.skyP99, m.furnaceNoiseP99, m.skyNoiseP99, refAlb[0].luminance(), refAlb[60].luminance(), refAlb[85].luminance(),
                              refAlb[89].luminance());
                md << b;
                logf("%s", b);
            }
            writeTextFile(out, md.str());
        }
}

// Table for candidate G (v1 metal): f = F D V g(mu_v) g(mu_l), g(mu; r, rho) solved per grid cell by symmetric Sinkhorn so
// that the energy leaving every incidence equals the multiple-scattering conductor's albedo (Heitz walk) for Schlick
// Fresnel with F0 = rho. Grid: mu = (k / 15)^2 (k = 0 at 1e-4), r = (j / 31)^2 (alpha = r^2), rho = 0.04 + 0.96 i / 7; row-major
// [rho][r][mu]. r = 0 is a mirror (no multiple scattering): g = 1. Validation: the published table looked up
// trilinearly (as a shader would) at off-grid roughness and colour, against the reference.
namespace gtab
{
constexpr int NMU = 16, NR = 32, NRHO = 8;
// rho axis from 0.04 (the common dielectric F0): as F0 -> 0 the single-scattering energy near normal incidence vanishes
// faster than the multiple-scattering part, so g is ill-conditioned there (g(mu 1) ~ 2-3 at F0 = 0), and a grid point at
// 0 would pull F0 = 0.04 far off under linear interpolation. Lookups clamp rho below 0.04 to the first row.
constexpr double kRho0 = 0.04;
// mu axis uniform in sqrt(mu) (mu_k = (k / 15)^2): g changes fastest at grazing incidence, where a uniform mu grid has a
// single interval over 86-90 deg (validation: 5.9 % albedo error at r 0.07, 89.5 deg).
double muAt(int k) { const double t = (double)k / (NMU - 1); return std::max(t * t, 1e-4); }
// r axis uniform in sqrt(r) (r_j = (j / 31)^2): between the mirror (g = 1) and slightly rough metal, g at grazing grows
// non-linearly (shadowing goes with alpha tan(theta)); validation with uniform r: 4.8 % at r 0.07, 2.8 % at r 0.2 (87.5 deg).
double rAt(int j) { const double t = (double)j / (NR - 1); return t * t; }
double lookup(const std::vector<float>& t, double mu, double r, double rho)
{
    const double x = std::sqrt(std::clamp(mu, 0.0, 1.0)) * (NMU - 1), y = std::sqrt(std::clamp(r, 0.0, 1.0)) * (NR - 1), z = std::clamp((rho - kRho0) / (1 - kRho0), 0.0, 1.0) * (NRHO - 1);
    const int x0 = std::min((int)x, NMU - 2), y0 = std::min((int)y, NR - 2), z0 = std::min((int)z, NRHO - 2);
    const double fx = x - x0, fy = y - y0, fz = z - z0;
    auto at = [&](int k, int j, int i) { return (double)t[((size_t)i * NR + j) * NMU + k]; };
    double v = 0;
    for (int dz = 0; dz < 2; ++dz)
        for (int dy = 0; dy < 2; ++dy)
            for (int dx = 0; dx < 2; ++dx)
                v += at(x0 + dx, y0 + dy, z0 + dz) * (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy) * (dz ? fz : 1 - fz);
    return v;
}
// g on the 90 incidence bins for one (r, rho) cell.
std::vector<double> solveCell(double r, double rho, uint32_t photons)
{
    std::vector<double> g(NI, 1.0);
    if (r <= 0) return g;
    const double a = scene::model::alphaFromRoughness((float)r);
    const Rgb f0((float)rho);
    std::vector<double> E(NI, 0.0);
    parallelFor(NI, [&](uint32_t ii) {
        reference::Pcg32 rng(0x6AB1 + ii, 31 + (uint64_t)(r * 1000) * 131 + (uint64_t)(rho * 1000));
        double sum = 0;
        for (uint32_t s = 0; s < photons; ++s)
        {
            float3 o;
            Rgb w;
            if (cond::walk(-incident((int)ii), a, f0, rng, o, w)) sum += w.g;
        }
        E[ii] = sum / photons;
    });
    cond::Def ss;
    ss.a = a;
    ss.r = r;
    ss.f0 = f0;
    ss.kind = 3;
    Table tf;
    cond::defTable(ss, photons, tf);
    std::vector<double> M((size_t)NI * NT, 0.0);
    for (int i = 0; i < NI; ++i)
        for (int t = 0; t < NT; ++t)
            for (int p = 0; p < NP; ++p) M[(size_t)i * NT + t] += tf.at(i, t, p).g;
    for (int it = 0; it < 500; ++it)
        for (int i = 0; i < NI; ++i)
        {
            double m = 0;
            for (int t = 0; t < NT; ++t) m += M[(size_t)i * NT + t] * g[t];
            if (m > 1e-12 && E[i] > 0) g[i] = std::sqrt(g[i] * E[i] / m);
        }
    return g;
}
double binLerp1(const std::vector<double>& v, double mu)
{
    const double th = std::acos(std::clamp(mu, 0.0, 1.0)) * 180 / kPi - 0.5;
    const int i0 = std::clamp((int)std::floor(th), 0, NI - 1), i1 = std::min(i0 + 1, NI - 1);
    const double t = std::clamp(th - i0, 0.0, 1.0);
    return v[i0] + t * (v[i1] - v[i0]);
}
} // namespace gtab

void metalGTable(const std::string& out, uint32_t photons)
{
    using namespace gtab;
    std::vector<float> table((size_t)NRHO * NR * NMU, 1.0f);
    for (int i = 0; i < NRHO; ++i)
        for (int j = 0; j < NR; ++j)
        {
            const double rho = kRho0 + (1 - kRho0) * i / (NRHO - 1), r = rAt(j);
            const std::vector<double> g = solveCell(r, rho, photons);
            for (int k = 0; k < NMU; ++k)
            {
                const double mu = muAt(k);
                table[((size_t)i * NR + j) * NMU + k] = (float)binLerp1(g, mu);
            }
            logf("  cell rho %.3f r %.3f: g(mu 1e-4, 0.2, 0.5, 1) = %.4f %.4f %.4f %.4f\n", rho, r, binLerp1(g, 1e-4), binLerp1(g, 0.2), binLerp1(g, 0.5), binLerp1(g, 1.0));
        }
    std::ostringstream s;
    s << "// v1 metal multiple-scattering scale for candidate G (C track, unx_study_material_layers metalgtable, photons " << photons
      << ").\n"
         "// f(v, l) = F(v.h) D V g(mu_v; r, rho) g(mu_l; r, rho), per colour channel with rho = that channel's F0 (Schlick\n"
         "// F0 + (1 - F0)(1 - v.h)^5; with Schlick the whole Fresnel curve is set by F0, so rho = F0 exactly). g is solved so\n"
         "// the energy leaving every incidence equals the multiple-scattering GGX conductor (Heitz 2016 walk, same Fresnel\n"
         "// at every bounce): reciprocal, forward and adjoint albedo equal to the reference.\n"
         "// Layout: row-major [rho][r][mu], 8 x 32 x 16. mu = cos(theta) = (k / 15)^2 (uniform in sqrt(mu); k = 0 evaluated at\n"
         "// 1e-4), r = perceptual roughness (j / 31)^2 (uniform in sqrt(r); alpha = r^2),\n"
         "// rho = 0.04 + 0.96 i / 7. Lookup: trilinear in (sqrt(mu), sqrt(r), (rho - 0.04) / 0.96) scaled to (15, 31, 7), clamped\n"
         "// to the grid (rho < 0.04 uses the first row: g is ill-conditioned as F0 -> 0). r = 0: g = 1.\n";
    s << "const float kMetalScatterScale[" << table.size() << "] = {";
    for (size_t i = 0; i < table.size(); ++i) s << (i % 8 == 0 ? "\n    " : " ") << format("%.6ff,", table[i]);
    s << "\n};\n";
    writeTextFile(out, s.str());

    // Validation: the published table, trilinear, at off-grid roughness and colours.
    const double rs[] = { 0.03, 0.07, 0.2, 0.45, 0.6, 0.7, 0.8, 0.93 };
    const Rgb cols[] = { Rgb(1.0f), Rgb(0.996f, 0.733f, 0.359f), Rgb(0.912f, 0.623f, 0.518f), Rgb(0.04f), Rgb(0.02f) };
    const char* colNames[] = { "white", "gold", "copper", "dielectric F0 0.04", "water F0 0.02 (below the grid)" };
    std::ostringstream md;
    md << "# kMetalScatterScale validation [measured]\n\nThe published table (" << out
       << ") looked up trilinearly per channel (rho = channel F0) vs the MS conductor. Worst relative luminance albedo error "
          "over incidence 0-89.5 deg (forward = fixed view, adjoint = fixed light). Validation photons "
       << photons << " per incidence bin.\n\n| F0 | r | adjoint worst (all / <=85°) | forward worst (all / <=85°) |\n|---|---|---|---|\n";
    for (int c = 0; c < 5; ++c)
        for (double r : rs)
        {
            const double a = scene::model::alphaFromRoughness((float)r);
            std::vector<double> refAlb(NI, 0.0);
            parallelFor(NI, [&](uint32_t ii) {
                reference::Pcg32 rng(0x7E57 + ii, 41 + (uint64_t)(r * 1000));
                Rgb sum;
                for (uint32_t s2 = 0; s2 < photons; ++s2)
                {
                    float3 o;
                    Rgb w;
                    if (cond::walk(-incident((int)ii), a, cols[c], rng, o, w)) sum += w;
                }
                refAlb[ii] = sum.luminance() / photons;
            });
            // Definition with the table: per-channel g from the lookup, stored per incidence bin for the Def evaluator.
            std::vector<Rgb> g(NI);
            for (int i = 0; i < NI; ++i)
            {
                const double mu = incident(i).z;
                for (int ch = 0; ch < 3; ++ch) (&g[i].r)[ch] = (float)lookup(table, mu, r, (&cols[c].r)[ch]);
            }
            cond::Def d;
            d.a = a;
            d.r = r;
            d.f0 = cols[c];
            d.kind = 2;
            d.g = &g;
            Table T;
            cond::defTable(d, photons, T);
            const std::vector<Rgb> fwd = cond::forwardAlbedo(d, photons);
            double adj = 0, fw = 0, adj85 = 0, fw85 = 0;
            int adjAt = 0, fwAt = 0;
            for (int i = 0; i < NI; ++i)
            {
                double sumT = 0;
                for (int t = 0; t < NT; ++t)
                    for (int p = 0; p < NP; ++p) sumT += T.at(i, t, p).luminance();
                const double ea = std::fabs(sumT / refAlb[i] - 1), ef = std::fabs(fwd[i].luminance() / refAlb[i] - 1);
                if (i + 0.5 <= 85)
                {
                    adj85 = std::max(adj85, ea);
                    fw85 = std::max(fw85, ef);
                }
                if (ea > adj)
                {
                    adj = ea;
                    adjAt = i;
                }
                if (ef > fw)
                {
                    fw = ef;
                    fwAt = i;
                }
            }
            md << format("| %s | %.2f | %.2f%% @%.1f° / %.2f%% | %.2f%% @%.1f° / %.2f%% |\n", colNames[c], r, 100 * adj, adjAt + 0.5, 100 * adj85, 100 * fw, fwAt + 0.5, 100 * fw85);
            logf("  validate %s r %.2f: adjoint %.2f%% @%.1f (<=85: %.2f%%), forward %.2f%% @%.1f (<=85: %.2f%%)\n", colNames[c], r, 100 * adj, adjAt + 0.5, 100 * adj85,
                 100 * fw, fwAt + 0.5, 100 * fw85);
        }
    const std::string mdPath = out.substr(0, out.find_last_of('.')) + "_validation.md";
    writeTextFile(mdPath, md.str());
}


// Shape study for the metal multiple-scattering term: f = (1 - w) G + w K on white F0 = 1 at rough r, against the MS
// conductor. Energy is exact for every w; w only moves the multiply scattered light between the single-scattering
// lobe (G) and the separable diffuse shape (K).
void metalMixStudy(const std::string& out, uint32_t photons)
{
    std::vector<double> rs = { 0.3, 0.45, 0.6, 0.8, 1.0 };
    {
        // Optional subset (rerunning part of the sweep): UNX_MIX_RS = comma-separated roughness values.
        char* v = nullptr;
        size_t n = 0;
        if (_dupenv_s(&v, &n, "UNX_MIX_RS") == 0 && v)
        {
            rs.clear();
            std::stringstream ss(v);
            std::string item;
            while (std::getline(ss, item, ',')) rs.push_back(std::stod(item));
        }
        free(v);
    }
    const double ws[] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    const Rgb f0(1.0f);
    const std::vector<EnvSample> furnace = environment(true), sky = environment(false);
    double skyWhite = 0;
    {
        const float3 sd = normalize(float3{ 0.45f, 0.62f, 0.64f });
        for (const EnvSample& e : sky)
            if (dot(e.dir, sd) > 0) skyWhite += e.L.luminance() * dot(e.dir, sd) * e.dw / kPi;
    }
    std::ostringstream md;
    md << "# Metal multiple scattering: mix (1 - w) G + w K vs the MS conductor, F0 = 1 [measured]\n\n"
          "`unx_study_material_layers metalmix`. Photons per incidence bin: "
       << photons
       << ". Columns as metal_ms.md (adjoint albedo worst, L1 worst, furnace / sun+sky dE mean / P99, render noise P99).\n\n"
          "| r | w | adjoint albedo worst | worst L1 | L1 noise | furnace dE | sun+sky dE | render noise P99 furnace / sky |\n|---|---|---|---|---|---|---|---|\n";
    for (double r : rs)
    {
        const double a = scene::model::alphaFromRoughness((float)r);
        Table phys, half;
        parallelFor(NI, [&](uint32_t ii) {
            const float3 wi = incident((int)ii);
            reference::Pcg32 rng(0x7A11 + ii, 23 + (uint64_t)(r * 1000));
            for (uint32_t k = 0; k < photons; ++k)
            {
                float3 o;
                Rgb wt;
                if (!cond::walk(-wi, a, f0, rng, o, wt)) continue;
                int bt, bp;
                binOf(o, bt, bp);
                const Rgb add = wt * (1.0f / photons);
                phys.at((int)ii, bt, bp) += add;
                if (k & 1) half.at((int)ii, bt, bp) += add * 2.0f;
            }
        });
        std::vector<Rgb> refAlb(NI);
        for (int i = 0; i < NI; ++i)
            for (int t = 0; t < NT; ++t)
                for (int q = 0; q < NP; ++q) refAlb[i] += phys.at(i, t, q);
        const std::vector<Rgb3> pf = renderSphere(phys, furnace, 64), ps = renderSphere(phys, sky, 64);
        cond::Def base;
        base.a = a;
        base.r = r;
        base.f0 = f0;
        {
            double e = 0;
            for (int i = 0; i < 1024; ++i)
            {
                const double mu = (i + 0.5) / 1024;
                e += base.E(mu) * 2 * mu / 1024;
            }
            base.Eavg = e;
            const double fa = 1.0;  // F0 = 1: F_avg = 1
            base.Fms = Rgb((float)(fa * fa * e / (1 - fa * (1 - e))));
        }
        std::vector<Rgb> g(NI, Rgb(1.0f));
        {
            cond::Def ss = base;
            ss.kind = 3;
            Table tf;
            cond::defTable(ss, photons / 2, tf);
            std::vector<double> M((size_t)NI * NT, 0.0), gg(NI, 1.0);
            for (int i = 0; i < NI; ++i)
                for (int t = 0; t < NT; ++t)
                    for (int q = 0; q < NP; ++q) M[(size_t)i * NT + t] += tf.at(i, t, q).g;
            for (int it = 0; it < 500; ++it)
                for (int i = 0; i < NI; ++i)
                {
                    double m = 0;
                    for (int t = 0; t < NT; ++t) m += M[(size_t)i * NT + t] * gg[t];
                    if (m > 1e-9 && refAlb[i].g > 0) gg[i] = std::sqrt(gg[i] * refAlb[i].g / m);
                }
            for (int i = 0; i < NI; ++i) g[i] = Rgb((float)gg[i]);
        }
        for (double w : ws)
        {
            cond::Def d = base;
            d.kind = 4;
            d.w = w;
            d.g = &g;
            Table T;
            cond::defTable(d, photons, T);
            const Metrics m = compare(T, phys, half, furnace, sky, skyWhite, &pf, &ps);
            double adj = 0;
            int adjAt = 0;
            for (int i = 0; i < NI; ++i)
            {
                double sumT = 0;
                for (int t = 0; t < NT; ++t)
                    for (int q = 0; q < NP; ++q) sumT += T.at(i, t, q).luminance();
                const double e = std::fabs(sumT / refAlb[i].luminance() - 1);
                if (e > adj)
                {
                    adj = e;
                    adjAt = i;
                }
            }
            const std::string row = format("| %.2f | %.2f | %.1f%% @%.0f° | %.3f @%.0f° | %.3f | %.2f / %.2f | %.2f / %.2f | %.2f / %.2f |\n", r, w, 100 * adj, adjAt + 0.5,
                                           m.l1Max, m.l1WorstTheta, m.l1Noise, m.furnaceMean, m.furnaceP99, m.skyMean, m.skyP99, m.furnaceNoiseP99, m.skyNoiseP99);
            md << row;
            logf("%s", row.c_str());
            writeTextFile(out, md.str());
        }
    }
}

// Final check of the metal multiple-scattering term as it ships: f = (1 - w(r)) F D V g(mu_v) g(mu_l) + w(r) (F D V + F_ms
// (1 - E(mu_v))(1 - E(mu_l)) / (pi (1 - E_avg))), with g from the published table (read back from the .inc file,
// trilinear per channel, rho = channel F0), E / E_avg from the core table, F_avg = F0 + (1 - F0) / 21, and w(r)
// piecewise linear through the points given as "r:w,r:w,..." (UNX_W_POINTS). Against the MS conductor at off-grid
// roughness and colours: adjoint and forward albedo, L1, white furnace and sun + sky dE.
void metalFinal(const std::string& out, uint32_t photons)
{
    // Table.
    std::vector<float> table;
    {
        const std::string incPath = "Results/C/MaterialLayers/tables/metal_scatter_scale.inc";
        std::ifstream f(incPath);
        if (!f) fail("metalfinal: cannot read %s", incPath.c_str());
        const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        size_t p = text.find('{');
        while (p != std::string::npos && p < text.size())
        {
            const size_t q = text.find_first_of("-0123456789.", p);
            if (q == std::string::npos || text.find('}', p) < q) break;
            char* end = nullptr;
            table.push_back(std::strtof(text.c_str() + q, &end));
            p = (size_t)(end - text.c_str());
        }
        if (table.size() != (size_t)gtab::NRHO * gtab::NR * gtab::NMU) fail("metalfinal: table has %zu values", table.size());
    }
    // w(r).
    std::vector<std::pair<double, double>> wp;
    {
        char* v = nullptr;
        size_t n = 0;
        std::string spec;
        if (_dupenv_s(&v, &n, "UNX_W_POINTS") == 0 && v) spec = v;
        free(v);
        if (spec.empty()) fail("metalfinal: set UNX_W_POINTS=r:w,r:w,...");
        std::stringstream ss(spec);
        std::string item;
        while (std::getline(ss, item, ','))
        {
            const size_t c = item.find(':');
            wp.push_back({ std::stod(item.substr(0, c)), std::stod(item.substr(c + 1)) });
        }
        std::sort(wp.begin(), wp.end());
    }
    auto wOf = [&](double r) {
        if (r <= wp.front().first) return wp.front().second;
        if (r >= wp.back().first) return wp.back().second;
        for (size_t i = 1; i < wp.size(); ++i)
            if (r <= wp[i].first)
            {
                const double t = (r - wp[i - 1].first) / (wp[i].first - wp[i - 1].first);
                return wp[i - 1].second + t * (wp[i].second - wp[i - 1].second);
            }
        return wp.back().second;
    };
    const double rs[] = { 0.03, 0.1, 0.25, 0.4, 0.55, 0.7, 0.9 };
    const Rgb cols[] = { Rgb(1.0f), Rgb(0.996f, 0.733f, 0.359f), Rgb(0.912f, 0.623f, 0.518f), Rgb(0.04f) };
    const char* colNames[] = { "white", "gold", "copper", "dielectric F0 0.04 (specular lobe only)" };
    const std::vector<EnvSample> furnace = environment(true), sky = environment(false);
    double skyWhite = 0;
    {
        const float3 sd = normalize(float3{ 0.45f, 0.62f, 0.64f });
        for (const EnvSample& e : sky)
            if (dot(e.dir, sd) > 0) skyWhite += e.L.luminance() * dot(e.dir, sd) * e.dw / kPi;
    }
    std::ostringstream md;
    md << "# Metal multiple scattering as shipped: (1 - w) G(table) + w K vs the MS conductor [measured]\n\n"
          "`unx_study_material_layers metalfinal` with UNX_W_POINTS as below. g: published kMetalScatterScale (trilinear, "
          "per channel, rho = F0); E: core table. Off-grid roughness. Photons per incidence bin: "
       << photons << ".\n\nw(r) points:";
    for (auto& q : wp) md << format(" (%.2f, %.2f)", q.first, q.second);
    md << "\n\n| F0 | r | w | adjoint albedo worst (all / <=85°) | forward worst (all / <=85°) | worst L1 | L1 noise | furnace dE | sun+sky dE | render noise P99 |\n"
          "|---|---|---|---|---|---|---|---|---|---|\n";
    for (int c = 0; c < 4; ++c)
        for (double r : rs)
        {
            const double a = scene::model::alphaFromRoughness((float)r);
            const Rgb f0 = cols[c];
            Table phys, half;
            parallelFor(NI, [&](uint32_t ii) {
                const float3 wi = incident((int)ii);
                reference::Pcg32 rng(0x51A1 + ii, 61 + (uint64_t)(r * 1000) + 7 * (uint64_t)c);
                for (uint32_t k = 0; k < photons; ++k)
                {
                    float3 o;
                    Rgb wt;
                    if (!cond::walk(-wi, a, f0, rng, o, wt)) continue;
                    int bt, bp;
                    binOf(o, bt, bp);
                    const Rgb add = wt * (1.0f / photons);
                    phys.at((int)ii, bt, bp) += add;
                    if (k & 1) half.at((int)ii, bt, bp) += add * 2.0f;
                }
            });
            std::vector<double> refAlb(NI, 0.0);
            for (int i = 0; i < NI; ++i)
                for (int t = 0; t < NT; ++t)
                    for (int q = 0; q < NP; ++q) refAlb[i] += phys.at(i, t, q).luminance();
            const std::vector<Rgb3> pf = renderSphere(phys, furnace, 64), ps = renderSphere(phys, sky, 64);
            cond::Def d;
            d.a = a;
            d.r = r;
            d.f0 = f0;
            d.kind = 4;
            d.w = wOf(r);
            {
                double e = 0;
                for (int i = 0; i < 1024; ++i)
                {
                    const double mu = (i + 0.5) / 1024;
                    e += d.E(mu) * 2 * mu / 1024;
                }
                d.Eavg = e;
                for (int ch = 0; ch < 3; ++ch)
                {
                    const double f = (&f0.r)[ch] + (1 - (&f0.r)[ch]) / 21.0;
                    (&d.Fms.r)[ch] = (float)(f * f * e / (1 - f * (1 - e)));
                }
            }
            std::vector<Rgb> g(NI);
            for (int i = 0; i < NI; ++i)
                for (int ch = 0; ch < 3; ++ch) (&g[i].r)[ch] = (float)gtab::lookup(table, incident(i).z, r, (&f0.r)[ch]);
            d.g = &g;
            Table T;
            cond::defTable(d, photons, T);
            const Metrics m = compare(T, phys, half, furnace, sky, skyWhite, &pf, &ps);
            const std::vector<Rgb> fwd = cond::forwardAlbedo(d, photons);
            double adj = 0, fw = 0, adj85 = 0, fw85 = 0;
            int adjAt = 0, fwAt = 0;
            for (int i = 0; i < NI; ++i)
            {
                double sumT = 0;
                for (int t = 0; t < NT; ++t)
                    for (int q = 0; q < NP; ++q) sumT += T.at(i, t, q).luminance();
                const double ea = std::fabs(sumT / refAlb[i] - 1), ef = std::fabs(fwd[i].luminance() / refAlb[i] - 1);
                if (ea > adj)
                {
                    adj = ea;
                    adjAt = i;
                }
                if (ef > fw)
                {
                    fw = ef;
                    fwAt = i;
                }
                if (i + 0.5 <= 85)
                {
                    adj85 = std::max(adj85, ea);
                    fw85 = std::max(fw85, ef);
                }
            }
            const std::string row = format("| %s | %.2f | %.2f | %.1f%% @%.0f° / %.1f%% | %.1f%% @%.0f° / %.1f%% | %.3f @%.0f° | %.3f | %.2f / %.2f | %.2f / %.2f | %.2f / %.2f |\n",
                                           colNames[c], r, d.w, 100 * adj, adjAt + 0.5, 100 * adj85, 100 * fw, fwAt + 0.5, 100 * fw85, m.l1Max, m.l1WorstTheta, m.l1Noise,
                                           m.furnaceMean, m.furnaceP99, m.skyMean, m.skyP99, m.furnaceNoiseP99, m.skyNoiseP99);
            md << row;
            logf("%s", row.c_str());
            writeTextFile(out, md.str());
        }
}

// Coat + film (design 3, item 3): film under the coat (outer index 1.5); the physical base uses the exact spectral film
// reflectance, the definition method (a).
void coatFilmStudy(const std::string& out, uint32_t photons)
{
    struct Case
    {
        const char* name;
        scene::model::Surface base;
        double nf, d;
        Substrate sub;
    };
    Substrate gold = metal("gold", "Au_johnson_christy_um_n_k.txt");
    fitRgb(gold, 1.0);
    auto mk = [](float3 c, float r, float m) {
        scene::model::Surface s;
        s.baseColor = c;
        s.roughness = r;
        s.metallic = m;
        return s;
    };
    std::vector<Case> cases = {
        { "film 2.4 / 300 nm on design default (1.5, 2), base r 0.3", mk({ 0.9f, 0.9f, 0.9f }, 0.3f, 1), 2.4, 300, constantSubstrate("default", 1.5, 2) },
        { "film 1.5 / 500 nm on fitted gold, base r 0.2", mk({ 0.9f, 0.7f, 0.3f }, 0.2f, 1), 1.5, 500, gold },
        { "film 2.4 / 150 nm on red paint dielectric (1.5, 0), base r 0.5", mk({ 0.6f, 0.05f, 0.05f }, 0.5f, 0), 2.4, 150, constantSubstrate("glass", 1.5, 0) },
    };
    const std::vector<EnvSample> furnace = environment(true), sky = environment(false);
    double skyWhite = 0;
    {
        const float3 s = normalize(float3{ 0.45f, 0.62f, 0.64f });
        for (const EnvSample& e : sky)
            if (dot(e.dir, s) > 0) skyWhite += e.L.luminance() * dot(e.dir, s) * e.dw / kPi;
    }
    std::ostringstream md;
    md << "# Clearcoat R1 + thin film under the coat vs physical model [measured]\n\n"
          "Coat r_c 0.12 over a filmed base (outer index 1.5). Physical: layer model (coat with microsurface multiple scattering) "
          "with the exact spectral film reflectance in "
          "the base specular; definition: R1 with film method (a). Same metrics and criteria as clearcoat_r1.md.\n\n"
          "| case | r_c | definition | worst albedo (rel @theta, abs) | albedo rel at 0/30/60/75/85° | worst L1 | L1 noise | furnace dE mean / P99 | sun+sky dE mean / P99 | render noise P99 furnace / sky | criteria |\n"
          "|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const Case& k : cases)
    {
        const FilmTable fp = makeFilm(true, kEta, k.nf, k.d, k.sub), fd = makeFilm(false, kEta, k.nf, k.d, k.sub);
        Config c;
        c.name = k.name;
        c.base = k.base;
        c.rc = 0.12;
        c.filmPhys = &fp;
        c.filmDef = &fd;
        c.msCoat = true;
        Definitions d(c);
        computeIntegrals(c, d);
        Table phys, half, r1;
        physicalTable(c, d, photons, phys, half);
        definitionTable(c, d, Which::R1, photons, r1);
        const std::vector<Rgb3> pf = renderSphere(phys, furnace, 64), ps = renderSphere(phys, sky, 64);
        const Metrics m = compare(r1, phys, half, furnace, sky, skyWhite, &pf, &ps);
        md << row(k.name, 0.12, "R1 + film (a)", m);
        logf("%s", row(k.name, 0.12, "R1 + film (a)", m).c_str());
    }
    writeTextFile(out, md.str());
}

// Tables the definition reads (design 1.1), eta 1.5, single-scattering microfacets (the physical model):
//   E_c(mu, r_c) 32 x 32, K(r_c) 32, A_x/B_x(mu', r_b) 32 x 32 (base specular split into f0 and 1 terms, weighted by the
//   smooth inner transmission 1 - F(mu_l; 1/eta)), Abar/Bbar(r_b) 32 (cosine-weighted means of the plain A, B).
// Grid: x = i / 31 (mu at 0 evaluated at 1e-4), y = j / 31, like the v1 albedo table. Written as a C++ include.
void exportTables(const std::string& out)
{
    const int n = 32;
    std::vector<double> Ec(n * n), K(n), Ax(n * n), Bx(n * n), Ab(n), Bb(n), EcMs(n * n), KMs(n);
    parallelFor(n, [&](uint32_t j) {
        const double r = (double)j / (n - 1), a = scene::model::alphaFromRoughness((float)r);
        std::vector<double> A(n), B(n);
        for (int i = 0; i < n; ++i)
        {
            const double mu = std::max((double)i / (n - 1), 1e-4);
            const float3 v{ (float)std::sqrt(1 - mu * mu), 0, (float)mu };
            double ec = 0, ax = 0, bx = 0, pa = 0, pb = 0;
            const uint32_t m = 1 << 16;
            for (uint32_t k = 0; k < m; ++k)
            {
                const float3 h = vndf(v, a, (k + 0.5) / m, reference::toUnitFloat(reference::reverseBits(k)));
                const float3 l = h * (2 * dot(v, h)) - v;
                if (l.z <= 0) continue;
                const double w = g2(mu, l.z, a) / g1(mu, a), voh = dot(v, h), s5 = std::pow(1 - voh, 5.0);
                ec += w * fresnelExact(voh, kEta);
                const double tin = 1 - fresnelExact(l.z, 1 / kEta);
                pa += w * (1 - s5);
                pb += w * s5;
                ax += w * (1 - s5) * tin;
                bx += w * s5 * tin;
            }
            Ec[j * n + i] = ec / m;
            Ax[j * n + i] = ax / m;
            Bx[j * n + i] = bx / m;
            A[i] = pa / m;
            B[i] = pb / m;
        }
        // Cosine-weighted hemisphere means (trapezoid over mu with weight 2 mu).
        double sa = 0, sb = 0, sw = 0;
        for (int i = 0; i + 1 < n; ++i)
        {
            const double m0 = (double)i / (n - 1), m1 = (double)(i + 1) / (n - 1), w = (m1 * m1 - m0 * m0);
            sa += 0.5 * (A[i] + A[i + 1]) * w;
            sb += 0.5 * (B[i] + B[i + 1]) * w;
            sw += w;
        }
        Ab[j] = sa / sw;
        Bb[j] = sb / sw;
        // K(r): inner diffuse reflectance of the rough coat.
        reference::Pcg32 rng(500 + j, 9);
        double R = 0;
        const uint32_t m = 1 << 20;
        for (uint32_t k = 0; k < m; ++k)
        {
            const float3 wo = cosineDir(rng.uniform(), rng.uniform());
            const float3 mm = vndf(wo, a, rng.uniform(), rng.uniform());
            const double cc = dot(wo, mm);
            if (rng.uniform() < fresnelExact(cc, 1 / kEta))
            {
                const float3 rr = mm * (float)(2 * cc) - wo;
                if (rr.z > 0) R += g2(wo.z, rr.z, a) / g1(wo.z, a);
            }
        }
        K[j] = R / m;
        // Multiple-scattering coat (the reference physics): directional reflectance from outside and the inner diffuse
        // reflectance, by the microsurface walk (ms::interact).
        for (int i = 0; i < n; ++i)
        {
            const double mu = std::max((double)i / (n - 1), 1e-4);
            const uint32_t q = 1 << 16;
            double hits = 0;
            for (uint32_t k = 0; k < q; ++k)
            {
                float3 w{ -(float)std::sqrt(1 - mu * mu), 0, -(float)mu };
                bool outside = true;
                if (ms::interact(w, outside, a, rng) && outside) hits += 1;
            }
            EcMs[j * n + i] = hits / q;
        }
        double ri = 0;
        const uint32_t q = 1 << 20;
        for (uint32_t k = 0; k < q; ++k)
        {
            float3 w = cosineDir(rng.uniform(), rng.uniform());  // upward inside the medium
            bool outside = false;
            if (ms::interact(w, outside, a, rng) && !outside) ri += 1;
        }
        KMs[j] = ri / q;
    });
    std::ostringstream s;
    s << "// Clearcoat tables (MATERIAL_LAYERS_KO.md 1.1), generated by unx_study_material_layers tables. eta = 1.5,\n"
         "// single-scattering GGX with height-correlated masking, exact dielectric Fresnel. Row-major [r * 32 + mu].\n";
    auto arr = [&](const char* name, const std::vector<double>& v) {
        s << "const float " << name << "[" << v.size() << "] = {";
        for (size_t i = 0; i < v.size(); ++i) s << (i % 8 == 0 ? "\n    " : " ") << format("%.7ff,", v[i]);
        s << "\n};\n";
    };
    arr("kCoatReflectance", Ec);
    arr("kCoatInnerDiffuseReflectance", K);
    arr("kBaseEscapeA", Ax);
    arr("kBaseEscapeB", Bx);
    arr("kBaseMeanA", Ab);
    arr("kBaseMeanB", Bb);
    s << "// Energy-conserving coat (microsurface multiple scattering, Heitz et al. 2016): the reference physics.\n";
    arr("kCoatReflectanceMs", EcMs);
    arr("kCoatInnerDiffuseReflectanceMs", KMs);
    writeTextFile(out, s.str());
    logf("K(r) at r = 0, 0.12 (j 4), 0.3 (j 9), 1: %.4f %.4f %.4f %.4f\n", K[0], K[4], K[9], K[31]);
}
} // namespace unx::study
