// Fibre scattering for hair strands (B10). Owner: E. Readers: M's coverage-fragment shading (material class hair), R's
// hit shading, the reference path tracer.
//
// Normalized longitudinal / azimuthal fibre scattering (d'Eon et al. 2011, Chiang et al. 2016; PBRT 4e 9.9), ported
// from the old engine's HairProgram (TitanNative). It keeps every internal order: R, TT, TRT exactly and the remaining
// orders in one closed-form tail (their attenuation sum), so a non-absorbing fibre scatters all incident energy (the
// white furnace holds for any roughness). Chosen over TressFX's shading (Kajiya-Kay / Marschner-style approximations
// without energy conservation or internal orders) as the higher-quality model (reassignment 3a708c49).
//
// Frame: +x along the fibre (tangent, root to tip), y and z span the normal plane; directions are unit vectors in that
// frame. h in [-1, 1] is the offset of the ray across the fibre (the azimuthal position of the hit). Parameters: eta
// (index of refraction, 1.55 for keratin), absorption sigma_a per unit fibre diameter (rgb), betaM longitudinal and
// betaN azimuthal roughness in (0, 1], tilt the cuticle scale angle (radians, ~2 degrees).
// hairKernel returns the radiance kernel f(wo, wi) |cos theta_i| (integrates to the directional albedo over wi).
#ifndef UNX_HAIR_BSDF_HLSLI
#define UNX_HAIR_BSDF_HLSLI

static const float kHairPi = 3.141592653589793f;

float hairFresnel(float cosine, float eta)
{
    const float c = saturate(abs(cosine)), sineSquared = (1 - c * c) / (eta * eta);
    if (sineSquared >= 1) return 1;
    const float transmitted = sqrt(max(0.0f, 1 - sineSquared));
    const float a = (eta * c - transmitted) / (eta * c + transmitted), b = (c - eta * transmitted) / (c + eta * transmitted);
    return 0.5f * (a * a + b * b);
}
// log(I0(x)) - x without overflow: asymptotic series from 12, power series below (tail after k = 24 < 3.6e-12).
float hairLogBesselMinusArgument(float x)
{
    if (x >= 12)
    {
        const float inverse = rcp(x);
        const float series = 1 + inverse * (0.125f + inverse * (0.0703125f + inverse * (0.0732421875f + inverse * 0.112152099609375f)));
        return -0.5f * log(2 * kHairPi * x) + log(series);
    }
    float term = 1, sum = 1;
    const float square = x * x * 0.25f;
    [loop] for (uint k = 1; k <= 24; ++k)
    {
        term *= square / (k * k);
        const float next = sum + term;
        if (next == sum) break;
        sum = next;
    }
    return log(sum) - x;
}
float hairLongitudinal(float sinI, float cosI, float sinO, float cosO, float variance)
{
    const float x = cosI * cosO / variance;
    // (cosI cosO - sinI sinO - 1) / v without subtracting nearly equal 1 / v terms at a narrow peak
    const float delta = (sinI + sinO) * (sinI + sinO) + (cosI - cosO) * (cosI - cosO);
    const float normalization = log(variance) + log(1 - exp(-2 / variance));
    return exp(hairLogBesselMinusArgument(x) - delta / (2 * variance) - normalization);
}
float hairAzimuth(float difference, float width)
{
    difference -= 2 * kHairPi * floor((difference + kHairPi) / (2 * kHairPi));
    const float e = exp(-abs(difference) / width), edge = exp(-kHairPi / width);
    return e / ((width * (1 + e) * (1 + e)) * ((1 - edge) / (1 + edge)));
}
float3 hairAttenuation(float sinO, float cosO, float h, float eta, float3 absorption, out float gammaT, out float3 through, out float fresnel)
{
    const float cosT = sqrt(max(0.0f, 1 - sinO * sinO / (eta * eta))), sinGammaT = h * cosO / sqrt(eta * eta - sinO * sinO);
    gammaT = asin(clamp(sinGammaT, -1.0f, 1.0f));
    through = exp(-absorption * (2 * sqrt(max(0.0f, 1 - sinGammaT * sinGammaT)) / cosT));
    fresnel = hairFresnel(cosO * sqrt(max(0.0f, 1 - h * h)), eta);
    const float3 first = (1 - fresnel) * (1 - fresnel) * through;
    const float3 denominator = 1 - fresnel * through;
    return fresnel + float3(denominator.x > 0 ? first.x / denominator.x : 0, denominator.y > 0 ? first.y / denominator.y : 0, denominator.z > 0 ? first.z / denominator.z : 0);
}
struct HairDistribution
{
    float so, co, phiO, gammaO, gammaT, variance, width, tilt;
    float3 reflection, transmission, internal, tail;
    float4 probabilities;
};
HairDistribution hairDistribution(float3 outgoing, float h, float eta, float3 absorption, float betaM, float betaN, float tilt)
{
    HairDistribution d;
    d.so = clamp(outgoing.x, -1.0f, 1.0f);
    d.co = sqrt(max(0.0f, 1 - d.so * d.so));
    d.phiO = atan2(outgoing.z, outgoing.y);
    d.gammaO = asin(clamp(h, -1.0f, 1.0f));
    d.tilt = tilt;
    float fresnel;
    float3 through;
    hairAttenuation(d.so, d.co, h, eta, absorption, d.gammaT, through, fresnel);
    const float rough = 0.726f * betaM + 0.812f * betaM * betaM + 3.7f * pow(betaM, 20);
    d.variance = rough * rough;
    d.width = 0.62665706865775f * (0.265f * betaN + 1.194f * betaN * betaN + 5.372f * pow(betaN, 22));
    d.reflection = fresnel;
    d.transmission = (1 - fresnel) * (1 - fresnel) * through;
    d.internal = d.transmission * fresnel * through;
    const float3 numerator = d.internal * fresnel * through, denominator = 1 - fresnel * through;
    d.tail = float3(denominator.x > 0 ? numerator.x / denominator.x : 0, denominator.y > 0 ? numerator.y / denominator.y : 0, denominator.z > 0 ? numerator.z / denominator.z : 0);
    d.probabilities = float4(dot(d.reflection, 1.0f / 3), dot(d.transmission, 1.0f / 3), dot(d.internal, 1.0f / 3), dot(d.tail, 1.0f / 3));
    d.probabilities /= dot(d.probabilities, 1);
    return d;
}
// Cuticle tilt: the longitudinal lobe of order p is rotated by -2, 1, 4, 0 tilts.
float2 hairShift(HairDistribution d, uint order)
{
    const float shift = order == 0 ? -2 * d.tilt : order == 1 ? d.tilt : order == 2 ? 4 * d.tilt : 0;
    float sn, cs;
    sincos(shift, sn, cs);
    return float2(d.so * cs + d.co * sn, abs(d.co * cs - d.so * sn));
}
float3 hairEvaluate(HairDistribution d, float3 incoming, out float pdf)
{
    const float si = clamp(incoming.x, -1.0f, 1.0f), ci = sqrt(max(0.0f, 1 - si * si)), phi = atan2(incoming.z, incoming.y) - d.phiO;
    float3 result = 0;
    pdf = 0;
    [loop] for (uint order = 0; order < 4; ++order)
    {
        const float3 energy = order == 0 ? d.reflection : order == 1 ? d.transmission : order == 2 ? d.internal : d.tail;
        const float2 shifted = hairShift(d, order);
        const float v = d.variance * (order == 0 ? 1 : order == 1 ? 0.25f : 4);
        const float azimuth = order == 3 ? rcp(2 * kHairPi) : hairAzimuth(phi - (2 * order * d.gammaT - 2 * d.gammaO + order * kHairPi), d.width);
        const float density = hairLongitudinal(si, ci, shifted.x, shifted.y, v) * azimuth;
        result += energy * density;
        pdf += d.probabilities[order] * density;
    }
    return result;
}
float3 hairKernel(float3 outgoing, float3 incoming, float h, float eta, float3 absorption, float betaM, float betaN, float tilt)
{
    float pdf;
    return hairEvaluate(hairDistribution(outgoing, h, eta, absorption, betaM, betaN, tilt), incoming, pdf);
}
struct HairSample
{
    float3 direction, weight;
    float pdf;
};
// Importance sample of the kernel (random in [0, 1)^3): lobe by its energy share, then the lobe's longitudinal and
// azimuthal inverses (the nearer logistic tail is inverted directly: 1 - CDF would lose it at narrow widths).
HairSample hairSample(HairDistribution d, float3 random)
{
    uint order = 0;
    float choice = random.z;
    [loop] while (order < 3 && choice >= d.probabilities[order])
    {
        choice -= d.probabilities[order];
        ++order;
    }
    choice /= d.probabilities[order];
    const float2 shifted = hairShift(d, order);
    const float variance = d.variance * (order == 0 ? 1 : order == 1 ? 0.25f : 4);
    const float cosine = 1 + variance * log(max(random.x, asfloat(0x00800000)) + (1 - random.x) * exp(-2 / variance));
    const float si = clamp(-cosine * shifted.x + sqrt(max(0.0f, 1 - cosine * cosine)) * cos(2 * kHairPi * random.y) * shifted.y, -1.0f, 1.0f);
    const float ci = sqrt(max(0.0f, 1 - si * si));
    float azimuth = 2 * kHairPi * choice;
    if (order < 3)
    {
        const float edge = exp(-kHairPi / d.width), q = min(choice, 1 - choice);
        float offset = kHairPi;
        if (q > 0) offset = -d.width * log((edge + q * (1 - edge)) / (1 - q * (1 - edge)));
        azimuth = 2 * order * d.gammaT - 2 * d.gammaO + order * kHairPi + (choice < 0.5f ? -offset : offset);
    }
    const float phi = d.phiO + azimuth;
    HairSample result;
    result.direction = float3(si, ci * cos(phi), ci * sin(phi));
    const float3 kernel = hairEvaluate(d, result.direction, result.pdf);
    result.weight = kernel / result.pdf;
    return result;
}
#endif
