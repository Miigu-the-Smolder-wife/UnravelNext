// Hair class on the CPU (unx/scene/HairModel.h): the twins of Passes/Hair/HairBsdf.hlsli and HairScattering.hlsli.
#include "unx/scene/HairModel.h"

#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>

namespace unx::scene::model
{
namespace
{
// ---- HairBsdf.hlsli, in T (float: the kernel's operations; double: the tests' reference)
template <typename T> constexpr T kPi = T(3.141592653589793);

template <typename T> T fresnel(T cosine, T eta)
{
    const T c = std::clamp(std::abs(cosine), T(0), T(1)), sineSquared = (1 - c * c) / (eta * eta);
    if (sineSquared >= 1) return 1;
    const T transmitted = std::sqrt(std::max(T(0), 1 - sineSquared));
    const T a = (eta * c - transmitted) / (eta * c + transmitted), b = (c - eta * transmitted) / (c + eta * transmitted);
    return T(0.5) * (a * a + b * b);
}
template <typename T> T logBesselMinusArgument(T x)
{
    if (x >= 12)
    {
        const T inverse = 1 / x;
        const T series = 1 + inverse * (T(0.125) + inverse * (T(0.0703125) + inverse * (T(0.0732421875) + inverse * T(0.112152099609375))));
        return T(-0.5) * std::log(2 * kPi<T> * x) + std::log(series);
    }
    T term = 1, sum = 1;
    const T square = x * x * T(0.25);
    for (uint32_t k = 1; k <= 24; ++k)
    {
        term *= square / T(k * k);
        const T next = sum + term;
        if (next == sum) break;
        sum = next;
    }
    return std::log(sum) - x;
}
template <typename T> T longitudinal(T sinI, T cosI, T sinO, T cosO, T variance)
{
    const T x = cosI * cosO / variance;
    const T delta = (sinI + sinO) * (sinI + sinO) + (cosI - cosO) * (cosI - cosO);
    const T normalization = std::log(variance) + std::log(1 - std::exp(-2 / variance));
    return std::exp(logBesselMinusArgument(x) - delta / (2 * variance) - normalization);
}
template <typename T> T azimuth(T difference, T width)
{
    difference -= 2 * kPi<T> * std::floor((difference + kPi<T>) / (2 * kPi<T>));
    const T e = std::exp(-std::abs(difference) / width), edge = std::exp(-kPi<T> / width);
    return e / ((width * (1 + e) * (1 + e)) * ((1 - edge) / (1 + edge)));
}
template <typename T> T varianceOf(T betaM)
{
    const T rough = T(0.726) * betaM + T(0.812) * betaM * betaM + T(3.7) * std::pow(betaM, T(20));
    return rough * rough;
}
template <typename T> T widthOf(T betaN) { return T(0.62665706865775) * (T(0.265) * betaN + T(1.194) * betaN * betaN + T(5.372) * std::pow(betaN, T(22))); }
template <typename T> T lobeShift(T tilt, uint32_t order) { return order == 0 ? -2 * tilt : order == 1 ? tilt : order == 2 ? 4 * tilt : T(0); }
template <typename T> T lobeVariance(T variance, uint32_t order) { return variance * (order == 0 ? T(1) : order == 1 ? T(0.25) : T(4)); }

// hairKernel (hairDistribution + hairEvaluate): the fibre at offset h.
template <typename T> std::array<T, 3> fibreKernel(const HairFibre& f, float3 outgoing, float3 incoming, T h)
{
    const T eta = f.eta, tilt = f.tilt;
    const T so = std::clamp(T(outgoing.x), T(-1), T(1)), co = std::sqrt(std::max(T(0), 1 - so * so));
    const T phiO = std::atan2(T(outgoing.z), T(outgoing.y)), gammaO = std::asin(std::clamp(h, T(-1), T(1)));
    const T cosT = std::sqrt(std::max(T(0), 1 - so * so / (eta * eta))), sinGammaT = h * co / std::sqrt(eta * eta - so * so);
    const T gammaT = std::asin(std::clamp(sinGammaT, T(-1), T(1)));
    const T fr = fresnel(co * std::sqrt(std::max(T(0), 1 - h * h)), eta);
    const T variance = varianceOf(T(f.betaM)), width = widthOf(T(f.betaN));
    const T si = std::clamp(T(incoming.x), T(-1), T(1)), ci = std::sqrt(std::max(T(0), 1 - si * si));
    const T phi = std::atan2(T(incoming.z), T(incoming.y)) - phiO;
    T density[4];
    for (uint32_t order = 0; order < 4; ++order)
    {
        const T shift = lobeShift(tilt, order), sn = std::sin(shift), cs = std::cos(shift);
        const T az = order == 3 ? 1 / (2 * kPi<T>) : azimuth(phi - (2 * T(order) * gammaT - 2 * gammaO + T(order) * kPi<T>), width);
        density[order] = longitudinal(si, ci, so * cs + co * sn, std::abs(co * cs - so * sn), lobeVariance(variance, order)) * az;
    }
    std::array<T, 3> result{};
    const float absorption[3] = { f.absorption.x, f.absorption.y, f.absorption.z };
    for (int c = 0; c < 3; ++c)
    {
        const T through = std::exp(-T(absorption[c]) * (2 * std::sqrt(std::max(T(0), 1 - sinGammaT * sinGammaT)) / cosT));
        const T transmission = (1 - fr) * (1 - fr) * through, internal = transmission * fr * through;
        const T denominator = 1 - fr * through, tail = denominator > 0 ? internal * fr * through / denominator : T(0);
        result[c] = fr * density[0] + transmission * density[1] + internal * density[2] + tail * density[3];
    }
    return result;
}

// ---- HairScattering.hlsli (float)
constexpr float kPiF = 3.141592653589793f;
constexpr float kForwardMax = 0.99f, kBackShiftMax = 0.785398163f;

float3 exp3(float3 v) { return { std::exp(v.x), std::exp(v.y), std::exp(v.z) }; }
float mean3(float3 v) { return dot(v, float3{ 1.0f / 3, 1.0f / 3, 1.0f / 3 }); }
float3 min3(float3 v, float m) { return { std::min(v.x, m), std::min(v.y, m), std::min(v.z, m) }; }

struct Rotated
{
    float sn, cs;
};
Rotated rotate(float sn, float cs, float shift)
{
    const float s = std::sin(shift), c = std::cos(shift);
    return { sn * c + cs * s, std::abs(cs * c - sn * s) };
}

struct Node
{
    float3 r, tt, trt, tail, centre, box;
    float weight;
};
Node node(uint32_t k, float so, float co, float eta, float3 absorption)
{
    const float step = kPiF / kHairNodes, gamma = -0.5f * kPiF + (k + 0.5f) * step;
    const float h = std::sin(gamma), cg = std::cos(gamma);
    const float etaP = std::sqrt(eta * eta - so * so), cosT = std::sqrt(std::max(0.0f, 1 - so * so / (eta * eta)));
    const float sinGammaT = std::clamp(h * co / etaP, -1.0f, 1.0f), cosGammaT = std::sqrt(std::max(0.0f, 1 - sinGammaT * sinGammaT));
    const float gammaT = std::asin(sinGammaT);
    const float3 through = exp3(-absorption * (2 * cosGammaT / cosT));
    const float fr = fresnel(co * cg, eta);
    Node n;
    n.weight = cg * std::sin(0.5f * step);
    n.r = { fr, fr, fr };
    n.tt = through * ((1 - fr) * (1 - fr));
    n.trt = n.tt * fr * through;
    const float3 numerator = n.trt * fr * through, denominator = float3{ 1, 1, 1 } - through * fr;
    n.tail = { denominator.x > 0 ? numerator.x / denominator.x : 0, denominator.y > 0 ? numerator.y / denominator.y : 0, denominator.z > 0 ? numerator.z / denominator.z : 0 };
    n.centre = { -2 * gamma, 2 * gammaT - 2 * gamma + kPiF, 4 * gammaT - 2 * gamma + 2 * kPiF };
    const float slope = cosGammaT > 1e-4f ? cg * co / (etaP * cosGammaT) : 0;
    n.box = float3{ std::abs(-2.0f), std::abs(2 * slope - 2), std::abs(4 * slope - 2) } * step;
    return n;
}
float component(float3 v, uint32_t i) { return i == 0 ? v.x : (i == 1 ? v.y : v.z); }

float langevin(float kappa) { return kappa < 1e-2f ? kappa / 3 : (kappa > 20 ? 1 - 1 / kappa : 1 / std::tanh(kappa) - 1 / kappa); }
// (mean sin theta_i, mean cos theta_i) of the longitudinal lobe about the inclination (sn, cs) with variance v
Rotated longitudinalMean(float sn, float cs, float v)
{
    (void)cs;
    const float kappa = 1 / v, A = langevin(kappa);
    return { -sn * A, std::sqrt(std::max(0.0f, 1 - (A / kappa + (1 - 3 * A / kappa) * sn * sn))) };
}
} // namespace

HairFibre hairFibreOf(const Material& m)
{
    HairFibre f;
    f.eta = m.ior;
    f.absorption = hairAbsorption(m);
    f.betaM = m.roughness;
    f.betaN = m.hairBetaN;
    f.tilt = m.hairTilt;
    return f;
}

float3 hairFibreKernel(const HairFibre& f, float3 outgoing, float3 incoming, float h)
{
    const std::array<float, 3> k = fibreKernel<float>(f, outgoing, incoming, h);
    return { k[0], k[1], k[2] };
}
std::array<double, 3> hairFibreKernelReference(const HairFibre& f, float3 outgoing, float3 incoming, double h) { return fibreKernel<double>(f, outgoing, incoming, h); }

float hairAzimuthCdf(float x, float width)
{
    const float turns = std::floor((x + kPiF) / (2 * kPiF));
    const float t = (x - 2 * kPiF * turns) / width, edge = std::exp(-kPiF / width);
    const float sigma = t >= 0 ? 1 / (1 + std::exp(-t)) : std::exp(t) / (1 + std::exp(t));
    return turns + (sigma * (1 + edge) - edge) / (1 - edge);
}
float hairAzimuthBox(float x, float width, float box)
{
    if (box < 1e-3f) return azimuth(x, width);
    return (hairAzimuthCdf(x + 0.5f * box, width) - hairAzimuthCdf(x - 0.5f * box, width)) / box;
}

HairStrand hairStrand(const HairFibre& f, float3 outgoing)
{
    HairStrand s;
    s.so = std::clamp(outgoing.x, -1.0f, 1.0f);
    s.co = std::sqrt(std::max(0.0f, 1 - s.so * s.so));
    s.phiO = std::atan2(outgoing.z, outgoing.y);
    s.variance = varianceOf(f.betaM);
    s.width = widthOf(f.betaN);
    s.tilt = f.tilt;
    s.tail = {};
    for (uint32_t k = 0; k < kHairNodes; ++k)
    {
        const Node n = node(k, s.so, s.co, f.eta, f.absorption);
        s.r[k] = n.weight * n.r;
        s.tt[k] = n.weight * n.tt;
        s.trt[k] = n.weight * n.trt;
        s.tail = s.tail + n.weight * n.tail;
        s.centre[k] = n.centre;
        s.box[k] = n.box;
    }
    return s;
}

float3 hairStrandKernel(const HairStrand& s, float3 incoming, float spread, bool forward)
{
    const float si = std::clamp(incoming.x, -1.0f, 1.0f), ci = std::sqrt(std::max(0.0f, 1 - si * si)), phi = std::atan2(incoming.z, incoming.y) - s.phiO;
    float m[4];
    for (uint32_t order = 0; order < 4; ++order)
    {
        const Rotated shifted = rotate(s.so, s.co, lobeShift(s.tilt, order));
        m[order] = longitudinal(si, ci, shifted.sn, shifted.cs, lobeVariance(s.variance, order) + spread);
    }
    float3 sum = s.tail * (m[3] / (2 * kPiF));
    for (uint32_t k = 0; k < kHairNodes; ++k)
    {
        float n[3];
        for (uint32_t p = 0; p < 3; ++p)
        {
            const float x = phi - component(s.centre[k], p), b = component(s.box[k], p);
            if (forward) n[p] = hairAzimuthBox(x, std::sqrt(s.width * s.width + b * b / (4 * kPiF * kPiF)), kPiF);
            else n[p] = hairAzimuthBox(x, s.width, b);
        }
        sum = sum + s.r[k] * (m[0] * n[0]) + s.tt[k] * (m[1] * n[1]) + s.trt[k] * (m[2] * n[2]);
    }
    return sum;
}

HairAverage hairAverage(const HairFibre& f, float sinTheta)
{
    const float so = std::clamp(sinTheta, -1.0f, 1.0f), co = std::sqrt(std::max(0.0f, 1 - so * so));
    const float variance = varianceOf(f.betaM), width = widthOf(f.betaN);
    HairAverage a;
    float vf = 0, vb = 0, sf = 0, sb = 0;
    for (uint32_t k = kHairNodes / 2; k < kHairNodes; ++k)
    {
        const Node n = node(k, so, co, f.eta, f.absorption);
        for (uint32_t order = 0; order < 4; ++order)
        {
            const float3 energy = (2 * n.weight) * (order == 0 ? n.r : order == 1 ? n.tt : order == 2 ? n.trt : n.tail);
            float far = 0.5f;
            if (order < 3)
            {
                const float b = component(n.box, order), c = component(n.centre, order);
                const float w = std::sqrt(width * width + b * b / (4 * kPiF * kPiF));
                far = hairAzimuthCdf(1.5f * kPiF - c, w) - hairAzimuthCdf(0.5f * kPiF - c, w);
            }
            const float mean = mean3(energy), v = lobeVariance(variance, order), shift = lobeShift(f.tilt, order);
            a.forward = a.forward + energy * far;
            a.backward = a.backward + energy * (1 - far);
            vf += mean * far * v;
            vb += mean * (1 - far) * v;
            sf += mean * far * shift;
            sb += mean * (1 - far) * shift;
        }
    }
    const float ef = mean3(a.forward), eb = mean3(a.backward);
    a.varianceForward = ef > 1e-6f ? vf / ef : variance;
    a.varianceBackward = eb > 1e-6f ? vb / eb : variance;
    a.shiftForward = ef > 1e-6f ? sf / ef : 0;
    a.shiftBackward = eb > 1e-6f ? sb / eb : 0;
    return a;
}

HairThrough hairThrough(const HairAverage& a, float count)
{
    const float3 af = min3(a.forward, kForwardMax);
    HairThrough t;
    t.direct = std::exp(-count);
    t.scattered = (exp3((af - float3{ 1, 1, 1 }) * count) - float3{ t.direct, t.direct, t.direct }) * kHairDensityForward;
    const float x = count * mean3(af);
    t.spread = a.varianceForward * (x > 1e-3f ? x / (1 - std::exp(-x)) : 1 + 0.5f * x);
    return t;
}

HairBack hairBackscatter(const HairAverage& a)
{
    const float3 af = min3(a.forward, kForwardMax), ab = a.backward, k = float3{ 1, 1, 1 } - af * af;
    HairBack b;
    const float3 first = ab * af * af, third = ab * ab * ab * af * af, k3 = k * k * k;
    b.albedo = { first.x / k.x + third.x / k3.x, first.y / k.y + third.y / k3.y, first.z / k.z + third.z / k3.z };
    const float f = mean3(af), g = mean3(ab), kf = 1 - f * f;
    b.shift = std::clamp(a.shiftBackward * (1 - 2 * g * g / (kf * kf)) + a.shiftForward * (2 * kf * kf + 4 * f * f * g * g) / (kf * kf * kf), -kBackShiftMax, kBackShiftMax);
    const float bf = std::sqrt(a.varianceForward), bb = std::sqrt(a.varianceBackward);
    const float sigma = (1 + kHairDensityBackward * f * f) * (std::sqrt(2 * a.varianceForward + a.varianceBackward) + g * g * std::sqrt(2 * a.varianceForward + 3 * a.varianceBackward)) /
                        (1 + g * g * (2 * bf + 3 * bb));
    b.variance = sigma * sigma;
    return b;
}

float3 hairStrandLight(const HairStrand& s, const HairAverage& a, float3 incoming, float countFront, float countBehind)
{
    const HairThrough t = hairThrough(a, countFront);
    float3 sum = t.direct * hairStrandKernel(s, incoming, 0, false);
    if (countFront > 0) sum = sum + t.scattered * hairStrandKernel(s, incoming, t.spread, true);
    const float embedded = 1 - std::exp(-countBehind);
    if (embedded > 0)
    {
        const HairBack b = hairBackscatter(a);
        const float si = std::clamp(incoming.x, -1.0f, 1.0f), ci = std::sqrt(std::max(0.0f, 1 - si * si));
        const float cosPhi = std::cos(std::atan2(incoming.z, incoming.y) - s.phiO);
        const Rotated shifted = rotate(s.so, s.co, b.shift);
        const float direct = longitudinal(si, ci, shifted.sn, shifted.cs, b.variance) * (0.5f * std::max(cosPhi, 0.0f));
        const float spread = longitudinal(si, ci, shifted.sn, shifted.cs, b.variance + t.spread) * ((1 + cosPhi) / (2 * kPiF));
        sum = sum + (kHairDensityBackward * embedded) * b.albedo * (float3{ 1, 1, 1 } * (t.direct * direct) + t.scattered * spread);
    }
    return sum;
}

HairMoments hairStrandMoments(const HairStrand& s, const HairAverage& a, float embedded)
{
    float3 energy[4] = { {}, {}, {}, s.tail }, facing[3] = {};
    for (uint32_t k = 0; k < kHairNodes; ++k)
    {
        float c[3];
        for (uint32_t p = 0; p < 3; ++p)
        {
            const float half = 0.5f * component(s.box[k], p);
            c[p] = std::cos(component(s.centre[k], p)) * (half > 1e-3f ? std::sin(half) / half : 1);
        }
        energy[0] = energy[0] + s.r[k];
        energy[1] = energy[1] + s.tt[k];
        energy[2] = energy[2] + s.trt[k];
        facing[0] = facing[0] + s.r[k] * c[0];
        facing[1] = facing[1] + s.tt[k] * c[1];
        facing[2] = facing[2] + s.trt[k] * c[2];
    }
    const float ps = kPiF * s.width, chi = ps < 1e-3f ? 1 : (ps > 40 ? 0 : ps / std::sinh(ps));
    HairMoments o;
    for (uint32_t order = 0; order < 4; ++order)
    {
        const Rotated shifted = rotate(s.so, s.co, lobeShift(s.tilt, order));
        const Rotated mean = longitudinalMean(shifted.sn, shifted.cs, lobeVariance(s.variance, order));
        o.albedo = o.albedo + energy[order];
        o.along = o.along + energy[order] * mean.sn;
        if (order < 3) o.across = o.across + facing[order] * (chi * mean.cs);
    }
    if (embedded > 0)
    {
        const HairBack b = hairBackscatter(a);
        const Rotated shifted = rotate(s.so, s.co, b.shift);
        const Rotated mean = longitudinalMean(shifted.sn, shifted.cs, b.variance);
        const float3 e = (kHairDensityBackward * embedded) * b.albedo;
        o.albedo = o.albedo + e;
        o.along = o.along + e * mean.sn;
        o.across = o.across + e * (0.25f * kPiF * mean.cs);
    }
    return o;
}
} // namespace unx::scene::model
