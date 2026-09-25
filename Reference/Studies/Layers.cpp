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
#include <cstring>
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
struct Definitions
{
    const Config& cfg;
    Integrals in;
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
    Rgb r1(float3 wo, float3 wi, Rgb* parts = nullptr, bool candA = false) const
    {
        if (wo.z <= 0 || wi.z <= 0) return {};
        const float3 h = normalize(wo + wi);
        double fc = ggxD({ 0, 0, 1 }, h, ac) * g2(wi.z, wo.z, ac) / (4.0 * wi.z * wo.z) * fresnelExact(dot(wo, h), kEta);
        double Ti = 1 - in.at(in.Rc, wi.z), To = 1 - in.at(in.Rc, wo.z), K = in.K;
        if (candA)
        {
            const double Ei = in.at(in.RcMs, wi.z), Eo = in.at(in.RcMs, wo.z);
            const double di = std::max(0.0, Ei - in.at(in.Rc, wi.z)), dout = std::max(0.0, Eo - in.at(in.Rc, wo.z));
            if (in.DeltaBar > 0) fc += di * dout / (kPi * in.DeltaBar);
            Ti = 1 - Ei;
            To = 1 - Eo;
            K = in.KMs;
        }
        const float3 pi = refractIn(wi), po = refractIn(wo);
        const Rgb f1 = baseBrdf(baseR1, cfg.filmDef, po, pi) * (float)(Ti * To / (kEta * kEta));
        const Rgb ret = in.atRgb(in.a, pi.z) - in.atRgb(in.e, pi.z);
        Rgb fms;
        const float* rb = &in.rhoBar.r;
        const float* rt = &ret.r;
        float* o = &fms.r;
        for (int c = 0; c < 3; ++c) o[c] = (float)(Ti * rt[c] * rb[c] / (1 - rb[c] * K) * To / (kPi * kEta * kEta));
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
float3 incident(int i)
{
    const double th = (i + 0.5) * (kPi / 2) / NI;
    return { (float)std::sin(th), 0, (float)std::cos(th) };
}

// Energy (luminance) leaving per incidence bin, split by the number of base interactions: 0 (coat only), 1, ..., 14,
// >= 15 (last entry).
constexpr int kSplit = 16;
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

enum class Which { R1, Old, R1A };
// parts (R1 only, optional): energy (luminance) of f_c, f_1, f_ms per incidence bin.
using Parts = std::vector<std::array<double, 3>>;
void definitionTable(const Config& c, const Definitions& d, Which which, uint32_t samples, Table& T, const std::vector<uint8_t>* mask = nullptr, Parts* parts = nullptr)
{
    if (parts) parts->assign(NI, { 0, 0, 0 });
    reference::Surface surfR1;
    surfR1.ng = surfR1.ns = { 0, 0, 1 };
    surfR1.bsdf = which != Which::Old ? d.baseR1 : c.base;
    parallelFor(NI, [&](uint32_t ii) {
        if (mask && !(*mask)[ii]) return;
        const float3 wi = incident((int)ii);
        const float3 pi = which != Which::Old ? refractIn(wi) : wi;
        std::array<double, 3> pp = { 0, 0, 0 };
        const reference::Bsdf base(surfR1, pi);
        reference::Pcg32 rng(0xDEF0 + ii, 13 + (uint64_t)(c.rc * 1000) + (which == Which::R1 ? 0 : which == Which::Old ? 7 : 3));
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
            const Rgb f = which != Which::Old ? d.r1(wo, wi, pr, which == Which::R1A) : d.old(wo, wi);
            int bt, bp;
            binOf(wo, bt, bp);
            const double wgt = wo.z / pdf / samples;
            T.at((int)ii, bt, bp) += f * (float)wgt;
            if (which != Which::Old)
                for (int k = 0; k < 3; ++k) pp[k] += pr[k].luminance() * wgt;
        }
        if (parts) (*parts)[ii] = pp;
    });
}

// ---------------------------------------------------------------------------------------------- metrics
struct Metrics
{
    double albedoRelMax = 0, albedoAbsAtRelMax = 0, albedoWorstTheta = 0;  // worst over theta_i (rel, when abs > 0.005)
    double albedoRel[5] = {};                                                // at 0/30/60/75/85 deg
    double l1Max = 0, l1WorstTheta = 0, l1Noise = 0;
    double furnaceMean = 0, furnaceP99 = 0, skyMean = 0, skyP99 = 0;
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
    std::snprintf(b, sizeof b, "| %s | %.2f | %s | %.1f%% @%.0f° (%+.3f) | %+.1f / %+.1f / %+.1f / %+.1f / %+.1f %% | %.3f @%.0f° | %.3f | %.2f / %.2f | %.2f / %.2f | %s |\n",
                  name.c_str(), rc, def, 100 * m.albedoRelMax, m.albedoWorstTheta, m.albedoAbsAtRelMax, 100 * m.albedoRel[0], 100 * m.albedoRel[1], 100 * m.albedoRel[2],
                  100 * m.albedoRel[3], 100 * m.albedoRel[4], m.l1Max, m.l1WorstTheta, m.l1Noise, m.furnaceMean, m.furnaceP99, m.skyMean, m.skyP99, m.pass() ? "PASS" : "FAIL");
    return b;
}
} // namespace

void clearcoatR1Study(const std::string& out, uint32_t photons, bool msCoat, bool candA)
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
          "`unx_study_material_layers " << (candA ? "clearcoat_r1a" : msCoat ? "clearcoat_r1_ms" : "clearcoat_r1") << "`. Physical coat: "
       << (msCoat ? "microsurface multiple scattering (Heitz et al. 2016, energy conserving)" : "single-scattering microfacets (energy lost at grazing)")
       << ". Criteria (MATERIAL_LAYERS 3): albedo rel <= 2 % (or abs <= 0.005), L1 <= 0.05, "
          "render dE76 mean <= 1.0 and P99 <= 2.3 (white furnace / sun + sky sphere, 64 x 64). Photons per incidence bin: "
          << photons << " (physical), " << photons << " (definition). 'L1 noise' = physical half vs full (MC floor).\n\n"
          "| base | r_c | definition | worst albedo (rel @theta, abs) | albedo rel at 0/30/60/75/85° | worst L1 | L1 noise | furnace dE mean / P99 | sun+sky dE mean / P99 | criteria |\n"
          "|---|---|---|---|---|---|---|---|---|---|\n";
    eq << "\n## Equivalent roughness of the base lobe outside (alpha_eq ~ eta alpha'_b)\n\n| base | r_c | alpha'_b | eta alpha'_b | physical 75 % half-angle | GGX(eta alpha'_b) 75 % half-angle |\n|---|---|---|---|---|---|\n";
    for (double rc : coats)
        for (const BaseDef& b : bases)
        {
            Config c;
            c.name = b.name;
            c.base = b.s;
            c.rc = rc;
            c.msCoat = msCoat;
            Definitions d(c);
            computeIntegrals(c, d);
            Table phys, half, r1, old;
            physicalTable(c, d, photons, phys, half);
            definitionTable(c, d, Which::R1, photons, r1);
            definitionTable(c, d, candA ? Which::R1A : Which::Old, photons, old);
            const std::vector<Rgb3> pf = renderSphere(phys, furnace, 64), ps = renderSphere(phys, sky, 64);
            const Metrics m1 = compare(r1, phys, half, furnace, sky, skyWhite, &pf, &ps), m0 = compare(old, phys, half, furnace, sky, skyWhite, &pf, &ps);
            const char* second = candA ? "R1 + A (coat MS)" : "1.1 original";
            md << row(b.name, rc, "R1", m1) << row(b.name, rc, second, m0);
            logf("%s%s", row(b.name, rc, "R1", m1).c_str(), row(b.name, rc, second, m0).c_str());
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
          "| case | r_c | definition | worst albedo (rel @theta, abs) | albedo rel at 0/30/60/75/85° | worst L1 | L1 noise | furnace dE mean / P99 | sun+sky dE mean / P99 | criteria |\n"
          "|---|---|---|---|---|---|---|---|---|---|\n";
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
