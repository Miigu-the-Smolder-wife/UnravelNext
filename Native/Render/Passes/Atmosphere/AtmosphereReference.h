#pragma once
// Double-precision reference of the S atmosphere model (tests and gates only). Independent of the GPU quadratures:
// fine midpoint integration with exact exponentials, optical depths integrated directly (no LUT). The multiple-scattering
// source Psi_ms is supplied by the caller (the GPU LUT read back, or multiScatter() below), so each GPU stage is checked
// against its own inputs.
#include "unx/scene/SceneData.h"

#include <array>
#include <functional>

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

using PsiFn = std::function<D3(D3 p, D3 sun)>;

// Hillaire 2020 Psi_ms at (altitude, sun cosine), the GPU formulation (Fibonacci directions, per-direction marching)
// with exact sun optical depths.
D3 multiScatter(const Model& m, double altitude, double sunCosine, int directions, int steps);

// Far-field sky radiance per unit illuminance from p along d (unit), sun disk excluded: single scattering with exact
// sun transmittance + sigma_s * psi + Lambertian ground.
D3 skyRadiance(const Model& m, D3 p, D3 d, D3 sun, const PsiFn& psi, int steps = 2048);
// Air in-scattering (per unit illuminance) and transmittance over [0, distance] from p along d; points below the
// model's surface take the surface air (the GPU's airLiftToSurface convention).
void aerial(const Model& m, D3 p, D3 d, double distance, D3 sun, const PsiFn& psi, D3& inscatter, D3& transmittance, int steps = 2048);
} // namespace unx::render::atmosphere::reference
