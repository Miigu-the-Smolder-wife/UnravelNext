// Material model v1 (INTERFACES_KO.md 8.1). HLSL mirror of unx::scene::model (Native/Scene/src/MaterialModel.cpp),
// which is authoritative; both must agree to float rounding. Owner: M.
#ifndef UNX_MATERIAL_MODEL_HLSLI
#define UNX_MATERIAL_MODEL_HLSLI
#include "Scene.hlsli"

#define MODEL_PI 3.14159265358979323846
#define MODEL_MIN_ALPHA 1e-4
#define MODEL_ALBEDO_TABLE_SIZE 32

struct ModelSurface
{
    uint cls;
    float3 baseColor;
    float roughness;
    float metallic;
    float specular;
    float transmission;
};

float modelAlpha(float roughness) { return max(roughness * roughness, MODEL_MIN_ALPHA); }
float3 modelF0(ModelSurface s) { return lerp((0.08 * s.specular).xxx, s.baseColor, s.metallic); }

// sinSqNH = |n x h|^2: the direct form avoids NoH^2 (a^2 - 1) + 1 cancelling to 0 at a = 1e-4 (mirror).
float modelD(float NoH, float sinSqNH, float alpha)
{
    const float a2 = alpha * alpha;
    const float t = sinSqNH + a2 * NoH * NoH;
    return a2 / (MODEL_PI * t * t);
}

float modelV(float NoV, float NoL, float alpha)
{
    const float a2 = alpha * alpha;
    const float gv = NoL * sqrt(NoV * NoV * (1 - a2) + a2);
    const float gl = NoV * sqrt(NoL * NoL * (1 - a2) + a2);
    return 0.5 / (gv + gl);
}

float3 modelFresnel(float3 f0, float VoH) { return f0 + (1 - f0) * pow(1 - saturate(VoH), 5.0); }

// Bilinear on the end-point-inclusive grid: identical addressing to directionalAlbedo() in C++.
float modelDirectionalAlbedo(float NoV, float roughness)
{
    StructuredBuffer<float> t = ResourceDescriptorHeap[g_materialModelLut];
    const float last = MODEL_ALBEDO_TABLE_SIZE - 1;
    const float x = saturate(NoV) * last, y = saturate(roughness) * last;
    const uint x0 = uint(x), y0 = uint(y);
    const uint x1 = min(x0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1), y1 = min(y0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1);
    const float fx = x - x0, fy = y - y0;
    const float a = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x0], b = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x1];
    const float c = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x0], d = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x1];
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

// Split specular directional albedo (A, B): f0 A + B = the lobe's albedo with Schlick Fresnel (specularAlbedo() in C++,
// same addressing as modelDirectionalAlbedo).
float2 modelSpecularAlbedo(float NoV, float roughness)
{
    StructuredBuffer<float2> t = ResourceDescriptorHeap[g_specularAlbedoLut];
    const float last = MODEL_ALBEDO_TABLE_SIZE - 1;
    const float x = saturate(NoV) * last, y = saturate(roughness) * last;
    const uint x0 = uint(x), y0 = uint(y);
    const uint x1 = min(x0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1), y1 = min(y0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1);
    const float fx = x - x0, fy = y - y0;
    const float2 a = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x0], b = t[y0 * MODEL_ALBEDO_TABLE_SIZE + x1];
    const float2 c = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x0], d = t[y1 * MODEL_ALBEDO_TABLE_SIZE + x1];
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

float3 modelEvaluate(ModelSurface s, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    const float3 albedo = s.baseColor * ((1 - s.metallic) / MODEL_PI);
    if (s.cls == MATERIAL_FOLIAGE && NoV * NoL < 0) return albedo * s.transmission;
    if (NoV <= 0 || NoL <= 0) return 0;
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), VoH = saturate(dot(v, h));
    const float alpha = modelAlpha(s.roughness);
    const float3 f0 = modelF0(s);
    const float3 nxh = cross(n, h);
    const float3 single = modelFresnel(f0, VoH) * (modelD(NoH, dot(nxh, nxh), alpha) * modelV(NoV, NoL, alpha));
    const float e = modelDirectionalAlbedo(NoV, s.roughness);
    const float3 compensation = 1 + f0 * (1 / e - 1);
    const float3 diffuse = s.cls == MATERIAL_FOLIAGE ? albedo * (1 - s.transmission) : albedo;
    return diffuse + single * compensation;
}

// ---- A9 clearcoat (MaterialModel.h evaluateCoated, v1.76): mirror of the C++ definition; tables in g_coatTable.
struct ModelCoat
{
    float cover;       // c
    float roughness;   // r_c
    uint coat;         // tabulated coat (0 eta 1.5, 1 eta 1.33)
    float eta;
};
#define MODEL_COAT_STRIDE 4224u

float modelCoatLookup2(uint base, float mu, float r)
{
    StructuredBuffer<float> t = ResourceDescriptorHeap[g_coatTable];
    const float last = MODEL_ALBEDO_TABLE_SIZE - 1;
    const float x = saturate(mu) * last, y = saturate(r) * last;
    const uint x0 = uint(x), y0 = uint(y);
    const uint x1 = min(x0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1), y1 = min(y0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1);
    const float fx = x - x0, fy = y - y0;
    return (t[base + y0 * MODEL_ALBEDO_TABLE_SIZE + x0] * (1 - fx) + t[base + y0 * MODEL_ALBEDO_TABLE_SIZE + x1] * fx) * (1 - fy) +
           (t[base + y1 * MODEL_ALBEDO_TABLE_SIZE + x0] * (1 - fx) + t[base + y1 * MODEL_ALBEDO_TABLE_SIZE + x1] * fx) * fy;
}
float modelCoatLookup1(uint base, float r)
{
    StructuredBuffer<float> t = ResourceDescriptorHeap[g_coatTable];
    const float y = saturate(r) * (MODEL_ALBEDO_TABLE_SIZE - 1);
    const uint y0 = uint(y), y1 = min(y0 + 1, MODEL_ALBEDO_TABLE_SIZE - 1);
    return t[base + y0] + (y - y0) * (t[base + y1] - t[base + y0]);
}

// ---- A9 sheen (MaterialModel.h evaluateSheen, MATERIAL_LAYERS 1.4): mirror of the C++ definition; its table follows the
// two coats' in g_coatTable (A, then E_sh: 64 columns at sqrt(mu), 32 rows at sqrt((r - 0.1) / 0.9)).
#define MODEL_SHEEN_TABLE (2u * MODEL_COAT_STRIDE)
#define MODEL_SHEEN_MU 64u
#define MODEL_SHEEN_R 32u
struct ModelSheen
{
    float3 color;      // C (0: none)
    float roughness;   // r_sh
};
float modelSheenLookup(uint base, float mu, float r)
{
    StructuredBuffer<float> t = ResourceDescriptorHeap[g_coatTable];
    const float x = sqrt(saturate(mu)) * (MODEL_SHEEN_MU - 1), y = sqrt(saturate((r - 0.1) / 0.9)) * (MODEL_SHEEN_R - 1);
    const uint x0 = min(uint(x), MODEL_SHEEN_MU - 2), y0 = min(uint(y), MODEL_SHEEN_R - 2);
    const float fx = x - x0, fy = y - y0;
    const uint b = base + y0 * MODEL_SHEEN_MU + x0;
    return (t[b] * (1 - fx) + t[b + 1] * fx) * (1 - fy) + (t[b + MODEL_SHEEN_MU] * (1 - fx) + t[b + MODEL_SHEEN_MU + 1] * fx) * fy;
}
float modelSheenArea(float mu, float r) { return modelSheenLookup(MODEL_SHEEN_TABLE, mu, r); }
float modelSheenAlbedo(float mu, float r) { return modelSheenLookup(MODEL_SHEEN_TABLE + MODEL_SHEEN_MU * MODEL_SHEEN_R, mu, r); }
// D G2 / (4 n.v n.l) with C = 1 (Charlie D, Smith masking from the tabulated projected area).
float modelSheenLobe(float r, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return 0;
    const float alpha = modelAlpha(r), inv = 1 / alpha, NoH = saturate(dot(n, normalize(v + l)));
    const float D = (2 + inv) * pow(max(0.0, 1 - NoH * NoH), 0.5 * inv) / (2 * MODEL_PI);
    const float G2 = 1 / max(modelSheenArea(NoV, r) / NoV + modelSheenArea(NoL, r) / NoL - 1, 1.0);
    return D * G2 / (4 * NoV * NoL);
}
// The lobe times the clipped cosine averaged over a sun disk of angular radius rho centred on l0 (MATERIAL_LAYERS 1.4):
//   disk above the horizon: the 4-point rule (l0 +- rho / sqrt(2) along both disk axes: exact to second order);
//   disk straddling it: the visible circular segment by Gauss-Legendre - 4 points in phi (t = rho sin phi along the
//   direction of increasing n.l, which absorbs the segment's square-root edge) x 3 across (the terminator band only);
//   below it: 0.
float modelSheenSun(float r, float3 n, float3 v, float3 l0, float rho)
{
    const float3 du = normalize(cross(abs(l0.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0), l0)), dw = cross(l0, du);
    const float NoL0 = dot(n, l0), gu = dot(n, du), gw = dot(n, dw), k = sqrt(gu * gu + gw * gw);
    if (NoL0 <= -k * rho) return 0;
    float sum = 0;
    if (NoL0 >= k * rho)
    {
        const float q = rho * 0.70710678;
        [unroll] for (uint i = 0; i < 4; ++i)
        {
            const float3 l = normalize(l0 + ((i & 2u) ? dw : du) * ((i & 1u) ? -q : q));
            sum += 0.25 * modelSheenLobe(r, n, v, l) * max(dot(n, l), 0.0);
        }
        return sum;
    }
    const float2 e = float2(gu, gw) / max(k, 1e-8), ep = float2(-e.y, e.x);
    const float phi0 = asin(clamp(-NoL0 / (k * rho), -1.0, 1.0)), half = 0.5 * (0.5 * MODEL_PI - phi0), mid = 0.5 * (0.5 * MODEL_PI + phi0);
    const float x4[4] = { -0.86113631, -0.33998104, 0.33998104, 0.86113631 }, w4[4] = { 0.34785485, 0.65214515, 0.65214515, 0.34785485 };
    const float x3[3] = { -0.77459667, 0, 0.77459667 }, w3[3] = { 0.55555556, 0.88888889, 0.55555556 };
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const float phi = mid + half * x4[i], t = rho * sin(phi), w = rho * cos(phi);
        [unroll] for (uint j = 0; j < 3; ++j)
        {
            const float2 o = e * t + ep * (w * x3[j]);
            const float3 l = normalize(l0 + du * o.x + dw * o.y);
            sum += w4[i] * w3[j] * modelSheenLobe(r, n, v, l) * max(dot(n, l), 0.0) * w * w;  // (dt = w dphi, ds = w dx)
        }
    }
    return sum * half / (MODEL_PI * rho * rho);
}
// The base's scale 1 - max(C) E_sh(n.v) (view side only).
float modelSheenKeep(ModelSheen sh, float NoV) { return 1 - max(sh.color.r, max(sh.color.g, sh.color.b)) * modelSheenAlbedo(max(NoV, 1e-4), sh.roughness); }
float3 modelEvaluateSheen(ModelSurface s, ModelSheen sh, float3 n, float3 v, float3 l)
{
    const float3 base = modelEvaluate(s, n, v, l);
    if (!(max(sh.color.r, max(sh.color.g, sh.color.b)) > 0)) return base;
    if (dot(n, v) <= 0 || dot(n, l) <= 0) return base;
    return sh.color * modelSheenLobe(sh.roughness, n, v, l) + base * modelSheenKeep(sh, dot(n, v));
}
// The sheen of a material (MATERIAL_SHEEN; none: colour 0).
ModelSheen modelSheenOf(GpuMaterial m)
{
    ModelSheen sh = (ModelSheen)0;
    sh.roughness = 0.5;
    if ((m.classFlags & (MATERIAL_LAYERED | MATERIAL_SHEEN)) != (MATERIAL_LAYERED | MATERIAL_SHEEN)) return sh;
    const GpuMaterialLayers layers = loadMaterialLayers(m.classFlags >> 16);
    sh.color = layers.sheenColor;
    sh.roughness = layers.sheenRoughness;
    return sh;
}

// exact unpolarised dielectric Fresnel; eta = n_t / n_i; 1 past the critical angle
float modelFresnelDielectric(float cosI, float eta)
{
    const float c = saturate(cosI);
    const float s2 = (1 - c * c) / (eta * eta);
    if (s2 >= 1) return 1;
    const float ct = sqrt(1 - s2);
    const float rs = (c - eta * ct) / (c + eta * ct), rp = (eta * c - ct) / (eta * c + ct);
    return 0.5 * (rs * rs + rp * rp);
}

float modelCoatEms(ModelCoat c, float mu) { return modelCoatLookup2(c.coat * MODEL_COAT_STRIDE + 1024, mu, c.roughness); }

// f_c: the coat's reflection (A2), without the cover.
float modelCoatLobe(ModelCoat c, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return 0;
    const uint t = c.coat * MODEL_COAT_STRIDE;
    const float ac = modelAlpha(c.roughness);
    const float ecv = modelCoatLookup2(t, NoV, c.roughness), ecl = modelCoatLookup2(t, NoL, c.roughness);
    const float emv = modelCoatLookup2(t + 1024, NoV, c.roughness), eml = modelCoatLookup2(t + 1024, NoL, c.roughness);
    const float3 h = normalize(v + l);
    const float NoH = saturate(dot(n, h)), VoH = saturate(dot(v, h));
    const float3 nxh = cross(n, h);
    const float scale = ecv > 0 && ecl > 0 ? sqrt(emv * eml / (ecv * ecl)) : 1.0;
    return modelD(NoH, dot(nxh, nxh), ac) * modelV(NoV, NoL, ac) * modelFresnelDielectric(VoH, c.eta) * scale;
}

// The light returned by the coat's inside per unit transmitted irradiance, per channel: (a - e)(mu'_l) rho / (1 - rho K_ms)
// (the f_ms factor without T(mu_v) T(mu_l) / (pi eta^2)); muIn = mu'_l.
float3 modelCoatReturned(ModelSurface s, ModelCoat c, float muIn)
{
    const uint t = c.coat * MODEL_COAT_STRIDE;
    const float3 F = modelF0(s), rd = s.baseColor * (1 - s.metallic);
    const float2 abl = modelSpecularAlbedo(muIn, s.roughness);
    const float axl = modelCoatLookup2(t + 2048, muIn, s.roughness), bxl = modelCoatLookup2(t + 3072, muIn, s.roughness);
    const float3 comp = 1 + F * (1 / modelDirectionalAlbedo(muIn, s.roughness) - 1);
    StructuredBuffer<float> table = ResourceDescriptorHeap[g_coatTable];
    const float3 returned = rd * table[t + 4192] + (F * (abl.x - axl) + (abl.y - bxl)) * comp;
    const float abar = modelCoatLookup1(t + 4128, s.roughness), bbar = modelCoatLookup1(t + 4160, s.roughness);
    const float3 rho = rd + (F * abar + bbar) * (1 + F * (1 / (abar + bbar) - 1));
    const float kms = modelCoatLookup1(t + 4096, c.roughness);
    return returned * rho / (1 - rho * kms);
}

float modelCoatRefractedCos(float mu, float eta) { return sqrt(max(0.0, 1 - (1 - mu * mu) / (eta * eta))); }

// f_1 + f_ms: the base seen through the coat, without the cover.
float3 modelCoatUnder(ModelSurface s, ModelCoat c, float3 n, float3 v, float3 l)
{
    const float NoV = dot(n, v), NoL = dot(n, l);
    if (NoV <= 0 || NoL <= 0) return 0;
    const float eta = c.eta, ac = modelAlpha(c.roughness);
    const float tv = 1 - modelCoatEms(c, NoV), tl = 1 - modelCoatEms(c, NoL);
    const float mv = modelCoatRefractedCos(NoV, eta), ml = modelCoatRefractedCos(NoL, eta);
    const float3 pv = (v - n * NoV) * (1 / eta) + n * mv, pl = (l - n * NoL) * (1 / eta) + n * ml;
    const float sv = 1 - NoV / (eta * mv), sl = 1 - NoL / (eta * ml);
    const float ab = modelAlpha(s.roughness);
    ModelSurface lobe = s;
    lobe.roughness = sqrt(sqrt(ab * ab + 0.25 * (sv * sv + sl * sl) * ac * ac));
    const float3 f1 = modelEvaluate(lobe, n, pv, pl) * (tv * tl / (eta * eta));
    const float3 fms = modelCoatReturned(s, c, ml) * (tl * tv / (MODEL_PI * eta * eta));
    return f1 + fms;
}

float3 modelEvaluateCoated(ModelSurface s, ModelCoat c, float3 n, float3 v, float3 l)
{
    const float3 base = modelEvaluate(s, n, v, l);
    if (!(c.cover > 0)) return base;
    if (dot(n, v) <= 0 || dot(n, l) <= 0) return base * (1 - c.cover);
    return base * (1 - c.cover) + (modelCoatLobe(c, n, v, l) + modelCoatUnder(s, c, n, v, l)) * c.cover;
}

// Outside-equivalent perceptual roughness of the base lobe seen through the coat at the viewer's side (MATERIAL_LAYERS
// 1.1: alpha_eq ~ eta alpha'_b, with the spread of both passes at mu_v), for lobe-shaped integrals (LTC, probe cones).
float modelCoatBaseRoughness(ModelSurface s, ModelCoat c, float NoV)
{
    const float mv = modelCoatRefractedCos(max(NoV, 1e-4), c.eta), sv = 1 - max(NoV, 1e-4) / (c.eta * mv), ac = modelAlpha(c.roughness), ab = modelAlpha(s.roughness);
    return sqrt(min(c.eta * sqrt(ab * ab + 0.5 * sv * sv * ac * ac), 1.0));
}

// The coat of a material (MATERIAL_LAYERED; none: cover 0).
ModelCoat modelCoatOf(GpuMaterial m)
{
    ModelCoat c = (ModelCoat)0;
    c.eta = 1.5;
    if ((m.classFlags & MATERIAL_LAYERED) == 0) return c;
    const GpuMaterialLayers layers = loadMaterialLayers(m.classFlags >> 16);
    c.cover = layers.clearcoat;
    c.roughness = layers.clearcoatRoughness;
    c.coat = layers.coat;
    c.eta = layers.coatEta;
    return c;
}

#endif
