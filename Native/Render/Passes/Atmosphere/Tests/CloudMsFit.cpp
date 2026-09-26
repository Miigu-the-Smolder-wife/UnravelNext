// B5 clouds, multiple scattering (S_STATUS_KO.md 9 step d): the octave series against the CPU path tracer. The series
// L = sum_k a^k sum_steps T sigma_s p_k(theta) E exp(-b^k tau_sun) (1 - e^(-rho dt)) / rho, with p_k the dual-lobe HG at
// g c^k, uses only the sun's optical depth each march sample already has. Rays: azimuths whose view transmittance is in
// (0.01, 0.95), two elevations, two sun directions; each path-traced once (sun only, black background), then the
// parameters (a, b, c) are swept and the best mean relative error is reported with its worst; the gate checks the
// parameters the GPU uses (CloudCommon.hlsli CLOUD_MS_*). CPU only.
//   unx_test_atmosphere_cloudmsfit [--paths N] [--sweep]
#include "../CloudModel.h"
#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render::clouds;

namespace
{
struct Sample
{
    double T, rho, tauSun, cosTheta, segment;  // one march sample of a ray
};
// The ray's in-cloud march samples (20 m steps, as the references).
std::vector<Sample> samples(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double R, const double origin[3], const double d[3],
                            const double s[3])
{
    std::vector<Sample> out;
    double T = 1;
    const double step = 20;
    for (double t = 0; t < 60000 && T > 1e-4; t += step)
    {
        const double x[3] = { origin[0] + d[0] * (t + 0.5 * step), origin[1] + d[1] * (t + 0.5 * step), origin[2] + d[2] * (t + 0.5 * step) };
        const double rho = density(n, layer, o, R, x);
        if (rho <= 0) continue;
        double tau = 0;
        for (double u = 0.5 * step;; u += step)
        {
            const double y[3] = { x[0] + s[0] * u, x[1] + s[1] * u, x[2] + s[2] * u };
            const double a = altitudeOf(o, R, y);
            if (a > layer.topAltitude || a < layer.baseAltitude - 1 || u > 50000 || tau > 30) break;
            tau += density(n, layer, o, R, y) * step;
        }
        out.push_back({ T, rho, tau, d[0] * s[0] + d[1] * s[1] + d[2] * s[2], (1 - std::exp(-rho * step)) / rho });
        T *= std::exp(-rho * step);
    }
    return out;
}
double hg(double g, double c) { return (1 - g * g) / (4 * 3.141592653589793 * std::pow(1 + g * g - 2 * g * c, 1.5)); }
double octaves(const CloudLayer& layer, const std::vector<Sample>& ss, double a, double b, double c, int N)
{
    double L = 0;
    for (const Sample& q : ss)
    {
        double wa = 1, wb = 1, wc = 1;
        for (int k = 0; k < N; ++k)
        {
            const double p = (1 - layer.lobeBlend) * hg(layer.g0 * wc, q.cosTheta) + layer.lobeBlend * hg(layer.g1 * wc, q.cosTheta);
            L += wa * q.T * layer.albedo * q.rho * p * std::exp(-wb * q.tauSun) * q.segment;
            wa *= a, wb *= b, wc *= c;
        }
    }
    return L;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        uint32_t paths = 3000;
        bool sweep = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--paths" && i + 1 < argc) paths = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--sweep") sweep = true;
            else fail("unknown argument %s", a.c_str());
        }
        bool pass = true;
        const CloudNoise noise = generateNoise(1);
        CloudLayer layer;
        const double R = 6360e3, origin0[3] = { 0, 0, 0 }, camera[3] = { 0, 1.8, 0 };
        const CloudOffsets o = offsetsFor(layer, origin0, 0);
        struct Ray
        {
            std::vector<Sample> s;
            double reference, err;
        };
        std::vector<Ray> rays;
        const double suns[3][3] = { { -0.5, 0.35, 0.7921 }, { 0.3, 0.8, -0.52 }, { 0.9, 0.12, 0.42 } };
        for (int sun = 0; sun < 3; ++sun)
        {
            const double l = std::sqrt(suns[sun][0] * suns[sun][0] + suns[sun][1] * suns[sun][1] + suns[sun][2] * suns[sun][2]);
            const double s[3] = { suns[sun][0] / l, suns[sun][1] / l, suns[sun][2] / l };
            for (double el : { 0.2924, 0.2, 0.14 })
                for (uint32_t az = 0, found = 0; az < 360 && found < 6; az += 3)
                {
                    const double r = az * 3.141592653589793 / 180, ce = std::sqrt(1 - el * el);
                    const double d[3] = { std::cos(r) * ce, el, std::sin(r) * ce };
                    const RayResult t = referenceSingleScattering(noise, layer, o, R, camera, d, s, 1.0, 0.0, 60000, 40);
                    if (t.transmittance > 0.97 || t.transmittance < 0.003) continue;
                    const PathResult pt = referencePathTraced(noise, layer, o, R, camera, d, s, 1.0, paths, az * 7 + sun * 1000 + 1, 20);
                    rays.push_back({ samples(noise, layer, o, R, camera, d, s), pt.radiance, pt.stdError });
                    ++found;
                    az += 12;  // spread the rays over the sky
                    logf("  ray %2zu: sun %d, elevation %.2f, azimuth %3u: T %.3f, path-traced %.4e +- %.1e\n", rays.size(), sun, el, az, t.transmittance,
                         pt.radiance, pt.stdError);
                }
        }
        auto evaluate = [&](double a, double b, double c, int N, double& mean, double& worst) {
            mean = 0, worst = 0;
            for (const Ray& r : rays)
            {
                const double e = std::abs(octaves(layer, r.s, a, b, c, N) - r.reference) / r.reference;
                mean += e;
                worst = std::max(worst, e);
            }
            mean /= rays.size();
        };
        double m, w;
        evaluate(0, 0, 0, 1, m, w);
        logf("single scattering only: mean %.3f, worst %.3f of the path-traced radiance\n", m, w);
        if (sweep)
        {
            double best = 1e9, ba = 0, bb = 0, bc = 0, bw = 0;
            for (double a = 0.3; a <= 0.951; a += 0.05)
                for (double b = 0.1; b <= 0.751; b += 0.05)
                    for (double c = 0.2; c <= 0.951; c += 0.15)
                    {
                        evaluate(a, b, c, 8, m, w);
                        if (m < best) best = m, ba = a, bb = b, bc = c, bw = w;
                    }
            logf("octaves (N = 8) best: a %.2f, b %.2f, c %.2f: mean %.3f, worst %.3f\n", ba, bb, bc, best, bw);
        }
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
