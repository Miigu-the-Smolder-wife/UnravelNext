// M shading helpers (ARCHITECTURE 2.11; INTERFACES 7.5, 8). Owner: M.
#ifndef UNX_M_SHADING_COMMON_HLSLI
#define UNX_M_SHADING_COMMON_HLSLI
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "MaterialModel.hlsli"

#define SH_PI 3.14159265358979

// ---------------------------------------------------------------- specular directional albedo
// Single-scattering specular albedo with Schlick F split by f0 (ShadingSystem.cpp builds it with the same visible-normal
// samples as the model's E table): E_ss(f0) = f0 A + B, A + B = E. With the model's multiple-scattering compensation
// the lobe's full albedo is (f0 A + B)(1 + f0 (1/E - 1)) (INTERFACES 8.1).
float2 shSpecularAB(uint lutSrv, float NoV, float roughness)
{
    StructuredBuffer<float2> t = ResourceDescriptorHeap[lutSrv];
    const float last = MODEL_ALBEDO_TABLE_SIZE - 1;
    const float x = saturate(NoV) * last, y = saturate(roughness) * last;
    const uint x0 = uint(x), y0 = uint(y);
    const uint x1 = min(x0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1), y1 = min(y0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1);
    const float fx = x - x0, fy = y - y0;
    const float2 a = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x0], b = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x1];
    const float2 c = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x0], d = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x1];
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

float3 shSpecularAlbedo(uint lutSrv, float3 f0, float NoV, float roughness)
{
    const float2 ab = shSpecularAB(lutSrv, NoV, roughness);
    const float e = ab.x + ab.y;
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
// 8.3), E = illuminance on a surface facing the sun (transmittance included). Three regimes by the lobe width alpha:
//   alpha >= 16 theta_s   the lobe is flat over the disk: point evaluation at the centre, relative error ~ (theta_s /
//                         alpha)^2 / 2 (0.64 % measured at 8.6 theta_s), times L_sun Omega = E * 2 / (1 + cos theta_s);
//   theta_s / 2 .. 16     polar product quadrature over the cap (4 Gauss-Legendre radii in area x 12 angles; the
//                         integrand is smooth in angle, so the trapezoid rule converges geometrically);
//   alpha < theta_s / 2   narrow lobe: the lobe's full albedo times the fraction of its reflected directions inside
//                         the disk (shSunLobeFraction).
// The two lower regimes blend linearly for alpha in [theta_s / 3, theta_s / 2].
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
        [unroll] for (uint k = 0; k < 12; ++k)
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

float3 shSunSpecular(uint lutSrv, float3 f0, float roughness, float alpha, float3 compensation, float3 n, float3 v, float NoV, float3 l0, float3 E,
                     float pixelAngle)
{
    const float sinS = sin(g_sunAngularRadius), cosS = cos(g_sunAngularRadius), thetaS = g_sunAngularRadius;
    const float3 LOmega = E * (2 / (1 + cosS));  // L_sun * solid angle of the cap
    if (alpha >= 16 * thetaS)
    {
        const float NoL = dot(n, l0);
        return NoL > 0 ? shSpecular(f0, alpha, compensation, n, v, l0, NoV, NoL) * NoL * LOmega : 0;
    }
    float3 quadrature = 0, mirror = 0;
    if (alpha >= thetaS / 3) quadrature = shSunSpecularQuadrature(f0, alpha, compensation, n, v, l0, NoV, sinS, cosS) * LOmega;
    if (alpha < thetaS / 2)
        mirror = shSpecularAlbedo(lutSrv, f0, NoV, roughness) * shSunLobeFraction(n, v, NoV, l0, alpha, pixelAngle) * E / (SH_PI * sinS * sinS);
    if (alpha >= thetaS / 2) return quadrature;
    if (alpha < thetaS / 3) return mirror;
    return lerp(mirror, quadrature, (alpha - thetaS / 3) * 6 / thetaS);
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

float shSrgbOetf(float c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }

// Display: sRGB OETF of the tone-mapped value; linear outputs (validation, secondary views): radiance x exposure.
float4 shEncodeOutput(float3 radiance)
{
    const float3 e = radiance * g_exposure;
#if OUTPUT == 0
    const float3 t = saturate(shPbrNeutral(max(e, 0.0)));
    return float4(shSrgbOetf(t.r), shSrgbOetf(t.g), shSrgbOetf(t.b), 1);
#else
    return float4(e, 1);
#endif
}

// Angular size of one pixel along the view ray (radians): |dD/dx| / |D| for the ray direction D of mPixelRay.
float shPixelAngle(float3 D, float3 Dx) { return length(Dx) / length(D); }

#endif
