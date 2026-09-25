#pragma once
// Double-precision reference of the S atmosphere model (tests and gates only). Independent of the GPU quadratures:
// fine midpoint integration with exact exponentials, optical depths integrated directly (no LUT). The multiple-scattering
// source and the ground's indirect irradiance are supplied by the caller (the GPU table read back through msTableLookup,
// or a test's own), so each GPU stage is checked against its own inputs; singleScattering() and sphereSource() give the
// orders the table starts from independently of the GPU.
#include "unx/scene/SceneData.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace unx::render::atmosphere::reference
{
struct D3
{
    double x = 0, y = 0, z = 0;
};
inline D3 operator+(D3 a, D3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline D3 operator-(D3 a, D3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline D3 operator*(D3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
inline D3 operator*(D3 a, D3 b) { return { a.x * b.x, a.y * b.y, a.z * b.z }; }
inline double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 normalize(D3 a);
D3 expNeg(D3 a);  // exp(-a) per channel

struct Model
{
    double bottom, top, hr, hm, g;
    D3 rayleigh, mieScattering, mieAbsorption, ozone, albedo;
    double ozoneCenter, ozoneWidth;
};
Model fromScene(const scene::Atmosphere& a);

struct Coefficients
{
    D3 extinction, rayleigh, mie;
};
Coefficients coefficients(const Model& m, double altitude);
double altitudeOf(const Model& m, D3 p);   // p relative to the surface origin, planet centre (0, -bottom, 0)
D3 upOf(const Model& m, D3 p);
bool hitsGround(const Model& m, D3 p, D3 d);
// Distance along d from p to the top of the atmosphere (d unit); negative when p is above it.
double distanceToTop(const Model& m, D3 p, D3 d);
double distanceToGround(const Model& m, D3 p, D3 d);  // +inf when missed

// Optical depth from p along d to the top of the atmosphere (+inf per channel when the ray hits the ground).
D3 opticalDepth(const Model& m, D3 p, D3 d, int intervals = 4096, bool groundBlocks = true);
// Optical depth from (altitude, cosine) to the top, as the transmittance LUT stores it.
D3 opticalDepth(const Model& m, double altitude, double cosine, int intervals = 4096);
D3 sunTransmittance(const Model& m, D3 p, D3 sun, int intervals = 2048);

double rayleighPhase(double cosine);
double miePhase(double cosine, double g);

// Multiple-scattering source per unit sigma_s at p for a viewer looking along d (unit), per unit illuminance (J^ of
// AtmosphereCommon.hlsli); the ground's irradiance from scattered light per unit illuminance at sun cosine mus.
using MsFn = std::function<D3(D3 p, D3 d, D3 sun)>;
using GroundFn = std::function<D3(double mus)>;

// Far-field sky radiance per unit illuminance from p along d (unit), sun disk excluded: single scattering with exact
// sun transmittance + (sigma_R + sigma_M) ms + the Lambertian ground lit by the sun and by 'ground'.
D3 skyRadiance(const Model& m, D3 p, D3 d, D3 sun, const MsFn& ms, const GroundFn& ground, int steps = 2048);
// Air in-scattering (per unit illuminance) and transmittance over [0, distance] from p along d; points below the
// model's surface take the surface air (the GPU's airLiftToSurface convention).
void aerial(const Model& m, D3 p, D3 d, double distance, D3 sun, const MsFn& ms, D3& inscatter, D3& transmittance, int steps = 2048);

// Order 1 of the multiple-scattering build (MsBuild.hlsl PASS 0): single scattering along the ray from p along d plus
// the ground's reflection of the direct sun, per unit illuminance.
D3 singleScattering(const Model& m, D3 p, D3 d, D3 sun, int steps = 2048);
// The sigma-weighted phase average of a radiance field over the sphere at p for view direction v (MsBuild.hlsl PASS 1):
// (sigma_R S_R + sigma_M S_M) / (sigma_R + sigma_M), S_i = integral of p_i(w.v) radiance(w) dw. Multiple importance
// sampling (balance heuristic) over three deterministic sets of 'nodes' each: Henyey-Greenstein about v and about the sun
// (polar x azimuth, stratified), and a Fibonacci sphere.
D3 sphereSource(const Model& m, D3 p, D3 v, D3 sun, const std::function<D3(D3 w)>& radiance, int nodes);  // + a horizon grid of 2 nodes

// ---- The GPU multiple-scattering table and ground irradiance row (AtmosphereCommon.hlsli), read back, with the same
// parameterisation, log-domain interpolation and exact weights (the hardware's are 8-bit: 1/512 of a texel step).
struct MsTable
{
    const std::vector<uint8_t>* texels = nullptr;  // RGBA16_UNORM log J^, n[1] x n[2] x (n[0] n[3]), rows 256 B aligned (readback)
    uint32_t n[4] = {};                            // (nu, mu_s, mu, r)
};
struct MsTexelCoords
{
    double altitude, mu, mus, nu;
};
// Geometry of texel (i_nu, i_mus, i_mu, i_r) (MsBuild.hlsl msTexel), nu clamped to the range mu and mu_s allow.
MsTexelCoords msTexel(const Model& m, const MsTable& t, uint32_t iNu, uint32_t iMus, uint32_t iMu, uint32_t iR);
D3 msTexelValue(const MsTable& t, uint32_t iNu, uint32_t iMus, uint32_t iMu, uint32_t iR);  // J^ (exp of the texel)
D3 msTableLookup(const Model& m, const MsTable& t, double altitude, double mu, double mus, double nu);
// Row transmittanceSize.y of the transmittance LUT (RGBA32F, width w): ground indirect irradiance at sun cosine mus.
D3 groundIndirectLookup(const std::vector<uint8_t>& transmittance, uint32_t w, uint32_t row, double mus);
} // namespace unx::render::atmosphere::reference
