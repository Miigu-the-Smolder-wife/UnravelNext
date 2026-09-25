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
} // namespace unx::scene::model
