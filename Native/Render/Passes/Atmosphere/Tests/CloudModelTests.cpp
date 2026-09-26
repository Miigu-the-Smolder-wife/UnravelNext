// B5 volumetric clouds, CPU part (CloudModel.h; S_STATUS_KO.md 9 step a): the noise textures are seamless and
// deterministic, the density is anchored to the world (an origin rebase and the wind advection move the field exactly),
// coverage controls the cloudy share monotonically (0 = clear), and the reference single-scattering integral converges
// in its step. No GPU.
#include "../CloudModel.h"
#include "unx/core/Log.h"

#include <cmath>
#include <cstdio>
#include <exception>
#include <vector>

using namespace unx;
using namespace unx::render::clouds;

namespace
{
// Mean absolute difference of neighbouring texels across the wrap seam against inside the volume (axis x).
void seam(const std::vector<uint8_t>& t, uint32_t n, double& across, double& inside)
{
    double a = 0, b = 0;
    for (uint32_t z = 0; z < n; ++z)
        for (uint32_t y = 0; y < n; ++y)
        {
            const size_t row = ((size_t)z * n + y) * n;
            a += std::abs((int)t[row] - (int)t[row + n - 1]);
            b += std::abs((int)t[row + n / 2] - (int)t[row + n / 2 - 1]);
        }
    across = a / ((double)n * n), inside = b / ((double)n * n);
}
} // namespace

int main()
{
    try
    {
        bool pass = true;
        auto report = [&](bool ok, const char* what, double value, double limit) {
            logf("  %-66s %.3e (limit %.1e) %s\n", what, value, limit, ok ? "ok" : "FAIL");
            pass = pass && ok;
        };
        const CloudNoise noise = generateNoise(1);
        {
            const CloudNoise again = generateNoise(1);
            const bool same = again.shape == noise.shape && again.detail == noise.detail && again.weather == noise.weather;
            report(same, "noise generation deterministic (bytes equal)", same ? 0 : 1, 0);
            double across, inside;
            seam(noise.shape, kShapeSize, across, inside);
            report(across <= 1.3 * inside + 1, "shape noise seamless (mean step across the wrap / inside)", across / inside, 1.3);
            seam(noise.detail, kDetailSize, across, inside);
            report(across <= 1.3 * inside + 1, "detail noise seamless (mean step across the wrap / inside)", across / inside, 1.3);
        }
        const double R = 6360e3;
        CloudLayer layer;
        const double origin0[3] = { 0, 0, 0 };
        // Coverage: the cloudy share of a 64 x 64 x 16 grid over 16 km x 16 km of the layer rises with coverage, 0 at 0.
        {
            double previous = -1;
            bool monotonic = true, clearAtZero = true;
            for (float c : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                layer.coverage = c;
                const CloudOffsets o = offsetsFor(layer, origin0, 0);
                uint32_t cloudy = 0, total = 0, columns = 0;
                for (uint32_t j = 0; j < 64; ++j)
                    for (uint32_t i = 0; i < 64; ++i)
                    {
                        bool any = false;
                        for (uint32_t k = 0; k < 16; ++k)
                        {
                            const double x[3] = { i * 250.0 - 8000, layer.baseAltitude + (k + 0.5) / 16 * (layer.topAltitude - layer.baseAltitude), j * 250.0 - 8000 };
                            const bool in = density(noise, layer, o, R, x) > 0;
                            cloudy += in ? 1u : 0u;
                            any = any || in;
                            ++total;
                        }
                        columns += any ? 1u : 0u;
                    }
                const double share = (double)cloudy / total;
                logf("  coverage %.2f: cloudy volume share %.3f, cloudy columns %.3f\n", c, share, columns / 4096.0);
                if (c == 0 && cloudy) clearAtZero = false;
                if (share < previous) monotonic = false;
                previous = share;
            }
            layer.coverage = 0.5f;
            report(clearAtZero, "coverage 0 is clear (cloudy samples)", clearAtZero ? 0 : 1, 0);
            report(monotonic, "cloudy share rises with coverage", monotonic ? 0 : 1, 0);
            report(previous > 0.2, "coverage 1: cloudy share", previous, 0.2);
        }
        // World anchoring: a rebase by delta (C9: multiples of 1024 m) shows the same field at x - delta; the wind moves
        // it by wind x dt.
        {
            layer.windX = 12.5, layer.windZ = -7.0;
            double worstRebase = 0, worstWind = 0;
            const double origin1[3] = { 3 * 1024.0, 0, -7 * 1024.0 };
            const double t0 = 1234.5, dt = 3.25;
            const CloudOffsets a = offsetsFor(layer, origin0, t0), b = offsetsFor(layer, origin1, t0), w = offsetsFor(layer, origin0, t0 + dt);
            for (uint32_t i = 0; i < 4096; ++i)
            {
                const double x[3] = { (i % 64) * 97.0 - 3000, layer.baseAltitude + 200 + (i / 64) * 30.0, (i / 64) * 131.0 - 4000 };
                const double y[3] = { x[0] - origin1[0], x[1] - origin1[1], x[2] - origin1[2] };
                const double z[3] = { x[0] + layer.windX * dt, x[1], x[2] + layer.windZ * dt };
                const double d0 = density(noise, layer, a, R, x);
                worstRebase = std::max(worstRebase, std::abs(density(noise, layer, b, R, y) - d0) / layer.sigmaMax);
                worstWind = std::max(worstWind, std::abs(density(noise, layer, w, R, z) - d0) / layer.sigmaMax);
            }
            // Offsets are float (the GPU's): 2^-24 of a period of 32 km is 2 mm, far below a texel.
            report(worstRebase < 2e-3, "origin rebase: density at x - delta after the shift (of sigma_max)", worstRebase, 2e-3);
            report(worstWind < 2e-3, "wind: density at x + wind dt after dt (of sigma_max)", worstWind, 2e-3);
            layer.windX = layer.windZ = 0;
        }
        // Reference integral: step 20 m against 10 m and 5 m, a ray through the layer toward a low sun.
        {
            const CloudOffsets o = offsetsFor(layer, origin0, 0);
            const double camera[3] = { 0, 1.8, 0 }, sun[3] = { -0.5, 0.35, 0.7921 };
            const double sl = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
            const double s[3] = { sun[0] / sl, sun[1] / sl, sun[2] / sl };
            // The first azimuth (elevation 17 deg) whose ray crosses cloud without saturating (0.05 < T < 0.9, 40 m steps).
            double d[3] = { 0, 0, 0 };
            for (uint32_t a = 0; a < 360; ++a)
            {
                const double az = a * 3.141592653589793 / 180;
                const double e[3] = { std::cos(az) * 0.9563, 0.2924, std::sin(az) * 0.9563 };
                const double t = referenceSingleScattering(noise, layer, o, R, camera, e, s, 1.0, 0.0, 20000, 40).transmittance;
                if (t < 0.9 && t > 0.05)
                {
                    d[0] = e[0], d[1] = e[1], d[2] = e[2];
                    break;
                }
            }
            const RayResult r20 = referenceSingleScattering(noise, layer, o, R, camera, d, s, 1.0, 0.0, 20000, 20);
            const RayResult r10 = referenceSingleScattering(noise, layer, o, R, camera, d, s, 1.0, 0.0, 20000, 10);
            const RayResult r5 = referenceSingleScattering(noise, layer, o, R, camera, d, s, 1.0, 0.0, 20000, 5);
            logf("  reference: L %.6e / %.6e / %.6e, T %.6f / %.6f / %.6f (steps 20 / 10 / 5 m)\n", r20.radiance, r10.radiance, r5.radiance, r20.transmittance,
                 r10.transmittance, r5.transmittance);
            const double e = std::abs(r10.radiance - r5.radiance) / std::max(r5.radiance, 1e-30);
            report(r5.transmittance < 0.999, "the test ray crosses cloud (T at 5 m steps)", r5.transmittance, 0.999);
            report(e < 0.01, "reference converged: |L(10 m) - L(5 m)| / L(5 m)", e, 0.01);
        }
        // Multiple scattering reference: the path tracer's first-collision estimate agrees with the deterministic single
        // scattering (4 standard errors), and the multiple-scattering share is reported (the GPU approximation's target).
        {
            const CloudOffsets o = offsetsFor(layer, origin0, 0);
            const double camera[3] = { 0, 1.8, 0 }, sun[3] = { -0.5, 0.35, 0.7921 };
            const double sl = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
            const double s[3] = { sun[0] / sl, sun[1] / sl, sun[2] / sl };
            uint32_t checked = 0, agree = 0;
            for (uint32_t a = 0; a < 360 && checked < 6; a += 7)
            {
                const double az = a * 3.141592653589793 / 180;
                const double e[3] = { std::cos(az) * 0.9563, 0.2924, std::sin(az) * 0.9563 };
                const RayResult ss = referenceSingleScattering(noise, layer, o, R, camera, e, s, 1.0, 0.0, 60000, 20);
                if (ss.transmittance > 0.9 || ss.transmittance < 0.01) continue;
                const PathResult pt = referencePathTraced(noise, layer, o, R, camera, e, s, 1.0, 4000, a + 1, 20);
                // The first-collision term's own error: the total's standard error scaled by the share.
                const double firstErr = pt.stdError * std::sqrt(std::max(pt.firstOrder / std::max(pt.radiance, 1e-30), 0.05));
                const bool ok = std::abs(pt.firstOrder - ss.radiance) < 4 * firstErr + 0.01 * ss.radiance;
                logf("  azimuth %3u deg: T %.3f, SS %.4e, path-traced first %.4e total %.4e (+- %.1e), multiple scattering %.1f %% of total, %.1f collisions -> %s\n",
                     a, ss.transmittance, ss.radiance, pt.firstOrder, pt.radiance, pt.stdError, 100 * (1 - pt.firstOrder / std::max(pt.radiance, 1e-30)),
                     pt.meanCollisions, ok ? "ok" : "FAIL");
                ++checked;
                agree += ok ? 1u : 0u;
            }
            report(checked >= 4 && agree == checked, "path tracer's first collision = single scattering (rays)", agree, checked);
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
