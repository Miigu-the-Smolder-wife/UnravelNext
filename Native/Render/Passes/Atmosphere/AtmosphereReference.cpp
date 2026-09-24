#include "AtmosphereReference.h"

#include <algorithm>
#include <cmath>
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

D3 multiScatter(const Model& m, double h, double mus, int directions, int steps)
{
    const D3 origin{ 0, h, 0 }, sun{ std::sqrt(std::max(0.0, 1 - mus * mus)), mus, 0 };
    D3 second, loss;
    for (int n = 0; n < directions; ++n)
    {
        const double z = 1 - 2 * (n + 0.5) / directions, phi = n * 2.399963229728653, s = std::sqrt(1 - z * z);
        const D3 d{ s * std::cos(phi), z, s * std::sin(phi) };
        const double ground = distanceToGround(m, origin, d);
        const double end = std::min(ground, std::max(0.0, distanceToTop(m, origin, d)));
        D3 T{ 1, 1, 1 };
        for (int j = 0; j < steps; ++j)
        {
            const double u0 = double(j) / steps, u1 = double(j + 1) / steps;
            const double step = end * (u1 * u1 - u0 * u0);
            const D3 p = origin + d * (end * u0 * u0 + 0.5 * step);
            const Coefficients c = coefficients(m, altitudeOf(m, p));
            D3 integral;
            for (int k = 0; k < 3; ++k)
            {
                const double e = (&c.extinction.x)[k], tau = e * step;
                (&integral.x)[k] = tau < 1e-9 ? step : (1 - std::exp(-tau)) / e;
            }
            second = second + T * integral * (c.rayleigh + c.mie) * sunTransmittance(m, p, sun, 256);
            D3 absorb = c.extinction - c.rayleigh - c.mie;
            loss = loss + T * integral * D3{ std::max(0.0, absorb.x), std::max(0.0, absorb.y), std::max(0.0, absorb.z) };
            T = T * expNeg(c.extinction * step);
        }
        if (ground < kInf)
        {
            const D3 p = origin + d * ground, up = upOf(m, p);
            second = second + T * m.albedo * (4 * std::max(0.0, dot(up, sun))) * sunTransmittance(m, p + up * 0.01, sun, 512);
            loss = loss + T * (D3{ 1, 1, 1 } - m.albedo);
        }
        else
            loss = loss + T;
    }
    return { second.x / (4 * kPi * loss.x), second.y / (4 * kPi * loss.y), second.z / (4 * kPi * loss.z) };
}

D3 skyRadiance(const Model& m, D3 p, D3 d, D3 sun, const PsiFn& psi, int steps)
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
        const D3 source = (c.rayleigh * pr + c.mie * pm) * sunTransmittance(m, q, sun, 512) + (c.rayleigh + c.mie) * psi(q, sun);
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
        radiance = radiance + T * m.albedo * (sunTransmittance(m, q + up * 0.01, sun, 512) * (std::max(0.0, dot(up, sun)) / kPi) + psi(q, sun));
    }
    return radiance;
}

void aerial(const Model& m, D3 p, D3 d, double distance, D3 sun, const PsiFn& psi, D3& inscatter, D3& transmittance, int steps)
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
        const D3 source = (c.rayleigh * pr + c.mie * pm) * sunTransmittance(m, q, sun, 512) + (c.rayleigh + c.mie) * psi(q, sun);
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
} // namespace unx::render::atmosphere::reference
