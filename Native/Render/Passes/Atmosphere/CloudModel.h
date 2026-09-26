#pragma once
// Volumetric clouds (B5, FEATURES_GAME 13, S_STATUS_KO.md 9): the density field shared by the GPU kernels (CloudCommon.hlsli,
// the same formulas) and the CPU reference used by the tests. Owner: S.
//
// Density (extinction, 1/m) at a world point x:
//   rho(x) = sigma_max * erode(shape(x) * profile(h_n, type) remapped by coverage, detail(x))
// with h_n the height in the layer [base, top] (altitude above the planet's surface), the weather map (2D: coverage,
// type; period P_w) giving the local coverage and cloud type, the shape noise (3D Perlin-Worley, period P_s) and the
// detail noise (3D Worley, period P_d) eroding the edges. Every texture repeats; its coordinate is x / P plus an offset
// the CPU computes in double precision from the world origin offset (C9 rebase) and the wind advection (World time), so
// the field is anchored to the world and moves with the wind without float drift.
#include <cstdint>
#include <vector>

namespace unx::render::clouds
{
constexpr uint32_t kShapeSize = 128;    // shape noise texels per axis (R8)
constexpr uint32_t kDetailSize = 32;    // detail noise texels per axis (R8)
constexpr uint32_t kWeatherSize = 512;  // weather map texels per axis (RG8: coverage, type)

struct CloudLayer  // authored (environment); all lengths in metres
{
    float baseAltitude = 1500, topAltitude = 4000;  // the layer above the planet's surface
    float coverage = 0.5f;                          // share of the weather map that becomes cloud (0 = clear)
    float sigmaMax = 0.04f;                         // extinction at full density (1/m): ~25 m mean free path, cumulus
    float albedo = 0.99f;                           // single-scattering albedo (water droplets: ~1)
    float detailStrength = 0.35f;                   // erosion of the edges by the detail noise
    float g0 = 0.8f, g1 = -0.2f, lobeBlend = 0.2f;  // phase: (1 - b) HG(g0) + b HG(g1)
    float shapePeriod = 4096, detailPeriod = 512, weatherPeriod = 32768;
    double windX = 0, windZ = 0;                    // wind (m/s) in the layer: the field advects with it
};

// The noise textures (generated once, deterministic; tileable over their size).
struct CloudNoise
{
    std::vector<uint8_t> shape;    // kShapeSize^3, x fastest
    std::vector<uint8_t> detail;   // kDetailSize^3
    std::vector<uint8_t> weather;  // kWeatherSize^2 x 2 (coverage, type)
};
CloudNoise generateNoise(uint32_t seed = 1);

// Offsets of the three texture coordinates (in periods, [0, 1)) for the world origin offset (renderer space + origin =
// world) and the advection time t (s): coordinate = x / P + offset.
struct CloudOffsets
{
    float shape[3], detail[3], weather[2];
    float origin[3];  // the world origin offset (world = renderer space + origin): altitude is taken in world space
};
CloudOffsets offsetsFor(const CloudLayer& layer, const double originOffset[3], double time);

// CPU density (1/m) at renderer-space x, the planet centre at world (0, -bottomRadius, 0) (the atmosphere's convention,
// anchored to the world so a rebase keeps the field; altitude by the cancellation-free form, exact in float):
// trilinear / bilinear sampling with wrap, as the GPU's linear-wrap sampler (with float weights; the GPU's 8-bit
// filter weights differ by <= 1/512 of a texel).
double density(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double bottomRadius, const double x[3]);

// Reference radiance along a view ray (tests): single scattering of the sun (illuminance E, direction toward the sun
// sunDir) with the exact transmittance (steps of 'step' metres) toward the camera and toward the sun, plus the
// transmitted background: L = sum T(0, s) sigma_s p(theta) E T_sun(s) ds + T(0, end) L_bg. Double precision.
struct RayResult
{
    double radiance, transmittance;
};
RayResult referenceSingleScattering(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double bottomRadius, const double origin[3],
                                    const double dir[3], const double sunDir[3], double sunIlluminance, double background, double maxDistance, double step);

// Reference multiple scattering (tests): a volumetric path tracer in the density field, sun only (no sky light, black
// background): delta tracking with the majorant sigma_max, at each collision the albedo, next-event estimation toward the
// sun (the transmittance by 'step' midpoint steps, as referenceSingleScattering) and a new direction from the dual-lobe
// HG phase; Russian roulette past 16 collisions. 'firstOrder' returns the mean of the first collision's estimate (its
// expectation is referenceSingleScattering's radiance), so total - firstOrder is the multiple scattering.
struct PathResult
{
    double radiance, firstOrder, stdError;  // stdError: of the radiance's mean
    double meanCollisions;
};
// skyRadiance > 0 adds a sky of that uniform radiance over the upper hemisphere (local up; the ground stays black): a path
// that leaves the layer upward after at least one collision gathers throughput x skyRadiance (the sky seen directly
// through the layer is the reader's T x sky, not part of this). With sunIlluminance 0 the sun's estimates are skipped.
PathResult referencePathTraced(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double bottomRadius, const double origin[3],
                               const double dir[3], const double sunDir[3], double sunIlluminance, uint32_t paths, uint32_t seed, double step,
                               double skyRadiance = 0);

// APPROXIMATION, NOT EXACT (user decision 2026-09-27 06:40; FEATURE_STATUS B5): the multiple scattering the GPU march
// uses until the exact grid solve (S_STATUS_KO.md 9 step 1, a must-do after the weekly reset) replaces it.
//   sun:  sum_{k < N} a^k T sigma_s p_k(theta) E exp(-b^k tau_sun) segment, p_k the dual-lobe HG at (g0 c^k, g1 c^k)
//         (k = 0 is the exact single scattering);
//   sky:  T sigma_s L_sky max(0, s0 + s1 h_n) segment, L_sky the sky's radiance over the upper hemisphere, h_n the height
//         in the layer (the octaves carry no light from the sky, which lights the undersides through the layer).
// Fitted against referencePathTraced (Tests/CloudMsFit.cpp --sweep; the errors are in S_STATUS_KO.md 9). CloudCommon.hlsli
// CLOUD_MS_* repeats these values.
constexpr double kMsA = 0.70, kMsB = 0.15, kMsC = 0.60, kSkyS0 = 0.152, kSkyS1 = 1.451;
constexpr int kMsOctaves = 2;
// The approximate model along a view ray with the reference's exact transmittances (steps of 'step' metres, as
// referenceSingleScattering): the CPU twin of CloudMarch.hlsl mode 4 (the tests compare the GPU's march against it).
RayResult referenceApproximate(const CloudNoise& n, const CloudLayer& layer, const CloudOffsets& o, double bottomRadius, const double origin[3],
                               const double dir[3], const double sunDir[3], double sunIlluminance, double skyRadiance, double maxDistance, double step,
                               double a = kMsA, double b = kMsB, double c = kMsC, int octaves = kMsOctaves, double s0 = kSkyS0,
                               double s1 = kSkyS1);

double phase(const CloudLayer& layer, double cosTheta);                            // the dual-lobe HG phase (1/sr)
double altitudeOf(const CloudOffsets& o, double bottomRadius, const double x[3]);  // above the planet's surface (m)
} // namespace unx::render::clouds
