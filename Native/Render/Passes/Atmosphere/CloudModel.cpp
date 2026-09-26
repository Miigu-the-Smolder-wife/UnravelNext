// Volumetric clouds: noise generation, density and the CPU reference (CloudModel.h).
#include "CloudModel.h"

#include <algorithm>
#include <cmath>

namespace unx::render::clouds
{
namespace
{
uint32_t hash(uint32_t x, uint32_t y, uint32_t z, uint32_t seed)
{
    uint32_t h = seed * 0x9E3779B9u ^ x * 0x85EBCA6Bu ^ y * 0xC2B2AE35u ^ z * 0x27D4EB2Fu;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return h;
}
double unit(uint32_t h) { return (h >> 8) * (1.0 / 16777216.0); }
uint32_t wrap(int32_t i, uint32_t n) { return (uint32_t)(((i % (int32_t)n) + (int32_t)n) % (int32_t)n); }
double fade(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }

// Tileable gradient (Perlin) noise with 'cells' lattice cells per unit, in [0, 1] (0.5 mean).
double perlin(double x, double y, double z, uint32_t cells, uint32_t seed)
{
    x *= cells, y *= cells, z *= cells;
    const int32_t ix = (int32_t)std::floor(x), iy = (int32_t)std::floor(y), iz = (int32_t)std::floor(z);
    const double fx = x - ix, fy = y - iy, fz = z - iz;
    double v[8];
    for (uint32_t c = 0; c < 8; ++c)
    {
        const int32_t ox = c & 1, oy = (c >> 1) & 1, oz = c >> 2;
        const uint32_t h = hash(wrap(ix + ox, cells), wrap(iy + oy, cells), wrap(iz + oz, cells), seed);
        // Gradient: a unit vector from the hash (uniform on the sphere).
        const double u = unit(h) * 2 - 1, phi = unit(h * 747796405u + 2891336453u) * 6.283185307179586;
        const double r = std::sqrt(std::max(0.0, 1 - u * u));
        v[c] = r * std::cos(phi) * (fx - ox) + r * std::sin(phi) * (fy - oy) + u * (fz - oz);
    }
    const double ux = fade(fx), uy = fade(fy), uz = fade(fz);
    auto lerp = [](double a, double b, double t) { return a + (b - a) * t; };
    const double a = lerp(lerp(v[0], v[1], ux), lerp(v[2], v[3], ux), uy), b = lerp(lerp(v[4], v[5], ux), lerp(v[6], v[7], ux), uy);
    return std::clamp(lerp(a, b, uz) / 1.7320508 + 0.5, 0.0, 1.0);  // gradient noise lies in [-sqrt(3)/2, sqrt(3)/2]
}

// Tileable Worley noise: 1 - distance to the nearest feature point (one per cell), in [0, 1].
double worley(double x, double y, double z, uint32_t cells, uint32_t seed)
{
    x *= cells, y *= cells, z *= cells;
    const int32_t ix = (int32_t)std::floor(x), iy = (int32_t)std::floor(y), iz = (int32_t)std::floor(z);
    double best = 1e9;
    for (int32_t dz = -1; dz <= 1; ++dz)
        for (int32_t dy = -1; dy <= 1; ++dy)
            for (int32_t dx = -1; dx <= 1; ++dx)
            {
                const uint32_t h = hash(wrap(ix + dx, cells), wrap(iy + dy, cells), wrap(iz + dz, cells), seed);
                const double px = ix + dx + unit(h), py = iy + dy + unit(h * 1664525u + 1013904223u), pz = iz + dz + unit(h * 22695477u + 1u);
                best = std::min(best, (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z));
            }
    return std::clamp(1 - std::sqrt(best), 0.0, 1.0);
}

double remap(double v, double lo, double hi, double nlo, double nhi) { return nlo + (v - lo) / (hi - lo) * (nhi - nlo); }
double saturate(double v) { return std::clamp(v, 0.0, 1.0); }
uint8_t byte(double v) { return (uint8_t)std::lround(saturate(v) * 255); }

double frac(double v) { return v - std::floor(v); }
} // namespace

// Altitude of renderer-space x above the planet (centre at world (0, -R, 0)): (|p|^2 - R^2) / (|p| + R) with p the world
// point relative to the centre, expanded so nothing cancels: h^2 + y^2 + 2 y R over (|p| + R).
double altitudeOf(const CloudOffsets& o, double R, const double x[3])
{
    const double wx = x[0] + o.origin[0], wy = x[1] + o.origin[1], wz = x[2] + o.origin[2];
    const double h2 = wx * wx + wz * wz;
    return (h2 + wy * wy + 2 * wy * R) / (std::sqrt(h2 + (wy + R) * (wy + R)) + R);
}

namespace
{

// Linear-wrap sampling of an R8 volume (n^3) at coordinate u (in periods; texel centres at (i + 0.5) / n).
double sample3(const std::vector<uint8_t>& t, uint32_t n, const double u[3])
{
    double f[3];
    int32_t i[3];
    for (int k = 0; k < 3; ++k)
    {
        const double x = u[k] * n - 0.5;
        i[k] = (int32_t)std::floor(x);
        f[k] = x - i[k];
    }
    double v = 0;
    for (uint32_t c = 0; c < 8; ++c)
    {
        const int32_t o[3] = { (int32_t)(c & 1), (int32_t)((c >> 1) & 1), (int32_t)(c >> 2) };
        double w = 1;
        for (int k = 0; k < 3; ++k) w *= o[k] ? f[k] : 1 - f[k];
        v += w * t[(wrap(i[2] + o[2], n) * n + wrap(i[1] + o[1], n)) * n + wrap(i[0] + o[0], n)] / 255.0;
    }
    return v;
}
void sample2(const std::vector<uint8_t>& t, uint32_t n, double u, double v, double out[2])
{
    const double x = u * n - 0.5, y = v * n - 0.5;
    const int32_t ix = (int32_t)std::floor(x), iy = (int32_t)std::floor(y);
    const double fx = x - ix, fy = y - iy;
    out[0] = out[1] = 0;
    for (uint32_t c = 0; c < 4; ++c)
    {
        const int32_t ox = c & 1, oy = c >> 1;
        const double w = (ox ? fx : 1 - fx) * (oy ? fy : 1 - fy);
        const size_t at = ((size_t)wrap(iy + oy, n) * n + wrap(ix + ox, n)) * 2;
        out[0] += w * t[at] / 255.0;
        out[1] += w * t[at + 1] / 255.0;
    }
}
} // namespace

CloudNoise generateNoise(uint32_t seed)
{
    CloudNoise n;
    n.shape.resize((size_t)kShapeSize * kShapeSize * kShapeSize);
    for (uint32_t z = 0; z < kShapeSize; ++z)
        for (uint32_t y = 0; y < kShapeSize; ++y)
            for (uint32_t x = 0; x < kShapeSize; ++x)
            {
                const double u = (x + 0.5) / kShapeSize, v = (y + 0.5) / kShapeSize, w = (z + 0.5) / kShapeSize;
                // Perlin fbm (billowy base) carved by inverted Worley fbm: the Perlin-Worley of Schneider 2015.
                const double p = 0.5 * perlin(u, v, w, 4, seed) + 0.3 * perlin(u, v, w, 8, seed + 1) + 0.2 * perlin(u, v, w, 16, seed + 2);
                const double wf = 0.625 * worley(u, v, w, 4, seed + 3) + 0.25 * worley(u, v, w, 8, seed + 4) + 0.125 * worley(u, v, w, 16, seed + 5);
                n.shape[((size_t)z * kShapeSize + y) * kShapeSize + x] = byte(remap(p, wf - 1, 1, 0, 1));
            }
    n.detail.resize((size_t)kDetailSize * kDetailSize * kDetailSize);
    for (uint32_t z = 0; z < kDetailSize; ++z)
        for (uint32_t y = 0; y < kDetailSize; ++y)
            for (uint32_t x = 0; x < kDetailSize; ++x)
            {
                const double u = (x + 0.5) / kDetailSize, v = (y + 0.5) / kDetailSize, w = (z + 0.5) / kDetailSize;
                const double wf = 0.625 * worley(u, v, w, 2, seed + 6) + 0.25 * worley(u, v, w, 4, seed + 7) + 0.125 * worley(u, v, w, 8, seed + 8);
                n.detail[((size_t)z * kDetailSize + y) * kDetailSize + x] = byte(wf);
            }
    n.weather.resize((size_t)kWeatherSize * kWeatherSize * 2);
    for (uint32_t y = 0; y < kWeatherSize; ++y)
        for (uint32_t x = 0; x < kWeatherSize; ++x)
        {
            const double u = (x + 0.5) / kWeatherSize, v = (y + 0.5) / kWeatherSize;
            const double cov = 0.55 * perlin(u, v, 0.25, 8, seed + 9) + 0.3 * perlin(u, v, 0.25, 16, seed + 10) + 0.15 * perlin(u, v, 0.25, 32, seed + 11);
            const double type = perlin(u, v, 0.75, 4, seed + 12);
            n.weather[((size_t)y * kWeatherSize + x) * 2] = byte(remap(cov, 0.3, 0.7, 0, 1));
            n.weather[((size_t)y * kWeatherSize + x) * 2 + 1] = byte(remap(type, 0.35, 0.65, 0, 1));
        }
    return n;
}

CloudOffsets offsetsFor(const CloudLayer& layer, const double originOffset[3], double time)
{
    // The field at time t is the field at t = 0 moved by wind x t: world point w samples coordinate (w - wind t) / P.
    const double wx = originOffset[0] - layer.windX * time, wy = originOffset[1], wz = originOffset[2] - layer.windZ * time;
    CloudOffsets o;
    o.shape[0] = (float)frac(wx / layer.shapePeriod), o.shape[1] = (float)frac(wy / layer.shapePeriod), o.shape[2] = (float)frac(wz / layer.shapePeriod);
    o.detail[0] = (float)frac(wx / layer.detailPeriod), o.detail[1] = (float)frac(wy / layer.detailPeriod), o.detail[2] = (float)frac(wz / layer.detailPeriod);
    o.weather[0] = (float)frac(wx / layer.weatherPeriod), o.weather[1] = (float)frac(wz / layer.weatherPeriod);
    o.origin[0] = (float)originOffset[0], o.origin[1] = (float)originOffset[1], o.origin[2] = (float)originOffset[2];
    return o;
}

double density(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double bottomRadius, const double x[3])
{
    const double altitude = altitudeOf(o, bottomRadius, x);
    const double hn = (altitude - layer.baseAltitude) / (layer.topAltitude - layer.baseAltitude);
    if (hn <= 0 || hn >= 1 || layer.coverage <= 0) return 0;
    double w[2];
    sample2(n.weather, kWeatherSize, x[0] / layer.weatherPeriod + o.weather[0], x[2] / layer.weatherPeriod + o.weather[1], w);
    // Local coverage: the weather map scaled by the global coverage (0.5 = the map as authored, 1 = twice, 0 = clear); the
    // shape noise is then carved at 1 - c (Schneider 2015): c is the share of the column the cloud fills.
    const double c = saturate(w[0] * 2 * layer.coverage);
    if (c <= 0) return 0;
    const double top = 0.35 + 0.65 * w[1];  // stratus (type 0) low and flat, cumulus (type 1) the whole layer
    const double profile = saturate(hn / 0.08) * saturate((top - hn) / (0.25 * top));
    const double us[3] = { x[0] / layer.shapePeriod + o.shape[0], x[1] / layer.shapePeriod + o.shape[1], x[2] / layer.shapePeriod + o.shape[2] };
    const double base = saturate((sample3(n.shape, kShapeSize, us) * profile - (1 - c)) / c);
    if (base <= 0) return 0;
    const double ud[3] = { x[0] / layer.detailPeriod + o.detail[0], x[1] / layer.detailPeriod + o.detail[1], x[2] / layer.detailPeriod + o.detail[2] };
    const double erosion = sample3(n.detail, kDetailSize, ud) * layer.detailStrength;
    return layer.sigmaMax * saturate((base - erosion) / std::max(1 - erosion, 1e-4));
}

double phase(const CloudLayer& layer, double cosTheta)
{
    auto hg = [&](double g) { return (1 - g * g) / (4 * 3.141592653589793 * std::pow(1 + g * g - 2 * g * cosTheta, 1.5)); };
    return (1 - layer.lobeBlend) * hg(layer.g0) + layer.lobeBlend * hg(layer.g1);
}

RayResult referenceSingleScattering(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double bottomRadius, const double origin[3],
                                    const double dir[3], const double sunDir[3], double sunIlluminance, double background, double maxDistance, double step)
{
    const double cosTheta = dir[0] * sunDir[0] + dir[1] * sunDir[1] + dir[2] * sunDir[2];
    const double p = phase(layer, cosTheta);
    double T = 1, L = 0;
    for (double s = 0; s < maxDistance; s += step)
    {
        const double ds = std::min(step, maxDistance - s), mid = s + 0.5 * ds;
        const double x[3] = { origin[0] + dir[0] * mid, origin[1] + dir[1] * mid, origin[2] + dir[2] * mid };
        const double rho = density(n, layer, o, bottomRadius, x);
        if (rho <= 0) continue;
        // Transmittance toward the sun until the ray leaves the layer.
        double tauSun = 0;
        for (double t = 0.5 * step;; t += step)
        {
            const double y[3] = { x[0] + sunDir[0] * t, x[1] + sunDir[1] * t, x[2] + sunDir[2] * t };
            const double altitude = altitudeOf(o, bottomRadius, y);
            if (altitude > layer.topAltitude || altitude < layer.baseAltitude - 1 || t > 50000) break;
            tauSun += density(n, layer, o, bottomRadius, y) * step;
        }
        const double segment = (1 - std::exp(-rho * ds)) / rho;  // integral of T over the segment / T at its start
        L += T * layer.albedo * rho * p * sunIlluminance * std::exp(-tauSun) * segment;
        T *= std::exp(-rho * ds);
    }
    return { L + T * background, T };
}
} // namespace unx::render::clouds
