#pragma once
// Sampling of the v1 material model (INTERFACES_KO.md 8.1). The BRDF value is evaluateModel (below): the same
// function as scene::model::evaluate, evaluated without the float cancellation of the GGX term; directions and pdfs:
//   specular   GGX visible-normal sampling (Heitz 2018), pdf = G1(v) D(h) / (4 n.v)
//   diffuse    cosine hemisphere about the shading normal
//   transmit   (Foliage) cosine hemisphere about the reversed shading normal
// Lobe probabilities follow the lobes' albedo estimates with a floor on the specular lobe, so every direction with
// f > 0 has pdf > 0.
// A9 layers (redesign V2.2, the reference had none: a glazed wall rendered as its base): a clearcoat is evaluated by
// scene::model::evaluateCoated (the shared definition) and sampled by a fourth lobe, GGX visible normals of the coat's
// alpha, taken with probability cover x max(F_eta(n.v), 0.1) (the others scaled by the rest); a sheen by evaluateSheen,
// sampled by the cosine lobe (at least 10 % of the choices). The pdf is the mixture's sum (one-sample MIS). Directions must lie on the same side of the geometric and the shading normal (no light leaks
// through the geometric surface from interpolated normals).
#include "RtScene.h"

#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>

namespace unx::reference
{
// The v1 model of scene::model::evaluate (INTERFACES_KO.md 8.1), evaluated in double precision with the GGX
// normal distribution written without cancellation: D = alpha^2 / (pi (|n x h|^2 + alpha^2 (n.h)^2)^2). The float form
// alpha^2 / (pi ((n.h)^2 (alpha^2 - 1) + 1)^2) evaluates to 1/0 for alpha = 1e-4 (roughness 0, mirrors) at n.h = 1
// because alpha^2 is below float resolution next to 1 (Docs/Design/Requests/20260925_C_ggx_precision.md). The value is
// the same function; tests compare it with scene::model::evaluate where that is well conditioned.
struct ModelTerms
{
    static double ggx(double noh, double sin2, double alpha)
    {
        const double a2 = alpha * alpha, t = sin2 + a2 * noh * noh;
        return a2 / (3.14159265358979323846 * t * t);
    }
    static double smith(double nov, double nol, double alpha)
    {
        const double a2 = alpha * alpha;
        const double gv = nol * std::sqrt(nov * nov * (1 - a2) + a2), gl = nov * std::sqrt(nol * nol * (1 - a2) + a2);
        return 0.5 / (gv + gl);
    }
};

inline double dot3(float3 a, float3 b) { return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z; }

// film: A9 thin film (MaterialModel.h evaluateFilm): F' = w F_film(v.h) + (1 - w) Schlick, F'(1) in the compensation
inline Rgb evaluateModel(const scene::model::Surface& s, float3 n, float3 v, float3 l, const scene::model::Film* film = nullptr)
{
    const double nov = dot3(n, v), nol = dot3(n, l);
    const double kd = (1 - s.metallic) / 3.14159265358979323846;
    if (s.cls == scene::MaterialClass::Foliage && nov * nol < 0)
        return Rgb(s.baseColor) * (float)(kd * s.transmission);
    if (nov <= 0 || nol <= 0) return {};
    double hx = (double)v.x + l.x, hy = (double)v.y + l.y, hz = (double)v.z + l.z;
    const double hl = std::sqrt(hx * hx + hy * hy + hz * hz);
    hx /= hl;
    hy /= hl;
    hz /= hl;
    const double noh = std::clamp((double)n.x * hx + (double)n.y * hy + (double)n.z * hz, 0.0, 1.0);
    const double voh = std::clamp((double)v.x * hx + (double)v.y * hy + (double)v.z * hz, 0.0, 1.0);
    const double cx = (double)n.y * hz - (double)n.z * hy, cy = (double)n.z * hx - (double)n.x * hz, cz = (double)n.x * hy - (double)n.y * hx;
    const double sin2 = cx * cx + cy * cy + cz * cz;
    const double alpha = scene::model::alphaFromRoughness(s.roughness);
    const double dv = ModelTerms::ggx(noh, sin2, alpha) * ModelTerms::smith(nov, nol, alpha);
    const float3 f0 = scene::model::f0(s);
    const double w = std::pow(1 - voh, 5.0);
    const double e = scene::model::directionalAlbedo((float)nov, s.roughness);
    const double diffuseScale = s.cls == scene::MaterialClass::Foliage ? kd * (1 - s.transmission) : kd;
    const double f0c[3] = { f0.x, f0.y, f0.z }, base[3] = { s.baseColor.x, s.baseColor.y, s.baseColor.z };
    double out[3];
    if (film)
    {
        const float3 fv = scene::model::filmFresnel(*film, f0, (float)voh), f1 = scene::model::filmFresnel(*film, f0, 1);
        const double fr[3] = { fv.x, fv.y, fv.z }, fn[3] = { f1.x, f1.y, f1.z };
        for (int c = 0; c < 3; ++c) out[c] = base[c] * diffuseScale + fr[c] * dv * (1 + fn[c] * (1 / e - 1));
        return { (float)out[0], (float)out[1], (float)out[2] };
    }
    for (int c = 0; c < 3; ++c)
    {
        const double fres = f0c[c] + (1 - f0c[c]) * w;
        out[c] = base[c] * diffuseScale + fres * dv * (1 + f0c[c] * (1 / e - 1));
    }
    return { (float)out[0], (float)out[1], (float)out[2] };
}

// A9 anisotropy (MaterialModel.h evaluateAnisotropic) in double: D = 1 / (pi at ab (x^2 / at^2 + y^2 / ab^2 + z^2)^2) has no
// cancellation; E_a is the table (anisoSpecularAlbedo).
inline Rgb evaluateAnisotropicModel(const scene::model::Surface& s, const scene::model::Anisotropy& a, float3 n, float3 v, float3 l)
{
    const double nov = dot3(n, v), nol = dot3(n, l);
    if (nov <= 0 || nol <= 0) return {};
    const double kd = (1 - s.metallic) / 3.14159265358979323846;
    double hx = (double)v.x + l.x, hy = (double)v.y + l.y, hz = (double)v.z + l.z;
    const double hl = std::sqrt(hx * hx + hy * hy + hz * hz);
    hx /= hl;
    hy /= hl;
    hz /= hl;
    const double ht = a.t.x * hx + a.t.y * hy + a.t.z * hz, hb = a.b.x * hx + a.b.y * hy + a.b.z * hz;
    const double hn = std::max(0.0, (double)n.x * hx + (double)n.y * hy + (double)n.z * hz);
    const double voh = std::clamp((double)v.x * hx + (double)v.y * hy + (double)v.z * hz, 0.0, 1.0);
    const float2 al = scene::model::anisoAlphas(s.roughness, a.strength);
    const double at = al.x, ab = al.y;
    const double x = ht / at, y = hb / ab, d = x * x + y * y + hn * hn;
    const double D = 1 / (3.14159265358979323846 * at * ab * d * d);
    const double vt = dot3(a.t, v), vb = dot3(a.b, v), lt = dot3(a.t, l), lb = dot3(a.b, l);
    const double V = 0.5 / (nol * std::sqrt(at * at * vt * vt + ab * ab * vb * vb + nov * nov) + nov * std::sqrt(at * at * lt * lt + ab * ab * lb * lb + nol * nol));
    const float2 e = scene::model::anisoSpecularAlbedo(float3{ (float)vt, (float)vb, (float)nov }, al.x, al.y);
    const float3 f0 = scene::model::f0(s);
    const double w = std::pow(1 - voh, 5.0);
    const double f0c[3] = { f0.x, f0.y, f0.z }, base[3] = { s.baseColor.x, s.baseColor.y, s.baseColor.z };
    double out[3];
    for (int c = 0; c < 3; ++c) out[c] = base[c] * kd + (f0c[c] + (1 - f0c[c]) * w) * D * V * (1 + f0c[c] * (1 / ((double)e.x + e.y) - 1));
    return { (float)out[0], (float)out[1], (float)out[2] };
}

struct BsdfSample
{
    float3 wi;
    Rgb f;
    float pdf = 0;
};

class Bsdf
{
public:
    Bsdf(const Surface& s, float3 wo, bool lambertOnly = false) : m_s(s), m_wo(wo), m_lambert(lambertOnly)
    {
        const float3 ref = std::fabs(s.ns.y) < 0.99f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
        m_t = normalize(cross(ref, s.ns));
        m_b = cross(s.ns, m_t);
        m_nov = std::max(dot(s.ns, wo), 1e-6f);
        m_alpha = m_alphaB = scene::model::alphaFromRoughness(s.bsdf.roughness);
        if (s.aniso.strength > 0 && !lambertOnly)
        {
            // A9: the lobe's own frame; visible-normal sampling of the stretched lobe (alpha_t along m_t)
            m_t = s.aniso.t;
            m_b = s.aniso.b;
            const float2 al = scene::model::anisoAlphas(s.bsdf.roughness, s.aniso.strength);
            m_alpha = al.x;
            m_alphaB = al.y;
        }
        if (m_lambert)
        {
            m_pSpec = 0;
            m_pDiff = 1;
            m_pTrans = 0;
            return;
        }
        m_coat = s.coat.cover > 0;
        m_sheen = s.sheen.color.x > 0 || s.sheen.color.y > 0 || s.sheen.color.z > 0;
        if (m_coat) m_alphaC = scene::model::alphaFromRoughness(s.coat.roughness);
        const Rgb f0(scene::model::f0(s.bsdf));
        const float fres = std::pow(1 - m_nov, 5.0f);
        // (sampling weight only; the film's F at n.v when present)
        const float ws = s.film.thickness > 0 ? Rgb(scene::model::filmFresnel(s.film, scene::model::f0(s.bsdf), m_nov)).luminance()
                                              : (f0 + (Rgb(1) - f0) * fres).luminance();
        const Rgb albedo = Rgb(s.bsdf.baseColor) * (1 - s.bsdf.metallic);
        const bool foliage = s.bsdf.cls == scene::MaterialClass::Foliage;
        const float wd = albedo.luminance() * (foliage ? 1 - s.bsdf.transmission : 1.0f);
        const float wt = foliage ? albedo.luminance() * s.bsdf.transmission : 0.0f;
        const float sum = ws + wd + wt;
        m_pSpec = sum > 0 ? std::max(ws / sum, 0.1f) : 1.0f;
        const float rest = sum > 0 ? (wd + wt) : 0.0f;
        m_pDiff = rest > 0 ? (1 - m_pSpec) * wd / rest : 0.0f;
        m_pTrans = rest > 0 ? (1 - m_pSpec) * wt / rest : 0.0f;
        if (rest <= 0) m_pSpec = 1;
        if (m_sheen && m_pDiff < 0.1f)
        {
            // the sheen lobe is broad: the cosine lobe samples it
            const float keep = 0.9f / std::max(m_pSpec + m_pTrans, 1e-6f);
            m_pSpec *= keep;
            m_pTrans *= keep;
            m_pDiff = 0.1f;
        }
        if (m_coat)
        {
            m_pCoat = std::clamp(s.coat.cover * std::max(scene::model::fresnelDielectric(m_nov, s.coat.eta), 0.1f), 0.05f, 0.9f);
            m_pSpec *= 1 - m_pCoat;
            m_pDiff *= 1 - m_pCoat;
            m_pTrans *= 1 - m_pCoat;
        }
    }

    // BRDF value without the cosine; zero for directions on inconsistent sides.
    Rgb eval(float3 wi) const
    {
        const float gn = dot(m_s.ng, wi), sn = dot(m_s.ns, wi);
        if (gn * sn <= 0) return {};
        if (m_lambert) return sn > 0 ? Rgb(m_s.bsdf.baseColor) * (1.0f / scene::model::kPi) : Rgb();
        if (sn < 0 && m_s.bsdf.cls != scene::MaterialClass::Foliage) return {};
        if (m_coat) return Rgb(scene::model::evaluateCoated(m_s.bsdf, m_s.coat, m_s.ns, m_wo, wi));
        if (m_sheen) return Rgb(scene::model::evaluateSheen(m_s.bsdf, m_s.sheen, m_s.ns, m_wo, wi));
        if (m_s.aniso.strength > 0) return evaluateAnisotropicModel(m_s.bsdf, m_s.aniso, m_s.ns, m_wo, wi);
        return evaluateModel(m_s.bsdf, m_s.ns, m_wo, wi, m_s.film.thickness > 0 ? &m_s.film : nullptr);
    }

    float pdf(float3 wi) const
    {
        const float sn = dot(m_s.ns, wi);
        if (sn > 0)
        {
            float p = m_pDiff * sn / scene::model::kPi;
            if (m_pSpec > 0) p += m_pSpec * specPdf(wi);
            if (m_pCoat > 0) p += m_pCoat * isotropicPdf(wi, m_alphaC);
            return p;
        }
        return m_pTrans * (-sn) / scene::model::kPi;
    }

    bool sample(float uLobe, float u1, float u2, BsdfSample& out) const
    {
        float3 wi;
        if (uLobe < m_pCoat)
        {
            wi = visibleNormalReflection(m_alphaC, m_alphaC, u1, u2);
            if (dot(wi, m_s.ns) <= 0) return false;
        }
        else if ((uLobe -= m_pCoat) < m_pSpec)
        {
            wi = sampleVisibleNormalReflection(u1, u2);
            if (dot(wi, m_s.ns) <= 0) return false;
        }
        else if (uLobe < m_pSpec + m_pDiff) wi = cosine(m_s.ns, u1, u2);
        else wi = cosine(-m_s.ns, u1, u2);
        out.wi = wi;
        out.f = eval(wi);
        out.pdf = pdf(wi);
        return out.pdf > 0 && !out.f.isZero();
    }

    float cosine(float3 wi) const { return std::fabs(dot(m_s.ns, wi)); }

private:
    float3 cosine(float3 n, float u1, float u2) const
    {
        const float r = std::sqrt(u1), phi = 2 * scene::model::kPi * u2;
        const float3 ref = std::fabs(n.y) < 0.99f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
        const float3 t = normalize(cross(ref, n)), b = cross(n, t);
        return normalize(t * (r * std::cos(phi)) + b * (r * std::sin(phi)) + n * std::sqrt(std::max(0.0f, 1 - u1)));
    }

    float specPdf(float3 wi) const
    {
        double hx = (double)m_wo.x + wi.x, hy = (double)m_wo.y + wi.y, hz = (double)m_wo.z + wi.z;
        const double hl = std::sqrt(hx * hx + hy * hy + hz * hz);
        hx /= hl;
        hy /= hl;
        hz /= hl;
        const float3 n = m_s.ns;
        const double noh = n.x * hx + n.y * hy + n.z * hz;
        if (noh <= 0) return 0;
        const double nov = m_nov;
        if (m_alpha != m_alphaB)
        {
            // A9: G1(v) D(h) / (4 n.v) of the stretched lobe
            const double at = m_alpha, ab = m_alphaB;
            const double x = (m_t.x * hx + m_t.y * hy + m_t.z * hz) / at, y = (m_b.x * hx + m_b.y * hy + m_b.z * hz) / ab, d = x * x + y * y + noh * noh;
            const double vt = dot3(m_t, m_wo), vb = dot3(m_b, m_wo);
            const double g1 = 2 * nov / (nov + std::sqrt(at * at * vt * vt + ab * ab * vb * vb + nov * nov));
            return (float)(g1 / (3.14159265358979323846 * at * ab * d * d) / (4 * nov));
        }
        return isotropicPdf(wi, m_alpha);
    }

    // G1(v) D(h) / (4 n.v) of an isotropic GGX lobe of alpha a about the shading normal (base or coat)
    float isotropicPdf(float3 wi, float a) const
    {
        double hx = (double)m_wo.x + wi.x, hy = (double)m_wo.y + wi.y, hz = (double)m_wo.z + wi.z;
        const double hl = std::sqrt(hx * hx + hy * hy + hz * hz);
        hx /= hl;
        hy /= hl;
        hz /= hl;
        const float3 n = m_s.ns;
        const double noh = n.x * hx + n.y * hy + n.z * hz;
        if (noh <= 0) return 0;
        const double nov = m_nov;
        const double cx = n.y * hz - n.z * hy, cy = n.z * hx - n.x * hz, cz = n.x * hy - n.y * hx;
        const double a2 = (double)a * a;
        const double g1 = 2 * nov / (nov + std::sqrt(a2 + (1 - a2) * nov * nov));
        return (float)(g1 * ModelTerms::ggx(noh, cx * cx + cy * cy + cz * cz, a) / (4 * nov));
    }

    float3 sampleVisibleNormalReflection(float u1, float u2) const { return visibleNormalReflection(m_alpha, m_alphaB, u1, u2); }

    float3 visibleNormalReflection(float a, float ab, float u1, float u2) const
    {
        // (alpha_t, alpha_b); equal for isotropic lobes
        const float3 v{ dot(m_wo, m_t), dot(m_wo, m_b), dot(m_wo, m_s.ns) };
        const float3 vh = normalize(float3{ a * v.x, ab * v.y, v.z });
        const float lensq = vh.x * vh.x + vh.y * vh.y;
        const float3 t1 = lensq > 0 ? float3{ -vh.y, vh.x, 0 } / std::sqrt(lensq) : float3{ 1, 0, 0 };
        const float3 t2 = cross(vh, t1);
        const float r = std::sqrt(u1), phi = 2 * scene::model::kPi * u2;
        const float p1 = r * std::cos(phi);
        const float s = 0.5f * (1 + vh.z);
        const float p2 = (1 - s) * std::sqrt(std::max(0.0f, 1 - p1 * p1)) + s * r * std::sin(phi);
        const float3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1 - p1 * p1 - p2 * p2));
        const float3 hl = normalize(float3{ a * nh.x, ab * nh.y, std::max(0.0f, nh.z) });
        const float3 h = m_t * hl.x + m_b * hl.y + m_s.ns * hl.z;
        return normalize(h * (2 * dot(m_wo, h)) - m_wo);
    }

    const Surface& m_s;
    float3 m_wo, m_t, m_b;
    bool m_lambert;
    bool m_coat = false, m_sheen = false;
    float m_nov = 1, m_alpha = 1, m_alphaB = 1, m_alphaC = 1, m_pSpec = 0, m_pDiff = 0, m_pTrans = 0, m_pCoat = 0;
};

inline float powerHeuristic(float a, float b)
{
    const float a2 = a * a, b2 = b * b;
    return a2 + b2 > 0 ? a2 / (a2 + b2) : 0.0f;
}
} // namespace unx::reference
