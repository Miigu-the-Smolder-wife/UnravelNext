// A strand of a groom (unx/scene/HairModel.h; the model: Passes/Hair/HairScattering.hlsli), CPU:
//   1. the width-averaged kernel (8 nodes) against the fibre's mean over 1,024 offsets in double precision: its albedo,
//      the white furnace, and the mean absolute difference over the sphere of incoming directions;
//   2. the spread kernel keeps the albedo; with no spread it is the plain kernel's mean over the half circle of azimuths;
//   3. the forward and backward averages a_f, a_b against the fibre's integrals over the two half spaces;
//   4. what n fibres let through against its definition (a Poisson count N of mean n: P(0) unscattered, d_f sum of
//      P(N) a_f^N scattered, the spread's event count);
//   5. the neighbourhood's backward albedo A_b against its series (one and three backward scatterings);
//   6. energy: a lone white fibre returns all it receives, a white volume returns towards the light's side at most what
//      it receives (the kernel with fibres behind, integrated over that half space) and lets through at most 1;
//   7. the moments (albedo, first moment along and across the fibre) against the kernel's integrals; smooth light
//      c + g . w through them against the integral of the kernel times that light.
#include "unx/core/Log.h"
#include "unx/scene/HairModel.h"
#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <vector>

using namespace unx;
using namespace unx::scene;
using namespace unx::scene::model;

namespace
{
uint32_t g_failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)

constexpr double kPiD = 3.14159265358979323846;

float3 direction(double sinTheta, double phi)
{
    const double c = std::sqrt(std::max(0.0, 1 - sinTheta * sinTheta));
    return { (float)sinTheta, (float)(c * std::cos(phi)), (float)(c * std::sin(phi)) };
}

HairFibre fibre(float eumelanin, float betaM, float betaN)
{
    Material m;
    m.cls = MaterialClass::Hair;
    m.ior = 1.55f;
    m.roughness = betaM;
    m.hairEumelanin = eumelanin;
    m.hairBetaN = betaN;
    HairFibre f = hairFibreOf(m);
    if (eumelanin == 0) f.absorption = {};  // white
    return f;
}

// Midpoint grid over the sphere of incoming directions in (sin theta, phi): weight per point 4 pi / (rows x columns).
constexpr uint32_t kRows = 64, kColumns = 180;
template <typename F> void overSphere(F&& f)
{
    for (uint32_t i = 0; i < kRows; ++i)
        for (uint32_t j = 0; j < kColumns; ++j) f(-1 + 2 * (i + 0.5) / kRows, 2 * kPiD * (j + 0.5) / kColumns, 4 * kPiD / (kRows * kColumns));
}
double luminance(const std::array<double, 3>& v) { return (v[0] + v[1] + v[2]) / 3; }
std::array<double, 3> d3(float3 v) { return { v.x, v.y, v.z }; }
} // namespace

int main()
{
    try
    {
        struct Config
        {
            const char* name;
            float eumelanin, betaM, betaN, sinO, phiO;
        };
        const Config configs[] = {
            { "white, head on", 0.0f, 0.3f, 0.3f, 0.0f, 0.3f },    { "white, inclined", 0.0f, 0.3f, 0.3f, 0.6f, 1.1f },
            { "white, smooth", 0.0f, 0.15f, 0.15f, 0.3f, -0.7f },  { "blond", 0.3f, 0.3f, 0.3f, 0.2f, 2.0f },
            { "blond, inclined", 0.3f, 0.25f, 0.4f, -0.7f, 0.0f }, { "dark", 1.3f, 0.3f, 0.3f, 0.1f, -2.5f },
            { "black, rough", 8.0f, 0.5f, 0.6f, 0.4f, 0.9f },
        };
        constexpr uint32_t kOffsets = 1024;
        double worstAlbedo = 0, worstFurnace = 0, worstL1 = 0, worstSpreadAlbedo = 0, worstForward = 0, worstAverage = 0, worstSum = 0;
        double worstMomentAlbedo = 0, worstAlong = 0, worstAcross = 0, worstSmooth = 0, worstReturn = 0, worstLone = 0;
        for (const Config& c : configs)
        {
            const HairFibre f = fibre(c.eumelanin, c.betaM, c.betaN);
            const float3 wo = direction(c.sinO, c.phiO);
            const HairStrand s = hairStrand(f, wo);
            const HairAverage aO = hairAverage(f, wo.x);
            // ---- 1 and 3: one pass over the sphere and the offsets
            std::array<double, 3> albedoRef{}, albedoNodes{}, forwardRef{}, backwardRef{}, albedoSpread{};
            double l1 = 0, forwardGap = 0, forwardNorm = 0;
            // ---- 7: the kernel's moments, lone and embedded (the back lobe's averages at the outgoing inclination)
            std::array<double, 3> m0{}, m1{}, m2{}, e0{}, e1{}, e2{}, smooth{};
            const float3 across = normalize(float3{ 0, wo.y, wo.z });
            const float3 gradient{ 0.35f, -0.2f, 0.25f };  // smooth light 1 + g . w (positive over the sphere)
            overSphere([&](double sinI, double phi, double weight) {
                const float3 wi = direction(sinI, phi);
                std::array<double, 3> ref{};
                for (uint32_t k = 0; k < kOffsets; ++k)
                {
                    const std::array<double, 3> v = hairFibreKernelReference(f, wo, wi, -1 + 2 * (k + 0.5) / kOffsets);
                    for (int ch = 0; ch < 3; ++ch) ref[ch] += v[ch] / kOffsets;
                }
                const std::array<double, 3> nodes = d3(hairStrandKernel(s, wi, 0, false));
                const std::array<double, 3> spread = d3(hairStrandKernel(s, wi, 0.05f, true));
                const std::array<double, 3> halfCircle = d3(hairStrandKernel(s, wi, 0, true));
                // the plain kernel's mean over the half circle of azimuths around wi (16 points)
                std::array<double, 3> meanHalf{};
                for (uint32_t q = 0; q < 16; ++q)
                {
                    const std::array<double, 3> v = d3(hairStrandKernel(s, direction(sinI, phi + kPiD * ((q + 0.5) / 16 - 0.5)), 0, false));
                    for (int ch = 0; ch < 3; ++ch) meanHalf[ch] += v[ch] / 16;
                }
                double dPhi = phi - c.phiO;
                dPhi -= 2 * kPiD * std::floor((dPhi + kPiD) / (2 * kPiD));
                const bool farSide = std::abs(dPhi) > 0.5 * kPiD;
                const std::array<double, 3> lone = d3(hairStrandLight(s, aO, wi, 0, 0)), embedded = d3(hairStrandLight(s, aO, wi, 0, 1e9f));
                worstLone = std::max(worstLone, std::abs(luminance(lone) - luminance(nodes)));
                const double light = 1 + dot(gradient, wi);
                for (int ch = 0; ch < 3; ++ch)
                {
                    albedoRef[ch] += ref[ch] * weight;
                    albedoNodes[ch] += nodes[ch] * weight;
                    albedoSpread[ch] += spread[ch] * weight;
                    (farSide ? forwardRef : backwardRef)[ch] += ref[ch] * weight;
                    m0[ch] += lone[ch] * weight;
                    m1[ch] += lone[ch] * weight * wi.x;
                    m2[ch] += lone[ch] * weight * dot(wi, across);
                    e0[ch] += embedded[ch] * weight;
                    e1[ch] += embedded[ch] * weight * wi.x;
                    e2[ch] += embedded[ch] * weight * dot(wi, across);
                    smooth[ch] += embedded[ch] * weight * light;
                }
                l1 += std::abs(luminance(ref) - luminance(nodes)) * weight;
                forwardGap += std::abs(luminance(halfCircle) - luminance(meanHalf)) * weight;
                forwardNorm += luminance(meanHalf) * weight;
            });
            const double albedo = luminance(albedoRef);
            for (int ch = 0; ch < 3; ++ch)
            {
                worstAlbedo = std::max(worstAlbedo, std::abs(albedoNodes[ch] - albedoRef[ch]));
                worstSpreadAlbedo = std::max(worstSpreadAlbedo, std::abs(albedoSpread[ch] - albedoNodes[ch]));
                if (c.eumelanin == 0) worstFurnace = std::max(worstFurnace, std::abs(albedoNodes[ch] - 1));
            }
            worstL1 = std::max(worstL1, l1 / albedo);
            worstForward = std::max(worstForward, forwardGap / forwardNorm);
            // ---- 3 (the fibre takes its energies from its 'outgoing' argument: the light's inclination here)
            const float av[2][3] = { { aO.forward.x, aO.forward.y, aO.forward.z }, { aO.backward.x, aO.backward.y, aO.backward.z } };
            for (int ch = 0; ch < 3; ++ch)
            {
                worstAverage = std::max({ worstAverage, std::abs(av[0][ch] - forwardRef[ch]), std::abs(av[1][ch] - backwardRef[ch]) });
                worstSum = std::max(worstSum, std::abs(av[0][ch] + av[1][ch] - albedoNodes[ch]));
            }
            const float variance = s.variance;
            CHECK(aO.varianceForward >= 0.25f * variance * 0.999f && aO.varianceForward <= 4 * variance * 1.001f);
            CHECK(aO.varianceBackward >= 0.25f * variance * 0.999f && aO.varianceBackward <= 4 * variance * 1.001f);
            CHECK(std::abs(aO.shiftForward) <= 4 * f.tilt * 1.001f && std::abs(aO.shiftBackward) <= 4 * f.tilt * 1.001f);
            // ---- 7
            const HairMoments lone = hairStrandMoments(s, aO, 0), embedded = hairStrandMoments(s, aO, 1);
            const float lm[3][3] = { { lone.albedo.x, lone.albedo.y, lone.albedo.z }, { lone.along.x, lone.along.y, lone.along.z }, { lone.across.x, lone.across.y, lone.across.z } };
            const float em[3][3] = { { embedded.albedo.x, embedded.albedo.y, embedded.albedo.z },
                                     { embedded.along.x, embedded.along.y, embedded.along.z },
                                     { embedded.across.x, embedded.across.y, embedded.across.z } };
            for (int ch = 0; ch < 3; ++ch)
            {
                worstMomentAlbedo = std::max({ worstMomentAlbedo, std::abs(lm[0][ch] - m0[ch]), std::abs(em[0][ch] - e0[ch]) });
                worstAlong = std::max({ worstAlong, std::abs(lm[1][ch] - m1[ch]), std::abs(em[1][ch] - e1[ch]) });
                worstAcross = std::max({ worstAcross, std::abs(lm[2][ch] - m2[ch]), std::abs(em[2][ch] - e2[ch]) });
                const double viaMoments = em[0][ch] + gradient.x * em[1][ch] + dot(gradient, across) * em[2][ch];
                worstSmooth = std::max(worstSmooth, std::abs(viaMoments - smooth[ch]) / std::max(smooth[ch], 1e-3));
            }
            std::printf("  %-16s albedo %.4f %.4f %.4f (fibre %.4f %.4f %.4f)  |nodes - fibre| / albedo %.4f  a_f %.4f a_b %.4f (fibre %.4f %.4f)  across %.4f (integral %.4f)\n",
                        c.name, albedoNodes[0], albedoNodes[1], albedoNodes[2], albedoRef[0], albedoRef[1], albedoRef[2], l1 / albedo, av[0][1], av[1][1], forwardRef[1],
                        backwardRef[1], lm[2][1], m2[1]);
        }
        std::printf("width average: worst |albedo - fibre's| %.2e, white furnace %.2e, mean |difference| / albedo %.3f; spread albedo %.2e; half circle %.3f\n", worstAlbedo,
                    worstFurnace, worstL1, worstSpreadAlbedo, worstForward);
        std::printf("averages: worst |a - fibre's| %.2e, |a_f + a_b - albedo| %.2e; moments: albedo %.2e, along %.2e, across %.2e, smooth light %.2e relative\n", worstAverage,
                    worstSum, worstMomentAlbedo, worstAlong, worstAcross, worstSmooth);
        CHECK(worstAlbedo < 5e-3);
        CHECK(worstFurnace < 5e-3);
        CHECK(worstL1 < 0.12);
        CHECK(worstSpreadAlbedo < 5e-3);
        CHECK(worstForward < 0.05);
        CHECK(worstAverage < 0.02);
        CHECK(worstSum < 1e-3);
        CHECK(worstLone < 1e-6);
        CHECK(worstMomentAlbedo < 5e-3);
        CHECK(worstAlong < 5e-3);
        CHECK(worstAcross < 0.03);
        CHECK(worstSmooth < 0.03);

        // ---- 4, 5, 6 over inclinations, colours and counts
        double worstThrough = 0, worstSpread = 0, worstSeries = 0, worstBound = 0, worstPassed = 0;
        for (float eumelanin : { 0.0f, 0.3f, 1.3f })
            for (float betaM : { 0.15f, 0.3f, 0.6f })
                for (float sinTheta : { 0.0f, 0.3f, 0.6f, 0.9f, 0.99f })
                {
                    const HairFibre f = fibre(eumelanin, betaM, 0.3f);
                    const HairAverage a = hairAverage(f, sinTheta);
                    const double af[3] = { std::min(a.forward.x, 0.99f), std::min(a.forward.y, 0.99f), std::min(a.forward.z, 0.99f) };
                    const double ab[3] = { a.backward.x, a.backward.y, a.backward.z };
                    for (float n : { 0.0f, 0.05f, 0.5f, 1.0f, 3.0f, 10.0f })
                    {
                        const HairThrough t = hairThrough(a, n);
                        const float scattered[3] = { t.scattered.x, t.scattered.y, t.scattered.z };
                        const double mean = (af[0] + af[1] + af[2]) / 3;
                        double p = std::exp(-(double)n), events = 0, mass = 0, sum[3] = {};
                        const double p0 = p;
                        for (uint32_t N = 1; N < 200; ++N)
                        {
                            p *= n / N;  // Poisson P(N)
                            for (int ch = 0; ch < 3; ++ch) sum[ch] += p * std::pow(af[ch], (double)N);
                            events += N * p * std::pow(mean, (double)N);
                            mass += p * std::pow(mean, (double)N);
                        }
                        worstThrough = std::max(worstThrough, std::abs(t.direct - p0));
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            worstThrough = std::max(worstThrough, std::abs(scattered[ch] - kHairDensityForward * sum[ch]));
                            worstPassed = std::max(worstPassed, (double)t.direct + scattered[ch]);
                        }
                        if (n > 0) worstSpread = std::max(worstSpread, std::abs(t.spread / a.varianceForward - events / mass) / (events / mass));
                    }
                    const HairBack b = hairBackscatter(a);
                    const float albedo[3] = { b.albedo.x, b.albedo.y, b.albedo.z };
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        // one backward scattering after i forward ones and i more on the way out; three backward ones
                        // (the paper's sums over the forward scatterings between them)
                        double one = 0, three = 0;
                        for (uint32_t i = 1; i < 4000; ++i)
                        {
                            const double x = std::pow(af[ch], 2.0 * i);
                            one += ab[ch] * x;
                            three += ab[ch] * ab[ch] * ab[ch] * i * x / (1 - af[ch] * af[ch]);
                        }
                        worstSeries = std::max(worstSeries, std::abs(albedo[ch] - (one + three)) / std::max(one + three, 1e-6));
                        if (eumelanin == 0) worstBound = std::max(worstBound, ab[ch] + kHairDensityBackward * (double)albedo[ch]);
                    }
                    CHECK(b.variance > 0 && std::abs(b.shift) <= 0.7854f);
                }
        std::printf("through n fibres: worst |value - Poisson sum| %.2e, spread events %.2e relative, most let through %.4f; A_b against its series %.2e relative; white volume "
                    "a_b + d_b A_b at most %.4f\n",
                    worstThrough, worstSpread, worstPassed, worstSeries, worstBound);
        CHECK(worstThrough < 1e-5);
        CHECK(worstSpread < 1e-3);
        CHECK(worstPassed <= 1.0 + 1e-6);
        CHECK(worstSeries < 1e-3);
        CHECK(worstBound <= 1.0);

        // ---- 6: a white strand at the lit face of a deep white volume (no fibre in front, fibres behind): what leaves
        // towards the light's half space, over outgoing directions for a fixed light = over incoming for a fixed viewer
        for (float sinO : { 0.0f, 0.5f, 0.9f })
            for (float betaM : { 0.15f, 0.3f, 0.6f })
            {
                const HairFibre f = fibre(0, betaM, 0.3f);
                const float3 wo = direction(sinO, 0.4);
                const HairStrand s = hairStrand(f, wo);
                double returned = 0, lone = 0;
                overSphere([&](double sinI, double phi, double weight) {
                    const float3 wi = direction(sinI, phi);
                    double dPhi = phi - 0.4;
                    dPhi -= 2 * kPiD * std::floor((dPhi + kPiD) / (2 * kPiD));
                    const HairAverage a = hairAverage(f, wi.x);
                    lone += hairStrandLight(s, a, wi, 0, 0).y * weight;
                    if (std::abs(dPhi) < 0.5 * kPiD) returned += hairStrandLight(s, a, wi, 0, 1e9f).y * weight;
                });
                worstReturn = std::max(worstReturn, returned);
                CHECK(std::abs(lone - 1) < 5e-3);
            }
        std::printf("white volume: at most %.4f of the light returns towards its side\n", worstReturn);
        CHECK(worstReturn <= 1.0);
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    if (g_failures)
    {
        std::fprintf(stderr, "%u checks failed\n", g_failures);
        return 1;
    }
    std::printf("hair model tests passed\n");
    return 0;
}
