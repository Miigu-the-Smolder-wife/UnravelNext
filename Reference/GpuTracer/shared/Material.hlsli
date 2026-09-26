// Material model v1 (INTERFACES_KO.md 8.1) and its sampling, the shared form of Reference/PathTracer/src/Bsdf.h.
// The CPU estimator evaluates the GGX terms in double; this float form has no cancellation either: |n x h|^2 is
// computed from the cross product (not 1 - (n.h)^2), so D = alpha^2 / (pi (|n x h|^2 + alpha^2 (n.h)^2)^2) stays
// exact to float rounding down to alpha = 1e-4 (mirrors). Lobe probabilities, sampling and the side rules are the
// CPU estimator's line for line. The includer defines rtAlbedoTableFetch(uint i) (scene::model::directionalAlbedoTable,
// 32 x 32, row-major with r along rows).
#ifndef UNX_RT_MATERIAL_HLSLI
#define UNX_RT_MATERIAL_HLSLI
#include "Compat.hlsli"

RT_BEGIN_NAMESPACE
RT_CONST uint kRtClassStandard = 0;
RT_CONST uint kRtClassFoliage = 1;
RT_CONST float kRtMinAlpha = 1e-4f;
RT_CONST uint kRtAlbedoTableSize = 32;

struct RtBsdfParams
{
    uint cls;
    float3 baseColor;
    float roughness;
    float metallic;
    float specular;
    float transmission;
};

// A shading point: position, geometric and shading normals (same side), BSDF parameters, emission.
struct RtSurface
{
    float3 p;
    float3 ng;
    float3 ns;
    bool frontFacing;
    RtBsdfParams bsdf;
    float3 emission;
    uint material;
    // Largest distance from p to the hit triangle's vertices (world; 0 = no triangle): the triangle term of the
    // ray-origin offset (Scene.hlsli rtOffsetRayOrigin).
    float extent;
};

RT_INLINE float rtAlphaFromRoughness(float r) { return max(r * r, kRtMinAlpha); }
RT_INLINE float3 rtF0(RtBsdfParams s)
{
    const float d = 0.08f * s.specular;
    const float3 dd = rtSplat3(d);
    return dd + (s.baseColor - dd) * s.metallic;
}

float rtAlbedoTableFetch(uint i);  // defined by the includer
RT_INLINE float rtDirectionalAlbedo(float NoV, float roughness)
{
    const float last = (float)(kRtAlbedoTableSize - 1);
    const float x = clamp(NoV, 0.0f, 1.0f) * last, y = clamp(roughness, 0.0f, 1.0f) * last;
    const uint x0 = (uint)x, y0 = (uint)y;
    const uint x1 = min(x0 + 1u, kRtAlbedoTableSize - 1u), y1 = min(y0 + 1u, kRtAlbedoTableSize - 1u);
    const float fx = x - (float)x0, fy = y - (float)y0;
    const float a = rtAlbedoTableFetch(y0 * kRtAlbedoTableSize + x0), b = rtAlbedoTableFetch(y0 * kRtAlbedoTableSize + x1);
    const float c = rtAlbedoTableFetch(y1 * kRtAlbedoTableSize + x0), d = rtAlbedoTableFetch(y1 * kRtAlbedoTableSize + x1);
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy;
}

RT_INLINE float rtGgx(float noh, float sin2, float alpha)
{
    const float a2 = alpha * alpha, t = sin2 + a2 * noh * noh;
    return a2 / (kRtPi * t * t);
}
RT_INLINE float rtSmith(float nov, float nol, float alpha)
{
    const float a2 = alpha * alpha;
    const float gv = nol * sqrt(nov * nov * (1 - a2) + a2), gl = nov * sqrt(nol * nol * (1 - a2) + a2);
    return 0.5f / (gv + gl);
}

// BRDF value without the cosine (scene::model::evaluate); n shading normal, v towards the viewer, l towards the light.
RT_INLINE float3 rtEvaluateModel(RtBsdfParams s, float3 n, float3 v, float3 l)
{
    const float nov = dot(n, v), nol = dot(n, l);
    const float kd = (1 - s.metallic) / kRtPi;
    if (s.cls == kRtClassFoliage && nov * nol < 0) return s.baseColor * (kd * s.transmission);
    if (nov <= 0 || nol <= 0) return rtSplat3(0);
    const float3 h = normalize(v + l);
    const float noh = clamp(dot(n, h), 0.0f, 1.0f);
    const float voh = clamp(dot(v, h), 0.0f, 1.0f);
    const float3 c = cross(n, h);
    const float sin2 = dot(c, c);
    const float alpha = rtAlphaFromRoughness(s.roughness);
    const float dv = rtGgx(noh, sin2, alpha) * rtSmith(nov, nol, alpha);
    const float3 f0 = rtF0(s);
    const float w1 = 1 - voh, w2 = w1 * w1;
    const float w = w2 * w2 * w1;
    const float e = rtDirectionalAlbedo(nov, s.roughness);
    const float diffuseScale = s.cls == kRtClassFoliage ? kd * (1 - s.transmission) : kd;
    const float3 fres = f0 + (rtSplat3(1) - f0) * w;
    const float3 comp = rtSplat3(1) + f0 * (1 / e - 1);
    return s.baseColor * diffuseScale + fres * comp * dv;
}

struct RtBsdf
{
    RtSurface s;
    float3 wo;
    float3 t;
    float3 b;
    bool lambert;
    float nov;
    float alpha;
    float pSpec;
    float pDiff;
    float pTrans;
};

RT_INLINE RtBsdf rtBsdfInit(RtSurface s, float3 wo, bool lambertOnly)
{
    RtBsdf o;
    o.s = s;
    o.wo = wo;
    o.lambert = lambertOnly;
    rtOrthonormal(s.ns, o.t, o.b);
    o.nov = max(dot(s.ns, wo), 1e-6f);
    o.alpha = rtAlphaFromRoughness(s.bsdf.roughness);
    if (lambertOnly)
    {
        o.pSpec = 0;
        o.pDiff = 1;
        o.pTrans = 0;
        return o;
    }
    const float3 f0 = rtF0(s.bsdf);
    const float fres = pow(1 - o.nov, 5.0f);
    const float ws = rtLuminance(f0 + (rtSplat3(1) - f0) * fres);
    const float3 albedo = s.bsdf.baseColor * (1 - s.bsdf.metallic);
    const bool foliage = s.bsdf.cls == kRtClassFoliage;
    const float wd = rtLuminance(albedo) * (foliage ? 1 - s.bsdf.transmission : 1.0f);
    const float wt = foliage ? rtLuminance(albedo) * s.bsdf.transmission : 0.0f;
    const float sum = ws + wd + wt;
    o.pSpec = sum > 0 ? max(ws / sum, 0.1f) : 1.0f;
    const float rest = sum > 0 ? (wd + wt) : 0.0f;
    o.pDiff = rest > 0 ? (1 - o.pSpec) * wd / rest : 0.0f;
    o.pTrans = rest > 0 ? (1 - o.pSpec) * wt / rest : 0.0f;
    if (rest <= 0) o.pSpec = 1;
    return o;
}

RT_INLINE float3 rtBsdfEval(RtBsdf b, float3 wi)
{
    const float gn = dot(b.s.ng, wi), sn = dot(b.s.ns, wi);
    if (gn * sn <= 0) return rtSplat3(0);
    if (b.lambert) return sn > 0 ? b.s.bsdf.baseColor * (1.0f / kRtPi) : rtSplat3(0);
    if (sn < 0 && b.s.bsdf.cls != kRtClassFoliage) return rtSplat3(0);
    return rtEvaluateModel(b.s.bsdf, b.s.ns, b.wo, wi);
}

RT_INLINE float rtBsdfSpecPdf(RtBsdf b, float3 wi)
{
    const float3 h = normalize(b.wo + wi);
    const float3 n = b.s.ns;
    const float noh = dot(n, h);
    if (noh <= 0) return 0;
    const float3 c = cross(n, h);
    const float a = b.alpha, a2 = a * a, nov = b.nov;
    const float g1 = 2 * nov / (nov + sqrt(a2 + (1 - a2) * nov * nov));
    return g1 * rtGgx(noh, dot(c, c), a) / (4 * nov);
}

RT_INLINE float rtBsdfPdf(RtBsdf b, float3 wi)
{
    const float sn = dot(b.s.ns, wi);
    if (sn > 0)
    {
        float p = b.pDiff * sn / kRtPi;
        if (b.pSpec > 0) p += b.pSpec * rtBsdfSpecPdf(b, wi);
        return p;
    }
    return b.pTrans * (-sn) / kRtPi;
}

RT_INLINE float3 rtCosineDirection(float3 n, float u1, float u2)
{
    const float r = sqrt(u1), phi = 2 * kRtPi * u2;
    float3 t, bb;
    rtOrthonormal(n, t, bb);
    return normalize(t * (r * cos(phi)) + bb * (r * sin(phi)) + n * sqrt(max(0.0f, 1 - u1)));
}

RT_INLINE float3 rtSampleVisibleNormalReflection(RtBsdf b, float u1, float u2)
{
    const float a = b.alpha;
    const float3 v = float3(dot(b.wo, b.t), dot(b.wo, b.b), dot(b.wo, b.s.ns));
    const float3 vh = normalize(float3(a * v.x, a * v.y, v.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0 ? float3(-vh.y, vh.x, 0) / sqrt(lensq) : float3(1, 0, 0);
    const float3 t2 = cross(vh, t1);
    const float r = sqrt(u1), phi = 2 * kRtPi * u2;
    const float p1 = r * cos(phi);
    const float sb = 0.5f * (1 + vh.z);
    const float p2 = (1 - sb) * sqrt(max(0.0f, 1 - p1 * p1)) + sb * r * sin(phi);
    const float3 nh = t1 * p1 + t2 * p2 + vh * sqrt(max(0.0f, 1 - p1 * p1 - p2 * p2));
    const float3 hl = normalize(float3(a * nh.x, a * nh.y, max(0.0f, nh.z)));
    const float3 h = b.t * hl.x + b.b * hl.y + b.s.ns * hl.z;
    return normalize(h * (2 * dot(b.wo, h)) - b.wo);
}

// Samples a direction; returns false when the sample carries no contribution.
RT_INLINE bool rtBsdfSample(RtBsdf b, float uLobe, float u1, float u2, RT_OUT(float3) wi, RT_OUT(float3) f, RT_OUT(float) pdf)
{
    f = rtSplat3(0);
    pdf = 0;
    if (uLobe < b.pSpec)
    {
        wi = rtSampleVisibleNormalReflection(b, u1, u2);
        if (dot(wi, b.s.ns) <= 0) return false;
    }
    else if (uLobe < b.pSpec + b.pDiff) wi = rtCosineDirection(b.s.ns, u1, u2);
    else wi = rtCosineDirection(-b.s.ns, u1, u2);
    f = rtBsdfEval(b, wi);
    pdf = rtBsdfPdf(b, wi);
    return pdf > 0 && !rtIsZero3(f);
}

RT_INLINE float rtBsdfCosine(RtBsdf b, float3 wi) { return abs(dot(b.s.ns, wi)); }

RT_INLINE float rtPowerHeuristic(float a, float b)
{
    const float a2 = a * a, b2 = b * b;
    return a2 + b2 > 0 ? a2 / (a2 + b2) : 0.0f;
}
RT_END_NAMESPACE

#endif
