#pragma once
// Hair class on the CPU: the twins of the renderer's Passes/Hair/HairBsdf.hlsli (the fibre) and
// Passes/Hair/HairScattering.hlsli (a strand of a groom: the fibre averaged across its width, and the light scattered
// more than once inside the hair volume by the dual-scattering approximation, Zinke et al. 2008). The model, its
// approximations and its units are stated in HairScattering.hlsli; every function here is the kernel's function of the
// same name in the same operations (float), so the GPU and the CPU agree to rounding (unx_unit_tests
// hair_scattering_on_the_gpu). hairFibreKernelReference is the fibre in double precision, for the tests' integrals.
#include "unx/scene/SceneData.h"

#include <array>

namespace unx::scene::model
{
// Frame: +x along the fibre, directions are unit vectors pointing away from it; a kernel is f(wo, wi) |cos theta_i|.
struct HairFibre
{
    float eta = 1.55f;       // index of refraction (Material::ior)
    float3 absorption{};     // sigma_a (hairAbsorption)
    float betaM = 0.3f;      // longitudinal roughness (Material::roughness)
    float betaN = 0.3f;      // azimuthal roughness
    float tilt = 0.0349f;    // cuticle scale angle (radians)
};
HairFibre hairFibreOf(const Material& m);

constexpr uint32_t kHairNodes = 8;            // HAIR_NODES
constexpr float kHairDensityForward = 0.7f;   // d_f
constexpr float kHairDensityBackward = 0.7f;  // d_b

// HairBsdf.hlsli hairKernel: the fibre at offset h in [-1, 1].
float3 hairFibreKernel(const HairFibre& f, float3 outgoing, float3 incoming, float h);
std::array<double, 3> hairFibreKernelReference(const HairFibre& f, float3 outgoing, float3 incoming, double h);

float hairAzimuthCdf(float x, float width);
float hairAzimuthBox(float x, float width, float box);

struct HairStrand
{
    float so = 0, co = 1, phiO = 0, variance = 0, width = 0, tilt = 0;
    float3 r[kHairNodes], tt[kHairNodes], trt[kHairNodes], centre[kHairNodes], box[kHairNodes];
    float3 tail{};
};
HairStrand hairStrand(const HairFibre& f, float3 outgoing);
float3 hairStrandKernel(const HairStrand& s, float3 incoming, float spread, bool forward);

struct HairAverage
{
    float3 forward{}, backward{};                       // a_f, a_b
    float varianceForward = 0, varianceBackward = 0;    // beta_f^2, beta_b^2
    float shiftForward = 0, shiftBackward = 0;          // alpha_f, alpha_b
};
HairAverage hairAverage(const HairFibre& f, float sinTheta);

struct HairThrough
{
    float direct = 1;
    float3 scattered{};
    float spread = 0;
};
HairThrough hairThrough(const HairAverage& a, float count);

struct HairBack
{
    float3 albedo{};
    float shift = 0, variance = 0;
};
HairBack hairBackscatter(const HairAverage& a);

// The strand's kernel for a light in its groom (a = hairAverage at the light's inclination).
float3 hairStrandLight(const HairStrand& s, const HairAverage& a, float3 incoming, float countFront, float countBehind);

struct HairMoments
{
    float3 albedo{}, along{}, across{};
};
HairMoments hairStrandMoments(const HairStrand& s, const HairAverage& a, float embedded);
} // namespace unx::scene::model
