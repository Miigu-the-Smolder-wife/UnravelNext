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
    // The cirrus sheet: fibres - noise four times finer across (v) than along (u), its coordinates bent by a slow noise
    // (the fibres curve and fan out), in patches of a few tens of kilometres. Tileable (every term has the map's period).
    n.cirrus.resize((size_t)kCirrusSize * kCirrusSize);
    for (uint32_t y = 0; y < kCirrusSize; ++y)
        for (uint32_t x = 0; x < kCirrusSize; ++x)
        {
            const double u = (x + 0.5) / kCirrusSize, v = (y + 0.5) / kCirrusSize;
            const double bu = u + 0.05 * (perlin(u, v, 0.1, 4, seed + 13) - 0.5), bv = v + 0.012 * (perlin(u, v, 0.6, 4, seed + 14) - 0.5);
            const double fibres = 0.5 * perlin(bu, 4 * bv, 0.3, 8, seed + 15) + 0.3 * perlin(bu, 4 * bv, 0.3, 16, seed + 16) + 0.2 * perlin(bu, 4 * bv, 0.3, 32, seed + 17);
            const double patches = perlin(u, v, 0.8, 3, seed + 18);
            n.cirrus[(size_t)y * kCirrusSize + x] = byte(remap(fibres, 0.3, 0.7, 0, 1) * saturate(remap(patches, 0.3, 0.6, 0, 1)));
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
    o.cirrus[0] = (float)frac((originOffset[0] - layer.cirrusWindX * time) / layer.cirrusPeriod);
    o.cirrus[1] = (float)frac((originOffset[2] - layer.cirrusWindZ * time) / layer.cirrusPeriod);
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
namespace
{
struct Rng
{
    uint64_t s;
    double next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return ((s >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }
};
// Distance along (p, d) to leave the layer's shell [base, top] from inside it (or to enter and leave it from outside): the
// span [t0, t1] of the first stretch inside; false when the ray never is inside.
bool shellSpan(const CloudLayer& layer, const CloudOffsets& o, double R, const double p[3], const double d[3], double& t0, double& t1)
{
    const double w[3] = { p[0] + o.origin[0], p[1] + o.origin[1] + R, p[2] + o.origin[2] };
    const double b = w[0] * d[0] + w[1] * d[1] + w[2] * d[2], r2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
    const double rb = R + layer.baseAltitude, rt = R + layer.topAltitude;
    const double discT = b * b - (r2 - rt * rt);
    if (discT <= 0) return false;
    double enter = std::max(-b - std::sqrt(discT), 0.0), leave = -b + std::sqrt(discT);
    const double discB = b * b - (r2 - rb * rb);
    if (discB > 0)
    {
        const double sB = std::sqrt(discB), inA = -b - sB, inB = -b + sB;
        if (r2 < rb * rb) enter = std::max(enter, inB);
        else if (inA > 0) leave = std::min(leave, inA);
    }
    t0 = enter, t1 = leave;
    return t1 > t0;
}
double sunTau(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double R, const double x[3], const double s[3], double step)
{
    double tau = 0;
    for (double t = 0.5 * step;; t += step)
    {
        const double y[3] = { x[0] + s[0] * t, x[1] + s[1] * t, x[2] + s[2] * t };
        const double a = altitudeOf(o, R, y);
        if (a > layer.topAltitude || a < layer.baseAltitude - 1 || t > 50000) break;
        tau += density(n, layer, o, R, y) * step;
        if (tau > 30) break;
    }
    return tau;
}
// Samples a direction from HG(g) about the axis d.
void sampleHg(double g, const double d[3], Rng& rng, double out[3])
{
    const double u = rng.next(), v = rng.next();
    double cosT;
    if (std::abs(g) < 1e-3) cosT = 1 - 2 * u;
    else
    {
        const double q = (1 - g * g) / (1 - g + 2 * g * u);
        cosT = (1 + g * g - q * q) / (2 * g);
    }
    cosT = std::clamp(cosT, -1.0, 1.0);
    const double sinT = std::sqrt(std::max(0.0, 1 - cosT * cosT)), phi = 2 * 3.141592653589793 * v;
    const double sgn = d[2] >= 0 ? 1.0 : -1.0, a = -1.0 / (sgn + d[2]), b = d[0] * d[1] * a;
    const double t[3] = { 1 + sgn * d[0] * d[0] * a, sgn * b, -sgn * d[0] }, bt[3] = { b, sgn + d[1] * d[1] * a, -d[1] };
    for (int k = 0; k < 3; ++k) out[k] = sinT * std::cos(phi) * t[k] + sinT * std::sin(phi) * bt[k] + cosT * d[k];
}
} // namespace

PathResult referencePathTraced(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double R, const double origin[3], const double dir[3],
                               const double sunDir[3], double E, uint32_t paths, uint32_t seed, double step, double skyRadiance)
{
    Rng rng{ 0x9E3779B97F4A7C15ull ^ ((uint64_t)seed * 0x100000001B3ull) };
    const double sigmaMax = layer.sigmaMax;
    double sum = 0, sum2 = 0, first = 0, collisions = 0;
    for (uint32_t p = 0; p < paths; ++p)
    {
        double x[3] = { origin[0], origin[1], origin[2] }, d[3] = { dir[0], dir[1], dir[2] };
        double throughput = 1, L = 0;
        bool firstCollision = true;
        for (uint32_t bounce = 0; bounce < 4096; ++bounce)
        {
            double t0, t1;
            if (!shellSpan(layer, o, R, x, d, t0, t1)) break;
            double t = t0;
            bool hit = false;
            while (true)
            {
                t += -std::log(1 - rng.next()) / sigmaMax;
                if (t >= t1) break;
                const double y[3] = { x[0] + d[0] * t, x[1] + d[1] * t, x[2] + d[2] * t };
                if (rng.next() * sigmaMax < density(n, layer, o, R, y))
                {
                    hit = true;
                    break;
                }
            }
            if (!hit)
            {
                // Left this stretch of the shell: continue along the ray (it may enter the layer again further on).
                for (int k = 0; k < 3; ++k) x[k] += d[k] * (t1 + 1e-3);
                if (!firstCollision)
                {
                    // Left the layer after scattering: upward (local up) sees the sky, downward the black ground.
                    const double up[3] = { x[0] + o.origin[0], x[1] + o.origin[1] + R, x[2] + o.origin[2] };
                    if (skyRadiance > 0 && up[0] * d[0] + up[1] * d[1] + up[2] * d[2] > 0) L += throughput * skyRadiance;
                    break;
                }
                if (t1 > 60000) break;
                continue;
            }
            for (int k = 0; k < 3; ++k) x[k] += d[k] * t;
            collisions += 1;
            throughput *= layer.albedo;
            const double cosSun = d[0] * sunDir[0] + d[1] * sunDir[1] + d[2] * sunDir[2];
            const double nee = E > 0 ? throughput * phase(layer, cosSun) * E * std::exp(-sunTau(n, layer, o, R, x, sunDir, step)) : 0.0;
            L += nee;
            if (firstCollision) first += nee, firstCollision = false;
            if (bounce >= 16)
            {
                const double q = std::min(0.95, throughput);
                if (rng.next() >= q) break;
                throughput /= q;
            }
            double nd[3];
            const bool back = rng.next() < layer.lobeBlend;
            sampleHg(back ? layer.g1 : layer.g0, d, rng, nd);
            d[0] = nd[0], d[1] = nd[1], d[2] = nd[2];
        }
        sum += L;
        sum2 += L * L;
    }
    PathResult r;
    r.radiance = sum / paths;
    r.firstOrder = first / paths;
    r.stdError = std::sqrt(std::max(0.0, sum2 / paths - r.radiance * r.radiance) / paths);
    r.meanCollisions = collisions / paths;
    return r;
}

RayResult referenceApproximate(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double R, const double origin[3], const double dir[3],
                               const double sunDir[3], double E, double skyRadiance, double maxDistance, double step, double a, double b, double c, int octaves,
                               double s0, double s1, double powder)
{
    const double cosTheta = dir[0] * sunDir[0] + dir[1] * sunDir[1] + dir[2] * sunDir[2];
    auto hg = [&](double g) { return (1 - g * g) / (4 * 3.141592653589793 * std::pow(1 + g * g - 2 * g * cosTheta, 1.5)); };
    double pk[16], ak[16], bk[16];
    for (int k = 0, K = std::min(octaves, 16); k < K; ++k)
    {
        const double ck = std::pow(c, k);
        pk[k] = (1 - layer.lobeBlend) * hg(layer.g0 * ck) + layer.lobeBlend * hg(layer.g1 * ck);
        ak[k] = std::pow(a, k), bk[k] = std::pow(b, k);
    }
    double T = 1, L = 0;
    for (double s = 0; s < maxDistance && T > 1e-6; s += step)
    {
        const double ds = std::min(step, maxDistance - s), mid = s + 0.5 * ds;
        const double x[3] = { origin[0] + dir[0] * mid, origin[1] + dir[1] * mid, origin[2] + dir[2] * mid };
        const double rho = density(n, layer, o, R, x);
        if (rho <= 0) continue;
        const double tauSun = E > 0 ? sunTau(n, layer, o, R, x, sunDir, step) : 0.0;
        double sun = 0;
        const double deep = 1 - powder * std::exp(-2 * tauSun);  // (the octaves past the first: CloudModel.h)
        for (int k = 0, K = std::min(octaves, 16); k < K; ++k) sun += ak[k] * pk[k] * std::exp(-bk[k] * tauSun) * (k > 0 ? deep : 1.0);
        const double hn = (altitudeOf(o, R, x) - layer.baseAltitude) / (layer.topAltitude - layer.baseAltitude);
        const double segment = (1 - std::exp(-rho * ds)) / rho;
        L += T * layer.albedo * rho * (sun * E + skyRadiance * std::max(0.0, s0 + s1 * hn)) * segment;
        T *= std::exp(-rho * ds);
    }
    return { L, T };
}
} // namespace unx::render::clouds
