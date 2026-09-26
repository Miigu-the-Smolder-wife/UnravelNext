#pragma once
// Water in the CPU reference (engine 1 W, with render C's conditions): a smooth dielectric interface and the medium behind
// it, the model W's water shading approximates (FEATURES_GAME 1.9 stage 1: refraction, absorption, reflection).
//   Interface   exact unpolarised Fresnel (mean of the s and p reflectances), total internal reflection past the
//               critical angle; smooth (a delta BSDF: no light sampling at this vertex; the material's roughness, 0.02
//               for water, is below what the comparison resolves - condition recorded). A path picks reflection with
//               probability F and refraction with 1 - F, so the throughput changes only by the radiance scale of a
//               refraction: radiance carried from the far side of the interface to the near side scales by
//               (n_near / n_far)^2.
//   Medium      Beer-Lambert absorption inside, sigma_a = -ln(baseColor): the material's baseColor is its transmittance
//               over 1 m (the authoring rule of W and W2). Entered by a refraction through a front face, left by a
//               refraction through a back face; bodies are single-sided meshes whose front faces point out of the
//               water (a pool's top surface over opaque walls, or a fluid's closed surface).
// Water instances must cast shadows: a light sample from under the water is blocked by the interface (sunlight through
// water reaches the reference only by paths that refract; a sun disk through a smooth interface needs manifold NEE, the
// first work after the 9/30 reset - condition recorded), never passes it straight.
#include "Bsdf.h"  // (dot3, RtScene)

#include <algorithm>
#include <cmath>

namespace unx::reference
{
inline double dielectricFresnel(double cosI, double eta)  // eta = n_incident / n_transmitted; 1 under total reflection
{
    cosI = std::clamp(cosI, 0.0, 1.0);
    const double s2 = eta * eta * (1 - cosI * cosI);
    if (s2 >= 1) return 1;
    const double ct = std::sqrt(1 - s2), rs = (eta * cosI - ct) / (eta * cosI + ct), rp = (eta * ct - cosI) / (eta * ct + cosI);
    return 0.5 * (rs * rs + rp * rp);
}

struct DielectricSample
{
    float3 wi;
    float weight = 1;       // throughput factor (the radiance scale of a refraction; 1 for a reflection)
    bool refracted = false;
    bool entering = false;  // a refraction into the medium (through a front face)
};

// One sample of the interface at s (shading normal s.ns out of the water) for the direction wo towards the viewer.
inline DielectricSample sampleDielectric(const Surface& s, float3 wo, float ior, float u)
{
    DielectricSample out;
    const float3 n = s.ns;
    double cosO = dot3(n, wo);
    const bool outside = cosO >= 0;
    const float3 nn = outside ? n : -n;  // the normal on wo's side
    cosO = std::fabs(cosO);
    const double eta = outside ? 1.0 / ior : ior;  // n_near / n_far
    const double F = dielectricFresnel(cosO, eta);
    if (u < F)
    {
        out.wi = normalize(nn * (float)(2 * cosO) - wo);
        return out;
    }
    const double s2 = eta * eta * (1 - cosO * cosO), cosT = std::sqrt(std::max(0.0, 1 - s2));
    out.wi = normalize(wo * (float)-eta + nn * (float)(eta * cosO - cosT));
    out.weight = (float)(eta * eta);
    out.refracted = true;
    out.entering = outside;
    return out;
}

// The medium's absorption per metre from its 1 m transmittance.
inline Rgb dielectricAbsorption(float3 transmittance)
{
    auto a = [](float t) { return -std::log(std::clamp(t, 1e-6f, 1.0f)); };
    return Rgb(float3{ a(transmittance.x), a(transmittance.y), a(transmittance.z) });
}
} // namespace unx::reference
