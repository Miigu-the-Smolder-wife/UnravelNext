// B5 clouds, multiple scattering: the APPROXIMATION (not exact; user decision 2026-09-27 06:40) the GPU march uses until
// the exact grid solve lands (S_STATUS_KO.md 9 step 1), against the CPU path tracer. The model (CloudModel.h
// referenceApproximate): the octave series L_sun = sum_{k<N} a^k sum_steps T sigma_s p_k(theta) E exp(-b^k tau_sun) segment
// (p_k the dual-lobe HG at g c^k) and the sky term L_sky = sum_steps T sigma_s L_s max(0, s0 + s1 h_n) segment. Rays:
// azimuths whose view transmittance is in (0.003, 0.97), five elevations, three suns; each path-traced twice (sun only;
// sky only: uniform upper hemisphere, black ground), then (a, b, c, N) and (s0, s1) are fitted (--sweep: a grid for the
// octaves, least squares in relative error for the linear weights; the sweep also tries a sun-diffuse height term
// mu_s (d0 + d1 h_n), which gained 1.5 points of the mean error and is not used) and the errors of the values the GPU uses
// (CloudModel.h kMs*, kSky*, repeated in CloudCommon.hlsli) are gated against the recorded ones (regression only: the
// model is not exact).
// CPU only.
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
    double T, rho, tauSun, segment, hn;  // one march sample of a ray
};
// The ray's in-cloud march samples (20 m steps, as the references).
std::vector<Sample> samples(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double R, const double origin[3], const double d[3],
                            const double s[3])
{
    std::vector<Sample> out;
    double T = 1;
    const double step = 20;
    for (double t = 0; t < 60000 && T > 1e-6; t += step)
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
        const double hn = (altitudeOf(o, R, x) - layer.baseAltitude) / (layer.topAltitude - layer.baseAltitude);
        out.push_back({ T, rho, tau, (1 - std::exp(-rho * step)) / rho, hn });
        T *= std::exp(-rho * step);
    }
    return out;
}
double hg(double g, double c) { return (1 - g * g) / (4 * 3.141592653589793 * std::pow(1 + g * g - 2 * g * c, 1.5)); }
// Recorded errors of the GPU values (S_STATUS_KO.md 9) with a margin for the references' noise.
constexpr double kGateSunMean = 0.50, kGateSunWorst = 1.00, kGateSkyMean = 0.20, kGateSkyWorst = 0.50;
struct Ray
{
    int sun;
    double cosTheta;
    std::vector<Sample> s;
    double sunRef, sunErr, skyRef, skyErr;
    double S0, S1;  // sky basis: sum T albedo rho segment (1, h_n)
    double mu;      // the sun's cosine to the local up at the camera's cloud (the diffuse sun term's scale)
};
// sum_steps T albedo rho segment max(0, w0 + w1 h_n): the height ramps as the GPU evaluates them (clamped per sample).
double ramp(const CloudLayer& layer, const Ray& r, double w0, double w1)
{
    double sum = 0;
    for (const Sample& q : r.s) sum += q.T * layer.albedo * q.rho * q.segment * std::max(0.0, w0 + w1 * q.hn);
    return sum;
}
// sum_k a^k p_k E_k(b) with E_k(b) = sum T albedo rho exp(-b^k tau) segment; powder: the octaves past the first x
// (1 - powder exp(-2 tau)) per sample (CloudModel.h referenceApproximate; 0: the series alone).
double octaves(const CloudLayer& layer, const Ray& r, double a, double b, double c, int N, double powder = 0)
{
    double L = 0, ak = 1, bk = 1, ck = 1;
    for (int k = 0; k < N; ++k)
    {
        const double p = (1 - layer.lobeBlend) * hg(layer.g0 * ck, r.cosTheta) + layer.lobeBlend * hg(layer.g1 * ck, r.cosTheta);
        double e = 0;
        for (const Sample& q : r.s)
            e += q.T * layer.albedo * q.rho * std::exp(-bk * q.tauSun) * q.segment * (k > 0 ? 1 - powder * std::exp(-2 * q.tauSun) : 1.0);
        L += ak * p * e;
        ak *= a, bk *= b, ck *= c;
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
        std::vector<Ray> rays;
        const double suns[3][3] = { { -0.5, 0.35, 0.7921 }, { 0.3, 0.8, -0.52 }, { 0.9, 0.12, 0.42 } };
        for (int sun = 0; sun < 3; ++sun)
        {
            const double l = std::sqrt(suns[sun][0] * suns[sun][0] + suns[sun][1] * suns[sun][1] + suns[sun][2] * suns[sun][2]);
            const double s[3] = { suns[sun][0] / l, suns[sun][1] / l, suns[sun][2] / l };
            for (double el : { 0.40, 0.2924, 0.24, 0.2, 0.14 })
                for (uint32_t az = 0, found = 0; az < 360 && found < 8; az += 3)
                {
                    const double r = az * 3.141592653589793 / 180, ce = std::sqrt(1 - el * el);
                    const double d[3] = { std::cos(r) * ce, el, std::sin(r) * ce };
                    const RayResult t = referenceSingleScattering(noise, layer, o, R, camera, d, s, 1.0, 0.0, 60000, 40);
                    if (t.transmittance > 0.97 || t.transmittance < 0.003) continue;
                    const PathResult pt = referencePathTraced(noise, layer, o, R, camera, d, s, 1.0, paths, az * 7 + sun * 1000 + 1, 20);
                    const PathResult sky = referencePathTraced(noise, layer, o, R, camera, d, s, 0.0, paths, az * 7 + sun * 1000 + 2, 20, 1.0);
                    Ray ray{ sun, d[0] * s[0] + d[1] * s[1] + d[2] * s[2], samples(noise, layer, o, R, camera, d, s), pt.radiance, pt.stdError, sky.radiance,
                             sky.stdError, 0, 0, std::max(0.0, s[1]) };
                    for (const Sample& q : ray.s)
                    {
                        const double w = q.T * layer.albedo * q.rho * q.segment;
                        ray.S0 += w, ray.S1 += w * q.hn;
                    }
                    rays.push_back(std::move(ray));
                    ++found;
                    az += 6;  // spread the rays over the sky
                    logf("  ray %2zu: sun %d, elevation %.2f, azimuth %3u: T %.3f, sun %.4e +- %.1e, sky %.4e +- %.1e\n", rays.size(), sun, el, az,
                         t.transmittance, pt.radiance, pt.stdError, sky.radiance, sky.stdError);
                }
        }
        // Sun model: octaves + mu (d0 + d1 h_n) (the light entering the layer's tops and diffusing, which the octaves'
        // tau toward the sun cannot see when the sun is low).
        auto sunModel = [&](const Ray& r, double a, double b, double c, int N, double d0, double d1) {
            return octaves(layer, r, a, b, c, N) + r.mu * ramp(layer, r, d0, d1);
        };
        auto evaluate = [&](double a, double b, double c, int N, double d0, double d1, double& mean, double& worst, int onlySun = -1) {
            mean = 0, worst = 0;
            int count = 0;
            for (const Ray& r : rays)
            {
                if (onlySun >= 0 && r.sun != onlySun) continue;
                const double e = std::abs(sunModel(r, a, b, c, N, d0, d1) - r.sunRef) / r.sunRef;
                mean += e, worst = std::max(worst, e), ++count;
            }
            mean /= std::max(count, 1);
        };
        // Least squares in relative error, sum ((w0 u + w1 v - y) / ref)^2, for two linear weights.
        auto solve2 = [&](auto&& u, auto&& v, auto&& y, auto&& ref, double& w0, double& w1) {
            double A00 = 0, A01 = 0, A11 = 0, y0 = 0, y1 = 0;
            for (const Ray& r : rays)
            {
                const double k = 1.0 / ref(r), U = u(r) * k, V = v(r) * k, Y = y(r) * k;
                A00 += U * U, A01 += U * V, A11 += V * V, y0 += U * Y, y1 += V * Y;
            }
            const double det = A00 * A11 - A01 * A01;
            w0 = det != 0 ? (y0 * A11 - y1 * A01) / det : 0, w1 = det != 0 ? (A00 * y1 - A01 * y0) / det : 0;
        };
        auto evaluateSky = [&](double s0, double s1, double& mean, double& worst) {
            mean = 0, worst = 0;
            for (const Ray& r : rays)
            {
                const double e = std::abs(ramp(layer, r, s0, s1) - r.skyRef) / r.skyRef;
                mean += e, worst = std::max(worst, e);
            }
            mean /= rays.size();
        };
        auto muS0 = [](const Ray& r) { return r.mu * r.S0; };
        auto muS1 = [](const Ray& r) { return r.mu * r.S1; };
        auto sunRefOf = [](const Ray& r) { return r.sunRef; };
        double m, w;
        evaluate(0, 0, 0, 1, 0, 0, m, w);
        logf("%zu rays; single scattering only: mean %.3f, worst %.3f of the path-traced sun radiance\n", rays.size(), m, w);
        if (sweep)
        {
            for (bool diffuse : { false, true })
            {
                double best = 1e9, ba = 0, bb = 0, bc = 0, bw = 0, bd0 = 0, bd1 = 0;
                int bn = 0;
                for (int N : { 2, 3, 4, 6, 8 })
                    for (double a = 0.3; a <= 0.951; a += 0.05)
                        for (double b = 0.05; b <= 0.751; b += 0.05)
                            for (double c = 0.1; c <= 0.951; c += 0.1)
                            {
                                double d0 = 0, d1 = 0;
                                if (diffuse)
                                    solve2(muS0, muS1, [&](const Ray& r) { return r.sunRef - octaves(layer, r, a, b, c, N); }, sunRefOf, d0, d1);
                                evaluate(a, b, c, N, d0, d1, m, w);
                                if (m < best) best = m, ba = a, bb = b, bc = c, bw = w, bn = N, bd0 = d0, bd1 = d1;
                            }
                logf("octaves%s best: N %d, a %.2f, b %.2f, c %.2f, d0 %.4f, d1 %.4f: mean %.3f, worst %.3f\n", diffuse ? " + diffuse" : "", bn, ba, bb, bc,
                     bd0, bd1, best, bw);
            }
            {
                // The powder term's strength fitted with the octaves (atmosphere.clouds.powder: the GPU's value is 0 until
                // this says otherwise): the best (a, b, c) at each strength, N as the GPU's.
                for (double powder : { 0.0, 0.25, 0.5, 0.75, 1.0 })
                {
                    double best = 1e9, ba = 0, bb = 0, bc = 0, bw = 0;
                    for (double a = 0.3; a <= 0.951; a += 0.05)
                        for (double b = 0.05; b <= 0.751; b += 0.05)
                            for (double c = 0.1; c <= 0.951; c += 0.1)
                            {
                                double mean = 0, worst = 0;
                                for (const Ray& r : rays)
                                {
                                    const double e = std::abs(octaves(layer, r, a, b, c, kMsOctaves, powder) - r.sunRef) / r.sunRef;
                                    mean += e, worst = std::max(worst, e);
                                }
                                mean /= rays.size();
                                if (mean < best) best = mean, ba = a, bb = b, bc = c, bw = worst;
                            }
                    logf("octaves with powder %.2f best: N %d, a %.2f, b %.2f, c %.2f: mean %.3f, worst %.3f\n", powder, kMsOctaves, ba, bb, bc, best, bw);
                }
            }
            double s0 = 0, s1 = 0;
            solve2([](const Ray& r) { return r.S0; }, [](const Ray& r) { return r.S1; }, [](const Ray& r) { return r.skyRef; },
                   [](const Ray& r) { return r.skyRef; }, s0, s1);
            evaluateSky(s0, s1, m, w);
            logf("sky best (least squares): s0 %.3f, s1 %.3f: mean %.3f, worst %.3f\n", s0, s1, m, w);
        }
        // The values the GPU uses (regression gate: the recorded errors of the approximation, plus the references' noise).
        evaluate(kMsA, kMsB, kMsC, kMsOctaves, 0, 0, m, w);
        logf("APPROXIMATION (not exact) sun, GPU values N %d a %.2f b %.2f c %.2f: mean %.3f, worst %.3f\n", kMsOctaves, kMsA, kMsB, kMsC, m, w);
        for (size_t i = 0; i < rays.size(); ++i)
        {
            const Ray& r = rays[i];
            logf("  ray %2zu (sun %d): sun model / reference %.3f (single %.3f), sky model / reference %.3f\n", i + 1, r.sun,
                 octaves(layer, r, kMsA, kMsB, kMsC, kMsOctaves) / r.sunRef, octaves(layer, r, 0, 0, 0, 1) / r.sunRef, ramp(layer, r, kSkyS0, kSkyS1) / r.skyRef);
        }
        for (int sun = 0; sun < 3; ++sun)
        {
            double ms, ws;
            evaluate(kMsA, kMsB, kMsC, kMsOctaves, 0, 0, ms, ws, sun);
            logf("  sun %d: mean %.3f, worst %.3f\n", sun, ms, ws);
        }
        const bool sunOk = m <= kGateSunMean && w <= kGateSunWorst;
        pass = pass && sunOk;
        evaluateSky(kSkyS0, kSkyS1, m, w);
        logf("APPROXIMATION (not exact) sky, GPU values s0 %.3f s1 %.3f: mean %.3f, worst %.3f\n", kSkyS0, kSkyS1, m, w);
        const bool skyOk = m <= kGateSkyMean && w <= kGateSkyWorst;
        pass = pass && skyOk;
        logf("  gates (recorded errors + margin): sun mean <= %.2f worst <= %.2f %s; sky mean <= %.2f worst <= %.2f %s\n", kGateSunMean, kGateSunWorst,
             sunOk ? "ok" : "FAIL", kGateSkyMean, kGateSkyWorst, skyOk ? "ok" : "FAIL");
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
