// M shading helpers (ARCHITECTURE 2.11; INTERFACES 7.5, 8). Owner: M.
#ifndef UNX_M_SHADING_COMMON_HLSLI
#define UNX_M_SHADING_COMMON_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "MaterialModel.hlsli"
#include "Passes/FX/ParticleLayer.hlsli"

#define SH_PI 3.14159265358979

// ---------------------------------------------------------------- specular directional albedo
// Single-scattering specular albedo with Schlick F split by f0 (the model's table, scene::model::specularAlbedoTable,
// same visible-normal samples as its E table; frame constant g_specularAlbedoLut, v1.25): E_ss(f0) = f0 A + B, A + B = E.
// With the model's multiple-scattering compensation the lobe's full albedo is (f0 A + B)(1 + f0 (1/E - 1)) (INTERFACES
// 8.1).
float3 shSpecularAlbedo(float3 f0, float NoV, float roughness)
{
    const float2 ab = modelSpecularAlbedo(NoV, roughness);
    const float e = ab.x + ab.y;
    // A9 thin film (modelFilmBegin; f0 = F'(1)): the film's share takes F_film at the representative v.h = n.v times E
    // (MATERIAL_LAYERS 1.3 review 2) - exact for smooth lobes (bubbles, the sun's glint), where a Schlick curve through
    // F'(1) would carry the normal-incidence colour to every angle.
#if MODEL_FILM
    if (g_modelFilmTable != 0)
        return lerp(g_modelFilmF0 * ab.x + ab.y, modelFilmTable(NoV) * e, g_modelFilmCover) * (1 + f0 * (1 / e - 1));
#endif
    return (f0 * ab.x + ab.y) * (1 + f0 * (1 / e - 1));
}

// Model specular lobe f_s (without the cosine) for unit n, v, l on the front side (INTERFACES 8.1, v1.4 D).
float3 shSpecular(float3 f0, float alpha, float3 compensation, float3 n, float3 v, float3 l, float NoV, float NoL)
{
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), VoH = saturate(dot(v, h));
    const float3 nxh = cross(n, h);
    return modelFresnel(f0, VoH) * (modelD(NoH, dot(nxh, nxh), alpha) * modelV(NoV, NoL, alpha)) * compensation;
}

// ---------------------------------------------------------------- sun disk
// Specular reflection of the solar disk: I = L_sun int_cap f_s (n.l) dw, L_sun = E / (pi sin^2 theta_s) (INTERFACES
// 8.3), E = illuminance on a surface facing the sun (transmittance included). Regimes by the lobe width alpha
// (errors relative to the lobe's peak, measured against dense and lobe-sampled references in ShadingTests.cpp):
//   alpha >= 16 theta_s   the lobe is flat over the disk: point evaluation at the centre, <= 0.13 %, times
//                         L_sun Omega = E * 2 / (1 + cos theta_s);
//   2 .. 16 theta_s       4-point disk rule, exact to degree 3 (shSunSpecular4): <= 0.25 %;
//   alpha < 2 theta_s     the lobe's full albedo times the fraction of its reflected directions inside the disk
//                         (shSunLobeFraction): <= 0.65 %;
//   terminator band       (the disk crosses the shading normal's horizon, alpha >= 2 theta_s): polar product
//                         quadrature over the cap with the cosine clipped per point (4 Gauss-Legendre radii in area x
//                         12 angles), the only rule that follows the kink; < 1 px wide on curved surfaces.
float3 shSunSpecularQuadrature(float3 f0, float alpha, float3 compensation, float3 n, float3 v, float3 l0, float NoV, float sinS, float cosS)
{
    const float4 glNodes = float4(0.0694318442, 0.3300094782, 0.6699905218, 0.9305681558);  // Gauss-Legendre on [0, 1]
    const float4 glWeights = float4(0.1739274226, 0.3260725774, 0.3260725774, 0.1739274226);
    const float3 t = normalize(abs(l0.y) < 0.99 ? cross(float3(0, 1, 0), l0) : cross(float3(1, 0, 0), l0));
    const float3 b = cross(l0, t);
    float3 sum = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        // Area-uniform radius: 1 - cos(theta) uniform in [0, 1 - cos theta_s].
        const float c = 1 - glNodes[i] * (1 - cosS), s = sqrt(max(1 - c * c, 0.0));
        float3 ring = 0;
        // (MODEL_FILM kernels: a loop, so the film's F table is not inlined 48 times - DXIL limit; same arithmetic)
#if MODEL_FILM
        [loop]
#else
        [unroll]
#endif
        for (uint k = 0; k < 12; ++k)
        {
            float sp, cp;
            sincos((k + 0.5) * (2 * SH_PI / 12), sp, cp);
            const float3 l = l0 * c + (t * cp + b * sp) * s;
            const float NoL = dot(n, l);
            if (NoL > 0) ring += shSpecular(f0, alpha, compensation, n, v, l, NoV, NoL) * NoL;
        }
        sum += ring * (glWeights[i] / 12);
    }
    return sum;  // mean of f_s cos over the cap
}

// Mean of max(n.l, 0) over the solar disk (uniform radiance): n.l is linear across the small disk (radius R = theta_s
// in the tangent plane at l0, gradient k = sqrt(1 - NoL0^2)), so with u = -NoL0 / (k R) the clipped mean is
//   (2 k R / pi) [ (1 - u^2)^(3/2) / 3 - (u / 2)(acos u - u sqrt(1 - u^2)) ],
// NoL0 when the whole disk is above the horizon (u <= -1) and 0 below (u >= 1). The terminator of a curved surface is
// a band of disk width, not a kink.
float shCapCosine(float NoL0)
{
    const float k = sqrt(max(1 - NoL0 * NoL0, 0.0)), kR = k * g_sunAngularRadius;
    if (NoL0 >= kR) return NoL0;
    if (NoL0 <= -kR) return 0;
    const float u = -NoL0 / kR, w = sqrt(max(1 - u * u, 0.0));
    return (2 * kR / SH_PI) * (w * w * w / 3 - 0.5 * u * (acos(u) - u * w));
}

// Mean of f_s cos over the cap by the 4-point disk rule (radius R / sqrt 2 in area measure, weights 1/4): exact for
// every polynomial of degree <= 3 over the disk, so for a lobe much wider than the disk the error is the fourth-order
// term ~ (theta_s / alpha)^4. Only Fresnel (a function of v.h, which the 0.27 deg disk moves by < 0.3 deg) and the
// compensation enter once; D, V and the cosine are taken per point, since at grazing light n.l itself changes by
// +-sin(theta_s) across the disk (measured in ShadingTests.cpp for 2 theta_s <= alpha < 16 theta_s).
float3 shSunSpecular4(float3 f0, float alpha, float3 compensation, float3 n, float3 v, float3 l0, float NoV, float cosS)
{
    const float3 t = normalize(abs(l0.y) < 0.99 ? cross(float3(0, 1, 0), l0) : cross(float3(1, 0, 0), l0));
    const float3 b = cross(l0, t);
    const float c = 1 - 0.5 * (1 - cosS), s = sqrt(max(1 - c * c, 0.0)) * 0.70710678;  // s * (+-t +- b) per point
    float sum = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const float3 l = l0 * c + (t * ((k & 1) ? s : -s) + b * ((k & 2) ? s : -s));
        const float NoL = dot(n, l);  // > 0: the caller keeps the disk above the horizon (NoL0 >= 2 sin theta_s)
        const float3 h = normalize(v + l);
        const float3 nxh = cross(n, h);
        sum += modelD(saturate(dot(n, h)), dot(nxh, nxh), alpha) * modelV(NoV, NoL, alpha) * NoL;
    }
    const float3 h0 = normalize(v + l0);
    return modelFresnel(f0, saturate(dot(v, h0))) * (0.25 * sum) * compensation;
}

// Fraction of a narrow specular lobe (alpha < theta_s / 2) whose reflected directions fall inside the solar disk.
// Around the mirror direction r (h = n), the GGX lobe in l-space is the P22 slope distribution scaled by 2 alpha in the
// plane of incidence and by 2 alpha cos(theta_d) across it (reflection doubles in-plane angles; out of plane the factor
// is 2 v.h). Stretching the cross-plane axis by 1 / cos(theta_d) makes it isotropic with radial CDF
// F(rho) = rho^2 / (w^2 + rho^2), w = 2 alpha, and turns the disk (tangent-plane circle of radius theta_s) into an
// ellipse. The mass inside is (1 / 2 pi) int [F(exit) - F(entry)] dphi over rays from the lobe centre; when the centre
// is outside, only the rays that meet the ellipse contribute and phi spans just the ellipse's tangent cone. The pixel's
// angular footprint widens w in quadrature (p / 2), so a mirror's reflected disk edge is filtered by the pixel box.
float shSunLobeFraction(float3 n, float3 v, float NoV, float3 l0, float alpha, float pixelAngle)
{
    const float3 r = reflect(-v, n);
    const float3 eIn = normalize(n - r * dot(r, n) + 1e-7 * v);
    const float3 eOut = cross(r, eIn);
    const float thetaS = g_sunAngularRadius;
    const float cosD = max(NoV, 1e-3);
    const float w2 = 4 * alpha * alpha + 0.25 * pixelAngle * pixelAngle;
    // Disk centre in the tangent plane at r (gnomonic; l0 . r > 0 here), ellipse in the isotropic frame.
    const float lr = dot(l0, r);
    if (lr <= 0) return 0;
    const float2 c = float2(dot(l0, eIn), dot(l0, eOut) / cosD) / lr;
    const float2 ab = float2(thetaS, thetaS / cosD);
    const float2 inv2 = 1 / (ab * ab);
    const float C = dot(c * c, inv2) - 1;
    float phi0, span;
    if (C < 0)
    {
        phi0 = 0;
        span = 2 * SH_PI;
    }
    else
    {
        // Tangent cone of the ellipse seen from the origin: in the frame scaled to the unit circle the cone half-angle
        // is asin(1 / |c'|); map its two edge directions back.
        const float2 cs = c / ab;
        const float dist = length(cs);
        const float half = asin(min(1 / dist, 1.0));
        const float base = atan2(cs.y, cs.x);
        float2 d0 = float2(cos(base - half), sin(base - half)) * ab, d1 = float2(cos(base + half), sin(base + half)) * ab;
        phi0 = atan2(d0.y, d0.x);
        span = atan2(d1.y, d1.x) - phi0;
        if (span < 0) span += 2 * SH_PI;
    }
    float mass = 0;
    [loop] for (uint k = 0; k < 32; ++k)
    {
        float sp, cp;
        sincos(phi0 + (k + 0.5) * span / 32, sp, cp);
        const float2 dir = float2(cp, sp);
        const float A = dot(dir * dir, inv2), B = -2 * dot(dir * c, inv2);
        const float disc = B * B - 4 * A * C;
        if (disc <= 0) continue;
        const float q = sqrt(disc);
        const float r0 = max((-B - q) / (2 * A), 0.0), r1 = (-B + q) / (2 * A);
        if (r1 <= r0) continue;
        mass += r1 * r1 / (w2 + r1 * r1) - r0 * r0 / (w2 + r0 * r0);
    }
    return mass * span / (32 * 2 * SH_PI);
}

float3 shSunSpecular(float3 f0, float roughness, float alpha, float3 compensation, float3 n, float3 v, float NoV, float3 l0, float3 E,
                     float pixelAngle)
{
    const float sinS = sin(g_sunAngularRadius), cosS = cos(g_sunAngularRadius), thetaS = g_sunAngularRadius;
    const float3 LOmega = E * (2 / (1 + cosS));  // L_sun * solid angle of the cap
    const float NoL0 = dot(n, l0);
    if (NoL0 <= -sinS) return 0;
    // Terminator band (the disk crosses the shading normal's horizon): the clipped cosine has a kink inside the disk, which
    // only the per-point quadrature follows; the band is < 1 px wide on curved surfaces, so its cost is negligible.
    const bool terminator = NoL0 < 2 * sinS;
    if (!terminator && alpha >= 16 * thetaS) return shSpecular(f0, alpha, compensation, n, v, l0, NoV, NoL0) * NoL0 * LOmega;
    if (!terminator && alpha >= 2 * thetaS) return shSunSpecular4(f0, alpha, compensation, n, v, l0, NoV, cosS) * LOmega;
    if (terminator && alpha >= 2 * thetaS) return shSunSpecularQuadrature(f0, alpha, compensation, n, v, l0, NoV, sinS, cosS) * LOmega;
    return shSpecularAlbedo(f0, NoV, roughness) * shSunLobeFraction(n, v, NoV, l0, alpha, pixelAngle) * E / (SH_PI * sinS * sinS);
}


// A9 clearcoat, the coat lobe's sun term: shSunSpecular with f0 = 1 and compensation 1 at the coat's roughness (the lobe's
// D V cos by the same disk rules) times this weight - the exact dielectric Fresnel and A2's energy scale at the disk centre
// (they move by < 0.3 deg over it), or for a lobe narrower than the disk (shSunSpecular's lobe fraction of the
// single-scattering albedo E(n.v)) the A2 lobe's albedo E_ms(n.v) over E(n.v). pointRule: the caller evaluated the lobe at
// the disk centre (shading experiment bit 1).
float shCoatSunWeight(ModelCoat c, float3 v, float3 l0, float NoV, float NoL0, bool pointRule)
{
    if (!pointRule && modelAlpha(c.roughness) < 2 * g_sunAngularRadius) return modelCoatEms(c, NoV) / modelDirectionalAlbedo(NoV, c.roughness);
    const uint tb = c.coat * MODEL_COAT_STRIDE;
    const float muL = max(NoL0, 1e-4), ecv = modelCoatLookup2(tb, NoV, c.roughness), ecl = modelCoatLookup2(tb, muL, c.roughness);
    const float scale = ecv > 0 && ecl > 0 ? sqrt(modelCoatEms(c, NoV) * modelCoatEms(c, muL) / (ecv * ecl)) : 1.0;
    return modelFresnelDielectric(saturate(dot(v, normalize(v + l0))), c.eta) * scale;
}

// Area of the unit pixel square on the inner side of a straight edge: unit normal nrm (pixel space, pointing inside),
// signed distance d of the pixel centre from the edge (positive inside). Exact for a half-plane.
float shHalfPlaneCoverage(float2 nrm, float d)
{
    const float a = max(abs(nrm.x), abs(nrm.y)), b = min(abs(nrm.x), abs(nrm.y));
    const float t = d + 0.5 * (a + b);  // distance of the edge from the square's outermost corner
    if (t <= 0) return 0;
    if (t >= a + b) return 1;
    if (b < 1e-6) return saturate(t / a);
    if (t <= b) return t * t / (2 * a * b);
    if (t <= a) return (t - 0.5 * b) / a;
    const float u = a + b - t;
    return 1 - u * u / (2 * a * b);
}

// asin for small arguments by its series (the GPU's asin approximation errs by ~1e-6 absolute, 5e-4 of the sun's
// angular radius); t < 0.1: truncation below 1e-11.
float shAsinSmall(float t)
{
    if (t >= 0.1) return asin(min(t, 1.0));
    const float t2 = t * t;
    return t * (1 + t2 * (1.0 / 6 + t2 * (3.0 / 40 + t2 * (5.0 / 112))));
}

// Fraction of a sky pixel covered by the solar disk: the disk edge as a straight line through the pixel (its radius,
// 9 px at 4K, is large against the pixel), placed by the exact angular distance and oriented by its screen gradient.
// Angles from the chord |dir - l0| (acos loses 1e-5 rad near 1 in float).
float shSunDiskCoverage(float3 D, float3 Dx, float3 Dy)
{
    const float3 l0 = normalize(g_sunDirection);
    const float invLen = rsqrt(dot(D, D));
    const float3 dir = D * invLen;
    const float3 c = dir - l0;
    const float chord = length(c);
    const float angle = 2 * shAsinSmall(0.5 * chord);
    const float margin = 2 * length(Dx) * invLen;
    if (angle > g_sunAngularRadius + margin) return 0;
    if (angle < g_sunAngularRadius - margin || chord < 1e-7) return 1;
    // d angle / d pixel: d dir = (dD - dir (dir . dD)) / |D|; d angle = (c . d dir) / (chord cos(angle / 2)).
    const float3 dirX = (Dx - dir * dot(dir, Dx)) * invLen, dirY = (Dy - dir * dot(dir, Dy)) * invLen;
    const float k = 1 / (chord * cos(0.5 * angle));
    const float2 g = float2(dot(c, dirX), dot(c, dirY)) * k;
    const float gl = length(g);
    return shHalfPlaneCoverage(-g / gl, (g_sunAngularRadius - angle) / gl);
}

// ---------------------------------------------------------------- local lights (INTERFACES 8.2)
// Illuminance (lux x colour) at a surface facing a punctual light: I / d^2 w(d), spots x saturate(cos spotScale +
// spotOffset)^2 on the angle from the light's axis; 'toLight' = light position - surface point (camera-relative
// difference), 'l' = unit direction to the light.
float3 shPunctualIlluminance(GpuLight light, float3 toLight, out float3 l)
{
    const float d2 = dot(toLight, toLight);
    const float d = sqrt(d2);
    l = toLight / max(d, 1e-9);
    const float x = d / max(light.range, 1e-6), x2 = x * x;
    const float w = saturate(1 - x2 * x2);
    float i = light.intensity * w * w / max(d2, 1e-12);
    if (lightType(light) == LIGHT_SPOT)
    {
        const float sp = saturate(dot(-l, light.forward) * light.spotScale + light.spotOffset);
        i *= sp * sp;
    }
    return light.color * i;
}

// ---------------------------------------------------------------- output (INTERFACES 7.5, 8.4)
// Khronos PBR Neutral (reference implementation constants), input linear Rec.709 radiance x exposure.
float3 shPbrNeutral(float3 color)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    const float peak = max(color.r, max(color.g, color.b));
    if (peak < startCompression) return color;
    const float d = 1 - startCompression;
    const float newPeak = 1 - d * d / (peak + d - startCompression);
    color *= newPeak / peak;
    const float g = 1 - 1 / (desaturation * (peak - newPeak) + 1);
    return lerp(color, newPeak.xxx, g);
}

// PBR Neutral generalised to an HDR display whose peak is 'peak' x paper white (the input and output in paper-white
// units): the same toe (absolute, below 0.08), the shoulder starting at 0.8 peak - 0.04 and reaching the peak
// asymptotically, desaturation by the compression relative to the peak. At peak 1 every operation is shPbrNeutral's.
float3 shPbrNeutralPeak(float3 color, float peak)
{
    const float startCompression = 0.8 * peak - 0.04;
    const float desaturation = 0.15;
    const float x = min(color.r, min(color.g, color.b));
    const float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;
    const float top = max(color.r, max(color.g, color.b));
    if (top < startCompression) return color;
    const float d = peak - startCompression;
    const float newPeak = peak - d * d / (top + d - startCompression);
    color *= newPeak / top;
    const float g = 1 - 1 / (desaturation * (top - newPeak) / peak + 1);
    return lerp(color, newPeak.xxx, g);
}

// Film curve (the default display rendering since U2): a filmic toe / straight / shoulder in log10 exposure, per channel
// in ACEScg (AP1) primaries between the ACES pre- and post-desaturation (0.96, 0.93), with the parameters games of the
// Unreal family ship by default (slope 0.88, toe 0.55, shoulder 0.26, black clip 0, white clip 0.04): scene grey 0.18
// stays 0.18, contrast 0.88 per decade around it, a smooth toe to black and a shoulder that rolls highlights off towards
// white (the per-channel shoulder desaturates them: the path to white of film). Input linear Rec.709 x exposure, output
// linear Rec.709 display light (1 = paper white). An HDR display (peak x paper white): below the knee 0.8 per channel
// the SDR image exactly; above it the SDR shoulder's range [0.8, 1.04) is expanded monotonically onto [0.8, 1.04 peak)
// by E(u) = u / (1 - u (1 - 1 / r)) (slope 1 at the knee, r = the ranges' ratio; the identity at peak 1).
float3 shFilm(float3 color, float peak)
{
    const float3x3 toAp1 = float3x3(0.6130973, 0.3395229, 0.0473793, 0.0701942, 0.9163556, 0.0134526, 0.0206156, 0.1095698, 0.8698151);
    const float3x3 toSrgb = float3x3(1.7050510, -0.6217921, -0.0832589, -0.1302564, 1.1408047, -0.0105483, -0.0240033, -0.1289690, 1.1529723);
    const float3 ap1Y = float3(0.2722287, 0.6740818, 0.0536895);
    const float slope = 0.88, toe = 0.55, shoulder = 0.26, blackClip = 0.0, whiteClip = 0.04;
    float3 a = mul(toAp1, color);
    a = max(lerp(dot(a, ap1Y).xxx, a, 0.96), 0.0);
    const float toeScale = 1 + blackClip - toe, shoulderScale = 1 + whiteClip - shoulder;
    const float bt = (0.18 + blackClip) / toeScale - 1;
    const float toeMatch = log10(0.18) - 0.5 * log((1 + bt) / (1 - bt)) * (toeScale / slope);
    const float straightMatch = (1 - toe) / slope - toeMatch;
    const float shoulderMatch = shoulder / slope - straightMatch;
    const float3 l = log10(max(a, 1e-10));
    const float3 straight = slope * (l + straightMatch);
    float3 toeColor = -blackClip + 2 * toeScale / (1 + exp((-2 * slope / toeScale) * (l - toeMatch)));
    float3 shoulderColor = (1 + whiteClip) - 2 * shoulderScale / (1 + exp((2 * slope / shoulderScale) * (l - shoulderMatch)));
    toeColor = select(l < toeMatch, toeColor, straight);
    shoulderColor = select(l > shoulderMatch, shoulderColor, straight);
    float3 t = saturate((l - toeMatch) / (shoulderMatch - toeMatch));
    t = shoulderMatch < toeMatch ? 1 - t : t;
    t = (3 - 2 * t) * t * t;
    a = lerp(toeColor, shoulderColor, t);
    a = max(lerp(dot(a, ap1Y).xxx, a, 0.93), 0.0);
    float3 d = max(mul(toSrgb, a), 0.0);
    if (peak > 1)
    {
        const float knee = 0.8, top = 1 + whiteClip, r = (top * peak - knee) / (top - knee);
        const float3 u = min(max(d - knee, 0.0) / (top - knee), 0.999999);
        d = select(d > knee, knee + (top - knee) * (u / (1 - u * (1 - 1 / r))), d);
    }
    return d;
}

// Unpolarised dielectric Fresnel reflectance (the mean of the s and p reflectances) for light meeting the interface at
// cos(theta_i) = cosI from the medium n1 into n2, eta = n1 / n2; 1 under total internal reflection. The glass class's F
// (INTERFACES 8.1, FEATURES_GAME 14.1) and the same equations as W's waterFresnel.
float shDielectricFresnel(float cosI, float eta)
{
    cosI = saturate(cosI);
    const float sin2T = eta * eta * (1.0 - cosI * cosI);
    if (sin2T >= 1.0) return 1.0;
    const float cosT = sqrt(1.0 - sin2T);
    const float rs = (eta * cosI - cosT) / (eta * cosI + cosT), rp = (eta * cosT - cosI) / (eta * cosT + cosI);
    return 0.5 * (rs * rs + rp * rp);
}

float shSrgbOetf(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

// Display: sRGB OETF of the tone-mapped value; linear outputs (validation, secondary views): radiance x exposure.
float4 shEncodeExposed(float3 e)
{
#if OUTPUT == 0
    const float3 t = saturate(shFilm(max(e, 0.0), 1.0));
    return float4(shSrgbOetf(t.r), shSrgbOetf(t.g), shSrgbOetf(t.b), 1);
#else
    return float4(e, 1);
#endif
}
float4 shEncodeOutput(float3 radiance) { return shEncodeExposed(radiance * g_exposure); }

// Automatic exposure (FEATURES_GAME 6.2; Exposure.cpp): the pixel's luminance (nit, before the exposure) into M's 64-bin
// log2 histogram from 2^-8 nit in half stops, weighted by a Gaussian of the distance from the view's centre (in units of
// half the view height, sigma 'centreSigma') in 1/64 steps. One atomic per distinct bin of the wave (at most 64 rounds).
// histogramUav UNX_NONE: not metered (secondary views).
void shExposureHistogram(uint histogramUav, float3 radiance, uint2 pixel, float centreSigma)
{
    if (histogramUav == UNX_NONE) return;
    const float y = dot(max(radiance, 0.0), float3(0.2126, 0.7152, 0.0722));
    const uint bin = y > 0 ? (uint)clamp((log2(y) + 8.0) * 2.0, 0.0, 63.0) : 0u;
    const float2 d = (float2(pixel) + 0.5 - 0.5 * float2(g_viewWidth, g_viewHeight)) / (0.5 * g_viewHeight);
    const uint weight = (uint)(64.0 * exp(-0.5 * dot(d, d) / (centreSigma * centreSigma)) + 0.5);
    if (weight == 0) return;
    RWByteAddressBuffer h = ResourceDescriptorHeap[histogramUav];
    [loop] for (uint round = 0; round < 64u; ++round)
    {
        const uint b = WaveReadLaneFirst(bin);
        if (bin == b)
        {
            const uint sum = WaveActiveSum(weight);
            if (WaveIsFirstLane()) h.InterlockedAdd(4u * b, sum);
            break;
        }
    }
}

// COVERAGE 12.4 structure 2 (B2): the light's specular is in R's reflection paths (FrameResources::areaLightStable, 1 bit
// per light; UNX_NONE: R's emitters are off and M evaluates every area light's LTC specular).
bool shSpecularInReflections(uint maskSrv, uint lightIndex)
{
    if (maskSrv == UNX_NONE) return false;
    ByteAddressBuffer mask = ResourceDescriptorHeap[maskSrv];
    return ((mask.Load(4u * (lightIndex >> 5)) >> (lightIndex & 31u)) & 1u) != 0;
}

// Particle layer composite (FX's ParticleLayer.hlsli; request 20260926_FX_particle_render_pass 4, FEATURES_GAME 0.A 6), in
// exposed radiance before the tone map: C = C_surface x T + L, where the layer's L already carries the exposure and the air
// between the camera and each particle, and C_surface the air in front of the surface. Each output writer calls it once on
// its final value from its linear sources, so a pixel written twice (a shading kernel, then an edge or coverage composite)
// still takes the particles once. Invalid indices (UNX_NONE): no particle layer in this view.
float3 shParticles(float3 exposed, uint2 pixel, uint layerSrv, uint edgesSrv)
{
    if (layerSrv == UNX_NONE) return exposed;
    Texture2D<float4> layer = ResourceDescriptorHeap[layerSrv];
    ByteAddressBuffer edges = ResourceDescriptorHeap[edgesSrv];
    const float4 lt = fxParticleLayerAt(layer, edges, pixel);
    return exposed * lt.a + lt.rgb;
}

// Angular size of one pixel along the view ray (radians): |dD/dx| / |D| for the ray direction D of mPixelRay.
float shPixelAngle(float3 D, float3 Dx) { return length(Dx) / length(D); }

#endif
