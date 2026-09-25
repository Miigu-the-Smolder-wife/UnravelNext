#include "AtmosphereReference.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace unx::render::atmosphere::reference
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kInf = std::numeric_limits<double>::infinity();

// Ray-sphere about the planet centre, relative to the surface origin (no large-number cancellation).
bool sphere(const Model& m, D3 p, D3 d, double radius, double& t0, double& t1)
{
    const double b = dot(p, d) + m.bottom * d.y;
    const double c = dot(p, p) + 2 * m.bottom * p.y - (radius - m.bottom) * (radius + m.bottom);
    const double disc = b * b - c;
    if (disc < 0) return false;
    const double root = std::sqrt(disc);
    const double q = -b - (b < 0 ? -root : root);
    if (q == 0)
    {
        t0 = t1 = 0;
        return true;
    }
    t0 = std::min(q, c / q);
    t1 = std::max(q, c / q);
    return true;
}
} // namespace

D3 normalize(D3 a)
{
    const double l = std::sqrt(dot(a, a));
    return a * (1 / l);
}

D3 expNeg(D3 a) { return { std::exp(-a.x), std::exp(-a.y), std::exp(-a.z) }; }

Model fromScene(const scene::Atmosphere& a)
{
    Model m;
    m.bottom = a.bottomRadius;
    m.top = a.topRadius;
    m.hr = a.rayleighScaleHeight;
    m.hm = a.mieScaleHeight;
    m.g = a.mieG;
    m.rayleigh = { a.rayleighScattering.x, a.rayleighScattering.y, a.rayleighScattering.z };
    m.mieScattering = { a.mieScattering.x, a.mieScattering.y, a.mieScattering.z };
    m.mieAbsorption = { a.mieAbsorption.x, a.mieAbsorption.y, a.mieAbsorption.z };
    m.ozone = { a.ozoneAbsorption.x, a.ozoneAbsorption.y, a.ozoneAbsorption.z };
    m.albedo = { a.groundAlbedo.x, a.groundAlbedo.y, a.groundAlbedo.z };
    m.ozoneCenter = a.ozoneCenter;
    m.ozoneWidth = a.ozoneWidth;
    return m;
}

Coefficients coefficients(const Model& m, double h)
{
    Coefficients c;
    if (h < 0 || h > m.top - m.bottom) return c;
    const double md = std::exp(-h / m.hm);
    c.rayleigh = m.rayleigh * std::exp(-h / m.hr);
    c.mie = m.mieScattering * md;
    c.extinction = c.rayleigh + c.mie + m.mieAbsorption * md + m.ozone * std::max(0.0, 1 - std::abs(h - m.ozoneCenter) / m.ozoneWidth);
    return c;
}

double altitudeOf(const Model& m, D3 p)
{
    const double h2 = dot(p, p) + 2 * m.bottom * p.y;
    return h2 / (std::sqrt(std::max(0.0, m.bottom * m.bottom + h2)) + m.bottom);
}

D3 upOf(const Model& m, D3 p) { return normalize(p + D3{ 0, m.bottom, 0 }); }

bool hitsGround(const Model& m, D3 p, D3 d)
{
    double t0, t1;
    return sphere(m, p, d, m.bottom, t0, t1) && t1 > 0 && t0 >= 0;
}

double distanceToTop(const Model& m, D3 p, D3 d)
{
    double t0, t1;
    if (!sphere(m, p, d, m.top, t0, t1)) return -1;
    return t1;
}

double distanceToGround(const Model& m, D3 p, D3 d)
{
    double t0, t1;
    if (!sphere(m, p, d, m.bottom, t0, t1) || t1 <= 0 || t0 < 0) return kInf;
    return t0;
}

D3 opticalDepth(const Model& m, D3 p, D3 d, int intervals, bool groundBlocks)
{
    if (groundBlocks && hitsGround(m, p, d)) return { kInf, kInf, kInf };
    const double L = distanceToTop(m, p, d);
    if (L <= 0) return {};
    // Quadratic spacing t = L u^2 concentrates samples where the density is highest for upward rays; the midpoint rule
    // in u integrates the smooth transformed integrand f(L u^2) 2 L u du.
    D3 tau;
    for (int i = 0; i < intervals; ++i)
    {
        const double u = (i + 0.5) / intervals;
        const double t = L * u * u;
        tau = tau + coefficients(m, altitudeOf(m, p + d * t)).extinction * (2 * L * u / intervals);
    }
    return tau;
}

D3 opticalDepth(const Model& m, double altitude, double cosine, int intervals)
{
    const D3 p{ 0, altitude, 0 };
    const D3 d{ std::sqrt(std::max(0.0, 1 - cosine * cosine)), cosine, 0 };
    return opticalDepth(m, p, d, intervals, false);  // the LUT domain: rays to the top, the tangent ray included
}

D3 sunTransmittance(const Model& m, D3 p, D3 sun, int intervals)
{
    if (hitsGround(m, p, sun)) return {};
    return expNeg(opticalDepth(m, p, sun, intervals, true));
}

double rayleighPhase(double c) { return 3.0 / (16 * kPi) * (1 + c * c); }
double miePhase(double c, double g)
{
    const double den = 1 + g * g - 2 * g * c;
    return (1 - g * g) / (4 * kPi * den * std::sqrt(den));
}

D3 skyRadiance(const Model& m, D3 p, D3 d, D3 sun, const MsFn& ms, const GroundFn& groundFn, int steps)
{
    const double ground = distanceToGround(m, p, d);
    const double end = std::min(ground, std::max(0.0, distanceToTop(m, p, d)));
    const double nu = dot(d, sun), pr = rayleighPhase(nu), pm = miePhase(nu, m.g);
    D3 radiance, T{ 1, 1, 1 };
    // Quadratic spacing from the observer (the density falls off along upward rays; horizontal rays are long).
    double prev = 0;
    for (int i = 0; i < steps; ++i)
    {
        const double u0 = double(i) / steps, u1 = double(i + 1) / steps;
        const double t0 = end * u0 * u0, t1 = end * u1 * u1, dt = t1 - t0;
        const D3 q = p + d * (0.5 * (t0 + t1));
        const Coefficients c = coefficients(m, altitudeOf(m, q));
        const D3 source = (c.rayleigh * pr + c.mie * pm) * sunTransmittance(m, q, sun, 512) + (c.rayleigh + c.mie) * ms(q, d, sun);
        D3 integral;
        for (int k = 0; k < 3; ++k)
        {
            const double e = (&c.extinction.x)[k], tau = e * dt;
            (&integral.x)[k] = tau < 1e-9 ? dt : (1 - std::exp(-tau)) / e;
        }
        radiance = radiance + T * source * integral;
        T = T * expNeg(c.extinction * dt);
        prev = t1;
    }
    (void)prev;
    if (ground < kInf)
    {
        const D3 q = p + d * ground, up = upOf(m, q);
        const double mus = dot(up, sun);
        radiance = radiance + T * m.albedo * ((sunTransmittance(m, q + up * 0.01, sun, 512) * std::max(0.0, mus) + groundFn(mus)) * (1 / kPi));
    }
    return radiance;
}

void aerial(const Model& m, D3 p, D3 d, double distance, D3 sun, const MsFn& ms, D3& inscatter, D3& transmittance, int steps)
{
    const double nu = dot(d, sun), pr = rayleighPhase(nu), pm = miePhase(nu, m.g);
    D3 L, T{ 1, 1, 1 };
    const double dt = distance / steps;
    for (int i = 0; i < steps; ++i)
    {
        D3 q = p + d * ((i + 0.5) * dt);
        const double h = altitudeOf(m, q);
        if (h < 0) q = q + upOf(m, q) * (-h);  // surface air below the model's surface (airLiftToSurface)
        const Coefficients c = coefficients(m, std::max(0.0, h));
        const D3 source = (c.rayleigh * pr + c.mie * pm) * sunTransmittance(m, q, sun, 512) + (c.rayleigh + c.mie) * ms(q, d, sun);
        D3 integral;
        for (int k = 0; k < 3; ++k)
        {
            const double e = (&c.extinction.x)[k], tau = e * dt;
            (&integral.x)[k] = tau < 1e-9 ? dt : (1 - std::exp(-tau)) / e;
        }
        L = L + T * source * integral;
        T = T * expNeg(c.extinction * dt);
    }
    inscatter = L;
    transmittance = T;
}
D3 singleScattering(const Model& m, D3 p, D3 d, D3 sun, int steps)
{
    const double ground = distanceToGround(m, p, d);
    const double end = std::min(ground, std::max(0.0, distanceToTop(m, p, d)));
    const double nu = dot(d, sun), pr = rayleighPhase(nu), pm = miePhase(nu, m.g);
    D3 radiance, T{ 1, 1, 1 };
    for (int i = 0; i < steps; ++i)
    {
        const double u0 = double(i) / steps, u1 = double(i + 1) / steps;
        const double t0 = end * u0 * u0, t1 = end * u1 * u1, dt = t1 - t0;
        const D3 q = p + d * (0.5 * (t0 + t1));
        const Coefficients c = coefficients(m, altitudeOf(m, q));
        D3 integral;
        for (int k = 0; k < 3; ++k)
        {
            const double e = (&c.extinction.x)[k], tau = e * dt;
            (&integral.x)[k] = tau < 1e-9 ? dt : (1 - std::exp(-tau)) / e;
        }
        radiance = radiance + T * (c.rayleigh * pr + c.mie * pm) * sunTransmittance(m, q, sun, 512) * integral;
        T = T * expNeg(c.extinction * dt);
    }
    if (ground < kInf)
    {
        const D3 q = p + d * ground, up = upOf(m, q);
        radiance = radiance + T * m.albedo * sunTransmittance(m, q + up * 0.01, sun, 512) * (std::max(0.0, dot(up, sun)) / kPi);
    }
    return radiance;
}

D3 sphereSource(const Model& m, D3 p, D3 v, D3 sun, const std::function<D3(D3 w)>& radiance, int nodes)
{
    const int polar = std::max(1, (int)std::lround(std::sqrt(double(nodes)))), azimuths = std::max(1, nodes / polar);
    const int perHg = polar * azimuths;
    const double g = m.g;
    auto hgCosine = [&](double u) {
        if (std::abs(g) < 1e-3) return 1 - 2 * u;
        const double s = (1 - g * g) / (1 - g + 2 * g * u);
        return std::clamp((1 + g * g - s * s) / (2 * g), -1.0, 1.0);
    };
    const int rings = std::max(1, (int)std::lround(std::sqrt(nodes / 2.0))), around = 2 * rings, grid = 2 * rings * around;
    const double hz = -std::acos(std::clamp(m.bottom / (m.bottom + std::max(0.0, altitudeOf(m, p))), 0.0, 1.0));
    // The grid's frame: the local vertical at p.
    const D3 up = upOf(m, p), side = normalize(std::abs(up.x) < 0.9 ? D3{ 1 - up.x * up.x, -up.x * up.y, -up.x * up.z } : D3{ -up.z * up.x, -up.z * up.y, 1 - up.z * up.z });
    const D3 side2{ up.y * side.z - up.z * side.y, up.z * side.x - up.x * side.z, up.x * side.y - up.y * side.x };
    auto gridPdf = [&](D3 w) {
        const double el = std::asin(std::clamp(dot(w, up), -1.0, 1.0));
        const double range = el >= hz ? kPi / 2 - hz : hz + kPi / 2;
        const double u = std::max(std::sqrt(std::clamp(std::abs(el - hz) / range, 0.0, 1.0)), 1e-4);
        return 0.5 / (2 * u * range) / (2 * kPi) / std::max(std::cos(el), 1e-9);
    };
    D3 sR, sM;
    for (int set = 0; set < 3; ++set)
    {
        const int count = set < 2 ? perHg : grid;
        const D3 axis = set == 0 ? v : sun;
        const D3 a = std::abs(axis.y) < 0.9 ? D3{ 0, 1, 0 } : D3{ 1, 0, 0 };
        const D3 e1 = normalize(D3{ axis.y * a.z - axis.z * a.y, axis.z * a.x - axis.x * a.z, axis.x * a.y - axis.y * a.x });
        const D3 e2{ axis.y * e1.z - axis.z * e1.y, axis.z * e1.x - axis.x * e1.z, axis.x * e1.y - axis.y * e1.x };
        for (int i = 0; i < count; ++i)
        {
            D3 w;
            if (set < 2)
            {
                const double ct = hgCosine((i / azimuths + 0.5) / polar), st = std::sqrt(std::max(0.0, 1 - ct * ct));
                const double phi = 2 * kPi * ((i % azimuths) + 0.5) / azimuths;
                w = normalize(axis * ct + (e1 * std::cos(phi) + e2 * std::sin(phi)) * st);
            }
            else
            {
                const int above = i / (rings * around), k = (i / around) % rings;
                const double u = (k + 0.5) / rings, phi = 2 * kPi * ((i % around) + 0.5) / around;
                const double el = above ? hz + u * u * (kPi / 2 - hz) : hz - u * u * (hz + kPi / 2);
                w = side * (std::cos(el) * std::cos(phi)) + up * std::sin(el) + side2 * (std::cos(el) * std::sin(phi));
            }
            const double cv = dot(w, v);
            const double density = perHg * (miePhase(cv, g) + miePhase(dot(w, sun), g)) + grid * gridPdf(w);
            const D3 L = radiance(w) * (1 / density);
            sR = sR + L * rayleighPhase(cv);
            sM = sM + L * miePhase(cv, g);
        }
    }
    const Coefficients c = coefficients(m, std::min(altitudeOf(m, p), (m.top - m.bottom) * 0.999999));
    D3 out;
    for (int k = 0; k < 3; ++k)
    {
        const double r = (&c.rayleigh.x)[k], mi = (&c.mie.x)[k];
        (&out.x)[k] = r + mi > 0 ? (r * (&sR.x)[k] + mi * (&sM.x)[k]) / (r + mi) : (&sR.x)[k];
    }
    return out;
}

namespace
{
double horizonElevation(const Model& m, double altitude) { return -std::acos(std::clamp(m.bottom / (m.bottom + std::max(0.0, altitude)), 0.0, 1.0)); }
double msAltitude(const Model& m, double u)
{
    const double H = std::sqrt((m.top - m.bottom) * (m.top + m.bottom)), rho = u * H;
    return rho * rho / (std::sqrt(rho * rho + m.bottom * m.bottom) + m.bottom);
}
double msR(const Model& m, double altitude)
{
    const double H = std::sqrt((m.top - m.bottom) * (m.top + m.bottom));
    return std::clamp(std::sqrt(std::max(0.0, altitude * (altitude + 2 * m.bottom))) / H, 0.0, 1.0);
}
constexpr double kMsTwilight = 0.4;  // ATMO_MS_TWILIGHT
double msSunCoord(double mus)
{
    if (mus >= 0) return 0.5 + 0.5 * (std::sqrt(0.04 + 3.2 * std::min(mus, 1.0)) - 0.2) / 1.6;
    const double m = std::min(-mus / kMsTwilight, 1.0);
    return 0.5 - 0.5 * (std::sqrt(1 + 8 * m) - 1) * 0.5;
}
double msSunCosine(double u)
{
    const double t = 2 * u - 1;
    return t >= 0 ? t * (0.2 + 0.8 * t) : -kMsTwilight * 0.5 * (-t + t * t);
}
double msNuCoord(double nu) { return std::pow(std::acos(std::clamp(nu, -1.0, 1.0)) / kPi, 2.0 / 3.0); }
double msNu(double u) { return std::cos(kPi * u * std::sqrt(u)); }
constexpr double kMsLogMin = -50.0;  // ATMO_MS_LOG_MIN
double msViewRow(const Model& m, uint32_t nMu, double altitude, double mu)
{
    const uint32_t half = nMu / 2;
    const double elevation = std::asin(std::clamp(mu, -1.0, 1.0)), horizon = horizonElevation(m, altitude);
    if (elevation >= horizon) return half + std::sqrt(std::clamp((elevation - horizon) / (kPi / 2 - horizon), 0.0, 1.0)) * (half - 1);
    return std::sqrt(std::clamp((horizon - elevation) / (horizon + kPi / 2), 0.0, 1.0)) * (half - 1);
}
double msViewCosine(const Model& m, uint32_t nMu, double altitude, uint32_t row)
{
    const uint32_t half = nMu / 2;
    const double u = double(row % half) / (half - 1), horizon = horizonElevation(m, altitude);
    const double elevation = row >= half ? horizon + u * u * (kPi / 2 - horizon) : horizon - u * u * (horizon + kPi / 2);
    return std::sin(elevation);
}
} // namespace

MsTexelCoords msTexel(const Model& m, const MsTable& t, uint32_t iNu, uint32_t iMus, uint32_t iMu, uint32_t iR)
{
    MsTexelCoords c;
    c.altitude = msAltitude(m, double(iR) / (t.n[3] - 1));
    c.mu = msViewCosine(m, t.n[2], c.altitude, iMu);
    c.mus = msSunCosine(double(iMus) / (t.n[1] - 1));
    const double sm = std::sqrt(std::max(0.0, 1 - c.mu * c.mu)), ss = std::sqrt(std::max(0.0, 1 - c.mus * c.mus));
    c.nu = std::clamp(msNu(double(iNu) / (t.n[0] - 1)), c.mu * c.mus - sm * ss, c.mu * c.mus + sm * ss);
    return c;
}

namespace
{
D3 msTexelLog(const MsTable& t, uint32_t iNu, uint32_t iMus, uint32_t iMu, uint32_t iR)
{
    // R16G16B16A16_UNORM: (ln J^ - kMsLogMin) / -kMsLogMin.
    // Texture N_mus x N_mu x (N_nu N_r) (AtmosphereCommon.hlsli); readback rows 256 B aligned.
    const size_t pitch = (size_t(t.n[1]) * 8 + 255) & ~size_t(255);
    const size_t offset = ((size_t(iNu) * t.n[3] + iR) * t.n[2] + iMu) * pitch + size_t(iMus) * 8;
    uint16_t v[3];
    std::memcpy(v, t.texels->data() + offset, 6);
    return D3{ kMsLogMin * (1 - v[0] / 65535.0), kMsLogMin * (1 - v[1] / 65535.0), kMsLogMin * (1 - v[2] / 65535.0) };
}
} // namespace

D3 msTexelValue(const MsTable& t, uint32_t iNu, uint32_t iMus, uint32_t iMu, uint32_t iR)
{
    return expNeg(msTexelLog(t, iNu, iMus, iMu, iR) * -1.0);
}

D3 msTableLookup(const Model& m, const MsTable& t, double altitude, double mu, double mus, double nu)
{
    // Continuous node coordinates; mu stays within its half (as the GPU's clamp-to-row-range does).
    const double y = msViewRow(m, t.n[2], altitude, mu), z = msR(m, altitude) * (t.n[3] - 1);
    const double fsCoord = msSunCoord(mus) * (t.n[1] - 1), fn = msNuCoord(nu) * (t.n[0] - 1);
    const uint32_t half = t.n[2] / 2;
    const uint32_t y0 = std::min((uint32_t)y, t.n[2] - 1), y1 = (y0 + 1 < t.n[2] && (y0 + 1) / half == y0 / half) ? y0 + 1 : y0;
    const uint32_t z0 = std::min((uint32_t)z, t.n[3] - 1), z1 = std::min(z0 + 1, t.n[3] - 1);
    const uint32_t n0 = std::min((uint32_t)fn, t.n[0] - 2), n1 = n0 + 1;
    const double s1 = std::min(std::floor(fsCoord), double(t.n[1] - 2)), ts = fsCoord - s1;
    // Catmull-Rom along mu_s (AtmosphereCommon.hlsli airCatmullRom), linear in the other axes.
    const double t2 = ts * ts, t3 = t2 * ts;
    const double ws[4] = { -0.5 * t3 + t2 - 0.5 * ts, 1.5 * t3 - 2.5 * t2 + 1, -1.5 * t3 + 2 * t2 + 0.5 * ts, 0.5 * t3 - 0.5 * t2 };
    const double fy = y - y0, fz = z - z0, fnu = fn - n0;
    D3 v;
    for (int k = 0; k < 4; ++k)
    {
        const uint32_t sk = (uint32_t)std::clamp(s1 - 1 + k, 0.0, double(t.n[1] - 1));
        for (int a = 0; a < 2; ++a)
            for (int c = 0; c < 2; ++c)
                for (int d = 0; d < 2; ++d)
                {
                    const double w = ws[k] * (a ? fnu : 1 - fnu) * (c ? fy : 1 - fy) * (d ? fz : 1 - fz);
                    if (w != 0) v = v + msTexelLog(t, a ? n1 : n0, sk, c ? y1 : y0, d ? z1 : z0) * w;
                }
    }
    return expNeg(v * -1.0);  // log-domain interpolation (AtmosphereCommon.hlsli)
}

D3 groundIndirectLookup(const std::vector<uint8_t>& transmittance, uint32_t w, uint32_t row, double mus)
{
    const double q = msSunCoord(std::clamp(mus, -1.0, 1.0)) * (w - 1);
    const uint32_t i0 = std::min((uint32_t)q, w - 2);
    const double f = q - i0;
    float a[4], b[4];
    std::memcpy(a, transmittance.data() + (size_t(row) * w + i0) * 16, 16);
    std::memcpy(b, transmittance.data() + (size_t(row) * w + i0 + 1) * 16, 16);
    return D3{ a[0], a[1], a[2] } * (1 - f) + D3{ b[0], b[1], b[2] } * f;
}
} // namespace unx::render::atmosphere::reference
