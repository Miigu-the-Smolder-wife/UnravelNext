#include "ThinFilm.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace unx::study
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

std::string dataDir() { return std::string(UNX_SOURCE_DIR) + "/Reference/Studies/data/"; }

struct Cmf
{
    std::vector<double> lambda, x, y, z;
    double sumY = 0;
    double m[3][3], inv[3][3];  // XYZ -> Rec.709 white-balanced to E, and its inverse
    Cmf()
    {
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
                lambda.push_back(l);
                x.push_back(a);
                y.push_back(b);
                z.push_back(d);
            }
        }
        double white[3] = { 0, 0, 0 };
        for (size_t i = 0; i < lambda.size(); ++i)
        {
            white[0] += x[i];
            white[1] += y[i];
            white[2] += z[i];
        }
        sumY = white[1];
        for (double& w : white) w /= sumY;
        const double s[3][3] = { { 3.2404542, -1.5371385, -0.4985314 }, { -0.9692660, 1.8760108, 0.0415560 }, { 0.0556434, -0.2040259, 1.0572252 } };
        for (int r = 0; r < 3; ++r)
        {
            const double w = s[r][0] * white[0] + s[r][1] * white[1] + s[r][2] * white[2];
            for (int c = 0; c < 3; ++c) m[r][c] = s[r][c] / w;
        }
        const double a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], ff = m[1][2], g = m[2][0], h = m[2][1], i = m[2][2];
        const double det = a * (e * i - ff * h) - b * (d * i - ff * g) + c * (d * h - e * g);
        inv[0][0] = (e * i - ff * h) / det; inv[0][1] = (c * h - b * i) / det; inv[0][2] = (b * ff - c * e) / det;
        inv[1][0] = (ff * g - d * i) / det; inv[1][1] = (a * i - c * g) / det; inv[1][2] = (c * d - a * ff) / det;
        inv[2][0] = (d * h - e * g) / det; inv[2][1] = (b * g - a * h) / det; inv[2][2] = (a * e - b * d) / det;
    }
    Rgb3 toRgb(const double xyz[3]) const
    {
        return { m[0][0] * xyz[0] + m[0][1] * xyz[1] + m[0][2] * xyz[2], m[1][0] * xyz[0] + m[1][1] * xyz[1] + m[1][2] * xyz[2],
                 m[2][0] * xyz[0] + m[2][1] * xyz[1] + m[2][2] * xyz[2] };
    }
};
const Cmf& cmf()
{
    static const Cmf c;
    return c;
}

cd sqrtUpper(cd z)
{
    cd q = std::sqrt(z);
    return q.imag() < 0 ? -q : q;
}

// Interface amplitude coefficients (s, p) between media with normal components qa, qb and permittivities ea, eb.
void coefficients(cd qa, cd qb, cd ea, cd eb, cd r[2])
{
    r[0] = (qa - qb) / (qa + qb);
    r[1] = (eb * qa - ea * qb) / (eb * qa + ea * qb);
}

// Gaussian fits of the XYZ sensitivities in Fourier space (Belcour & Barla 2017 supplemental), normalised by int y.
struct Gauss
{
    double val, pos, var;
};
const Gauss kFits[4] = { { 5.4856e-13, 1.6810e+06, 4.3278e+09 }, { 4.4201e-13, 1.7953e+06, 9.3046e+09 }, { 5.2481e-13, 2.2084e+06, 6.6121e+09 },
                         { 9.7470e-14, 2.2399e+06, 4.5282e+09 } };  // X, Y, Z, second X lobe
constexpr double kNorm = 1.0685e-7;

// Spectral integration of an RGB method's reflectance evaluated at bin centres (used for evanescent films).
Rgb3 binnedExact(double n0, double cos0, double nf, double d, const Substrate& s, int bins, int channel)
{
    const Cmf& c = cmf();
    double xyz[3] = { 0, 0, 0 };
    const double lo = c.lambda.front(), hi = c.lambda.back();
    for (int b = 0; b < bins; ++b)
    {
        const double a0 = lo + (hi - lo) * b / bins, a1 = lo + (hi - lo) * (b + 1) / bins, mid = 0.5 * (a0 + a1);
        const cd sub = channel >= 0 ? cd(s.rgbN[channel], s.rgbK[channel]) : s.at(mid);
        const double R = airy(n0, cos0, nf, sub, d, mid);
        for (size_t i = 0; i < c.lambda.size(); ++i)
            if (c.lambda[i] >= a0 && (c.lambda[i] < a1 || b == bins - 1))
            {
                xyz[0] += R * c.x[i];
                xyz[1] += R * c.y[i];
                xyz[2] += R * c.z[i];
            }
    }
    for (double& v : xyz) v /= c.sumY;
    return c.toRgb(xyz);
}

// Previous engine's band average of the Airy reflectance over inverse wavelength (Poisson kernel), one polarisation.
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
double bandAverage(double n0, double cos0, double nf, double d, cd sub, double lo, double hi)
{
    const double s2 = n0 * n0 * (1 - cos0 * cos0);
    const cd e2 = sub * sub;
    const cd q2 = sqrtUpper(e2 - s2);
    const double q0 = n0 * cos0;
    if (d == 0)
    {
        cd r[2];
        coefficients(q0, q2, n0 * n0, e2, r);
        return 0.5 * (std::norm(r[0]) + std::norm(r[1]));
    }
    const double q1 = std::sqrt(nf * nf - s2);
    cd r01[2], r12[2];
    coefficients(q0, q1, n0 * n0, nf * nf, r01);
    coefficients(q1, q2, nf * nf, e2, r12);
    const double path = 2 * d * q1;
    return 0.5 * (bandReflectance(r01[0].real(), r12[0], path, lo, hi) + bandReflectance(r01[1].real(), r12[1], path, lo, hi));
}
} // namespace

cd Substrate::at(double lambdaNm) const
{
    if (!spectral) return cd(n, k);
    const double l = lambdaNm * 1e-3;
    size_t i = 1;
    while (i + 1 < wl.size() && wl[i] < l) ++i;
    const double t = std::clamp((l - wl[i - 1]) / (wl[i] - wl[i - 1]), 0.0, 1.0);
    return cd(sn[i - 1] + t * (sn[i] - sn[i - 1]), sk[i - 1] + t * (sk[i] - sk[i - 1]));
}

Substrate constantSubstrate(const std::string& name, double n, double k)
{
    Substrate s;
    s.name = name;
    s.n = n;
    s.k = k;
    for (int c = 0; c < 3; ++c)
    {
        s.rgbN[c] = n;
        s.rgbK[c] = k;
    }
    return s;
}

Substrate metal(const std::string& name, const std::string& file)
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
    for (int ch = 0; ch < 3; ++ch)
    {
        const cd v = s.at(at[ch]);
        s.rgbN[ch] = v.real();
        s.rgbK[ch] = v.imag();
    }
    return s;
}

double airy(double n0, double cos0, double nf, cd n2, double dNm, double lambdaNm)
{
    const double s2 = n0 * n0 * (1 - cos0 * cos0);
    const cd q0 = n0 * cos0, q1 = sqrtUpper(cd(nf * nf - s2, 0)), q2 = sqrtUpper(n2 * n2 - s2);
    cd r01[2], r12[2];
    coefficients(q0, q1, n0 * n0, nf * nf, r01);
    coefficients(q1, q2, nf * nf, n2 * n2, r12);
    const cd phase = std::exp(cd(0, 1) * (4 * kPi * dNm / lambdaNm) * q1);
    double R = 0;
    for (int p = 0; p < 2; ++p) R += 0.5 * std::norm((r01[p] + r12[p] * phase) / (1.0 + r01[p] * r12[p] * phase));
    return R;
}

Rgb3 filmReference(double n0, double cos0, double nf, double d, const Substrate& s)
{
    const Cmf& c = cmf();
    double xyz[3] = { 0, 0, 0 };
    for (size_t i = 0; i < c.lambda.size(); ++i)
    {
        const double R = airy(n0, cos0, nf, s.at(c.lambda[i]), d, c.lambda[i]);
        xyz[0] += R * c.x[i];
        xyz[1] += R * c.y[i];
        xyz[2] += R * c.z[i];
    }
    for (double& v : xyz) v /= c.sumY;
    return c.toRgb(xyz);
}

Rgb3 filmA(double n0, double cos0, double nf, double d, const Substrate& s)
{
    const Cmf& c = cmf();
    const double s2 = n0 * n0 * (1 - cos0 * cos0);
    Rgb3 out{};
    for (int ch = 0; ch < 3; ++ch)
    {
        if (nf * nf - s2 <= 0)
        {
            // Evanescent film (total internal reflection at the outer interface): no interference fringes; the exact
            // Airy reflectance is smooth in wavelength and integrated over 16 bins.
            out[ch] = binnedExact(n0, cos0, nf, d, s, 16, ch)[ch];
            continue;
        }
        const cd sub(s.rgbN[ch], s.rgbK[ch]);
        const double q0 = n0 * cos0, q1 = std::sqrt(nf * nf - s2);
        const cd q2 = sqrtUpper(sub * sub - s2);
        cd r01[2], r12[2];
        coefficients(q0, q1, n0 * n0, nf * nf, r01);
        coefficients(q1, q2, nf * nf, sub * sub, r12);
        const double phase = 2 * kPi * (2 * d * 1e-3 * q1) * 1e-6;  // opd (um) -> m
        double xyz[3] = { 0, 0, 0 };
        for (int p = 0; p < 2; ++p)
        {
            const double R12 = std::norm(r01[p]), R23 = std::norm(r12[p]), T = 1 - R12;
            const double r = std::abs(r01[p]) * std::abs(r12[p]), phi = std::arg(-r01[p] * r12[p]);
            const double Rs = T * T * R23 / (1 - R12 * R23);
            for (double& v : xyz) v += 0.5 * (R12 + Rs);
            for (int g = 0; g < 4; ++g)
            {
                const Gauss& G = kFits[g];
                const int axis = g == 3 ? 0 : g;
                const double amp = G.val * std::sqrt(2 * kPi * G.var) / kNorm;
                const double theta = G.pos * phase + phi;
                double sum = 0, rm = 1;
                for (int m = 1; m <= 3; ++m)
                {
                    rm *= r;
                    sum += rm * std::cos(m * theta) * std::exp(-G.var * (m * phase) * (m * phase));
                }
                // m >= 4: sum r^m cos(m theta) = Re[z^4 / (1 - z)], z = r e^{i theta}, damped with the m = 4 Gaussian.
                const cd z = r * std::exp(cd(0, theta));
                sum += std::real(std::pow(z, 4) / (1.0 - z)) * std::exp(-G.var * (4 * phase) * (4 * phase));
                xyz[axis] += 0.5 * 2 * (Rs - T) * amp * sum;
            }
        }
        out[ch] = c.toRgb(xyz)[ch];
    }
    return out;
}

Rgb3 filmB(double n0, double cos0, double nf, double d, const Substrate& s)
{
    const double bands[3][2] = { { 580, 700 }, { 490, 580 }, { 400, 490 } };
    const double s2 = n0 * n0 * (1 - cos0 * cos0);
    Rgb3 out{};
    for (int ch = 0; ch < 3; ++ch)
        out[ch] = (d > 0 && nf * nf - s2 <= 0) ? 1.0 : bandAverage(n0, cos0, nf, d, cd(s.rgbN[ch], s.rgbK[ch]), bands[ch][0], bands[ch][1]);
    return out;
}

Rgb3 filmC(double n0, double cos0, double nf, double d, const Substrate& s, int bins)
{
    const double s2 = n0 * n0 * (1 - cos0 * cos0);
    if (d > 0 && nf * nf - s2 <= 0) return binnedExact(n0, cos0, nf, d, s, bins, -1);
    const Cmf& c = cmf();
    // Bins uniform in wavenumber over the CMF support.
    const double k0 = 1 / c.lambda.back(), k1 = 1 / c.lambda.front();
    double xyz[3] = { 0, 0, 0 };
    for (int b = 0; b < bins; ++b)
    {
        const double lo = 1 / (k1 - (k1 - k0) * b / bins), hi = 1 / (k1 - (k1 - k0) * (b + 1) / bins);  // lo < hi (nm)
        // (c) stores the substrate per bin: spectral n, k at the bin centre (N complex values per material).
        const double R = bandAverage(n0, cos0, nf, d, s.at(0.5 * (lo + hi)), lo, hi);
        for (size_t i = 0; i < c.lambda.size(); ++i)
            if (c.lambda[i] >= lo && (c.lambda[i] < hi || b == bins - 1))
            {
                xyz[0] += R * c.x[i];
                xyz[1] += R * c.y[i];
                xyz[2] += R * c.z[i];
            }
    }
    for (double& v : xyz) v /= c.sumY;
    return c.toRgb(xyz);
}

Rgb3 fitRgb(Substrate& s, double n0)
{
    std::vector<Rgb3> ref(90);
    for (int deg = 0; deg < 90; ++deg) ref[deg] = filmReference(n0, std::cos(deg * kPi / 180), 1.0, 0, s);
    Rgb3 worst{};
    for (int ch = 0; ch < 3; ++ch)
    {
        auto err = [&](double n, double k) {
            double e = 0;
            for (int deg = 0; deg < 90; ++deg)
            {
                const double diff = airy(n0, std::cos(deg * kPi / 180), 1.0, cd(n, k), 0, 550) - ref[deg][ch];
                e += diff * diff;
            }
            return e;
        };
        // Coarse grid, then the smallest k within 0.1 % of the best error (degenerate valleys), then refine locally.
        double best = 1e30;
        std::vector<std::array<double, 3>> grid;
        for (int in = 1; in <= 60; ++in)
            for (int ik = 0; ik <= 120; ++ik)
            {
                const double n = in * 0.05, k = ik * 0.1, e = err(n, k);
                grid.push_back({ n, k, e });
                best = std::min(best, e);
            }
        std::array<double, 3> pick{ 0, 1e30, 0 };
        for (const auto& g : grid)
            if (g[2] <= best * 1.001 + 1e-9 && g[1] < pick[1]) pick = g;
        double bn = pick[0], bk = pick[1], be = pick[2];
        for (int in = -25; in <= 25; ++in)
            for (int ik = -50; ik <= 50; ++ik)
            {
                const double n = pick[0] + in * 0.002, k = pick[1] + ik * 0.002;
                if (n <= 0 || k < 0) continue;
                const double e = err(n, k);
                if (e < be)
                {
                    be = e;
                    bn = n;
                    bk = k;
                }
            }
        s.rgbN[ch] = bn;
        s.rgbK[ch] = bk;
        for (int deg = 0; deg < 90; ++deg)
            worst[ch] = std::max(worst[ch], std::fabs(airy(n0, std::cos(deg * kPi / 180), 1.0, cd(bn, bk), 0, 550) - ref[deg][ch]));
    }
    return worst;
}

double luminance(const Rgb3& c) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

double deltaE76(const Rgb3& a, const Rgb3& b, double whiteY)
{
    const Cmf& c = cmf();
    auto lab = [&](const Rgb3& rgb) {
        double xyz[3], wxyz[3];
        for (int r = 0; r < 3; ++r)
        {
            xyz[r] = c.inv[r][0] * rgb[0] + c.inv[r][1] * rgb[1] + c.inv[r][2] * rgb[2];
            wxyz[r] = (c.inv[r][0] + c.inv[r][1] + c.inv[r][2]) * whiteY;
        }
        auto f = [](double t) { return t > 216.0 / 24389.0 ? std::cbrt(t) : (24389.0 / 27.0 * t + 16) / 116.0; };
        const double fx = f(xyz[0] / wxyz[0]), fy = f(xyz[1] / wxyz[1]), fz = f(xyz[2] / wxyz[2]);
        return Rgb3{ 116 * fy - 16, 500 * (fx - fy), 200 * (fy - fz) };
    };
    const Rgb3 la = lab(a), lb = lab(b);
    return std::sqrt((la[0] - lb[0]) * (la[0] - lb[0]) + (la[1] - lb[1]) * (la[1] - lb[1]) + (la[2] - lb[2]) * (la[2] - lb[2]));
}
} // namespace unx::study
