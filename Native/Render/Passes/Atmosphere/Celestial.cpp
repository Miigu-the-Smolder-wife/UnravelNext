#include "Celestial.h"

#include <algorithm>
#include <cmath>

namespace unx::render::sky
{
namespace
{
constexpr double kPi = 3.14159265358979323846, kDeg = kPi / 180;
constexpr double kEarthRadiusKm = 6378.14, kMoonRadiusKm = 1737.4, kAuKm = 149597870.7;

double wrapDeg(double a) { return a - 360.0 * std::floor(a / 360.0); }
double sinD(double a) { return std::sin(a * kDeg); }
double cosD(double a) { return std::cos(a * kDeg); }

struct Vec3d
{
    double x, y, z;
};
Vec3d sub(Vec3d a, Vec3d b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
Vec3d scale(Vec3d a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double dotd(Vec3d a, Vec3d b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3d unit(Vec3d a)
{
    const double l = std::sqrt(dotd(a, a));
    return scale(a, 1 / l);
}
// Ecliptic (longitude, latitude in degrees) -> equatorial unit vector (x to the vernal equinox, z to the pole).
Vec3d eclipticToEquatorial(double lambda, double beta, double epsilon)
{
    const double cb = cosD(beta);
    return { cb * cosD(lambda), cosD(epsilon) * cb * sinD(lambda) - sinD(epsilon) * sinD(beta), sinD(epsilon) * cb * sinD(lambda) + cosD(epsilon) * sinD(beta) };
}
} // namespace

double julianDay(int year, int month, int day, double hoursUt)
{
    // Meeus 7.1 (Gregorian calendar).
    if (month <= 2)
    {
        year -= 1;
        month += 12;
    }
    const int a = year / 100, b = 2 - a + a / 4;
    return std::floor(365.25 * (year + 4716)) + std::floor(30.6001 * (month + 1)) + day + b - 1524.5 + hoursUt / 24.0;
}

CelestialState celestial(const CelestialTime& t)
{
    CelestialState s;
    const double jd = julianDay(t.year, t.month, t.day, t.hoursUt), n = jd - 2451545.0, T = n / 36525.0;
    s.julianDay = jd;
    const double epsilon = 23.439 - 0.0000004 * n;
    // Sun (Almanac low precision): 0.01 deg.
    const double L = wrapDeg(280.460 + 0.9856474 * n), g = wrapDeg(357.528 + 0.9856003 * n);
    const double lambdaSun = L + 1.915 * sinD(g) + 0.020 * sinD(2 * g);
    const double rSunAu = 1.00014 - 0.01671 * cosD(g) - 0.00014 * cosD(2 * g);
    const Vec3d sunEq = eclipticToEquatorial(lambdaSun, 0, epsilon);
    // Moon (Almanac low precision): ~0.3 deg longitude, ~0.2 deg latitude, parallax 0.003 deg.
    const double lambdaMoon = 218.32 + 481267.881 * T + 6.29 * sinD(135.0 + 477198.87 * T) - 1.27 * sinD(259.3 - 413335.36 * T) +
                              0.66 * sinD(235.7 + 890534.22 * T) + 0.21 * sinD(269.9 + 954397.74 * T) - 0.19 * sinD(357.5 + 35999.05 * T) -
                              0.11 * sinD(186.5 + 966404.03 * T);
    const double betaMoon = 5.13 * sinD(93.3 + 483202.02 * T) + 0.28 * sinD(228.2 + 960400.89 * T) - 0.28 * sinD(318.3 + 6003.15 * T) -
                            0.17 * sinD(217.6 - 407332.21 * T);
    const double parallax = 0.9508 + 0.0518 * cosD(135.0 + 477198.87 * T) + 0.0095 * cosD(259.3 - 413335.36 * T) + 0.0078 * cosD(235.7 + 890534.22 * T) +
                            0.0028 * cosD(269.9 + 954397.74 * T);
    const double rMoonEarthRadii = 1 / sinD(parallax);
    const Vec3d moonEq = eclipticToEquatorial(lambdaMoon, betaMoon, epsilon);
    // Local sidereal time (IAU 1982 GMST).
    const double gmst = wrapDeg(280.46061837 + 360.98564736629 * n + 0.000387933 * T * T - T * T * T / 38710000.0);
    const double lst = wrapDeg(gmst + t.longitudeDeg), phi = t.latitudeDeg;
    s.gmstDeg = gmst;
    s.sunEquatorial = { (float)sunEq.x, (float)sunEq.y, (float)sunEq.z };
    s.moonEquatorial = { (float)moonEq.x, (float)moonEq.y, (float)moonEq.z };
    s.moonGeocentricDistanceKm = rMoonEarthRadii * kEarthRadiusKm;
    const double cl = cosD(lst), sl = sinD(lst), cp = cosD(phi), sp = sinD(phi);
    // Equatorial -> world (east, up, -north): see the header. Rows.
    const double m[3][3] = { { -sl, cl, 0 }, { cp * cl, cp * sl, sp }, { sp * cl, sp * sl, -cp } };
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) s.equatorialToWorld.m[r][c] = c < 3 ? (float)m[r][c] : 0.0f;
    auto toWorld = [&](Vec3d e) {
        return float3{ (float)(m[0][0] * e.x + m[0][1] * e.y + m[0][2] * e.z), (float)(m[1][0] * e.x + m[1][1] * e.y + m[1][2] * e.z),
                       (float)(m[2][0] * e.x + m[2][1] * e.y + m[2][2] * e.z) };
    };
    s.sun = normalize(toWorld(sunEq));
    // Topocentric moon: from the observer (on a sphere of the Earth's radius), not the Earth's centre.
    const Vec3d observer{ cp * cl, cp * sl, sp };
    const Vec3d moonFromObserver = sub(scale(moonEq, rMoonEarthRadii), observer);
    s.moon = normalize(toWorld(unit(moonFromObserver)));
    s.moonDistanceKm = std::sqrt(dotd(moonFromObserver, moonFromObserver)) * kEarthRadiusKm;
    s.sunAltitudeDeg = std::asin(std::clamp((double)s.sun.y, -1.0, 1.0)) / kDeg;
    s.moonAltitudeDeg = std::asin(std::clamp((double)s.moon.y, -1.0, 1.0)) / kDeg;
    // Phase angle at the moon: between the directions to the sun and to the observer.
    const Vec3d moonPos = scale(moonEq, rMoonEarthRadii * kEarthRadiusKm);  // geocentric, km
    const Vec3d sunPos = scale(sunEq, rSunAu * kAuKm);
    const Vec3d toSun = unit(sub(sunPos, moonPos)), toEarth = unit(scale(moonPos, -1));
    s.moonPhaseAngleDeg = std::acos(std::clamp(dotd(toSun, toEarth), -1.0, 1.0)) / kDeg;
    s.moonIlluminatedFraction = 0.5 * (1 + cosD(s.moonPhaseAngleDeg));
    return s;
}

DirectionalLight directionalLight(const CelestialState& s, const scene::Sun& sunAtTop, float moonAlbedo)
{
    DirectionalLight out;
    out.sun = sunAtTop;
    if (s.sunAltitudeDeg >= -6.0)
    {
        out.sun.direction = s.sun;
        return out;
    }
    const double alpha = s.moonPhaseAngleDeg * kDeg;
    const double phase = (std::sin(alpha) + (kPi - alpha) * std::cos(alpha)) / kPi;  // Lambert sphere
    const double ratio = kMoonRadiusKm / s.moonDistanceKm;
    out.moon = true;
    out.sun.direction = s.moon;
    out.sun.illuminance = (float)(moonAlbedo * sunAtTop.illuminance * ratio * ratio * (2.0 / 3.0) * phase);
    out.sun.angularRadius = (float)std::asin(ratio);
    out.sun.color = sunAtTop.color * float3{ 1.0f, 0.96f, 0.90f };  // the lunar surface reflects red more (authored tint)
    return out;
}
} // namespace unx::render::sky
