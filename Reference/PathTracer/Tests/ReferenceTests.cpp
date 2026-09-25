// Reference path tracer against closed forms. Each case renders a small image of a large Lambertian plane seen from
// straight above with a narrow field of view (the pixel footprint is a few millimetres, so every pixel sees the same
// point to first order) and compares the patch mean with the analytic radiance. The tolerance is 4 standard errors of
// the patch mean, measured from the two independent halves, plus a small deterministic allowance where stated.
//   point       L = f * I * w(d) / d^2 * cos                 (deterministic: no noise)
//   rect        E = 4 pi L F_corner(a/2c, b/2c)              (differential area -> parallel rectangle)
//   sphere      E = pi L (r/c)^2                             (fully visible sphere light)
//   sun         L = f * E_TOA * exp(-tau_sun) * cos(theta)   (absorbing, non-scattering atmosphere; tau by quadrature)
//   skythin     sky radiance of an optically thin atmosphere = single-scattering integral (nested quadrature)
//   sky         full atmosphere: forced in-scattering NEE and collision NEE agree (two unbiased estimators)
//   bsdf        E[f cos / pdf] with BSDF sampling equals E[f cos / p_uniform] (sampling/pdf consistency), per material
//   atmosphere  optical-depth table error against 8192-panel quadrature (measured, printed)
#include "unx/core/Log.h"
#include "unx/reference/PathTracer.h"
#include "unx/scene/MaterialModel.h"

#include "Atmosphere.h"
#include "Bsdf.h"
#include "Sampler.h"

#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace unx;

namespace
{
constexpr float kPi = 3.14159265358979323846f;
#define CHECK(c) do { if (!(c)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #c); } while (0)

scene::Scene planeScene(float albedo)
{
    scene::Scene s;
    s.name = "test_plane";
    scene::Material m;
    m.name = "lambert";
    m.baseColor = { albedo, albedo, albedo };
    m.roughness = 1.0f;
    m.specular = 0.0f;  // f0 = 0: the Schlick term is < 1e-7 for the near-normal directions used here
    s.materials.push_back(m);
    scene::Mesh mesh;
    mesh.name = "plane";
    const float e = 2000.0f;
    mesh.positions = { { -e, 0, -e }, { -e, 0, e }, { e, 0, e }, { e, 0, -e } };
    mesh.normals = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 } };
    mesh.uv0 = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };
    mesh.submeshes = { { 0, 6, 0 } };
    s.meshes.push_back(mesh);
    scene::Instance in;
    in.mesh = 0;
    s.instances.push_back(in);
    // No atmosphere, no sun unless a test sets them.
    s.atmosphere.rayleighScattering = { 0, 0, 0 };
    s.atmosphere.mieScattering = { 0, 0, 0 };
    s.atmosphere.mieAbsorption = { 0, 0, 0 };
    s.atmosphere.ozoneAbsorption = { 0, 0, 0 };
    s.atmosphere.groundAlbedo = { 0, 0, 0 };
    s.sun.illuminance = 0;
    scene::Camera c;
    c.name = "down";
    c.position = { 0, 50, 0 };
    c.forward = { 0, -1, 0 };
    c.up = { 0, 0, -1 };
    c.verticalFov = 0.0005f;  // 16 px over 2.5 cm at 50 m
    c.ev100 = 0;              // exposure 1/1.2
    s.cameras.push_back(c);
    return s;
}

struct Measured
{
    double mean = 0, stderr_ = 0;
};

Measured renderPatch(const scene::Scene& s, uint32_t spp)
{
    reference::PathTracer pt(s);
    reference::RenderSettings rs;
    rs.width = 16;
    rs.height = 16;
    rs.samplesPerPixel = spp;
    rs.russianRouletteStart = 4;
    rs.samplesPerPass = spp / 2;
    const reference::RenderOutput out = pt.render(reference::resolveCamera(s, { "down", "", 0 }), rs);
    CHECK(out.stats.nanSamples == 0 && out.stats.truncatedPaths == 0);
    const double exposure = 1.0 / 1.2;
    double a = 0, b = 0;
    for (size_t i = 0; i < out.halfA.rgb.size(); i += 3)
    {
        a += out.halfA.rgb[i + 1];
        b += out.halfB.rgb[i + 1];
    }
    const double n = (double)(out.halfA.rgb.size() / 3);
    a /= n * exposure;
    b /= n * exposure;
    // Two independent estimates of the same mean: sigma(mean of both) ~ |a - b| / 2 on average; use a robust floor.
    Measured m;
    m.mean = 0.5 * (a + b);
    m.stderr_ = std::max(std::fabs(a - b) / 2.0, 1e-6 * std::fabs(m.mean));
    return m;
}

void expectNear(const char* name, const Measured& m, double expected, double relAllowance)
{
    const double tol = 4 * m.stderr_ + relAllowance * std::fabs(expected);
    const double err = m.mean - expected;
    logf("  %-8s measured %.6g  expected %.6g  rel.err %+.2e  (tolerance %.2e)\n", name, m.mean, expected, err / expected, tol / std::fabs(expected));
    if (std::fabs(err) > tol) fail("%s: measured %.6g, expected %.6g", name, m.mean, expected);
}

float windowW(float d, float range)
{
    const float r = d / range, r4 = r * r * r * r;
    const float w = std::clamp(1 - r4, 0.0f, 1.0f);
    return w * w;
}

double lambert(float albedo) { return albedo / kPi; }

void testPoint()
{
    scene::Scene s = planeScene(0.5f);
    scene::Light l;
    l.type = scene::LightType::Point;
    l.position = { 0, 3, 0 };
    l.intensity = 1000;
    l.range = 30;
    l.castShadow = true;
    s.lights.push_back(l);
    const Measured m = renderPatch(s, 64);
    expectNear("point", m, lambert(0.5f) * 1000 * windowW(3, 30) / 9.0, 1e-4);
}

void testRect()
{
    scene::Scene s = planeScene(0.5f);
    scene::Light l;
    l.type = scene::LightType::Rect;
    l.position = { 0, 2, 0 };
    l.forward = { 0, -1, 0 };
    l.right = { 1, 0, 0 };
    l.size = { 1.2f, 0.6f };
    l.intensity = 4000;
    l.range = 20;
    l.castShadow = true;
    s.lights.push_back(l);
    const Measured m = renderPatch(s, 8192);
    const double X = 0.6 / 2.0, Y = 0.3 / 2.0;
    const double F = (X / std::sqrt(1 + X * X) * std::atan(Y / std::sqrt(1 + X * X)) + Y / std::sqrt(1 + Y * Y) * std::atan(X / std::sqrt(1 + Y * Y))) / (2 * kPi);
    const double E = 4 * kPi * 4000 * F;
    expectNear("rect", m, lambert(0.5f) * E * windowW(2, 20), 1e-4);
}

void testSphere()
{
    scene::Scene s = planeScene(0.5f);
    scene::Light l;
    l.type = scene::LightType::Sphere;
    l.position = { 0, 1.5f, 0 };
    l.size = { 0.2f, 0 };
    l.intensity = 20000;
    l.range = 15;
    l.castShadow = true;
    s.lights.push_back(l);
    const Measured m = renderPatch(s, 8192);
    const double E = kPi * 20000 * (0.2 / 1.5) * (0.2 / 1.5);
    expectNear("sphere", m, lambert(0.5f) * E * windowW(1.5f, 15), 1e-4);
}

void testDiskAndTube()
{
    // Disk under the same geometry as a rect of equal area gives a slightly different E; check the disk against
    // its own closed form E = pi L R^2 / (R^2 + c^2) for a point on the axis.
    scene::Scene s = planeScene(0.5f);
    scene::Light l;
    l.type = scene::LightType::Disk;
    l.position = { 0, 2, 0 };
    l.forward = { 0, -1, 0 };
    l.size = { 0.4f, 0 };
    l.intensity = 5000;
    l.range = 20;
    l.castShadow = true;
    s.lights.push_back(l);
    const Measured m = renderPatch(s, 8192);
    const double E = kPi * 5000 * 0.16 / (0.16 + 4.0);
    expectNear("disk", m, lambert(0.5f) * E * windowW(2, 20), 1e-4);
}

void testShadow()
{
    // A black 2 x 2 m slab at 1.5 m between the plane point under the camera and a point light / the sun: the patch
    // must be black up to the slab's grazing Schlick reflection (f0 = 0 still reflects at grazing angles, ~1e-5 of
    // direct); a leak of direct light would be ~100%.
    for (int sun = 0; sun < 2; ++sun)
    {
        scene::Scene s = planeScene(0.5f);
        scene::Mesh box;
        box.name = "slab";
        const float3 lo{ -1, 1.5f, -1 }, hi{ 1, 1.6f, 1 };
        const float3 c[8] = { { lo.x, lo.y, lo.z }, { hi.x, lo.y, lo.z }, { hi.x, hi.y, lo.z }, { lo.x, hi.y, lo.z },
                              { lo.x, lo.y, hi.z }, { hi.x, lo.y, hi.z }, { hi.x, hi.y, hi.z }, { lo.x, hi.y, hi.z } };
        for (const float3& p : c)
        {
            box.positions.push_back(p);
            box.normals.push_back(normalize(float3{ p.x, p.y - 1.55f, p.z }));
        }
        box.indices = { 0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 3, 7, 6, 3, 6, 2, 0, 4, 7, 0, 7, 3, 1, 2, 6, 1, 6, 5 };
        scene::Material black;
        black.name = "black";
        black.baseColor = { 0, 0, 0 };
        black.specular = 0;
        black.roughness = 1;
        s.materials.push_back(black);
        box.submeshes = { { 0, 36, 1 } };
        s.meshes.push_back(box);
        scene::Instance in;
        in.mesh = 1;
        s.instances.push_back(in);
        s.cameras[0].position = { 0, 1.0f, 0 };  // between plane and slab, looking down
        if (sun)
        {
            s.sun.direction = normalize(float3{ 0.1f, 1, 0.05f });
            s.sun.illuminance = 100000;
        }
        else
        {
            scene::Light l;
            l.type = scene::LightType::Point;
            l.position = { 0, 3, 0 };
            l.intensity = 1000;
            l.range = 30;
            l.castShadow = true;
            s.lights.push_back(l);
        }
        const Measured m = renderPatch(s, 64);
        const double direct = sun ? lambert(0.5f) * 100000 * 0.99 : lambert(0.5f) * 1000 * windowW(3, 30) / 9.0;
        logf("  shadow %-5s patch %.4g (direct would be %.4g)\n", sun ? "sun" : "point", m.mean, direct);
        if (m.mean > 1e-3 * direct) fail("shadow: occluder does not block the %s", sun ? "sun" : "point light");
    }
}

void testSunAbsorbing()
{
    scene::Scene s = planeScene(0.5f);
    s.atmosphere.ozoneAbsorption = { 0.650e-6f, 1.881e-6f, 0.085e-6f };
    s.atmosphere.mieAbsorption = { 2e-6f, 2e-6f, 2e-6f };
    const float elev = 30.0f * kPi / 180;
    s.sun.direction = { std::cos(elev), std::sin(elev), 0 };
    s.sun.illuminance = 100000;
    const Measured m = renderPatch(s, 256);
    // Optical depth (green channel) from the origin to the top along the sun direction, by fine quadrature.
    const double R = s.atmosphere.bottomRadius, Rt = s.atmosphere.topRadius, mu = std::sin(elev);
    const double len = -R * mu + std::sqrt(R * R * (mu * mu - 1) + Rt * Rt);
    const int n = 200000;
    double tau = 0;
    for (int i = 0; i < n; ++i)
    {
        const double t = (i + 0.5) * len / n;
        const double x = t * std::cos(elev), y = R + t * mu;
        const double h = std::sqrt(x * x + y * y) - R;
        tau += (2e-6 * std::exp(-h / 1200.0) + 1.881e-6 * std::max(0.0, 1 - std::fabs(h - 25000.0) / 15000.0)) * len / n;
    }
    // The camera ray (50 m straight down) crosses the same absorbing air.
    const double tauView = 2e-6 * 1200.0 * (1 - std::exp(-50.0 / 1200.0));
    const double expected = lambert(0.5f) * 100000 * std::exp(-tau) * std::sin(elev) * std::exp(-tauView);
    expectNear("sun", m, expected, 2e-4);  // allowance: the 0.27 deg disk is integrated, the closed form uses its centre
}

scene::Scene skyScene(float coefficientScale, float3 view)
{
    scene::Scene s;
    s.name = "test_sky";
    auto scale3 = [&](float3 v) { return float3{ v.x * coefficientScale, v.y * coefficientScale, v.z * coefficientScale }; };
    s.atmosphere.rayleighScattering = scale3(s.atmosphere.rayleighScattering);
    s.atmosphere.mieScattering = scale3(s.atmosphere.mieScattering);
    s.atmosphere.mieAbsorption = scale3(s.atmosphere.mieAbsorption);
    s.atmosphere.ozoneAbsorption = scale3(s.atmosphere.ozoneAbsorption);
    s.atmosphere.groundAlbedo = { 0, 0, 0 };
    const float elev = 30.0f * kPi / 180;
    s.sun.direction = { std::cos(elev), std::sin(elev), 0 };
    s.sun.illuminance = 100000;
    scene::Camera c;
    c.name = "sky";
    c.position = { 0, 2, 0 };
    c.forward = normalize(view);
    const float3 right = normalize(cross(c.forward, float3{ 0, 0, 1 }));
    c.up = normalize(cross(right, c.forward));
    c.verticalFov = 0.0005f;
    c.ev100 = 0;
    s.cameras.push_back(c);
    return s;
}

Measured renderSky(const scene::Scene& s, uint32_t spp, bool forced)
{
    reference::PathTracer pt(s);
    reference::RenderSettings rs;
    rs.width = 4;
    rs.height = 4;
    rs.samplesPerPixel = spp;
    rs.russianRouletteStart = 4;
    rs.samplesPerPass = spp / 2;
    rs.forcedInScattering = forced;
    const reference::RenderOutput out = pt.render(reference::resolveCamera(s, { "sky", "", 0 }), rs);
    CHECK(out.stats.nanSamples == 0 && out.stats.truncatedPaths == 0);
    double a = 0, b = 0;
    for (size_t i = 0; i < out.halfA.rgb.size(); i += 3)
    {
        a += out.halfA.rgb[i + 2];  // blue: the most scattering channel
        b += out.halfB.rgb[i + 2];
    }
    const double n = (double)(out.halfA.rgb.size() / 3) * (1.0 / 1.2);
    Measured m;
    m.mean = 0.5 * (a + b) / n;
    m.stderr_ = std::fabs(a - b) / (2.0 * n);
    return m;
}

// Single-scattering sky radiance (blue channel) by nested quadrature, independent of the reference's code.
double singleScatterSky(const scene::Scene& s, float3 view)
{
    const scene::Atmosphere& a = s.atmosphere;
    const double R = a.bottomRadius, Rt = a.topRadius, g = a.mieG;
    auto ext = [&](double h) {
        return a.rayleighScattering.z * std::exp(-h / a.rayleighScaleHeight) + (a.mieScattering.z + a.mieAbsorption.z) * std::exp(-h / a.mieScaleHeight) +
               a.ozoneAbsorption.z * std::max(0.0, 1 - std::fabs(h - a.ozoneCenter) / a.ozoneWidth);
    };
    // Optical depth from (x, y, z) (scene coordinates) to the top along d, midpoint rule with n steps; returns length.
    auto toTop = [&](double ox, double oy, double oz, double dx, double dy, double dz, int n, double& tau) {
        const double py = oy + R, b = ox * dx + py * dy + oz * dz, c = ox * ox + py * py + oz * oz - Rt * Rt;
        const double len = -b + std::sqrt(b * b - c);
        tau = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (i + 0.5) * len / n, x = ox + dx * t, y = py + dy * t, z = oz + dz * t;
            tau += ext(std::sqrt(x * x + y * y + z * z) - R) * len / n;
        }
        return len;
    };
    const double vx = view.x, vy = view.y, vz = view.z;
    const double sx = s.sun.direction.x, sy = s.sun.direction.y, sz = s.sun.direction.z;
    const double mu = vx * sx + vy * sy + vz * sz;
    const double pR = 3.0 / (16 * kPi) * (1 + mu * mu), den = 1 + g * g - 2 * g * mu;
    const double pM = (1 - g * g) / (4 * kPi * den * std::sqrt(den));
    double unused;
    const double len = toTop(0, 2, 0, vx, vy, vz, 1, unused);
    // Non-uniform steps (dense near the ground where the density is): t = len * u^3.
    const int n = 6000;
    double L = 0, tauView = 0, prevT = 0;
    for (int i = 0; i < n; ++i)
    {
        const double u0 = (double)i / n, u1 = (double)(i + 1) / n;
        const double t0 = len * u0 * u0 * u0, t1 = len * u1 * u1 * u1, t = 0.5 * (t0 + t1), dt = t1 - t0;
        (void)prevT;
        const double x = vx * t, y = 2 + vy * t, z = vz * t;
        const double h = std::sqrt(x * x + (y + R) * (y + R) + z * z) - R;
        const double tauMid = tauView + ext(h) * dt * 0.5;
        double tauSun = 0;
        toTop(x, y, z, sx, sy, sz, 3000, tauSun);
        const double sig = a.rayleighScattering.z * std::exp(-h / a.rayleighScaleHeight) * pR + a.mieScattering.z * std::exp(-h / a.mieScaleHeight) * pM;
        L += std::exp(-tauMid) * sig * s.sun.illuminance * std::exp(-tauSun) * dt;
        tauView += ext(h) * dt;
        prevT = t1;
    }
    return L;
}

void testSkySingleScatter()
{
    // Optically thin atmosphere (coefficients x 0.01): multiple scattering is ~tau ~ 0.3% of single scattering.
    for (const float3 view : { float3{ 0, 1, 0 }, normalize(float3{ 1, 0.09f, 0.1f }) })
    {
        const scene::Scene s = skyScene(0.01f, view);
        const Measured m = renderSky(s, 1 << 18, true);
        expectNear("sky thin", m, singleScatterSky(s, view), 6e-3);
    }
}

void testSkyEstimators()
{
    // Full atmosphere: forced in-scattering NEE vs NEE at tracked collisions (same expectation, different variance).
    for (const float3 view : { float3{ 0, 1, 0 }, normalize(float3{ 1, 0.09f, 0.1f }), normalize(float3{ -1, 0.2f, 0.3f }) })
    {
        const scene::Scene s = skyScene(1.0f, view);
        const Measured a = renderSky(s, 1 << 16, true), b = renderSky(s, 1 << 16, false);
        const double diff = a.mean - b.mean, tol = 4 * std::sqrt(a.stderr_ * a.stderr_ + b.stderr_ * b.stderr_) + 1e-4 * std::fabs(a.mean);
        logf("  sky full view (%.2f %.2f %.2f): forced %.5g (+-%.2g)  collision %.5g (+-%.2g)  diff %+.2e rel (tol %.2e)\n", view.x, view.y, view.z, a.mean, a.stderr_, b.mean,
             b.stderr_, diff / a.mean, tol / a.mean);
        if (std::fabs(diff) > tol) fail("sky estimators disagree");
    }
}

void testBsdfSampling()
{
    struct Case
    {
        scene::MaterialClass cls;
        float3 base;
        float roughness, metallic, transmission, nov;
    };
    const Case cases[] = {
        { scene::MaterialClass::Standard, { 0.5f, 0.5f, 0.5f }, 0.5f, 0.0f, 0.0f, 0.8f },
        { scene::MaterialClass::Standard, { 0.9f, 0.6f, 0.3f }, 0.2f, 1.0f, 0.0f, 0.5f },
        { scene::MaterialClass::Standard, { 0.1f, 0.1f, 0.1f }, 0.35f, 0.0f, 0.0f, 0.26f },
        { scene::MaterialClass::Standard, { 0.2f, 0.2f, 0.2f }, 0.9f, 0.3f, 0.0f, 0.1f },
        { scene::MaterialClass::Foliage, { 0.06f, 0.12f, 0.03f }, 0.5f, 0.0f, 0.3f, 0.7f },
    };
    for (const Case& c : cases)
    {
        reference::Surface s;
        s.ng = s.ns = { 0, 0, 1 };
        s.bsdf.cls = c.cls;
        s.bsdf.baseColor = c.base;
        s.bsdf.roughness = c.roughness;
        s.bsdf.metallic = c.metallic;
        s.bsdf.transmission = c.transmission;
        const float3 wo{ std::sqrt(1 - c.nov * c.nov), 0, c.nov };
        const reference::Bsdf b(s, wo);
        const uint32_t N = 1 << 20;
        double sumB = 0, sumB2 = 0, sumU = 0, sumU2 = 0, pdfInt = 0;
        reference::Pcg32 rng(12345, 7);
        for (uint32_t i = 0; i < N; ++i)
        {
            reference::BsdfSample bs;
            double vb = 0;
            if (b.sample(rng.uniform(), rng.uniform(), rng.uniform(), bs)) vb = bs.f.g * b.cosine(bs.wi) / bs.pdf;
            sumB += vb;
            sumB2 += vb * vb;
            // Uniform sphere.
            const float z = 1 - 2 * rng.uniform(), r = std::sqrt(std::max(0.0f, 1 - z * z)), phi = 2 * kPi * rng.uniform();
            const float3 w{ r * std::cos(phi), r * std::sin(phi), z };
            const double vu = b.eval(w).g * b.cosine(w) * 4 * kPi;
            sumU += vu;
            sumU2 += vu * vu;
            pdfInt += b.pdf(w) * 4 * kPi;
        }
        const double mB = sumB / N, mU = sumU / N;
        const double sB = std::sqrt(std::max(0.0, sumB2 / N - mB * mB) / N), sU = std::sqrt(std::max(0.0, sumU2 / N - mU * mU) / N);
        const double diff = std::fabs(mB - mU), tol = 5 * std::sqrt(sB * sB + sU * sU);
        logf("  bsdf r=%.2f m=%.1f cls=%d nov=%.2f: E_bsdf %.5f  E_uniform %.5f  |diff| %.2e (tol %.2e), integral of pdf %.4f\n", c.roughness, c.metallic, (int)c.cls, c.nov, mB,
             mU, diff, tol, pdfInt / N);
        if (diff > tol) fail("bsdf sampling inconsistent with evaluation");
        if (pdfInt / N > 1.02) fail("bsdf pdf integrates above 1");
    }
}

void testModelAgreement()
{
    // evaluateModel (double, cancellation-free GGX) equals scene::model::evaluate where float is well conditioned
    // (roughness >= 0.2; below that the float form's own cancellation error reaches 1e-3), and stays finite for
    // roughness 0 at the mirror direction where the float form divides by 0.
    reference::Pcg32 rng(99, 3);
    double worst = 0;
    for (int i = 0; i < 200000; ++i)
    {
        scene::model::Surface s;
        s.cls = (i & 7) == 0 ? scene::MaterialClass::Foliage : scene::MaterialClass::Standard;
        s.baseColor = { rng.uniform(), rng.uniform(), rng.uniform() };
        s.roughness = 0.2f + 0.8f * rng.uniform();
        s.metallic = rng.uniform() < 0.5f ? 0.0f : rng.uniform();
        s.specular = rng.uniform();
        s.transmission = s.cls == scene::MaterialClass::Foliage ? rng.uniform() : 0.0f;
        auto dir = [&]() {
            const float z = rng.uniform(), r = std::sqrt(1 - z * z), p = 2 * kPi * rng.uniform();
            return float3{ r * std::cos(p), r * std::sin(p), z };
        };
        const float3 n{ 0, 0, 1 }, v = dir();
        float3 l = dir();
        if (s.cls == scene::MaterialClass::Foliage && (i & 1)) l.z = -l.z;
        if (v.z < 0.05f || std::fabs(l.z) < 0.05f) continue;
        const float3 a = scene::model::evaluate(s, n, v, l);
        const reference::Rgb b = reference::evaluateModel(s, n, v, l);
        const float av[3] = { a.x, a.y, a.z }, bv[3] = { b.r, b.g, b.b };
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs((double)bv[c] - av[c]) / std::max(1e-3, (double)std::fabs(av[c])));
    }
    scene::model::Surface mirror;
    mirror.baseColor = { 0.95f, 0.95f, 0.95f };
    mirror.metallic = 1;
    mirror.roughness = 0;
    const float3 n{ 0, 0, 1 }, v = normalize(float3{ 0.3f, 0, 1 }), l = normalize(float3{ -0.3f, 0, 1 });
    const reference::Rgb m = reference::evaluateModel(mirror, n, v, l);
    const float3 mf = scene::model::evaluate(mirror, n, v, l);
    logf("  model: max relative difference to scene::model::evaluate %.2e (roughness >= 0.2); mirror direction, roughness 0: %.4g (float form: %.4g)\n",
         worst, m.g, mf.y);
    if (worst > 2e-4) fail("evaluateModel differs from scene::model::evaluate");
    if (!m.finite() || m.g <= 0) fail("evaluateModel is not finite for a mirror");
}

void testHold()
{
    // waitWhileHeld: a record naming a dead process is stale (returns at once); a pid-less marker holds until removed.
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "unx_hold_test";
    fs::create_directories(dir);
    const fs::path stale = dir / "current.json", marker = dir / "HOLD";
    { std::ofstream(stale) << "{\"track\":\"X\",\"pid\":4294967,\"started\":\"t\"}"; }
    auto t0 = std::chrono::steady_clock::now();
    reference::waitWhileHeld({ stale });
    const double staleSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    { std::ofstream(marker) << "hold"; }
    std::thread remover([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        fs::remove(marker);
    });
    t0 = std::chrono::steady_clock::now();
    reference::waitWhileHeld({ stale, marker });
    const double heldSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    remover.join();
    fs::remove(stale);
    logf("  hold: stale record released in %.2f s, marker held %.2f s (removed at 0.70 s)\n", staleSec, heldSec);
    if (staleSec > 0.5) fail("stale GPU lock record held the render");
    if (heldSec < 0.65 || heldSec > 1.5) fail("manual hold marker not honoured");
}

void testAtmosphereTable()
{
    scene::Atmosphere atm;
    reference::AtmosphereModel m(atm);
    double relT = 0;
    const double absTau = m.selfCheck(4000, &relT, true);
    logf("  atmosphere table: max |tau error| %.3g, max relative T error %.3g (T > 1e-4)\n", absTau, relT);
    if (relT > 1e-4) fail("atmosphere table error %.3g > 1e-4", relT);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const char* only = argc > 1 ? argv[1] : nullptr;
        auto run = [&](const char* name, void (*fn)()) {
            if (only && std::strcmp(only, name) != 0) return;
            logf("[%s]\n", name);
            fn();
        };
        run("hold", testHold);
        run("atmosphere", testAtmosphereTable);
        run("model", testModelAgreement);
        run("bsdf", testBsdfSampling);
        run("point", testPoint);
        run("rect", testRect);
        run("sphere", testSphere);
        run("disk", testDiskAndTube);
        run("shadow", testShadow);
        run("sun", testSunAbsorbing);
        run("skythin", testSkySingleScatter);
        run("sky", testSkyEstimators);
        logf("reference tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAILED: %s\n", e.what());
        return 1;
    }
}
