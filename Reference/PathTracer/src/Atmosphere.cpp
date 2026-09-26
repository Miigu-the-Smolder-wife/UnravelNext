#include "Atmosphere.h"

#include "unx/core/Jobs.h"
#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>

namespace unx::reference
{
namespace
{
// 8-point Gauss-Legendre nodes/weights on [-1, 1].
constexpr double kGlX[8] = { -0.9602898564975363, -0.7966664774136267, -0.5255324099163290, -0.1834346424956498,
                             0.1834346424956498, 0.5255324099163290, 0.7966664774136267, 0.9602898564975363 };
constexpr double kGlW[8] = { 0.1012285362903763, 0.2223810344533745, 0.3137066458778873, 0.3626837833783620,
                             0.3626837833783620, 0.3137066458778873, 0.2223810344533745, 0.1012285362903763 };

double cubicWeight(double t, int k)
{
    // Catmull-Rom weights for samples at -1, 0, 1, 2 around t in [0, 1].
    const double t2 = t * t, t3 = t2 * t;
    switch (k)
    {
    case 0: return -0.5 * t3 + t2 - 0.5 * t;
    case 1: return 1.5 * t3 - 2.5 * t2 + 1.0;
    case 2: return -1.5 * t3 + 2.0 * t2 + 0.5 * t;
    default: return 0.5 * t3 - 0.5 * t2;
    }
}
} // namespace

AtmosphereModel::AtmosphereModel(const scene::Atmosphere& a, bool buildTable) : m_a(a)
{
    m_R = a.bottomRadius;
    m_Rt = a.topRadius;
    if (!(m_Rt > m_R && m_R > 0)) fail("atmosphere: invalid radii");
    m_H = std::sqrt(m_Rt * m_Rt - m_R * m_R);
    if (!buildTable) return;
    m_table.resize((size_t)kTableMu * kTableR);
    Jobs::instance().parallelFor(kTableR, [&](uint32_t ir) {
        for (uint32_t im = 0; im < kTableMu; ++im) m_table[(size_t)ir * kTableMu + im] = tableEntry(ir, im);
    });
}

std::array<float, 3> AtmosphereModel::tableEntry(uint32_t ir, uint32_t im) const
{
    const double xr = (double)ir / (kTableR - 1);
    const double rho = m_H * xr, r = std::sqrt(rho * rho + m_R * m_R);
    const double dMin = m_Rt - r, dMax = rho + m_H;
    const double xm = (double)im / (kTableMu - 1);
    const double d = dMin + xm * (dMax - dMin);
    const double mu = d <= 0 ? 1.0 : std::clamp((m_H * m_H - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
    const Rgb t = depthTopDirect(r, mu, 256);
    return { t.r, t.g, t.b };
}

AtmosphereModel::Coefficients AtmosphereModel::at(double h) const
{
    const float rayleigh = (float)std::exp(-h / m_a.rayleighScaleHeight), mie = (float)std::exp(-h / m_a.mieScaleHeight);
    const float ozone = (float)std::max(0.0, 1.0 - std::fabs(h - m_a.ozoneCenter) / m_a.ozoneWidth);
    Coefficients c;
    c.scatteringRayleigh = Rgb(m_a.rayleighScattering) * rayleigh;
    c.scatteringMie = Rgb(m_a.mieScattering) * mie;
    c.extinction = c.scatteringRayleigh + c.scatteringMie + Rgb(m_a.mieAbsorption) * mie + Rgb(m_a.ozoneAbsorption) * ozone;
    return c;
}

double AtmosphereModel::altitude(const Double3& p) const
{
    const double y = p.y + m_R;
    return std::sqrt(p.x * p.x + y * y + p.z * p.z) - m_R;
}

float AtmosphereModel::phaseRayleigh(float c) const { return 0.05968310365946075f * (1 + c * c); }

float AtmosphereModel::phaseMie(float c) const
{
    const float g = m_a.mieG, den = 1 + g * g - 2 * g * c;
    return (1 - g * g) / (12.566370614359172f * den * std::sqrt(den));
}

float3 AtmosphereModel::samplePhase(float3 forward, float wR, float wM, float u1, float u2, float u3, float& pdf) const
{
    float c;
    if (u1 * (wR + wM) < wR)
    {
        // Rayleigh: invert the CDF (3/8)(mu + mu^3/3 + 4/3) = u  ->  mu^3 + 3 mu + (4 - 8u) = 0 (Cardano).
        const double q = 4.0 - 8.0 * u2, s = std::sqrt(q * q / 4.0 + 1.0);
        c = (float)(std::cbrt(-q / 2.0 + s) + std::cbrt(-q / 2.0 - s));
    }
    else
    {
        const float g = m_a.mieG;
        const float t = (1 - g * g) / (1 - g + 2 * g * u2);
        c = (1 + g * g - t * t) / (2 * g);
    }
    c = std::clamp(c, -1.0f, 1.0f);
    const float sinT = std::sqrt(std::max(0.0f, 1 - c * c)), phi = 6.283185307f * u3;
    const float3 ref = std::fabs(forward.y) < 0.99f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    const float3 t1 = normalize(cross(ref, forward)), t2 = cross(forward, t1);
    const float3 w = normalize(forward * c + t1 * (sinT * std::cos(phi)) + t2 * (sinT * std::sin(phi)));
    pdf = (wR * phaseRayleigh(c) + wM * phaseMie(c)) / (wR + wM);
    return w;
}

double AtmosphereModel::groundDistance(const Double3& o, float3 d) const
{
    const double ox = o.x, oy = o.y + m_R, oz = o.z;
    const double b = ox * d.x + oy * d.y + oz * d.z, c = ox * ox + oy * oy + oz * oz - m_R * m_R;
    if (c <= 0) return -1;  // at or below the planet surface (scene valleys): only scene geometry is hit there
    const double disc = b * b - c;
    if (disc < 0 || b > 0) return -1;
    const double t = -b - std::sqrt(disc);
    return t > 0 ? t : -1;
}

double AtmosphereModel::topDistance(const Double3& o, float3 d) const
{
    const double ox = o.x, oy = o.y + m_R, oz = o.z;
    const double b = ox * d.x + oy * d.y + oz * d.z, c = ox * ox + oy * oy + oz * oz - m_Rt * m_Rt;
    const double disc = b * b - c;
    if (disc < 0) return -1;
    return -b + std::sqrt(disc);
}

Rgb AtmosphereModel::integrate(const Double3& o, float3 d, double t0, double t1, uint32_t panels) const
{
    double acc[3] = { 0, 0, 0 };
    const double w = (t1 - t0) / panels;
    for (uint32_t p = 0; p < panels; ++p)
    {
        const double mid = t0 + (p + 0.5) * w, half = 0.5 * w;
        for (int k = 0; k < 8; ++k)
        {
            const double t = mid + half * kGlX[k];
            const Coefficients c = at(altitude({ o.x + d.x * t, o.y + d.y * t, o.z + d.z * t }));
            acc[0] += kGlW[k] * half * c.extinction.r;
            acc[1] += kGlW[k] * half * c.extinction.g;
            acc[2] += kGlW[k] * half * c.extinction.b;
        }
    }
    return { (float)acc[0], (float)acc[1], (float)acc[2] };
}

Rgb AtmosphereModel::depthTopDirect(double r, double mu, uint32_t panels) const
{
    // Canonical frame: origin on the +Y axis at radius r, direction in the XY plane.
    const Double3 o{ 0, r - m_R, 0 };
    const float3 d{ (float)std::sqrt(std::max(0.0, 1 - mu * mu)), (float)mu, 0 };
    const double len = -r * mu + std::sqrt(std::max(0.0, r * r * (mu * mu - 1) + m_Rt * m_Rt));
    if (len <= 0) return {};
    // Split at the point of closest approach (grazing rays have their density peak there).
    const double tStar = -r * mu;
    if (tStar > 0 && tStar < len)
    {
        const uint32_t p0 = std::max(1u, (uint32_t)std::lround(panels * tStar / len));
        return integrate(o, d, 0, tStar, p0) + integrate(o, d, tStar, len, std::max(1u, panels - p0));
    }
    return integrate(o, d, 0, len, panels);
}

Rgb AtmosphereModel::depthTopTable(double r, double mu) const
{
    if (m_table.empty()) fail("atmosphere: no tau_top table (the model was built without it)");
    r = std::clamp(r, m_R, m_Rt);
    const double rho = std::sqrt(std::max(0.0, r * r - m_R * m_R));
    const double d = -r * mu + std::sqrt(std::max(0.0, r * r * (mu * mu - 1) + m_Rt * m_Rt));
    const double dMin = m_Rt - r, dMax = rho + m_H;
    const double xm = std::clamp((d - dMin) / std::max(dMax - dMin, 1e-9), 0.0, 1.0), xr = rho / m_H;
    const double fm = xm * (kTableMu - 1), fr = xr * (kTableR - 1);
    const int im = std::min((int)fm, (int)kTableMu - 2), ir = std::min((int)fr, (int)kTableR - 2);
    const double tm = fm - im, tr = fr - ir;
    // Samples outside the table (the Catmull-Rom stencil of the first and last cells) are quadratic extrapolations of
    // the three nearest inside samples, so the boundary cells keep third-order accuracy. The last x_mu cell is the
    // horizon, where the optical depth is steepest; clamping there (duplicating the end sample) left errors of 0.04.
    auto at = [&](int rr, int mm, int c) {
        auto row = [&](int r, int m) -> double {
            const int nm = (int)kTableMu;
            if (m < 0) return 3.0 * m_table[(size_t)r * nm][c] - 3.0 * m_table[(size_t)r * nm + 1][c] + m_table[(size_t)r * nm + 2][c];
            if (m >= nm) return 3.0 * m_table[(size_t)r * nm + nm - 1][c] - 3.0 * m_table[(size_t)r * nm + nm - 2][c] + m_table[(size_t)r * nm + nm - 3][c];
            return m_table[(size_t)r * nm + m][c];
        };
        const int nr = (int)kTableR;
        if (rr < 0) return 3.0 * row(0, mm) - 3.0 * row(1, mm) + row(2, mm);
        if (rr >= nr) return 3.0 * row(nr - 1, mm) - 3.0 * row(nr - 2, mm) + row(nr - 3, mm);
        return row(rr, mm);
    };
    double wr[4], wm[4];
    for (int k = 0; k < 4; ++k)
    {
        wr[k] = cubicWeight(tr, k);
        wm[k] = cubicWeight(tm, k);
    }
    double acc[3] = { 0, 0, 0 };
    const bool inside = ir >= 1 && ir + 2 < (int)kTableR && im >= 1 && im + 2 < (int)kTableMu;
    for (int a = 0; a < 4; ++a)
    {
        double row[3] = { 0, 0, 0 };
        if (inside)
        {
            const std::array<float, 3>* p = &m_table[(size_t)(ir - 1 + a) * kTableMu + (im - 1)];
            for (int b = 0; b < 4; ++b)
                for (int c = 0; c < 3; ++c) row[c] += wm[b] * p[b][c];
        }
        else
            for (int b = 0; b < 4; ++b)
                for (int c = 0; c < 3; ++c) row[c] += wm[b] * at(ir - 1 + a, im - 1 + b, c);
        for (int c = 0; c < 3; ++c) acc[c] += wr[a] * row[c];
    }
    return { (float)std::max(0.0, acc[0]), (float)std::max(0.0, acc[1]), (float)std::max(0.0, acc[2]) };
}

Rgb AtmosphereModel::valleyChordDepth(const Double3& o, float3 d, double& tExit, uint32_t& panels) const
{
    // One 8-point panel per 2 km of the chord's altitude span (the short-segment rule): the chord's lowest point is o or its
    // closest approach to the centre; its highest is the surface crossing.
    const double ox = o.x, oy = o.y + m_R, oz = o.z;
    const double r = std::sqrt(ox * ox + oy * oy + oz * oz);
    const double b = ox * d.x + oy * d.y + oz * d.z, c = r * r - m_R * m_R;
    tExit = -b + std::sqrt(b * b - c);
    double hMin = r - m_R;
    if (-b > 0 && -b < tExit) hMin = std::min(hMin, std::sqrt(std::max(0.0, r * r - b * b)) - m_R);
    panels = std::max(1u, (uint32_t)std::ceil(-hMin / 2000.0));
    return integrate(o, d, 0, tExit, panels);
}

Rgb AtmosphereModel::opticalDepthToTop(const Double3& o, float3 d) const
{
    const double ox = o.x, oy = o.y + m_R, oz = o.z;
    const double r = std::sqrt(ox * ox + oy * oy + oz * oz);
    const double mu = (ox * d.x + oy * d.y + oz * d.z) / r;
    if (r < m_R)
    {
        // Below the planet surface (inside a scene valley): the chord up to the surface crossing (valleyChordDepth), then
        // the table from the surface.
        double tExit;
        uint32_t panels;
        const Rgb chord = valleyChordDepth(o, d, tExit, panels);
        const Double3 e{ o.x + d.x * tExit, o.y + d.y * tExit, o.z + d.z * tExit };
        const double ex = e.x, ey = e.y + m_R, ez = e.z;
        const double re = std::sqrt(ex * ex + ey * ey + ez * ez);
        const double mue = (ex * d.x + ey * d.y + ez * d.z) / re;
        return chord + depthTopTable(m_R, std::max(mue, 0.0));
    }
    if (r > m_Rt) return {};
    // Ground intersection: rays below the horizon never reach the top.
    const double muH = -std::sqrt(std::max(0.0, 1 - (m_R / r) * (m_R / r)));
    if (mu < muH) return Rgb(INFINITY);
    return depthTopTable(r, mu);
}

Rgb AtmosphereModel::opticalDepth(const Double3& o, float3 d, double len) const
{
    if (len <= 0) return {};
    if (len < kShortSegment)
    {
        // Gauss-Legendre accuracy depends on how much the density changes across a panel, i.e. on the altitude span:
        // one 8-point panel per 2 km of altitude (the integrand exp(-h/H) over 2 km of a 1.2 km scale height is
        // integrated to ~1e-12 relative); horizontal segments need a single panel whatever their length.
        const Double3 e{ o.x + d.x * len, o.y + d.y * len, o.z + d.z * len };
        double hMin = std::min(altitude(o), altitude(e)), hMax = std::max(altitude(o), altitude(e));
        const double oy = o.y + m_R, tStar = -(o.x * d.x + oy * d.y + o.z * d.z);
        if (tStar > 0 && tStar < len) hMin = std::min(hMin, altitude({ o.x + d.x * tStar, o.y + d.y * tStar, o.z + d.z * tStar }));
        const uint32_t panels = std::max(1u, (uint32_t)std::ceil((hMax - hMin) / 2000.0));
        return integrate(o, d, 0, len, panels);
    }
    const Double3 e{ o.x + d.x * len, o.y + d.y * len, o.z + d.z * len };
    auto clampPositive = [](Rgb t) { return Rgb(std::max(0.0f, t.r), std::max(0.0f, t.g), std::max(0.0f, t.b)); };
    if (groundDistance(o, d) < 0)
        return clampPositive(opticalDepthToTop(o, d) - opticalDepthToTop(e, d));
    const float3 back{ -d.x, -d.y, -d.z };
    return clampPositive(opticalDepthToTop(e, back) - opticalDepthToTop(o, back));
}

double AtmosphereModel::selfCheck(uint32_t samples, double* maxRelT, bool verbose) const
{
    double maxAbs = 0, maxRel = 0;
    uint64_t state = 0x853C49E6748FEA9Bull;
    auto rnd = [&]() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return (double)(state >> 11) * (1.0 / 9007199254740992.0);
    };
    for (uint32_t i = 0; i < samples; ++i)
    {
        // Altitudes biased to the scene range: half the samples below 1 km.
        const double h = (i & 1) ? rnd() * 1000.0 : rnd() * 100000.0;
        const double r = m_R + h;
        const double muH = -std::sqrt(std::max(0.0, 1 - (m_R / r) * (m_R / r)));
        const double mu = muH + (1 - muH) * std::pow(rnd(), 2.0);  // denser near the horizon
        const Rgb a = depthTopTable(r, mu), b = depthTopDirect(r, mu, 8192);
        for (float dt : { a.r - b.r, a.g - b.g, a.b - b.b })
        {
            const double e = std::fabs(dt);
            maxAbs = std::max(maxAbs, e);
        }
        // Transmittance error where the transmittance is not negligible (> 1e-4).
        const float tb[3] = { b.r, b.g, b.b }, ta[3] = { a.r, a.g, a.b };
        for (int c = 0; c < 3; ++c)
            if (std::exp(-tb[c]) > 1e-4)
            {
                const double e = std::fabs(std::expm1(-(double)(ta[c] - tb[c])));
                if (e > maxRel && verbose)
                    logf("    worst so far: h %.3f m, mu %.7f (muH %.7f), channel %d, table %.6f direct %.6f\n", h, mu, muH, c, ta[c], tb[c]);
                maxRel = std::max(maxRel, e);
            }
    }
    if (maxRelT) *maxRelT = maxRel;
    return maxAbs;
}
} // namespace unx::reference
