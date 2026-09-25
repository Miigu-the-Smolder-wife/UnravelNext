// Material v2 approximation errors (Docs/Design/MATERIAL_LAYERS_KO.md section 3), measured against physical references.
//   unx_study_material_layers thinfilm  <out.md>   RGB thin-film approximations vs the spectral Airy reflectance
//   unx_study_material_layers clearcoat <out.md>   clearcoat definition vs a Monte Carlo layered model
// CPU only, at most 4 worker threads, below-normal priority (the machine is shared with measurements and the user).
//
// Thin film. Reference: for every wavelength 360-830 nm (1 nm) the exact Airy reflectance of outer medium n0 / film
// (eta_f, thickness d) / substrate (complex n + ik), s and p averaged, integrated against the CIE 1931 2-degree
// colour-matching functions (illuminant E), converted to linear Rec.709 white-balanced to E (a constant reflectance R
// maps to RGB (R, R, R)). Metals use Johnson & Christy spectral n, k; the RGB methods get the n, k triplet sampled at
// 650 / 550 / 450 nm (the values the renderer would store). Methods:
//   (a) Belcour & Barla 2017: the Airy intensity series with the spectral integral of every interference term replaced
//       by the paper's Gaussian fits of the XYZ sensitivities in Fourier space (m <= 3, DC term 1), exact complex
//       interface coefficients, outer medium index as a parameter (1.0 bare, 1.5 under a coat), XYZ -> Rec.709
//       white-balanced to E (the paper's code uses CIE 1931 RGB, and its conductor phase mixes the n(1 + ik) and
//       n + ik conventions). Per-channel substrates run it once per channel and keep that channel.
//   (b) the previous engine (TitanNative ThinFilmProgram.h filmBand): exact Airy reflectance averaged over inverse
//       wavelength in three bands (580-700, 490-580, 400-490 nm) in closed form, n0 generalised the same way.
// Error metrics over theta 0..89 deg (1 deg) x d 0..2000 nm (5 nm): max / mean |dRGB|, max / mean CIELAB dE76 (E white).
//
// Clearcoat. Definition (MATERIAL_LAYERS 1.1, c = 1): f = f_c + T_c(mu_o) T_c(mu_i) f_base, f_c = D V F_schlick(0.04)
// (1 + 0.04 (1/E(mu_o) - 1)), T_c = 1 - (0.04 A + B)(1 + 0.04 (1/E - 1)); A, B integrated exactly here (the design
// reads them from a 32 x 32 table), E from scene::model::directionalAlbedo (the definition's table). Physical model:
// position-free random walk (Guo et al. 2018 style) between a rough dielectric top interface (GGX alpha_c, exact
// Fresnel n = 1.5, refraction, total internal reflection, single-scattering microfacets: VNDF sampling, G1 weights)
// and the v1 base BRDF evaluated with the in-medium directions, no absorption, zero thickness. Both give the energy
// reflected into 18 x 18 (theta_o x |phi_o - phi_i|) bins per unit incident energy; reported: albedo difference and the
// energy-normalised L1 bin difference. The difference contains exactly the design's section-3 items 1 and 2 (internal
// reflection, refraction) plus Schlick vs exact Fresnel of the coat (isolated by the black-base rows).
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/scene/MaterialModel.h"

#include "Bsdf.h"
#include "Sampler.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

using namespace unx;
using cd = std::complex<double>;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr unsigned kThreads = 4;

void parallel(uint32_t count, const std::function<void(uint32_t)>& fn)
{
    std::atomic<uint32_t> next{ 0 };
    std::vector<std::thread> workers;
    for (unsigned t = 0; t < kThreads; ++t)
        workers.emplace_back([&] {
            for (uint32_t i = next++; i < count; i = next++) fn(i);
        });
    for (auto& w : workers) w.join();
}

std::string dataDir() { return std::string(UNX_SOURCE_DIR) + "/Reference/Studies/data/"; }

// ------------------------------------------------------------------------------------------------ colour
struct Cmf
{
    std::vector<double> lambda, x, y, z;
};
Cmf loadCmf()
{
    Cmf c;
    std::ifstream f(dataDir() + "cie1931_2deg_xyz_1nm.csv");
    if (!f) fail("missing CIE table");
    std::string line;
    while (std::getline(f, line))
    {
        double l, a, b, d;
        char c1, c2, c3;
        std::istringstream s(line);
        if (s >> l >> c1 >> a >> c2 >> b >> c3 >> d)
        {
            c.lambda.push_back(l);
            c.x.push_back(a);
            c.y.push_back(b);
            c.z.push_back(d);
        }
    }
    return c;
}

// XYZ -> linear Rec.709 (IEC 61966-2-1), then white-balanced so that XYZ of illuminant E maps to (1, 1, 1).
struct Rec709E
{
    double m[3][3];
    double inv[3][3];
    void init(const double whiteXyz[3])
    {
        const double s[3][3] = { { 3.2404542, -1.5371385, -0.4985314 }, { -0.9692660, 1.8760108, 0.0415560 }, { 0.0556434, -0.2040259, 1.0572252 } };
        for (int r = 0; r < 3; ++r)
        {
            const double w = s[r][0] * whiteXyz[0] + s[r][1] * whiteXyz[1] + s[r][2] * whiteXyz[2];
            for (int c = 0; c < 3; ++c) m[r][c] = s[r][c] / w;
        }
        // Inverse (for Lab).
        const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
        const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
        inv[0][0] = (e * i - f * h) / det; inv[0][1] = (c * h - b * i) / det; inv[0][2] = (b * f - c * e) / det;
        inv[1][0] = (f * g - d * i) / det; inv[1][1] = (a * i - c * g) / det; inv[1][2] = (c * d - a * f) / det;
        inv[2][0] = (d * h - e * g) / det; inv[2][1] = (b * g - a * h) / det; inv[2][2] = (a * e - b * d) / det;
    }
    std::array<double, 3> apply(const double xyz[3]) const
    {
        return { m[0][0] * xyz[0] + m[0][1] * xyz[1] + m[0][2] * xyz[2], m[1][0] * xyz[0] + m[1][1] * xyz[1] + m[1][2] * xyz[2],
                 m[2][0] * xyz[0] + m[2][1] * xyz[1] + m[2][2] * xyz[2] };
    }
};

// CIELAB dE76 between two linear RGB reflectances (white = RGB (1, 1, 1) = illuminant E).
double deltaE(const Rec709E& cs, const std::array<double, 3>& a, const std::array<double, 3>& b, const double white[3])
{
    auto lab = [&](const std::array<double, 3>& rgb) {
        double xyz[3];
        for (int r = 0; r < 3; ++r) xyz[r] = cs.inv[r][0] * rgb[0] + cs.inv[r][1] * rgb[1] + cs.inv[r][2] * rgb[2];
        auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16) / 116.0; };
        const double fx = f(xyz[0] / white[0]), fy = f(xyz[1] / white[1]), fz = f(xyz[2] / white[2]);
        return std::array<double, 3>{ 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz) };
    };
    const auto la = lab(a), lb = lab(b);
    return std::sqrt((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) + (la[2] - lb[2]) * (la[2] - lb[2]));
}

// ------------------------------------------------------------------------------------------------ thin-film physics
// Exact reflectance of n0 / film (n1, thickness d nm) / substrate (n2 complex) at incidence cos0 in medium n0.
double airy(double n0, double cos0, double n1, cd n2, double dNm, double lambdaNm)
{
    const double s2 = n0 * n0 * (1 - cos0 * cos0);
    const cd q0 = n0 * cos0;
    const cd q1 = std::sqrt(cd(n1 * n1 - s2, 0));
    cd q2 = std::sqrt(n2 * n2 - s2);
    if (q2.imag() < 0) q2 = -q2;
    const cd e0 = n0 * n0, e1 = n1 * n1, e2 = n2 * n2;
    const cd r01s = (q0 - q1) / (q0 + q1), r12s = (q1 - q2) / (q1 + q2);
    const cd r01p = (e1 * q0 - e0 * q1) / (e1 * q0 + e0 * q1), r12p = (e2 * q1 - e1 * q2) / (e2 * q1 + e1 * q2);
    const cd phase = std::exp(cd(0, 1) * (4 * kPi * dNm / lambdaNm) * q1);
    const cd rs = (r01s + r12s * phase) / (1.0 + r01s * r12s * phase), rp = (r01p + r12p * phase) / (1.0 + r01p * r12p * phase);
    return 0.5 * (std::norm(rs) + std::norm(rp));
}

// (b) previous engine: band average over inverse wavelength (TitanNative filmBand), n0 generalised.
double bandReflectance(double first, cd second, double path, double waveLow, double waveHigh)
{
    const double r = std::norm(second), a = first * first, den = (1 - a) + a * (1 - r);
    if (den == 0) return 1;
    const double transfer = (1 - a) * (1 - r) / den, baseline = 1 - transfer;
    const cd z = second * first;
    const double q = std::abs(z);
    if (q == 0) return baseline;
    const double phase = std::arg(z), width = 2 * kPi * path * (1 / waveLow - 1 / waveHigh);
    const double mid = kPi * path * (1 / waveLow + 1 / waveHigh) + phase;
    if (width == 0)
    {
        const double inter = 2 * q * std::cos(mid);
        return (a + r + inter) / (1 + a * r + inter);
    }
    const double sh = std::sin(width * 0.5), ch = std::cos(width * 0.5);
    const double num = 2 * q * sh * (std::cos(mid) + q * ch), div = 1 + 2 * q * std::cos(mid) * ch + q * q * std::cos(width);
    return baseline + 2 * transfer * std::atan2(num, div) / width;
}
double filmBand(double n0, double cos0, double nf, double dNm, double n, double k, double lo, double hi)
{
    const double s2 = n0 * n0 * (1 - cos0 * cos0);
    const cd sq(n * n - k * k, 2 * n * k);
    cd q2 = std::sqrt(sq - s2);
    if (q2.imag() < 0) q2 = -q2;
    const double q0 = n0 * cos0;
    if (dNm == 0)
    {
        const cd s = (q0 - q2) / (q0 + q2), p = (sq * q0 - n0 * n0 * q2) / (sq * q0 + n0 * n0 * q2);
        return 0.5 * (std::norm(s) + std::norm(p));
    }
    if (nf * nf - s2 <= 0) return 1;  // total internal reflection at the film (evanescent; exact only in the reference)
    const double q1 = std::sqrt(nf * nf - s2);
    const double s0 = (q0 - q1) / (q0 + q1), p0 = (nf * nf * q0 - n0 * n0 * q1) / (nf * nf * q0 + n0 * n0 * q1);
    const cd s1 = (q1 - q2) / (q1 + q2), p1 = (sq * q1 - nf * nf * q2) / (sq * q1 + nf * nf * q2);
    const double path = 2 * dNm * q1;
    return 0.5 * (bandReflectance(s0, s1, path, lo, hi) + bandReflectance(p0, p1, path, lo, hi));
}

std::array<double, 3> sensitivity(double opdUm, double shift)
{
    const double phase = 2 * kPi * opdUm * 1.0e-6;
    const double val[3] = { 5.4856e-13, 4.4201e-13, 5.2481e-13 }, pos[3] = { 1.6810e+06, 1.7953e+06, 2.2084e+06 }, var[3] = { 4.3278e+09, 9.3046e+09, 6.6121e+09 };
    std::array<double, 3> xyz;
    for (int c = 0; c < 3; ++c) xyz[c] = val[c] * std::sqrt(2 * kPi * var[c]) * std::cos(pos[c] * phase + shift) * std::exp(-var[c] * phase * phase);
    xyz[0] += 9.7470e-14 * std::sqrt(2 * kPi * 4.5282e+09) * std::cos(2.2399e+06 * phase + shift) * std::exp(-4.5282e+09 * phase * phase);
    for (double& v : xyz) v /= 1.0685e-7;
    return xyz;
}
// (a) the Belcour & Barla 2017 method: Airy intensity series R = R01 + Rs + sum_m 2 (Rs - T) r^m cos(m (delta + phi)),
// truncated at m = 3, with the spectral integral of each cosine replaced by the Gaussian fits of the XYZ sensitivities
// in Fourier space; DC term S0 = 1 (their Mitsuba version). Interface amplitudes and phases are the exact complex
// Fresnel coefficients (the published code mixes the n(1 + ik) and n + ik conventions in the conductor phase).
std::array<double, 3> belcourXyz(double n0, double cos0, double nf, double dNm, double n3, double k3)
{
    const double s2 = n0 * n0 * (1 - cos0 * cos0);
    if (nf * nf - s2 <= 0) return { 1, 1, 1 };  // total reflection at the film: the method has no evanescent coupling
    const double q0 = n0 * cos0, q1 = std::sqrt(nf * nf - s2);
    const cd n2(n3, k3);
    cd q2 = std::sqrt(n2 * n2 - s2);
    if (q2.imag() < 0) q2 = -q2;
    const cd e0 = n0 * n0, e1 = nf * nf, e2 = n2 * n2;
    const cd r01[2] = { (q0 - q1) / (q0 + q1), (e1 * q0 - e0 * q1) / (e1 * q0 + e0 * q1) };
    const cd r12[2] = { (q1 - q2) / (q1 + q2), (e2 * q1 - e1 * q2) / (e2 * q1 + e1 * q2) };
    const double opdUm = 2 * dNm * 1e-3 * q1;  // 2 n_f d cos(theta_f)
    std::array<double, 3> I{ 0, 0, 0 };
    for (int pol = 0; pol < 2; ++pol)
    {
        const double R12 = std::norm(r01[pol]), R23 = std::norm(r12[pol]), T = 1 - R12;
        const double r123 = std::abs(r01[pol]) * std::abs(r12[pol]);
        const double phi = std::arg(-r01[pol] * r12[pol]);  // phase of r10 r12 per round trip
        const double Rs = T * T * R23 / (1 - R12 * R23);
        for (int c = 0; c < 3; ++c) I[c] += 0.5 * (R12 + Rs);
        double Cm = Rs - T;
        for (int m = 1; m <= 3; ++m)
        {
            Cm *= r123;
            const auto S = sensitivity(m * opdUm, m * phi);
            for (int c = 0; c < 3; ++c) I[c] += 0.5 * Cm * 2 * S[c];
        }
    }
    return I;
}

struct Substrate
{
    std::string name;
    bool spectral = false;
    double n = 1.5, k = 0;                      // constant substrate
    std::vector<double> wl, sn, sk;             // spectral (um)
    double rgbN[3] = {}, rgbK[3] = {};
    cd at(double lambdaNm) const
    {
        if (!spectral) return cd(n, k);
        const double l = lambdaNm * 1e-3;
        size_t i = 1;
        while (i + 1 < wl.size() && wl[i] < l) ++i;
        const double t = std::clamp((l - wl[i - 1]) / (wl[i] - wl[i - 1]), 0.0, 1.0);
        return cd(sn[i - 1] + t * (sn[i] - sn[i - 1]), sk[i - 1] + t * (sk[i] - sk[i - 1]));
    }
};
Substrate loadMetal(const std::string& name, const std::string& file)
{
    Substrate s;
    s.name = name;
    s.spectral = true;
    std::ifstream f(dataDir() + file);
    double a, b, c;
    while (f >> a >> b >> c)
    {
        s.wl.push_back(a);
        s.sn.push_back(b);
        s.sk.push_back(c);
    }
    if (s.wl.size() < 10) fail("bad metal table %s", file.c_str());
    const double at[3] = { 650, 550, 450 };
    for (int c2 = 0; c2 < 3; ++c2)
    {
        const cd v = s.at(at[c2]);
        s.rgbN[c2] = v.real();
        s.rgbK[c2] = v.imag();
    }
    return s;
}

void thinFilm(const std::string& out)
{
    const Cmf cmf = loadCmf();
    double white[3] = { 0, 0, 0 }, sumY = 0;
    for (size_t i = 0; i < cmf.lambda.size(); ++i)
    {
        white[0] += cmf.x[i];
        white[1] += cmf.y[i];
        white[2] += cmf.z[i];
        sumY += cmf.y[i];
    }
    for (double& w : white) w /= sumY;
    Rec709E cs;
    cs.init(white);

    std::vector<Substrate> subs;
    {
        Substrate g;
        g.name = "dielectric 1.5";
        g.n = 1.5;
        subs.push_back(g);
        Substrate d;
        d.name = "design default n 1.5, k 2";
        d.n = 1.5;
        d.k = 2;
        subs.push_back(d);
        for (Substrate* s : { &subs[0], &subs[1] })
            for (int c = 0; c < 3; ++c)
            {
                s->rgbN[c] = s->n;
                s->rgbK[c] = s->k;
            }
        subs.push_back(loadMetal("gold (J&C)", "Au_johnson_christy_um_n_k.txt"));
        subs.push_back(loadMetal("copper (J&C)", "Cu_johnson_christy_um_n_k.txt"));
        // Same metals with the per-channel (n, k) fitted to the spectral reference at d = 0 in air (least squares over
        // incidence 0-89 deg) instead of point samples at 650/550/450 nm.
        for (int mi = 2; mi <= 3; ++mi)
        {
            Substrate f = subs[mi];
            f.name = subs[mi].name.substr(0, subs[mi].name.find(' ')) + " (J&C, fitted RGB n,k)";
            std::array<std::array<double, 3>, 90> ref{};
            for (int deg = 0; deg < 90; ++deg)
            {
                const double c0 = std::cos(deg * kPi / 180);
                double xyz[3] = { 0, 0, 0 };
                for (size_t i = 0; i < cmf.lambda.size(); ++i)
                {
                    const double R = airy(1.0, c0, 1.0, f.at(cmf.lambda[i]), 0, cmf.lambda[i]);
                    xyz[0] += R * cmf.x[i];
                    xyz[1] += R * cmf.y[i];
                    xyz[2] += R * cmf.z[i];
                }
                for (double& v : xyz) v /= sumY;
                ref[deg] = cs.apply(xyz);
            }
            for (int ch = 0; ch < 3; ++ch)
            {
                double best = 1e30;
                for (int in = 1; in <= 300; ++in)
                    for (int ik = 0; ik <= 600; ++ik)
                    {
                        const double n = in * 0.01, k = ik * 0.02;
                        double e = 0;
                        for (int deg = 0; deg < 90; ++deg)
                        {
                            const double diff = airy(1.0, std::cos(deg * kPi / 180), 1.0, cd(n, k), 0, 550) - ref[deg][ch];
                            e += diff * diff;
                        }
                        if (e < best)
                        {
                            best = e;
                            f.rgbN[ch] = n;
                            f.rgbK[ch] = k;
                        }
                    }
            }
            logf("%s: fitted n (%.2f %.2f %.2f) k (%.2f %.2f %.2f); point samples n (%.2f %.2f %.2f) k (%.2f %.2f %.2f)\n", f.name.c_str(), f.rgbN[0], f.rgbN[1], f.rgbN[2],
                 f.rgbK[0], f.rgbK[1], f.rgbK[2], subs[mi].rgbN[0], subs[mi].rgbN[1], subs[mi].rgbN[2], subs[mi].rgbK[0], subs[mi].rgbK[1], subs[mi].rgbK[2]);
            subs.push_back(f);
        }
    }
    const double films[3] = { 1.33, 1.5, 2.4 }, outers[2] = { 1.0, 1.5 };
    struct Stat
    {
        double maxRgb = 0, sumRgb = 0, maxDe = 0, sumDe = 0, n = 0;
        double maxRgbThin = 0, maxRgbThick = 0, maxDe0 = 0, maxTir = 0;
        std::vector<float> des;
        double p99() const
        {
            std::vector<float> v = des;
            if (v.empty()) return 0;
            const size_t k = (size_t)(0.99 * (v.size() - 1));
            std::nth_element(v.begin(), v.begin() + (ptrdiff_t)k, v.end());
            return v[k];
        }
        void add(double e, double de, double d, bool tir)
        {
            if (tir)
            {
                maxTir = std::max(maxTir, e);
                return;
            }
            des.push_back((float)de);
            maxRgb = std::max(maxRgb, e);
            sumRgb += e;
            maxDe = std::max(maxDe, de);
            sumDe += de;
            n += 1;
            (d <= 400 ? maxRgbThin : maxRgbThick) = std::max(d <= 400 ? maxRgbThin : maxRgbThick, e);
            if (d == 0) maxDe0 = std::max(maxDe0, de);
        }
    };
    struct Case
    {
        double n0, nf;
        size_t sub;
        Stat a, b;
    };
    std::vector<Case> cases;
    for (double n0 : outers)
        for (double nf : films)
            for (size_t si = 0; si < subs.size(); ++si) cases.push_back({ n0, nf, si, {}, {} });
    parallel((uint32_t)cases.size(), [&](uint32_t ci) {
        Case& c = cases[ci];
        const Substrate& s = subs[c.sub];
        for (int deg = 0; deg <= 89; ++deg)
        {
            const double cos0 = std::cos(deg * kPi / 180);
            for (int di = 0; di <= 400; ++di)
            {
                const double d = di * 5.0;
                double xyz[3] = { 0, 0, 0 };
                for (size_t i = 0; i < cmf.lambda.size(); ++i)
                {
                    const double R = airy(c.n0, cos0, c.nf, s.at(cmf.lambda[i]), d, cmf.lambda[i]);
                    xyz[0] += R * cmf.x[i];
                    xyz[1] += R * cmf.y[i];
                    xyz[2] += R * cmf.z[i];
                }
                for (double& v : xyz) v /= sumY;
                const auto ref = cs.apply(xyz);
                std::array<double, 3> ra, rb;
                const double bands[3][2] = { { 580, 700 }, { 490, 580 }, { 400, 490 } };
                for (int ch = 0; ch < 3; ++ch)
                {
                    const auto ax = belcourXyz(c.n0, cos0, c.nf, d, s.rgbN[ch], s.rgbK[ch]);
                    const double axd[3] = { ax[0], ax[1], ax[2] };
                    ra[ch] = std::max(0.0, cs.apply(axd)[ch]);
                    rb[ch] = filmBand(c.n0, cos0, c.nf, d, s.rgbN[ch], s.rgbK[ch], bands[ch][0], bands[ch][1]);
                }
                auto err = [&](const std::array<double, 3>& t) {
                    return std::max({ std::fabs(t[0] - ref[0]), std::fabs(t[1] - ref[1]), std::fabs(t[2] - ref[2]) });
                };
                const bool tir = c.n0 * std::sin(deg * kPi / 180) > c.nf;
                c.a.add(err(ra), deltaE(cs, ra, ref, white), d, tir);
                c.b.add(err(rb), deltaE(cs, rb, ref, white), d, tir);
            }
        }
    });
    std::ostringstream md;
    md << "# Thin-film RGB approximations vs spectral Airy reflectance [measured]\n\n"
          "`unx_study_material_layers thinfilm`. Grid: incidence 0-89 deg (1 deg) x film thickness 0-2000 nm (5 nm). Reference: "
          "exact polarised Airy reflectance per wavelength (360-830 nm, 1 nm), CIE 1931 2 deg, illuminant E, linear Rec.709 "
          "white-balanced to E. |dRGB| = max channel abs error of the reflectance; dE = CIELAB dE76 (white = E). "
          "(a) Belcour & Barla 2017: Airy intensity series (m <= 3) with Gaussian-fit sensitivity Fourier terms, exact complex interface Fresnel, Rec.709/E. "
          "(b) previous engine: band-averaged Airy (580-700 / 490-580 / 400-490 nm). Metals: Johnson & Christy spectral n,k "
          "(reference) and their values at 650/550/450 nm (RGB methods). Statistics exclude incidence beyond the film's critical "
          "angle (film index below the outer index); 'TIR max' is the max |dRGB| there (both RGB methods set R = 1; the exact "
          "thin film couples to the substrate evanescently).\n\n"
          "| outer | film n | substrate | (a) max dRGB | (a) mean | (a) max dRGB d<=400 / >400 | (a) max dE | (a) P99 dE | (a) mean dE | (a) dE at d=0 | (b) max dRGB | (b) mean | (b) max dRGB d<=400 / >400 | (b) max dE | (b) P99 dE | (b) mean dE | (b) dE at d=0 | TIR max (a) / (b) |\n"
          "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|\n";
    for (const Case& c : cases)
    {
        char row[512];
        std::snprintf(row, sizeof row, "| %.1f | %.2f | %s | %.3f | %.4f | %.3f / %.3f | %.1f | %.1f | %.2f | %.2f | %.3f | %.4f | %.3f / %.3f | %.1f | %.1f | %.2f | %.2f | %s |\n", c.n0, c.nf,
                      subs[c.sub].name.c_str(), c.a.maxRgb, c.a.sumRgb / c.a.n, c.a.maxRgbThin, c.a.maxRgbThick, c.a.maxDe, c.a.p99(), c.a.sumDe / c.a.n, c.a.maxDe0, c.b.maxRgb,
                      c.b.sumRgb / c.b.n, c.b.maxRgbThin, c.b.maxRgbThick, c.b.maxDe, c.b.p99(), c.b.sumDe / c.b.n, c.b.maxDe0,
                      c.a.maxTir > 0 || c.b.maxTir > 0 ? format("%.2f / %.2f", c.a.maxTir, c.b.maxTir).c_str() : "-");
        md << row;
    }
    writeTextFile(out, md.str());
    logf("%s", md.str().c_str());
}

// ------------------------------------------------------------------------------------------------ clearcoat
double g1(double mu, double alpha)
{
    const double a2 = alpha * alpha, m2 = mu * mu;
    return 2 * mu / (mu + std::sqrt(a2 + (1 - a2) * m2));
}
float3 vndf(float3 w, double alpha, double u1, double u2)  // w in the local frame (z = normal side of w)
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
double fresnelExact(double cosi, double eta)  // eta = n_t / n_i
{
    const double s2 = (1 - cosi * cosi) / (eta * eta);
    if (s2 >= 1) return 1;
    const double ct = std::sqrt(1 - s2);
    const double rs = (cosi - eta * ct) / (cosi + eta * ct), rp = (eta * cosi - ct) / (eta * cosi + ct);
    return 0.5 * (rs * rs + rp * rp);
}

struct AB
{
    std::vector<double> A, B;  // on mu = (i + 0.5) / 1024
    double at(const std::vector<double>& t, double mu) const
    {
        const double x = std::clamp(mu * 1024 - 0.5, 0.0, 1023.0);
        const size_t i = std::min((size_t)x, (size_t)1022);
        const double f = x - i;
        return t[i] * (1 - f) + t[i + 1] * f;
    }
};
AB specularSplit(double alpha)
{
    AB ab;
    ab.A.resize(1024);
    ab.B.resize(1024);
    parallel(1024, [&](uint32_t i) {
        const double mu = (i + 0.5) / 1024;
        const float3 v{ (float)std::sqrt(1 - mu * mu), 0, (float)mu };
        double a = 0, b = 0;
        const uint32_t n = 1 << 16;
        for (uint32_t k = 0; k < n; ++k)
        {
            const float3 h = vndf(v, alpha, (k + 0.5) / n, reference::toUnitFloat(reference::reverseBits(k)));
            const double voh = dot(v, h);
            const float3 l = h * (float)(2 * voh) - v;
            if (l.z <= 0) continue;
            // DV cos / pdf with VNDF = G2/G1 ~ height-correlated via the model V: 4 V NoL NoV / G1(v)
            const double w = 4.0 * scene::model::visibilitySmithGgxCorrelated((float)mu, l.z, (float)alpha) * l.z * mu / g1(mu, alpha);
            const double s = std::pow(1 - voh, 5.0);
            a += w * (1 - s);
            b += w * s;
        }
        ab.A[i] = a / n;
        ab.B[i] = b / n;
    });
    return ab;
}

void clearcoat(const std::string& out)
{
    struct Base
    {
        const char* name;
        scene::model::Surface s;
    };
    std::vector<Base> bases;
    auto mk = [](scene::MaterialClass cls, float3 c, float r, float m, float spec) {
        scene::model::Surface s;
        s.cls = cls;
        s.baseColor = c;
        s.roughness = r;
        s.metallic = m;
        s.specular = spec;
        return s;
    };
    bases.push_back({ "black (coat only)", mk(scene::MaterialClass::Standard, { 0, 0, 0 }, 1.0f, 0, 0) });
    bases.push_back({ "white diffuse 0.8", mk(scene::MaterialClass::Standard, { 0.8f, 0.8f, 0.8f }, 0.9f, 0, 0.5f) });
    bases.push_back({ "red paint r 0.5", mk(scene::MaterialClass::Standard, { 0.6f, 0.05f, 0.05f }, 0.5f, 0, 0.5f) });
    bases.push_back({ "metal flake r 0.3", mk(scene::MaterialClass::Standard, { 0.9f, 0.6f, 0.3f }, 0.3f, 1, 0.5f) });
    bases.push_back({ "black glossy r 0.2", mk(scene::MaterialClass::Standard, { 0.02f, 0.02f, 0.02f }, 0.2f, 0, 0.5f) });
    const float coatRough[3] = { 0.05f, 0.12f, 0.3f };
    const int angles[5] = { 0, 30, 60, 75, 85 };
    const int NT = 18, NP = 18;
    auto bin = [&](float3 w) {
        const double th = std::acos(std::clamp((double)w.z, 0.0, 1.0));
        const double ph = std::fabs(std::atan2((double)w.y, (double)w.x));  // relative to the incident plane (+x)
        const int it = std::min(NT - 1, (int)(th / (kPi / 2) * NT)), ip = std::min(NP - 1, (int)(ph / kPi * NP));
        return it * NP + ip;
    };
    std::ostringstream md;
    md << "# Clearcoat definition vs Monte Carlo layered model [measured]\n\n"
          "`unx_study_material_layers clearcoat`. Coat c = 1, n = 1.5, zero thickness, no absorption. Energy reflected per unit "
          "incident energy (Rec.709 luminance). 'L1 bins' = sum over 18 x 18 (theta_o, |dphi|) bins of |E_def - E_phys| / "
          "total E_phys (angular shape error). Monte Carlo: 4 M samples per method and cell; noise about 1e-3 in the L1 column.\n\n"
          "| base | r_c | theta_i | albedo def | albedo phys | def - phys | rel | L1 bins |\n|---|---|---|---|---|---|---|---|\n";
    for (float rc : coatRough)
    {
        const double ac = scene::model::alphaFromRoughness(rc);
        const AB ab = specularSplit(ac);
        auto Rc = [&](double mu) {
            const double e = scene::model::directionalAlbedo((float)mu, rc);
            return (0.04 * ab.at(ab.A, mu) + ab.at(ab.B, mu)) * (1 + 0.04 * (1 / e - 1));
        };
        for (const Base& base : bases)
            for (int deg : angles)
            {
                const double th = std::max(1e-3, deg * kPi / 180);
                const float3 wi{ (float)std::sin(th), 0, (float)std::cos(th) };  // towards the light
                const uint32_t N = 1 << 22, chunks = 64;
                std::vector<std::array<double, NT * NP>> defBins(chunks), physBins(chunks);
                reference::Surface surf;
                surf.ng = surf.ns = { 0, 0, 1 };
                surf.bsdf = base.s;
                parallel(chunks, [&](uint32_t ch) {
                    auto& D = defBins[ch];
                    auto& P = physBins[ch];
                    D.fill(0);
                    P.fill(0);
                    reference::Pcg32 rng(0x51A7 + ch, 17 + (uint64_t)deg * 131 + (uint64_t)(rc * 1000));
                    const reference::Bsdf baseAtWi(surf, wi);
                    const double Ti = 1 - Rc(wi.z);
                    for (uint32_t s = 0; s < N / chunks; ++s)
                    {
                        // ---- definition: E_bin += f_def(v = wo, l = wi) cos(wo) / pdf_mix(wo)
                        {
                            const double u = rng.uniform();
                            float3 wo{ 0, 0, -1 };  // stays invalid (zero contribution) if the chosen strategy fails
                            if (u < 0.3)
                            {
                                const float3 h = vndf(wi, ac, rng.uniform(), rng.uniform());
                                wo = h * (2 * dot(wi, h)) - wi;
                            }
                            else if (u < 0.8)
                            {
                                reference::BsdfSample bs;
                                if (baseAtWi.sample(rng.uniform(), rng.uniform(), rng.uniform(), bs)) wo = bs.wi;
                            }
                            else
                            {
                                const double r = std::sqrt(rng.uniform()), p = 2 * kPi * rng.uniform();
                                wo = { (float)(r * std::cos(p)), (float)(r * std::sin(p)), (float)std::sqrt(std::max(0.0, 1 - r * r)) };
                            }
                            if (wo.z > 1e-6f)
                            {
                                const float3 h = normalize(wo + wi);
                                const double noh = h.z, voh = dot(wo, h);
                                const double sin2 = (double)h.x * h.x + (double)h.y * h.y;
                                const double dvv = reference::ModelTerms::ggx(noh, sin2, ac) * reference::ModelTerms::smith(wo.z, wi.z, ac);
                                const double eo = scene::model::directionalAlbedo(wo.z, rc);
                                const double fc = dvv * (0.04 + 0.96 * std::pow(1 - voh, 5.0)) * (1 + 0.04 * (1 / eo - 1));
                                const reference::Rgb fb = reference::evaluateModel(base.s, { 0, 0, 1 }, wo, wi);
                                const double T = (1 - Rc(wo.z)) * Ti;
                                const double pc = g1(wi.z, ac) * reference::ModelTerms::ggx(noh, sin2, ac) / (4 * wi.z);
                                const double pdf = 0.3 * pc + 0.5 * baseAtWi.pdf(wo) + 0.2 * wo.z / kPi;
                                if (pdf > 0) D[bin(wo)] += (fc + T * fb.luminance()) * wo.z / pdf;
                            }
                        }
                        // ---- physical random walk from the light (energy transport)
                        {
                            float3 d = -wi;  // travel direction
                            double w = 1;
                            reference::Rgb wrgb(1.0f);
                            bool inside = false;
                            for (int bounce = 0; bounce < 256; ++bounce)
                            {
                                if (!inside)
                                {
                                    // top interface from above: frame z up
                                    const float3 wo = -d;
                                    const float3 m = vndf(wo, ac, rng.uniform(), rng.uniform());
                                    const double c = dot(wo, m);
                                    const double F = fresnelExact(c, 1.5);
                                    if (rng.uniform() < F)
                                    {
                                        const float3 r = m * (float)(2 * c) - wo;
                                        if (r.z <= 0) break;
                                        w *= g1(r.z, ac);
                                        P[bin(r)] += w * wrgb.luminance();
                                        break;
                                    }
                                    const double eta = 1 / 1.5, ct = std::sqrt(std::max(0.0, 1 - eta * eta * (1 - c * c)));
                                    const float3 t = normalize(m * (float)(eta * c - ct) - wo * (float)eta);
                                    if (t.z >= 0) break;
                                    w *= g1(-t.z, ac);
                                    d = t;
                                    inside = true;
                                }
                                else if (d.z < 0)
                                {
                                    // base, photon arriving from l = -d; sample v with the base sampler, evaluate f(v, l)
                                    const float3 l = -d;
                                    const reference::Bsdf b(surf, l);
                                    reference::BsdfSample bs;
                                    if (!b.sample(rng.uniform(), rng.uniform(), rng.uniform(), bs)) break;
                                    const float3 v = bs.wi;
                                    const reference::Rgb f = reference::evaluateModel(base.s, { 0, 0, 1 }, v, l);
                                    wrgb *= f * (v.z / bs.pdf);
                                    if (wrgb.isZero()) break;
                                    d = v;
                                }
                                else
                                {
                                    // top interface from below: work in the mirrored frame (z -> -z) whose normal points
                                    // into the medium; there the direction away from the interface is wof = mirror(-d).
                                    const float3 wof{ -d.x, -d.y, d.z };
                                    const float3 m = vndf(wof, ac, rng.uniform(), rng.uniform());
                                    const double c = dot(wof, m);
                                    const double F = fresnelExact(c, 1 / 1.5);
                                    if (rng.uniform() < F)
                                    {
                                        const float3 r = m * (float)(2 * c) - wof;
                                        if (r.z <= 0) break;
                                        w *= g1(r.z, ac);
                                        d = float3{ r.x, r.y, -r.z };  // back down in the real frame
                                    }
                                    else
                                    {
                                        const double eta = 1.5, ct = std::sqrt(std::max(0.0, 1 - eta * eta * (1 - c * c)));
                                        const float3 t = normalize(m * (float)(eta * c - ct) - wof * (float)eta);
                                        if (t.z >= 0) break;
                                        w *= g1(-t.z, ac);
                                        const float3 out{ t.x, t.y, -t.z };  // real frame: going up, out
                                        P[bin(out)] += w * wrgb.luminance();
                                        break;
                                    }
                                }
                                if (bounce > 8)
                                {
                                    const double q = std::min(1.0, w * wrgb.max());
                                    if (rng.uniform() >= q) break;
                                    w /= q;
                                }
                            }
                        }
                    }
                });
                std::array<double, NT * NP> Dsum{}, Psum{};
                for (uint32_t ch = 0; ch < chunks; ++ch)
                    for (int k = 0; k < NT * NP; ++k)
                    {
                        Dsum[k] += defBins[ch][k] / N;
                        Psum[k] += physBins[ch][k] / N;
                    }
                double ad = 0, ap = 0, l1 = 0;
                for (int k = 0; k < NT * NP; ++k)
                {
                    ad += Dsum[k];
                    ap += Psum[k];
                    l1 += std::fabs(Dsum[k] - Psum[k]);
                }
                char row[256];
                std::snprintf(row, sizeof row, "| %s | %.2f | %d | %.4f | %.4f | %+.4f | %+.1f%% | %.3f |\n", base.name, rc, deg, ad, ap, ad - ap, ap > 0 ? 100 * (ad - ap) / ap : 0.0,
                              ap > 0 ? l1 / ap : 0.0);
                md << row;
                logf("%s", row);
            }
    }
    writeTextFile(out, md.str());
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
        if (argc < 3) fail("usage: unx_study_material_layers thinfilm|clearcoat <out.md>");
        if (std::strcmp(argv[1], "thinfilm") == 0) thinFilm(argv[2]);
        else if (std::strcmp(argv[1], "clearcoat") == 0) clearcoat(argv[2]);
        else fail("unknown study %s", argv[1]);
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
