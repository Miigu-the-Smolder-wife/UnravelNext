// A strand of a groom as M's hair records shade it (CoverageHair.hlsl): E's fibre model (HairBsdf.hlsli) averaged across
// the fibre's width, and the light scattered more than once inside the hair volume by the dual-scattering approximation
// (Zinke et al. 2008, "Dual Scattering Approximation for Fast Multiple Scattering in Hair"; the reference renderer's
// EvaluateHairMultipleScattering has the same two parts). C++ twin: scene::model (unx/scene/HairModel.h), compared on
// the GPU by Passes/Test/HairScatteringModel.hlsl.
//
// Frame and units as HairBsdf.hlsli: +x along the fibre, directions are unit vectors pointing away from it, a kernel is
// f(wo, wi) |cos theta_i| (radiance towards wo per unit of illuminance on the plane facing wi; over wi it integrates to
// the albedo).
//
// 1. Across the width. A strand is narrower than a pixel, so a record sees every offset h in [-1, 1] at once: the kernel
//    is the mean over h, taken at HAIR_NODES offsets h = sin gamma, gamma uniform in (-pi/2, pi/2) (weights cos gamma).
//    Inside a node the lobe energies are constant and each azimuthal centre Phi_p(gamma) is linear, so the lobe of
//    width s becomes its mean over the box |dPhi_p / dgamma| x pi / HAIR_NODES (hairAzimuthBox, a closed form of the
//    lobe's CDF): the boxes of neighbouring nodes meet, the sum is smooth in the azimuth. [measured against the fibre's
//    mean over 1,024 offsets, unx_test_scene_hairmodel: the albedo within 4e-3, the mean absolute difference over the
//    incoming directions 2 to 8 % of the albedo]
// 2. Forward and backward averages (the paper's a_f, a_b, beta_f, beta_b, alpha_f, alpha_b) for light of inclination
//    theta: the lobes' energies split by the share of each azimuthal lobe on the far side of the fibre (|phi| > pi/2),
//    their longitudinal variances and cuticle shifts averaged with those energies (hairAverage). Variances and shifts
//    are one number for the three colours (weighted by the colours' mean energy).
// 3. Global part: n fibres lie between the point and the light (an expected count from E's density volume, so the true
//    count is taken as Poisson with mean n). Unscattered light: exp(-n). Light scattered forward by every fibre it met:
//    d_f (exp(-n (1 - a_f)) - exp(-n)), arriving spread over the half circle of azimuths around the light's direction
//    and over inclinations with variance beta_f^2 x (the mean number of fibres it met, n a_f / (1 - exp(-n a_f))). The
//    strand answers it with its lobes widened by that variance and averaged over that half circle (hairStrandKernel
//    with 'forward').
// 4. Local part: light that leaves the strand's neighbourhood backwards after one or three backward scatterings,
//    A_b = a_b a_f^2 / (1 - a_f^2) + a_b^3 a_f^2 / (1 - a_f^2)^3, as one longitudinal lobe of shift Delta_b and standard
//    deviation sigma_b (the paper's equations 14 to 16) times d_b; over the azimuth cos(phi) / 2 on the light's side
//    (the paper: 1 / pi there, which steps at the sides), under the spread light (1 + cos phi) / (2 pi). It exists only
//    where fibres lie behind the strand: times 1 - exp(-n_behind), so a lone fibre is the fibre model alone.
// d_f = d_b = 0.7 (the paper's density factors). a_f is held under 0.99 and |Delta_b| under pi / 4 (white fibres: the
// series' sums grow without bound).
// Energy: over wi the width-averaged kernel integrates to the fibre's albedo, with or without the spread; a white
// volume returns towards the light's side at most a_b + d_b A_b <= 1 and lets through exp(-n) + the scattered part <= 1.
#ifndef UNX_HAIR_SCATTERING_HLSLI
#define UNX_HAIR_SCATTERING_HLSLI
#include "Passes/Hair/HairBsdf.hlsli"

#define HAIR_NODES 8u
#define HAIR_DENSITY_FORWARD 0.7f
#define HAIR_DENSITY_BACKWARD 0.7f
#define HAIR_FORWARD_MAX 0.99f
#define HAIR_BACK_SHIFT_MAX 0.785398163f

// The azimuthal lobe's CDF (hairAzimuth: a logistic of width s cut to [-pi, pi]) continued over the turns:
// F(x + 2 pi) = F(x) + 1.
float hairAzimuthCdf(float x, float width)
{
    const float turns = floor((x + kHairPi) / (2 * kHairPi));
    const float t = (x - 2 * kHairPi * turns) / width, edge = exp(-kHairPi / width);
    const float sigma = t >= 0 ? 1 / (1 + exp(-t)) : exp(t) / (1 + exp(t));
    return turns + (sigma * (1 + edge) - edge) / (1 - edge);
}
// Mean of the lobe over [x - box / 2, x + box / 2] (a box under 1e-3: the lobe at x).
float hairAzimuthBox(float x, float width, float box)
{
    if (box < 1e-3f) return hairAzimuth(x, width);
    return (hairAzimuthCdf(x + 0.5f * box, width) - hairAzimuthCdf(x - 0.5f * box, width)) / box;
}
float hairVarianceOf(float betaM)
{
    const float rough = 0.726f * betaM + 0.812f * betaM * betaM + 3.7f * pow(betaM, 20);
    return rough * rough;
}
float hairWidthOf(float betaN) { return 0.62665706865775f * (0.265f * betaN + 1.194f * betaN * betaN + 5.372f * pow(betaN, 22)); }
// (sin, cos) of the inclination theta + shift.
float2 hairRotate(float sn, float cs, float shift)
{
    float s, c;
    sincos(shift, s, c);
    return float2(sn * c + cs * s, abs(cs * c - sn * s));
}
// The lobes of one node for light or view of inclination (so, co): energies of R, TT, TRT and the tail (rgb each), the
// azimuthal centres of the first three and their boxes over the node, the node's weight.
struct HairNode
{
    float3 r, tt, trt, tail;
    float3 centre, box;
    float weight;
};
HairNode hairNode(uint k, float so, float co, float eta, float3 absorption)
{
    const float step = kHairPi / HAIR_NODES, gamma = -0.5f * kHairPi + (k + 0.5f) * step;
    float h, cg;
    sincos(gamma, h, cg);
    const float etaP = sqrt(eta * eta - so * so), cosT = sqrt(max(0.0f, 1 - so * so / (eta * eta)));
    const float sinGammaT = clamp(h * co / etaP, -1.0f, 1.0f), cosGammaT = sqrt(max(0.0f, 1 - sinGammaT * sinGammaT));
    const float gammaT = asin(sinGammaT);
    const float3 through = exp(-absorption * (2 * cosGammaT / cosT));
    const float fresnel = hairFresnel(co * cg, eta);
    HairNode n;
    n.weight = cg * sin(0.5f * step);  // (the midpoint rule over gamma of cos gamma / 2: the weights sum to 1)
    n.r = fresnel;
    n.tt = (1 - fresnel) * (1 - fresnel) * through;
    n.trt = n.tt * fresnel * through;
    const float3 numerator = n.trt * fresnel * through, denominator = 1 - fresnel * through;
    n.tail = float3(denominator.x > 0 ? numerator.x / denominator.x : 0, denominator.y > 0 ? numerator.y / denominator.y : 0, denominator.z > 0 ? numerator.z / denominator.z : 0);
    n.centre = float3(-2 * gamma, 2 * gammaT - 2 * gamma + kHairPi, 4 * gammaT - 2 * gamma + 2 * kHairPi);
    const float slope = cosGammaT > 1e-4f ? cg * co / (etaP * cosGammaT) : 0;  // d gamma_t / d gamma
    n.box = abs(float3(-2, 2 * slope - 2, 4 * slope - 2)) * step;
    return n;
}

// ---- 1. the strand seen from 'outgoing': its nodes with their weights folded into the energies
struct HairStrand
{
    float so, co, phiO, variance, width, tilt;
    float3 r[HAIR_NODES], tt[HAIR_NODES], trt[HAIR_NODES], centre[HAIR_NODES], box[HAIR_NODES];
    float3 tail;
};
HairStrand hairStrand(float3 outgoing, float eta, float3 absorption, float betaM, float betaN, float tilt)
{
    HairStrand s;
    s.so = clamp(outgoing.x, -1.0f, 1.0f);
    s.co = sqrt(max(0.0f, 1 - s.so * s.so));
    s.phiO = atan2(outgoing.z, outgoing.y);
    s.variance = hairVarianceOf(betaM);
    s.width = hairWidthOf(betaN);
    s.tilt = tilt;
    s.tail = 0;
    [loop] for (uint k = 0; k < HAIR_NODES; ++k)
    {
        const HairNode n = hairNode(k, s.so, s.co, eta, absorption);
        s.r[k] = n.weight * n.r;
        s.tt[k] = n.weight * n.tt;
        s.trt[k] = n.weight * n.trt;
        s.tail += n.weight * n.tail;
        s.centre[k] = n.centre;
        s.box[k] = n.box;
    }
    return s;
}
// The strand as a local light with a specular scale lights it: the fibre's surface reflection (R, the primary
// highlight) is the light's specular part and takes 'scale' (the light's specular scale over its diffuse one); what
// went through the fibre (TT, TRT, the tail) and the volume's multiple scattering stay with the diffuse scale the
// illuminance carries. (Unreal's hair BSDF returns its whole response as transmission, on the diffuse scale alone.)
HairStrand hairStrandSpecular(HairStrand s, float scale)
{
    [loop] for (uint k = 0; k < HAIR_NODES; ++k) s.r[k] *= scale;
    return s;
}
float hairLobeShift(float tilt, uint order) { return order == 0 ? -2 * tilt : order == 1 ? tilt : order == 2 ? 4 * tilt : 0; }
float hairLobeVariance(float variance, uint order) { return variance * (order == 0 ? 1 : order == 1 ? 0.25f : 4); }
// The width-averaged kernel. spread: variance added to every lobe's inclination; forward: the light arrives over the
// half circle of azimuths around 'incoming' (each lobe's mean over a box of pi; the node's own box goes into the lobe's
// width as the logistic of the same variance, box / (2 pi)).
float3 hairStrandKernel(HairStrand s, float3 incoming, float spread, bool forward)
{
    const float si = clamp(incoming.x, -1.0f, 1.0f), ci = sqrt(max(0.0f, 1 - si * si)), phi = atan2(incoming.z, incoming.y) - s.phiO;
    float m[4];
    [loop] for (uint order = 0; order < 4; ++order)
    {
        const float2 shifted = hairRotate(s.so, s.co, hairLobeShift(s.tilt, order));
        m[order] = hairLongitudinal(si, ci, shifted.x, shifted.y, hairLobeVariance(s.variance, order) + spread);
    }
    float3 sum = s.tail * (m[3] / (2 * kHairPi));
    [loop] for (uint k = 0; k < HAIR_NODES; ++k)
    {
        const float3 x = phi - s.centre[k], b = s.box[k];
        float3 n;
        if (forward)
        {
            const float3 w = sqrt(s.width * s.width + b * b / (4 * kHairPi * kHairPi));
            n = float3(hairAzimuthBox(x.x, w.x, kHairPi), hairAzimuthBox(x.y, w.y, kHairPi), hairAzimuthBox(x.z, w.z, kHairPi));
        }
        else n = float3(hairAzimuthBox(x.x, s.width, b.x), hairAzimuthBox(x.y, s.width, b.y), hairAzimuthBox(x.z, s.width, b.z));
        sum += s.r[k] * (m[0] * n.x) + s.tt[k] * (m[1] * n.y) + s.trt[k] * (m[2] * n.z);
    }
    return sum;
}

// ---- 2. forward and backward averages for light of inclination asin(sinTheta)
struct HairAverage
{
    float3 forward, backward;                // a_f, a_b
    float varianceForward, varianceBackward;  // beta_f^2, beta_b^2
    float shiftForward, shiftBackward;        // alpha_f, alpha_b
};
HairAverage hairAverage(float sinTheta, float eta, float3 absorption, float betaM, float betaN, float tilt)
{
    const float so = clamp(sinTheta, -1.0f, 1.0f), co = sqrt(max(0.0f, 1 - so * so));
    const float variance = hairVarianceOf(betaM), width = hairWidthOf(betaN);
    HairAverage a;
    a.forward = a.backward = 0;
    float vf = 0, vb = 0, sf = 0, sb = 0;
    // (the split is even in h: the nodes of h > 0, twice)
    [loop] for (uint k = HAIR_NODES / 2; k < HAIR_NODES; ++k)
    {
        const HairNode n = hairNode(k, so, co, eta, absorption);
        [loop] for (uint order = 0; order < 4; ++order)
        {
            const float3 energy = (2 * n.weight) * (order == 0 ? n.r : order == 1 ? n.tt : order == 2 ? n.trt : n.tail);
            float far = 0.5f;
            if (order < 3)
            {
                const float w = sqrt(width * width + n.box[order] * n.box[order] / (4 * kHairPi * kHairPi));
                far = hairAzimuthCdf(1.5f * kHairPi - n.centre[order], w) - hairAzimuthCdf(0.5f * kHairPi - n.centre[order], w);
            }
            const float mean = dot(energy, 1.0f / 3), v = hairLobeVariance(variance, order), shift = hairLobeShift(tilt, order);
            a.forward += energy * far;
            a.backward += energy * (1 - far);
            vf += mean * far * v;
            vb += mean * (1 - far) * v;
            sf += mean * far * shift;
            sb += mean * (1 - far) * shift;
        }
    }
    const float ef = dot(a.forward, 1.0f / 3), eb = dot(a.backward, 1.0f / 3);
    a.varianceForward = ef > 1e-6f ? vf / ef : variance;
    a.varianceBackward = eb > 1e-6f ? vb / eb : variance;
    a.shiftForward = ef > 1e-6f ? sf / ef : 0;
    a.shiftBackward = eb > 1e-6f ? sb / eb : 0;
    return a;
}

// ---- 3. what n fibres between the point and the light let through
struct HairThrough
{
    float direct;      // unscattered
    float3 scattered;  // scattered forward by every fibre it met
    float spread;      // the scattered light's variance over the inclination
};
HairThrough hairThrough(HairAverage a, float count)
{
    const float3 af = min(a.forward, HAIR_FORWARD_MAX);
    HairThrough t;
    t.direct = exp(-count);
    t.scattered = HAIR_DENSITY_FORWARD * (exp(-count * (1 - af)) - t.direct);
    const float x = count * dot(af, 1.0f / 3);
    t.spread = a.varianceForward * (x > 1e-3f ? x / (1 - exp(-x)) : 1 + 0.5f * x);
    return t;
}

// ---- 4. the neighbourhood's backward lobe
struct HairBack
{
    float3 albedo;  // A_b
    float shift, variance;
};
HairBack hairBackscatter(HairAverage a)
{
    const float3 af = min(a.forward, HAIR_FORWARD_MAX), ab = a.backward, k = 1 - af * af;
    HairBack b;
    b.albedo = ab * af * af / k + ab * ab * ab * af * af / (k * k * k);
    const float f = dot(af, 1.0f / 3), g = dot(ab, 1.0f / 3), kf = 1 - f * f;
    b.shift = clamp(a.shiftBackward * (1 - 2 * g * g / (kf * kf)) + a.shiftForward * (2 * kf * kf + 4 * f * f * g * g) / (kf * kf * kf), -HAIR_BACK_SHIFT_MAX,
                    HAIR_BACK_SHIFT_MAX);
    const float bf = sqrt(a.varianceForward), bb = sqrt(a.varianceBackward);
    const float sigma = (1 + HAIR_DENSITY_BACKWARD * f * f) * (sqrt(2 * a.varianceForward + a.varianceBackward) + g * g * sqrt(2 * a.varianceForward + 3 * a.varianceBackward)) /
                        (1 + g * g * (2 * bf + 3 * bb));
    b.variance = sigma * sigma;
    return b;
}

// The strand's kernel for a light in its groom: 'a' = hairAverage at the light's inclination (incoming.x), countFront =
// fibres between the point and the light, countBehind = fibres past the point along the light's path.
float3 hairStrandLight(HairStrand s, HairAverage a, float3 incoming, float countFront, float countBehind)
{
    const HairThrough t = hairThrough(a, countFront);
    float3 sum = t.direct * hairStrandKernel(s, incoming, 0, false);
    if (countFront > 0) sum += t.scattered * hairStrandKernel(s, incoming, t.spread, true);
    const float embedded = 1 - exp(-countBehind);
    if (embedded > 0)
    {
        const HairBack b = hairBackscatter(a);
        const float si = clamp(incoming.x, -1.0f, 1.0f), ci = sqrt(max(0.0f, 1 - si * si));
        const float cosPhi = cos(atan2(incoming.z, incoming.y) - s.phiO);
        const float2 shifted = hairRotate(s.so, s.co, b.shift);
        const float direct = hairLongitudinal(si, ci, shifted.x, shifted.y, b.variance) * (0.5f * max(cosPhi, 0.0f));
        const float spread = hairLongitudinal(si, ci, shifted.x, shifted.y, b.variance + t.spread) * ((1 + cosPhi) / (2 * kHairPi));
        sum += (HAIR_DENSITY_BACKWARD * embedded) * b.albedo * (t.direct * direct + t.scattered * spread);
    }
    return sum;
}

// ---- the strand under smooth light: the kernel's integral and first moment over wi (direct light of a lone or embedded
// strand, no fibres in front), so that light L(w) = c + g . w gives  c albedo + (g . x) along + (g . e) across, e the unit
// direction of the outgoing direction's part across the fibre. 'a' = hairAverage at the outgoing inclination,
// embedded = 1 - exp(-fibres around the strand).
// The inclination's moments are exact (a lobe of variance v is a von Mises-Fisher distribution of concentration 1 / v
// about its cone: mean sine -sin(theta_o + alpha) A, A = coth(1/v) - v; mean cosine taken as sqrt(1 - mean sine^2
// squared moment)); the azimuth's is the lobe centres' cosine times the logistic's (pi s / sinh(pi s)) and the box's.
struct HairMoments
{
    float3 albedo, along, across;
};
float hairLangevin(float kappa) { return kappa < 1e-2f ? kappa / 3 : (kappa > 20 ? 1 - 1 / kappa : 1 / tanh(kappa) - 1 / kappa); }
// (mean sin theta_i, mean cos theta_i) of the longitudinal lobe about the inclination (sn, cs) with variance v.
float2 hairLongitudinalMean(float sn, float cs, float v)
{
    const float kappa = 1 / v, A = hairLangevin(kappa);
    return float2(-sn * A, sqrt(max(0.0f, 1 - (A / kappa + (1 - 3 * A / kappa) * sn * sn))));
}
HairMoments hairStrandMoments(HairStrand s, HairAverage a, float embedded)
{
    float3 energy[4] = { float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0), s.tail }, facing[3] = { float3(0, 0, 0), float3(0, 0, 0), float3(0, 0, 0) };
    [loop] for (uint k = 0; k < HAIR_NODES; ++k)
    {
        const float3 half = 0.5f * s.box[k];
        const float3 sinc = float3(half.x > 1e-3f ? sin(half.x) / half.x : 1, half.y > 1e-3f ? sin(half.y) / half.y : 1, half.z > 1e-3f ? sin(half.z) / half.z : 1);
        const float3 c = cos(s.centre[k]) * sinc;
        energy[0] += s.r[k];
        energy[1] += s.tt[k];
        energy[2] += s.trt[k];
        facing[0] += s.r[k] * c.x;
        facing[1] += s.tt[k] * c.y;
        facing[2] += s.trt[k] * c.z;
    }
    const float ps = kHairPi * s.width, chi = ps < 1e-3f ? 1 : (ps > 40 ? 0 : ps / sinh(ps));
    HairMoments o;
    o.albedo = o.along = o.across = 0;
    [loop] for (uint order = 0; order < 4; ++order)
    {
        const float2 shifted = hairRotate(s.so, s.co, hairLobeShift(s.tilt, order));
        const float2 mean = hairLongitudinalMean(shifted.x, shifted.y, hairLobeVariance(s.variance, order));
        o.albedo += energy[order];
        o.along += energy[order] * mean.x;
        if (order < 3) o.across += facing[order] * (chi * mean.y);
    }
    if (embedded > 0)
    {
        const HairBack b = hairBackscatter(a);
        const float2 shifted = hairRotate(s.so, s.co, b.shift);
        const float2 mean = hairLongitudinalMean(shifted.x, shifted.y, b.variance);
        const float3 e = (HAIR_DENSITY_BACKWARD * embedded) * b.albedo;
        o.albedo += e;
        o.along += e * mean.x;
        o.across += e * (0.25f * kHairPi * mean.y);
    }
    return o;
}
#endif
