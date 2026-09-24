#pragma once
// The atmosphere of INTERFACES_KO.md 8.3 as a participating medium (the previous engine's model, scene::Atmosphere):
//   sigma_s = rayleigh * exp(-h / H_R) + mie * exp(-h / H_M)
//   sigma_t = sigma_s + mieAbsorption * exp(-h / H_M) + ozone * max(0, 1 - |h - c| / w)
//   phase   Rayleigh 3/(16 pi)(1 + cos^2), Mie Henyey-Greenstein(g)
// Planet centre (0, -R, 0): the scene origin sits on the surface. Positions are handled in double precision.
//
// Optical depth: short segments (< kShortSegment) by composite Gauss-Legendre directly; long ones by a table of the
// depth to the top of the atmosphere, tau_top(r, mu), bicubic in Bruneton's (x_mu, x_r) parameterisation with 2048 x
// 1024 entries built by 256-panel Gauss-Legendre quadrature. selfCheck() measures the table error against direct
// quadrature so the error bound is a measured number, not an assumption.
#include "unx/scene/SceneData.h"

#include <array>
#include <cstdint>
#include <vector>

namespace unx::reference
{
struct Rgb
{
    float r = 0, g = 0, b = 0;
    Rgb() = default;
    Rgb(float a, float b_, float c) : r(a), g(b_), b(c) {}
    explicit Rgb(float v) : r(v), g(v), b(v) {}
    explicit Rgb(float3 v) : r(v.x), g(v.y), b(v.z) {}
    Rgb operator+(const Rgb& o) const { return { r + o.r, g + o.g, b + o.b }; }
    Rgb operator-(const Rgb& o) const { return { r - o.r, g - o.g, b - o.b }; }
    Rgb operator*(const Rgb& o) const { return { r * o.r, g * o.g, b * o.b }; }
    Rgb operator/(const Rgb& o) const { return { r / o.r, g / o.g, b / o.b }; }
    Rgb operator*(float s) const { return { r * s, g * s, b * s }; }
    Rgb& operator+=(const Rgb& o) { r += o.r; g += o.g; b += o.b; return *this; }
    Rgb& operator*=(const Rgb& o) { r *= o.r; g *= o.g; b *= o.b; return *this; }
    Rgb& operator*=(float s) { r *= s; g *= s; b *= s; return *this; }
    float max() const { return r > g ? (r > b ? r : b) : (g > b ? g : b); }
    float avg() const { return (r + g + b) * (1.0f / 3.0f); }
    float luminance() const { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
    bool isZero() const { return r == 0 && g == 0 && b == 0; }
    bool finite() const;
};
inline Rgb expNeg(const Rgb& t);

struct Double3
{
    double x = 0, y = 0, z = 0;
};

class AtmosphereModel
{
public:
    static constexpr double kShortSegment = 20000.0;  // m

    explicit AtmosphereModel(const scene::Atmosphere& a);

    struct Coefficients
    {
        Rgb scatteringRayleigh, scatteringMie, extinction;
    };
    Coefficients at(double altitude) const;
    double altitude(const Double3& p) const;  // above the planet surface
    float phaseRayleigh(float cosine) const;
    float phaseMie(float cosine) const;
    // Samples an outgoing direction for scattering at a point where the path arrived along 'forward' (propagation
    // direction) from the mixture weighted by the local Rayleigh/Mie scattering (grey average); returns its pdf,
    // which equals the mixture phase value (so the phase weight is exactly Rgb-mixture / pdf).
    float3 samplePhase(float3 forward, float wRayleigh, float wMie, float u1, float u2, float u3, float& pdf) const;

    // Distance along the ray to the planet (ground) and to the top of the atmosphere; negative if not hit.
    double groundDistance(const Double3& o, float3 d) const;
    double topDistance(const Double3& o, float3 d) const;

    // Optical depth of the segment o + d t, t in [0, length]. The segment must not pass through the planet.
    Rgb opticalDepth(const Double3& o, float3 d, double length) const;
    // Optical depth from o to the top of the atmosphere along d; +infinity if the ray hits the ground.
    Rgb opticalDepthToTop(const Double3& o, float3 d) const;

    // Measured maximum absolute error of opticalDepthToTop() against 8192-panel direct quadrature over random
    // (r, mu) in the scene-relevant range (altitude 0-100 km, all non-ground directions).
    double selfCheck(uint32_t samples, double* maxRelTransmittanceError = nullptr, bool verbose = false) const;

    const scene::Atmosphere& params() const { return m_a; }
    double bottomRadius() const { return m_a.bottomRadius; }

private:
    Rgb depthTopTable(double r, double mu) const;
    Rgb depthTopDirect(double r, double mu, uint32_t panels) const;
    Rgb integrate(const Double3& o, float3 d, double t0, double t1, uint32_t panels) const;

    scene::Atmosphere m_a;
    double m_R, m_Rt, m_H;  // bottom, top radius; sqrt(Rt^2 - R^2)
    static constexpr uint32_t kTableMu = 2048, kTableR = 1024;
    std::vector<std::array<float, 3>> m_table;  // [ir * kTableMu + imu]
};

inline Rgb expNeg(const Rgb& t);
} // namespace unx::reference

#include <cmath>
namespace unx::reference
{
inline bool Rgb::finite() const { return std::isfinite(r) && std::isfinite(g) && std::isfinite(b); }
inline Rgb expNeg(const Rgb& t) { return { std::exp(-t.r), std::exp(-t.g), std::exp(-t.b) }; }
} // namespace unx::reference
