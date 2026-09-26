#pragma once
// Material model v1 (INTERFACES_KO.md 8.1): the one BRDF definition the real-time renderer (M: Passes/Common/
// MaterialModel.hlsli) and the reference path tracer (C) both implement. This C++ version is authoritative; the HLSL
// mirror must agree to float rounding (checked by M's tests).
//
// Standard class:
//   alpha = max(r^2, kMinAlpha)                               r = perceptual roughness
//   f0    = lerp(0.08 * specular * (1,1,1), baseColor, metallic)
//   D     = alpha^2 / (pi * ((n.h)^2 (alpha^2 - 1) + 1)^2)                      GGX
//   V     = 0.5 / (n.l sqrt((n.v)^2 (1 - alpha^2) + alpha^2) + n.v sqrt((n.l)^2 (1 - alpha^2) + alpha^2))   Smith, height-correlated
//   F     = f0 + (1 - f0) (1 - v.h)^5                                            Schlick
//   f_s   = D V F * (1 + f0 (1 / E(n.v, r) - 1))                                  multiple-scattering compensation (Turquin 2019)
//   f_d   = (1 - metallic) baseColor / pi                                         Lambert
//   f     = f_d + f_s                     for n.l > 0 and n.v > 0 (front side)
// E(mu, r) is the directional albedo of the single-scattering specular lobe with F = 1, tabulated on a 32 x 32 grid
// that includes its end points (mu_i = i/31 along x with mu_0 evaluated at 1e-4, r_j = j/31 along y) and read
// bilinearly at x = mu * 31, y = r * 31.
// Foliage class: front side uses Standard with f_d scaled by (1 - transmission); the back side (n.l < 0 < n.v or the
// reverse) is diffuse transmission transmission * (1 - metallic) * baseColor / pi.
#include "unx/core/Math.h"
#include "unx/scene/SceneData.h"

#include <vector>

namespace unx::scene::model
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinAlpha = 1e-4f;
constexpr uint32_t kAlbedoTableSize = 32;

struct Surface
{
    MaterialClass cls = MaterialClass::Standard;
    float3 baseColor{ 0.5f, 0.5f, 0.5f };
    float roughness = 0.5f;
    float metallic = 0;
    float specular = 0.5f;
    float transmission = 0;
};

float alphaFromRoughness(float roughness);
float3 f0(const Surface& s);
// GGX D. sinSqNH = |n x h|^2 = 1 - NoH^2 computed directly: NoH^2 (a^2 - 1) + 1 cancels to 0 in float at a = 1e-4 (mirror).
float distributionGgx(float NoH, float sinSqNH, float alpha);
float visibilitySmithGgxCorrelated(float NoV, float NoL, float alpha);
float3 fresnelSchlick(float3 f0, float VoH);

// kAlbedoTableSize^2 values, row-major with r along rows: table[ri * 32 + mi]. Deterministic (Hammersley, 4096 samples
// per grid point, visible-normal sampling). Uploaded unchanged as the renderer's materialModelLut (StructuredBuffer<float>).
const std::vector<float>& directionalAlbedoTable();
float directionalAlbedo(float NoV, float roughness);
// Split specular directional albedo (INTERFACES 8.1, v1.25): the same visible-normal samples as E split by the Schlick
// weight w = (1 - v.h)^5, A = mean(weight (1 - w)), B = mean(weight w), so f0 A + B is the lobe's albedo with Schlick
// Fresnel and A + B = E (to float rounding). 2 values per grid point (A, B), same grid and addressing as E. Uploaded
// unchanged as the renderer's specularAlbedoLut (StructuredBuffer<float2>).
const std::vector<float>& specularAlbedoTable();
float2 specularAlbedo(float NoV, float roughness);

// BRDF value (without the cosine), world-space unit vectors: n shading normal, v towards the viewer, l towards the light.
float3 evaluate(const Surface& s, float3 n, float3 v, float3 l);

// Clearcoat layer (A9; MATERIAL_LAYERS_KO.md 1.1 as measured by C: R1 with candidates A2 and S, Results/C/MaterialLayers/
// clearcoat_r1e.md - within the section 3 criteria on dielectric bases; metal bases under a coat fail on lobe shape and
// are an open redesign item). A clear dielectric of index eta (a tabulated coat: kCoatEtas), GGX alpha_c = alpha(r_c),
// covering the fraction c of the Standard surface s: f = (1 - c) f_s + c (f_c + f_1 + f_ms) for n.v, n.l > 0:
//   f_c  = D(h; a_c) V(a_c) F_eta(v.h) sqrt(E_ms(mu_v) E_ms(mu_l) / (E_c(mu_v) E_c(mu_l)))   coat reflection (A2: the single-
//          scattering lobe scaled reciprocally to the energy-conserving coat's reflectance E_ms)
//   T(mu) = 1 - E_ms(mu)                                                                  transmission (no absorption)
//   p    = the smooth refraction of v, l into the coat (mu' = sqrt(1 - (1 - mu^2) / eta^2), same azimuth)
//   f_1  = T(mu_v) T(mu_l) f_s'(p_v, p_l) / eta^2, f_s' = s with alpha'^2 = alpha_b^2 + (s_v^2 + s_l^2) alpha_c^2 / 4,
//          s(mu) = 1 - mu / (eta mu')                                                     first pass through the coat (S)
//   f_ms = T(mu_l) (a - e)(mu'_l) rho / (1 - rho K_ms) T(mu_v) / (pi eta^2)               light returned by the coat's
//          inside, per channel: a = the base albedo, e = its part leaving through the smooth interface at once,
//          a - e = rho_d K_0 + (f0 (A - A_x) + (B - B_x)) comp(mu'), rho = rho_d + (f0 Abar + Bbar) (1 + f0 (1 / Ebar - 1)),
//          rho_d = (1 - metallic) baseColor, comp(mu) = 1 + f0 (1 / E(mu, r_b) - 1), K_0 = the smooth coat's inner
//          diffuse reflectance, K_ms(r_c) the rough energy-conserving coat's.
// Tables per coat (C's unx_study_material_layers tables, same 32 x 32 grid and addressing as E): E_c, E_ms, A_x, B_x
// (mu, r), K_ms, Abar, Bbar (r), K_0.
constexpr float kCoatEtas[2] = { 1.5f, 1.33f };
struct Coat
{
    float cover = 0;         // c in [0, 1]
    float roughness = 0.05f; // perceptual r_c
    float eta = 1.5f;        // one of kCoatEtas
};
// The GPU table (StructuredBuffer<float>): coat k's block at k * kCoatTableStride: E_c [0, 1024), E_ms [1024, 2048),
// A_x [2048, 3072), B_x [3072, 4096), K_ms [4096, 4128), Abar [4128, 4160), Bbar [4160, 4192), K_0 at 4192.
constexpr uint32_t kCoatTableStride = 4224;
const std::vector<float>& coatTable();
uint32_t coatIndex(float eta);  // the tabulated coat of this index (fails for others)
float fresnelDielectric(float cosI, float eta);  // exact, unpolarised; eta = n_t / n_i; 1 past the critical angle
float3 evaluateCoated(const Surface& s, const Coat& c, float3 n, float3 v, float3 l);
float evaluateCoatLobe(const Coat& c, float3 n, float3 v, float3 l);  // f_c alone (without the cover)

// Hair class (INTERFACES 8.1 v1.66): the fibre's absorption sigma_a (PBRT 4e's convention: per unit fibre radius, the
// chord of the unit-radius cross-section is the path length; HairBsdf.hlsli hairAttenuation). With melanin (eumelanin +
// pheomelanin > 0): d'Eon et al. 2011, eu (0.419, 0.697, 1.37) + pheo (0.187, 0.4, 1.05). Otherwise baseColor is the
// target colour c (clamped to [1e-4, 1]): Chiang et al. 2016, sigma_a = (ln c / (5.969 - 0.215 bN + 2.532 bN^2 -
// 10.73 bN^3 + 5.574 bN^4 + 0.245 bN^5))^2 with bN = hairBetaN. The reference tracers and the GPU material record call it.
float3 hairAbsorption(const Material& m);
} // namespace unx::scene::model
