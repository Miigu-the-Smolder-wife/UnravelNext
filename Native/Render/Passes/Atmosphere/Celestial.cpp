#include "Celestial.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

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

namespace unx::render::sky
{
namespace
{
uint64_t splitmix(uint64_t& state)
{
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
double uniform01(uint64_t& state) { return (splitmix(state) >> 11) * (1.0 / 9007199254740992.0); }

// Linear sRGB of a black body at t kelvin, luminance 1 (Kim et al. 2002 Planckian locus, 1667-25000 K).
float3 blackBody(double t)
{
    t = std::clamp(t, 1667.0, 25000.0);
    const double t1 = 1e3 / t, t2 = t1 * t1, t3 = t2 * t1;
    const double x = t <= 4000 ? -0.2661239 * t3 - 0.2343589 * t2 + 0.8776956 * t1 + 0.179910 : -3.0258469 * t3 + 2.1070379 * t2 + 0.2226347 * t1 + 0.240390;
    const double y = t <= 2222   ? -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683
                     : t <= 4000 ? -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867
                                 : 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    const double X = x / y, Y = 1, Z = (1 - x - y) / y;
    const double r = std::max(0.0, 3.2406 * X - 1.5372 * Y - 0.4986 * Z), g = std::max(0.0, -0.9689 * X + 1.8758 * Y + 0.0415 * Z),
                 b = std::max(0.0, 0.0557 * X - 0.2040 * Y + 1.0570 * Z);
    const double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    return { (float)(r / lum), (float)(g / lum), (float)(b / lum) };
}
Star makeStar(double raDeg, double decDeg, double vmag, double bv)
{
    const double cd = cosD(decDeg);
    Star s;
    s.equatorial = { (float)(cd * cosD(raDeg)), (float)(cd * sinD(raDeg)), (float)sinD(decDeg) };
    const double t = 4600.0 * (1.0 / (0.92 * bv + 1.7) + 1.0 / (0.92 * bv + 0.62));  // Ballesteros 2012
    s.illuminance = blackBody(t) * (float)(2.54e-6 * std::pow(10.0, -0.4 * vmag));
    return s;
}
// Cube cell of an equatorial direction (Celestial.hlsli celestialCell: the same mapping).
uint32_t cellOf(float3 e, uint32_t n)
{
    const float ax = std::fabs(e.x), ay = std::fabs(e.y), az = std::fabs(e.z);
    uint32_t face;
    float u, v;
    if (ax >= ay && ax >= az) face = e.x > 0 ? 0 : 1, u = e.y / ax, v = e.z / ax;
    else if (ay >= az) face = e.y > 0 ? 2 : 3, u = e.x / ay, v = e.z / ay;
    else face = e.z > 0 ? 4 : 5, u = e.x / az, v = e.y / az;
    const uint32_t i = std::min(n - 1, (uint32_t)std::max(0.0f, (u + 1) * 0.5f * n)), j = std::min(n - 1, (uint32_t)std::max(0.0f, (v + 1) * 0.5f * n));
    return (face * n + j) * n + i;
}
} // namespace

CelestialFrame celestialFrame(const CelestialState& s, const DirectionalLight& light, const scene::Sun& sunAtTop, float moonAlbedo, float airglowRadiance)
{
    CelestialFrame f;
    f.moonDirection = s.moon;
    f.moonAngularRadius = (float)std::asin(kMoonRadiusKm / s.moonDistanceKm);
    f.sunDirection = s.sun;
    f.sunIlluminance = sunAtTop.illuminance;
    f.sunColor = sunAtTop.color;
    f.moonAlbedo = moonAlbedo;
    f.equatorialToWorld = s.equatorialToWorld;
    f.airglowRadiance = airglowRadiance;
    f.flags = (light.moon ? 1u : 0u) | 2u | 4u;
    return f;
}

std::vector<Star> syntheticStars(uint64_t seed)
{
    std::vector<Star> out;
    uint64_t state = seed * 0x2545F4914F6CDD1Dull + 1;
    const double total = std::pow(10.0, 0.5 * 6.5 + 0.7);
    const float3 galacticPole{ (float)(cosD(27.12825) * cosD(192.85948)), (float)(cosD(27.12825) * sinD(192.85948)), (float)sinD(27.12825) };
    while (out.size() < (size_t)total)
    {
        const double z = 2 * uniform01(state) - 1, phi = 2 * kPi * uniform01(state), r = std::sqrt(1 - z * z);
        const float3 e{ (float)(r * std::cos(phi)), (float)(r * std::sin(phi)), (float)z };
        const double b = std::asin(std::clamp((double)dot(e, galacticPole), -1.0, 1.0)) / kDeg;
        if (uniform01(state) * 3 > 1 + 2 * std::exp(-std::fabs(b) / 15.0)) continue;  // galactic concentration
        const double m = (std::log10(std::max(uniform01(state), 1e-12) * total) - 0.7) / 0.5;  // N(< m) = 10^(0.5 m + 0.7)
        const double bv = 1.6 * uniform01(state);
        Star s = makeStar(std::atan2((double)e.y, (double)e.x) / kDeg, std::asin((double)e.z) / kDeg, m, bv);
        s.equatorial = e;
        out.push_back(s);
    }
    return out;
}

std::vector<Star> loadStarCatalogue(const std::string& path)
{
    std::vector<Star> out;
    std::ifstream f(path, std::ios::binary);
    float rec[4];
    while (f.read(reinterpret_cast<char*>(rec), sizeof rec)) out.push_back(makeStar(rec[0], rec[1], rec[2], rec[3]));
    return out;
}

std::vector<uint32_t> packStars(const std::vector<Star>& stars)
{
    const uint32_t n = kStarCellsPerFace, cells = 6 * n * n;
    std::vector<std::vector<uint32_t>> lists(cells);
    for (uint32_t k = 0; k < (uint32_t)stars.size(); ++k)
    {
        const float3 e = stars[k].equatorial;
        const float3 up = std::fabs(e.z) < 0.9f ? float3{ 0, 0, 1 } : float3{ 1, 0, 0 };
        const float3 t = normalize(cross(up, e)), b = cross(e, t);
        uint32_t seen[9];
        uint32_t count = 0;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const uint32_t c = cellOf(normalize(e + t * (dx * kStarMargin) + b * (dy * kStarMargin)), n);
                bool dup = false;
                for (uint32_t i = 0; i < count; ++i) dup = dup || seen[i] == c;
                if (!dup)
                {
                    seen[count++] = c;
                    lists[c].push_back(k);
                }
            }
    }
    std::vector<uint32_t> words(2 * cells);
    std::vector<uint32_t> records;
    for (uint32_t c = 0; c < cells; ++c)
    {
        words[2 * c] = (uint32_t)(records.size() / 8);
        words[2 * c + 1] = (uint32_t)lists[c].size();
        for (uint32_t k : lists[c])
        {
            const Star& s = stars[k];
            const float r[8] = { s.equatorial.x, s.equatorial.y, s.equatorial.z, 0, s.illuminance.x, s.illuminance.y, s.illuminance.z, 0 };
            for (float x : r)
            {
                uint32_t u;
                std::memcpy(&u, &x, 4);
                records.push_back(u);
            }
        }
    }
    words.insert(words.end(), records.begin(), records.end());
    return words;
}

void packCelestialFrame(const CelestialFrame& f, uint32_t starCount, uint32_t starBufferSrv, uint32_t words[32])
{
    float v[32] = {};
    auto put3 = [&](int at, float3 x) { v[at] = x.x, v[at + 1] = x.y, v[at + 2] = x.z; };
    put3(0, f.moonDirection);
    v[3] = f.moonAngularRadius;
    put3(4, f.sunDirection);
    v[7] = f.sunIlluminance;
    put3(8, f.sunColor);
    v[11] = f.moonAlbedo;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) v[12 + 4 * r + c] = c < 3 ? f.equatorialToWorld.m[r][c] : 0.0f;
    v[24] = f.airglowRadiance;
    std::memcpy(words, v, sizeof v);
    words[25] = f.flags;
    words[26] = starCount;
    words[27] = kStarCellsPerFace;
    words[28] = starBufferSrv;
    words[29] = words[30] = words[31] = 0;
}
} // namespace unx::render::sky
