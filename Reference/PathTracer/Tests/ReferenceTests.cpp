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
//   water       a smooth water interface over an emitting floor (Dielectric.h; black sky, no sun), CPU only:
//               from above  L = (1 - F(theta)) T^(d / cos theta_t) Le / n^2   (normal incidence and 50 degrees)
//               from below  past the critical angle L = Le_floor (total internal reflection, sigma = 0); at 30 degrees
//                           L = F_in Le_floor + (1 - F_in) n^2 Le_ceiling (the radiance gain of leaving the water)
// With --gpu as the first argument the rendering cases run on the GPU reference tracer (Reference/GpuTracer) against
// the same closed forms (the validation gate's furnace and energy tests); the CPU-only cases are skipped.
#include "unx/core/Log.h"
#include "unx/reference/GpuPathTracer.h"
#include "unx/reference/PathTracer.h"
#include "unx/scene/MaterialModel.h"

#include "Atmosphere.h"
#include "Bsdf.h"
#include "CutFace.h"
#include "RtScene.h"
#include "HoldRecord.h"
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

bool g_gpu = false;  // --gpu: render on the GPU tracer

reference::RenderOutput renderOn(const scene::Scene& s, const reference::ResolvedCamera& cam, const reference::RenderSettings& rs)
{
    if (g_gpu)
    {
        reference::GpuPathTracer gpt(s, UNX_SOURCE_DIR, "unx_test_reference --gpu");
        return gpt.render(cam, rs);
    }
    reference::PathTracer pt(s);
    return pt.render(cam, rs);
}

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

Measured renderPatch(const scene::Scene& s, uint32_t spp, bool sunCaustics = true, uint32_t surfaceOrderMax = 0xFFFFFFFFu)
{
    reference::RenderSettings rs;
    rs.width = 16;
    rs.height = 16;
    rs.samplesPerPixel = spp;
    rs.russianRouletteStart = 4;
    rs.samplesPerPass = spp / 2;
    rs.sunCaustics = sunCaustics;
    rs.surfaceOrderMax = surfaceOrderMax;
    const reference::RenderOutput out = renderOn(s, reference::resolveCamera(s, { "down", "", 0 }), rs);
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

// A9 clearcoat in a render: a point light over a coated plane (white base 0.8, coat 1, roughness 0.3 so the lobe is
// resolved by the 16 x 16 patch) seen from above: L = evaluateCoated(v, l) cos I / d^2 (the direct light's closed form),
// and the same plane's ratio coated / uncoated against the model's ratio (the engine's coat/no-coat difference was 0.57
// against the reference's 1.000 before the reference had the layer).
void testCoat()
{
    auto scene = [](float cover) {
        scene::Scene s = planeScene(0.8f);
        s.materials[0].roughness = 0.5f;
        s.materials[0].specular = 0.5f;
        s.materials[0].clearcoat = cover;
        s.materials[0].clearcoatRoughness = 0.3f;
        scene::Light l;
        l.type = scene::LightType::Point;
        l.position = { 2, 3, 0 };
        l.intensity = 1000;
        l.range = 30;
        l.castShadow = true;
        s.lights.push_back(l);
        return s;
    };
    const float3 v{ 0, 1, 0 }, lp{ 2, 3, 0 };
    const float d2 = dot(lp, lp);
    const float3 l = normalize(lp);
    scene::model::Surface su;
    su.baseColor = { 0.8f, 0.8f, 0.8f };
    su.roughness = 0.5f;
    su.specular = 0.5f;
    scene::model::Coat coat;
    coat.cover = 1;
    coat.roughness = 0.3f;
    const double coated = scene::model::evaluateCoated(su, coat, v, v, l).y * l.y * 1000 * windowW(std::sqrt(d2), 30) / d2;
    const double bare = scene::model::evaluate(su, v, v, l).y * l.y * 1000 * windowW(std::sqrt(d2), 30) / d2;
    const Measured mc = renderPatch(scene(1), 64), mb = renderPatch(scene(0), 64);
    expectNear("coated", mc, coated, 1e-3);
    expectNear("bare", mb, bare, 1e-3);
    logf("  coated / bare: rendered %.4f, model %.4f\n", mc.mean / mb.mean, coated / bare);
}

// A10 glass (Dielectric.h): closed forms at normal incidence over the lit patch (point light straight above the patch's
// centre at 3 m, plane albedo 0.5). F = ((n - 1) / (n + 1))^2 for n = 1.5.
//   pane (two-sided, tint 0.9) between the camera and the plane:       L = T_pane L_plane
//   solid slab (12 mm box, transmittance 0.9 over 1 cm) instead:      L = (1 - F)^2 tau L_plane, tau = 0.9^1.2 (one pass)
//   pane between the light and the plane (camera past its edge):       L = T_pane L_plane (the light sample through the pane)
// Each case keeps the surface orders its closed form counts (renderPatch surfaceOrderMax): the camera's glass vertex and
// the plane (2; the slab's entry and exit: 3; light through a pane: 1), without the plane-glass-plane interreflections.
scene::Mesh glassQuad(float y, float half)
{
    scene::Mesh m;
    m.name = "pane";
    m.positions = { { -half, y, -half }, { -half, y, half }, { half, y, half }, { half, y, -half } };
    m.normals = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 } };
    m.uv0 = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };
    m.indices = { 0, 1, 2, 0, 2, 3 };
    m.submeshes = { { 0, 6, 1 } };
    return m;
}
scene::Mesh glassSlab(float y0, float y1, float half)
{
    // an axis-aligned box, faces out (one-sided solid body)
    scene::Mesh m;
    m.name = "slab";
    const float3 lo{ -half, y0, -half }, hi{ half, y1, half };
    const float3 n[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int f = 0; f < 6; ++f)
    {
        const float3 out = n[f];
        const float3 u = std::fabs(out.y) > 0.5f ? float3{ 1, 0, 0 } : float3{ 0, 1, 0 };
        const float3 v = cross(out, u);
        const float3 c = (lo + hi) * 0.5f, h = (hi - lo) * 0.5f;
        auto corner = [&](float a, float b) {
            const float3 q = out + u * a + v * b;
            return float3{ c.x + q.x * h.x, c.y + q.y * h.y, c.z + q.z * h.z };
        };
        const uint32_t base = (uint32_t)m.positions.size();
        for (auto [a, b] : { std::pair{ -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } })
        {
            m.positions.push_back(corner(a, b));
            m.normals.push_back(out);
            m.uv0.push_back({ a * 0.5f + 0.5f, b * 0.5f + 0.5f });
        }
        m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });  // counter-clockwise about out
    }
    m.submeshes = { { 0, 36, 1 } };
    return m;
}
void testGlass()
{
    const double F = 0.04, tint = 0.9;
    const double Tpane = (1 - F) * (1 - F) * tint / (1 - F * F * tint * tint);
    const double tau = std::pow(0.9, 1.2), Tslab = (1 - F) * (1 - F) * tau;
    auto withLight = [](scene::Scene& s) {
        scene::Light l;
        l.type = scene::LightType::Point;
        l.position = { 0, 3, 0 };
        l.intensity = 1000;
        l.range = 30;
        l.castShadow = true;
        s.lights.push_back(l);
    };
    const double bare = lambert(0.5f) * 1000 * windowW(3, 30) / 9.0;
    auto glassMaterial = [](bool twoSided, float3 tint3) {
        scene::Material g;
        g.name = twoSided ? "pane glass" : "solid glass";
        g.cls = scene::MaterialClass::Glass;
        g.baseColor = tint3;
        g.roughness = 0;
        g.ior = 1.5f;
        g.twoSided = twoSided;
        g.attenuationDistance = 0.01f;
        return g;
    };
    {
        scene::Scene s = planeScene(0.5f);
        withLight(s);
        s.materials.push_back(glassMaterial(true, { 0.9f, 0.9f, 0.9f }));
        s.meshes.push_back(glassQuad(10, 5));
        scene::Instance in;
        in.mesh = 1;
        s.instances.push_back(in);
        expectNear("pane", renderPatch(s, 256, true, 2), Tpane * bare, 2e-3);
    }
    {
        scene::Scene s = planeScene(0.5f);
        withLight(s);
        s.materials.push_back(glassMaterial(false, { 0.9f, 0.9f, 0.9f }));
        s.meshes.push_back(glassSlab(10, 10.012f, 5));
        scene::Instance in;
        in.mesh = 1;
        s.instances.push_back(in);
        expectNear("slab", renderPatch(s, 1024, true, 3), Tslab * bare, 2e-3);
    }
    {
        scene::Scene s = planeScene(0.5f);
        withLight(s);
        s.materials.push_back(glassMaterial(true, { 0.9f, 0.9f, 0.9f }));
        s.meshes.push_back(glassQuad(2, 0.5f));  // between the light (3 m) and the patch; the camera (50 m) sees past it
        scene::Instance in;
        in.mesh = 1;
        s.instances.push_back(in);
        s.cameras[0].position = { 15, 50, 0 };  // (its ray crosses y = 2 at x = 0.6: past the pane's edge)
        s.cameras[0].forward = normalize(float3{ -15, -50, 0 });
        expectNear("through", renderPatch(s, 256, true, 1), Tpane * bare, 2e-3);
    }
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

// The sun case split by medium (the GPU tracer once failed "sun" by -46 %): no absorbing air, ozone only, Mie absorption
// only; each against its closed form (optical depth by fine quadrature, the view ray's share included).
void testSunParts()
{
    struct Part
    {
        const char* name;
        float ozone, mie, elevationDegrees;
    };
    for (const Part p : { Part{ "clear90", 0, 0, 90 }, Part{ "clear60", 0, 0, 60 }, Part{ "clear", 0, 0, 30 }, Part{ "clear10", 0, 0, 10 }, Part{ "ozone", 1.881e-6f, 0, 30 },
                          Part{ "mie", 0, 2e-6f, 30 } })
    {
        const float elev = p.elevationDegrees * kPi / 180;
        scene::Scene s = planeScene(0.5f);
        s.atmosphere.ozoneAbsorption = { p.ozone, p.ozone, p.ozone };
        s.atmosphere.mieAbsorption = { p.mie, p.mie, p.mie };
        s.sun.direction = { std::cos(elev), std::sin(elev), 0 };
        s.sun.illuminance = 100000;
        const Measured m = renderPatch(s, 256);
        const double R = s.atmosphere.bottomRadius, Rt = s.atmosphere.topRadius, mu = std::sin(elev);
        const double len = -R * mu + std::sqrt(R * R * (mu * mu - 1) + Rt * Rt);
        const int n = 200000;
        double tau = 0;
        for (int i = 0; i < n; ++i)
        {
            const double t = (i + 0.5) * len / n;
            const double x = t * std::cos(elev), y = R + t * mu;
            const double h = std::sqrt(x * x + y * y) - R;
            tau += (p.mie * std::exp(-h / 1200.0) + p.ozone * std::max(0.0, 1 - std::fabs(h - 25000.0) / 15000.0)) * len / n;
        }
        const double tauView = p.mie * 1200.0 * (1 - std::exp(-50.0 / 1200.0));
        // the floor's full model BRDF (specular 0 means f0 = 0, but the Schlick term (1 - cos)^5 is 0.39 at 80 degrees'
        // incidence: the Lambert-only closed form is off by +6e-4 at a 10 degree sun), viewed straight down
        scene::model::Surface floor;
        floor.baseColor = { 0.5f, 0.5f, 0.5f };
        floor.roughness = 1.0f;
        floor.specular = 0.0f;
        const float f = scene::model::evaluate(floor, float3{ 0, 1, 0 }, float3{ 0, 1, 0 }, s.sun.direction).y;
        expectNear(p.name, m, f * 100000 * std::exp(-tau) * std::sin(elev) * std::exp(-tauView), 2e-4);
    }
}

// Sun caustic: a 2 x 2 m mirror (metal, base colour 1, roughness 0.03 -> alpha 9e-4) reflects the sun onto a Lambert
// floor (albedo 0.1). Floor irradiance at the viewed patch = E_sun (cos(direct) + R cos(mirror image)), R = the mirror's
// directional albedo (v1 metal, F0 = 1: 1 within 0.1 % at this angle, v1_metal_furnace.md). The light tracer
// (default) and the camera paths alone (sunCaustics = false, many more samples) estimate the same expectation.
// Floor -> mirror -> floor interreflection adds < 0.3 % (albedo 0.1, mirror solid angle 0.14 sr).
void testSunCaustic()
{
    scene::Scene s = planeScene(0.1f);
    scene::Material mm;
    mm.name = "mirror";
    mm.baseColor = { 1, 1, 1 };
    mm.metallic = 1;
    mm.roughness = 0.03f;
    s.materials.push_back(mm);
    const float elev = 60.0f * kPi / 180;
    const float3 ws{ std::cos(elev), std::sin(elev), 0 };
    s.sun.direction = ws;
    s.sun.illuminance = 100000;
    const float3 M{ -3, 3, 0 }, r = normalize(float3{ 0, 0, 0 } - M), n = normalize(r + ws);
    const float3 t1{ 0, 0, 1 }, t2 = normalize(cross(n, t1));
    scene::Mesh mesh;
    mesh.name = "mirror";
    mesh.positions = { M - t1 - t2, M + t1 - t2, M + t1 + t2, M - t1 + t2 };
    mesh.normals = { n, n, n, n };
    mesh.uv0 = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };
    if (dot(cross(mesh.positions[1] - mesh.positions[0], mesh.positions[2] - mesh.positions[0]), n) < 0) mesh.indices = { 0, 2, 1, 0, 3, 2 };
    mesh.submeshes = { { 0, 6, 1 } };
    s.meshes.push_back(mesh);
    scene::Instance in;
    in.mesh = 1;
    s.instances.push_back(in);
    s.cameras[0].position = { 0, 5, 0 };
    s.cameras[0].verticalFov = 0.16f;  // 0.8 m patch around the origin, inside the uniform part of the caustic
    const double expected = lambert(0.1f) * 100000 * (std::sin(elev) + 1.0 * dot(float3{ 0, 1, 0 }, -r));
    const Measured lt = renderPatch(s, 256);
    expectNear("caustic", lt, expected, 1e-2);
    const Measured cp = renderPatch(s, 65536, false);
    logf("  caustic  camera paths only (65536 spp): %.6g +- %.2g (light tracer %.6g +- %.2g)\n", cp.mean, cp.stderr_, lt.mean, lt.stderr_);
    if (std::fabs(cp.mean - lt.mean) > 4 * std::sqrt(cp.stderr_ * cp.stderr_ + lt.stderr_ * lt.stderr_) + 1e-3 * lt.mean)
        fail("caustic: camera paths %.6g vs light tracer %.6g", cp.mean, lt.mean);
    if (!(lt.stderr_ < cp.stderr_)) fail("caustic: the light tracer is not less noisy (%.3g vs %.3g)", lt.stderr_, cp.stderr_);
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
    reference::RenderSettings rs;
    rs.width = 4;
    rs.height = 4;
    rs.samplesPerPixel = spp;
    rs.russianRouletteStart = 4;
    rs.samplesPerPass = spp / 2;
    rs.forcedInScattering = forced;
    const reference::RenderOutput out = renderOn(s, reference::resolveCamera(s, { "sky", "", 0 }), rs);
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
        float aniso = 0;  // A9 anisotropy strength (frame: t rotated 0.5 rad about n from x)
        float coat = 0, coatRoughness = 0.05f, coatEta = 1.5f;  // A9 clearcoat
        float3 sheen{ 0, 0, 0 };                                // A9 sheen colour (roughness 0.5)
    };
    const Case cases[] = {
        { scene::MaterialClass::Standard, { 0.5f, 0.5f, 0.5f }, 0.5f, 0.0f, 0.0f, 0.8f },
        { scene::MaterialClass::Standard, { 0.9f, 0.6f, 0.3f }, 0.2f, 1.0f, 0.0f, 0.5f },
        { scene::MaterialClass::Standard, { 0.1f, 0.1f, 0.1f }, 0.35f, 0.0f, 0.0f, 0.26f },
        { scene::MaterialClass::Standard, { 0.2f, 0.2f, 0.2f }, 0.9f, 0.3f, 0.0f, 0.1f },
        { scene::MaterialClass::Foliage, { 0.06f, 0.12f, 0.03f }, 0.5f, 0.0f, 0.3f, 0.7f },
        { scene::MaterialClass::Standard, { 0.95f, 0.64f, 0.54f }, 0.3f, 1.0f, 0.0f, 0.6f, 0.8f },
        { scene::MaterialClass::Standard, { 0.9f, 0.9f, 0.9f }, 0.1f, 1.0f, 0.0f, 0.3f, 0.95f },
        { scene::MaterialClass::Standard, { 0.4f, 0.3f, 0.2f }, 0.5f, 0.0f, 0.0f, 0.9f, 0.5f },
        { scene::MaterialClass::Standard, { 0.8f, 0.8f, 0.8f }, 0.4f, 0.0f, 0.0f, 0.8f, 0, 1.0f, 0.05f, 1.5f },   // glazed tile
        { scene::MaterialClass::Standard, { 0.3f, 0.5f, 0.7f }, 0.3f, 0.0f, 0.0f, 0.3f, 0, 0.6f, 0.2f, 1.33f },  // wet, grazing
        { scene::MaterialClass::Standard, { 0.9f, 0.9f, 0.9f }, 0.3f, 1.0f, 0.0f, 0.6f, 0, 1.0f, 0.1f, 1.5f },   // coated metal
        { scene::MaterialClass::Standard, { 0.2f, 0.1f, 0.4f }, 0.8f, 0.0f, 0.0f, 0.4f, 0, 0, 0.05f, 1.5f, { 0.8f, 0.7f, 0.9f } },  // cloth
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
        s.aniso.strength = c.aniso;
        s.aniso.t = { std::cos(0.5f), std::sin(0.5f), 0 };
        s.aniso.b = { -std::sin(0.5f), std::cos(0.5f), 0 };
        s.coat.cover = c.coat;
        s.coat.roughness = c.coatRoughness;
        s.coat.eta = c.coatEta;
        s.sheen.color = c.sheen;
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
        logf("  bsdf r=%.2f m=%.1f cls=%d nov=%.2f s=%.2f coat=%.1f sheen=%.1f: E_bsdf %.5f  E_uniform %.5f  |diff| %.2e (tol %.2e), integral of pdf %.4f\n", c.roughness, c.metallic,
             (int)c.cls, c.nov, c.aniso, c.coat, c.sheen.y, mB, mU, diff, tol, pdfInt / N);
        if (diff > tol) fail("bsdf sampling inconsistent with evaluation");
        if (c.coat > 0 || c.sheen.y > 0)
        {
            // A9 layers: the evaluation is the shared model's, and the directional albedo by the sampler equals a
            // deterministic quadrature of it (midpoint in (cos theta, phi), 1024 x 1024) within 0.5 %
            const uint32_t Q = 1024;
            double quad = 0;
            for (uint32_t i = 0; i < Q; ++i)
                for (uint32_t j = 0; j < Q; ++j)
                {
                    const float z = (i + 0.5f) / Q, r = std::sqrt(std::max(0.0f, 1 - z * z)), phi = 2 * kPi * (j + 0.5f) / Q;
                    const float3 l{ r * std::cos(phi), r * std::sin(phi), z };
                    const float3 m = c.coat > 0 ? scene::model::evaluateCoated(s.bsdf, s.coat, s.ns, wo, l) : scene::model::evaluateSheen(s.bsdf, s.sheen, s.ns, wo, l);
                    const double e = b.eval(l).g;
                    if (std::fabs(e - m.y) > 1e-5 * std::max(1.0, (double)m.y)) fail("layered reference evaluation differs from the model at (%.3f, %.3f, %.3f)", l.x, l.y, l.z);
                    quad += m.y * z * (2 * kPi / Q / Q);
                }
            logf("    layered albedo: sampler %.5f, quadrature of the model %.5f (%+.3f %%)\n", mB, quad, 100 * (mB / quad - 1));
            if (std::fabs(mB / quad - 1) > 0.005) fail("layered albedo by the sampler differs from the model's integral by more than 0.5 %%");
        }
        if (c.aniso > 0)
        {
            // A9: the double evaluation against the C++ model (MaterialModel.h evaluateAnisotropic), relative to E / pi
            scene::model::Anisotropy a = s.aniso;
            double worst = 0;
            reference::Pcg32 r2(99, 3);
            for (int k = 0; k < 4096; ++k)
            {
                const float z = r2.uniform(), rr = std::sqrt(std::max(0.0f, 1 - z * z)), phi = 2 * kPi * r2.uniform();
                const float3 w{ rr * std::cos(phi), rr * std::sin(phi), std::max(z, 0.01f) };
                const float3 m = scene::model::evaluateAnisotropic(s.bsdf, a, s.ns, wo, normalize(w));
                const double ref = b.eval(normalize(w)).g;  // relative to the value, floored at the lobe's scale E / pi
                worst = std::max(worst, std::fabs(ref - m.y) / std::max(ref, std::max(mB / kPi, 1e-3)));
            }
            logf("    reference vs C++ model (anisotropic): %.2e\n", worst);
            if (worst > 1e-4) fail("anisotropic reference evaluation disagrees with the model");
        }
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
    // A9 thin film: the reference's film (evaluateModel with the Film) is the model's evaluateFilm
    double worstFilm = 0;
    scene::model::Film film;
    film.thickness = 420, film.ior = 1.33f, film.substrate = scene::model::FilmSubstrate::Copper, film.coverage = 0.85f;
    for (uint32_t i = 0; i < 2000; ++i)
    {
        scene::model::Surface s;
        s.baseColor = { rng.uniform(), rng.uniform(), rng.uniform() };
        s.roughness = 0.2f + 0.8f * rng.uniform();
        s.metallic = rng.uniform();
        const float z0 = rng.uniform(), z1 = rng.uniform(), p0 = 2 * kPi * rng.uniform(), p1 = 2 * kPi * rng.uniform();
        const float3 fv{ std::sqrt(1 - z0 * z0) * std::cos(p0), std::sqrt(1 - z0 * z0) * std::sin(p0), z0 };
        const float3 fl{ std::sqrt(1 - z1 * z1) * std::cos(p1), std::sqrt(1 - z1 * z1) * std::sin(p1), z1 };
        if (fv.z < 0.05f || fl.z < 0.05f) continue;
        film.thickness = 2000 * rng.uniform();
        const float3 a = scene::model::evaluateFilm(s, film, n, fv, fl);
        const reference::Rgb b = reference::evaluateModel(s, n, fv, fl, &film);
        const float av[3] = { a.x, a.y, a.z }, bv[3] = { b.r, b.g, b.b };
        for (int c = 0; c < 3; ++c) worstFilm = std::max(worstFilm, std::fabs((double)bv[c] - av[c]) / std::max(1e-3, (double)std::fabs(av[c])));
    }
    logf("  model: thin film, max relative difference to scene::model::evaluateFilm %.2e\n", worstFilm);
    if (worstFilm > 2e-4) fail("evaluateModel with a thin film differs from scene::model::evaluateFilm");
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
    // Live holder (this process): "correctness" does not hold; "timing" and the older kind-less record do.
    const fs::path live = dir / "live.json";
    const unsigned long self = GetCurrentProcessId();
    auto write = [&](const char* kind) {
        std::ofstream o(live);
        o << "{\"track\":\"X\"," << (kind ? std::string("\"kind\":\"") + kind + "\"," : std::string()) << "\"pid\":" << self << ",\"started\":\"t\"}";
    };
    write("correctness");
    const bool corr = reference::holdActive(live);
    write("timing");
    const bool timing = reference::holdActive(live);
    write(nullptr);
    const bool legacy = reference::holdActive(live);
    // A record written long before the process now using its pid was created (a reboot reused the pid) is stale.
    {
        std::ofstream o(live);
        o << "{\"track\":\"X\",\"kind\":\"timing\",\"pid\":" << self << ",\"started\":\"2000-01-01T00:00:00\"}";
    }
    const bool reused = reference::holdActive(live);
    fs::remove(live);
    logf("  hold kinds (live holder): correctness %d, timing %d, no kind %d, reused pid %d\n", corr, timing, legacy, reused);
    if (corr || !timing || !legacy) fail("GPU lock kind not honoured (correctness must not hold; timing and kind-less must)");
    if (reused) fail("a lock record older than its pid's process (reused pid) held the render");
}

// Scene valleys (geometry below the planet surface): the chord's optical depth by opticalDepthToTop's rule (one 8-point
// panel per 2 km of the chord's altitude span) against the same chord with 65,536 panels, for short, long, grazing, deep
// and steep chords: relative error of tau and of the transmittance both <= 1e-4 (a tenth of a 10-bit output step).
void testValleyChords()
{
    scene::Atmosphere atm;
    const reference::AtmosphereModel m(atm, false);
    struct Case
    {
        const char* name;
        reference::Double3 o;
        float3 d;
    };
    const Case cases[] = {
        { "short steep (50 m deep, 73 deg up)", { 0, -50, 0 }, normalize(float3{ 0.3f, 1, 0 }) },
        { "long horizontal (2 km deep)", { 0, -2000, 0 }, float3{ 1, 0, 0 } },
        { "grazing (10 m deep, 0.03 deg up)", { 0, -10, 0 }, normalize(float3{ 1, 0.0005f, 0 }) },
        { "deep, first down (5 km deep)", { 0, -5000, 0 }, normalize(float3{ 1, -0.01f, 0 }) },
        { "middle (800 m deep, 11 deg up)", { 0, -800, 0 }, normalize(float3{ 1, 0.2f, 0 }) },
    };
    double worstTau = 0, worstT = 0;
    for (const Case& c : cases)
    {
        double tExit;
        uint32_t panels;
        const reference::Rgb rule = m.valleyChordDepth(c.o, c.d, tExit, panels);
        const reference::Rgb ref = m.directIntegral(c.o, c.d, 0, tExit, 1u << 16);
        const double rt[3] = { rule.r, rule.g, rule.b }, ft[3] = { ref.r, ref.g, ref.b };
        double relTau = 0, relT = 0;
        for (int k = 0; k < 3; ++k)
        {
            relTau = std::max(relTau, std::abs(rt[k] - ft[k]) / std::max(ft[k], 1e-30));
            relT = std::max(relT, std::abs(std::exp(-rt[k]) - std::exp(-ft[k])) / std::exp(-ft[k]));
        }
        worstTau = std::max(worstTau, relTau);
        worstT = std::max(worstT, relT);
        logf("  valley %-38s chord %.4g km, %u panels: tau %.6g %.6g %.6g (65536 panels %.6g %.6g %.6g), relative error tau %.2e, T %.2e\n", c.name,
             tExit / 1000, panels, rule.r, rule.g, rule.b, ref.r, ref.g, ref.b, relTau, relT);
    }
    if (worstTau > 1e-4 || worstT > 1e-4) fail("valley chord quadrature error: tau %.3g, T %.3g (> 1e-4)", worstTau, worstT);
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

// A11 cut face surface (CutFace.h, INTERFACES 8.1 Cut): an axis-facing square and a 45 degree square, each one Cut
// submesh (its outline is the mesh border, so every outline edge is a boundary edge), a 4 x 4 texture of distinct colours.
//   - axis face: base colour = the texture at (cutScale p_x, cutScale p_y) away from the outline; within 0.35 x the damage
//     width of the outline the damage band always covers (d = 1: base x 0.55, roughness 1 - 0.4 (1 - r)); beyond the width
//     never;
//   - 45 degree face: base colour = the mean of the x and y projections' taps (weights 1/2); with no normal texture the
//     shading normal is the geometric normal (whiteout of a flat texel is exact).
// Water (Dielectric.h): the plane scene's plane becomes an emitting, non-reflecting floor at y = 0 under a water surface
// at y = d (a single-sided quad facing up, out of the water); black sky, no sun. Every path from the camera reflects off
// the interface with probability F (then reaches the black sky or, from below, the floor) or refracts, so the estimate
// is a Bernoulli mixture of exact terms: no light sampling happens at the delta interface. The emitters are Standard
// surfaces of albedo 0, which still reflect by Schlick's (1 - v.h)^5 at f0 = 0 (a few % of directional albedo for a
// rough lobe); light they reflect back into the water is a second-order term of the Standard model, not of the
// interface, so the renders keep surface order 1 (the interface vertex; the emitters' emission is counted at it).
double dielectricF(double cosI, double eta)
{
    const double s2 = eta * eta * (1 - cosI * cosI);
    if (s2 >= 1) return 1;
    const double ct = std::sqrt(1 - s2), rs = (eta * cosI - ct) / (eta * cosI + ct), rp = (eta * ct - cosI) / (eta * ct + cosI);
    return 0.5 * (rs * rs + rp * rp);
}

scene::Scene waterScene(float depth, float transmittance, float floorNits, float ceilingNits)
{
    scene::Scene s = planeScene(0.0f);
    s.materials[0].emissive = { floorNits, floorNits, floorNits };
    scene::Material water;
    water.name = "water";
    water.cls = scene::MaterialClass::Water;
    water.baseColor = { transmittance, transmittance, transmittance };
    water.roughness = 0.02f;
    water.ior = 1.333f;
    s.materials.push_back(water);
    auto quad = [&](float y, bool up, uint32_t material) {
        scene::Mesh m = s.meshes[0];
        m.name = up ? "up" : "down";
        for (float3& p : m.positions) p.y = y;
        for (float3& n : m.normals) n = { 0, up ? 1.0f : -1.0f, 0 };
        if (!up) m.indices = { 0, 2, 1, 0, 3, 2 };
        m.submeshes = { { 0, 6, material } };
        s.meshes.push_back(m);
        scene::Instance in;
        in.mesh = (uint32_t)s.meshes.size() - 1;
        s.instances.push_back(in);
    };
    quad(depth, true, 1);
    if (ceilingNits > 0)
    {
        scene::Material ceiling = s.materials[0];
        ceiling.name = "ceiling";
        ceiling.emissive = { ceilingNits, ceilingNits, ceilingNits };
        s.materials.push_back(ceiling);
        quad(depth + 1, false, 2);
    }
    return s;
}

void aim(scene::Scene& s, float3 position, float3 forward)
{
    s.cameras[0].position = position;
    s.cameras[0].forward = normalize(forward);
    s.cameras[0].up = { 0, 0, -1 };  // every direction here lies in the xy plane
}

void testWater()
{
    const double n = 1.333, d = 0.5, T = 0.945, Le = 100;
    // From above: normal incidence and 50 degrees from the normal (camera 50 m away, looking at the surface point x = 0).
    for (double deg : { 0.0, 50.0 })
    {
        const double th = deg * kPi / 180, cosI = std::cos(th), sinT = std::sin(th) / n, cosT = std::sqrt(1 - sinT * sinT);
        scene::Scene s = waterScene((float)d, (float)T, (float)Le, 0);
        aim(s, { (float)(50 * std::sin(th)), (float)(d + 50 * cosI), 0 }, { (float)-std::sin(th), (float)-cosI, 0 });
        const double expected = (1 - dielectricF(cosI, 1 / n)) * std::pow(T, d / cosT) * Le / (n * n);
        expectNear(deg == 0 ? "above0" : "above50", renderPatch(s, 256, true, 1), expected, 1e-5);
    }
    // From below (sigma = 0: the camera starts inside the body, which the tracer's medium state only learns at a
    // refraction through the front face). 60 degrees is past the critical angle asin(1/n) = 48.6: every path reflects.
    {
        scene::Scene s = waterScene((float)d, 1.0f, (float)Le, 300);
        const double th = 60 * kPi / 180;
        aim(s, { 0, (float)(d - 0.2), 0 }, { (float)std::sin(th), (float)std::cos(th), 0 });
        expectNear("tir60", renderPatch(s, 64, true, 1), Le, 1e-5);
    }
    {
        scene::Scene s = waterScene((float)d, 1.0f, (float)Le, 300);
        const double th = 30 * kPi / 180, F = dielectricF(std::cos(th), n);
        aim(s, { 0, (float)(d - 0.2), 0 }, { (float)std::sin(th), (float)std::cos(th), 0 });
        expectNear("below30", renderPatch(s, 256, true, 1), F * Le + (1 - F) * n * n * 300, 1e-5);
    }
}

void testCutFace()
{
    using namespace unx::reference;
    scene::Scene sc;
    sc.name = "cut face";
    scene::Texture tex;
    tex.width = tex.height = 4;
    tex.format = scene::TextureFormat::Rgba8Linear;
    for (uint32_t i = 0; i < 16; ++i) tex.texels.insert(tex.texels.end(), { (uint8_t)(16 * i), (uint8_t)(255 - 12 * i), (uint8_t)(40 + 9 * i), 255 });
    sc.textures.push_back(tex);
    sc.materials.resize(1);
    sc.materials[0].cls = scene::MaterialClass::Cut;
    sc.materials[0].baseColor = { 1, 1, 1 };
    sc.materials[0].roughness = 0.5f;
    sc.materials[0].baseColorTexture = 0;
    sc.materials[0].cutScale = 0.5f;
    sc.materials[0].cutDamageWidth = 0.04f;
    auto square = [](float3 a, float3 b, float3 c, float3 d) {
        scene::Mesh m;
        m.name = "cut square";
        m.positions = { a, b, c, d };
        const float3 n = normalize(cross(b - a, d - a));
        m.normals = { n, n, n, n };
        m.indices = { 0, 1, 2, 0, 2, 3 };
        m.submeshes.push_back({ 0, 6, 0 });
        return m;
    };
    sc.meshes.push_back(square({ 0, 0, 0 }, { 1, 0, 0 }, { 1, 1, 0 }, { 0, 1, 0 }));                  // faces +z
    sc.meshes.push_back(square({ 3, 0, 0 }, { 3, 0, 1 }, { 4, 1, 1 }, { 4, 1, 0 }));                  // faces (-1, 1, 0) / sqrt 2
    sc.instances.resize(2);
    sc.instances[0].mesh = 0;
    sc.instances[1].mesh = 1;
    scene::validate(sc);
    RtScene rt(sc, 0.0f, 1);
    Texture reference(tex);
    auto hitAt = [&](float3 o, float3 dir) {
        Hit h;
        if (!rt.intersect(o, dir, 0, 100, ~0u, h)) fail("cut face: ray missed");
        return rt.surface(h, dir);
    };
    auto near3 = [](float3 a, float3 b, float tol) { return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol && std::fabs(a.z - b.z) <= tol; };
    auto rgb = [](Texel t) { return float3{ t.r, t.g, t.b }; };
    uint32_t checked = 0, damaged = 0;
    for (uint32_t i = 0; i < 40; ++i)
        for (uint32_t j = 0; j < 40; ++j)
        {
            const float x = (i + 0.37f) / 40, y = (j + 0.61f) / 40;
            const Surface s = hitAt({ x, y, 1 }, { 0, 0, -1 });
            const float edge = std::min(std::min(x, 1 - x), std::min(y, 1 - y));
            const float3 tap = rgb(reference.sample({ 0.5f * x, 0.5f * y }));
            if (edge > 0.04f)
            {
                if (!near3(s.bsdf.baseColor, tap, 1e-5f)) fail("cut face: base colour at (%.3f, %.3f) is not the z projection's tap", x, y);
                if (std::fabs(s.bsdf.roughness - 0.5f) > 1e-6f) fail("cut face: damage outside the band at (%.3f, %.3f)", x, y);
                ++checked;
            }
            else if (edge < 0.35f * 0.04f)
            {
                if (!near3(s.bsdf.baseColor, tap * 0.55f, 1e-5f) || std::fabs(s.bsdf.roughness - 0.8f) > 1e-5f) fail("cut face: no damage at (%.3f, %.3f), %.4f from the outline", x, y, edge);
                ++damaged;
            }
        }
    // 45 degree face: object normal (-1, 1, 0) / sqrt 2 -> projections x (sign -1) and y (sign +1) at weight 1/2 each.
    const float3 ng = normalize(float3{ -1, 1, 0 });
    for (uint32_t k = 0; k < 16; ++k)
    {
        const float a = 0.2f + 0.04f * k, z = 0.3f + 0.025f * k;
        const float3 p{ 3 + a, a, z };
        const Surface s = hitAt(p + ng * 2.0f, -ng);
        const float3 tapX = rgb(reference.sample({ -0.5f * p.y, 0.5f * p.z })), tapY = rgb(reference.sample({ 0.5f * p.z, 0.5f * p.x }));
        if (!near3(s.bsdf.baseColor, (tapX + tapY) * 0.5f, 1e-4f)) fail("cut face: 45 degree base colour is not the mean of the x and y taps");
        if (!near3(s.ns, ng, 1e-5f)) fail("cut face: flat whiteout moved the normal");
        ++checked;
    }
    logf("  %u undamaged and %u damaged samples match the definition\n", checked, damaged);
    if (damaged == 0) fail("cut face: no sample inside the damage band");
}

int main(int argc, char** argv)
{
    try
    {
        int arg = 1;
        if (argc > arg && std::strcmp(argv[arg], "--gpu") == 0)
        {
            g_gpu = true;
            ++arg;
        }
        const char* only = argc > arg ? argv[arg] : nullptr;
        auto run = [&](const char* name, void (*fn)()) {
            if (only && std::strcmp(only, name) != 0) return;
            const bool cpuOnly = std::strcmp(name, "glass") == 0 || std::strcmp(name, "hold") == 0 || std::strcmp(name, "atmosphere") == 0 || std::strcmp(name, "model") == 0 || std::strcmp(name, "bsdf") == 0 || std::strcmp(name, "cut") == 0 || std::strcmp(name, "water") == 0;
            if (g_gpu && cpuOnly) return;
            logf("[%s]\n", name);
            fn();
        };
        run("hold", testHold);
        run("valley", testValleyChords);
        run("atmosphere", testAtmosphereTable);
        run("model", testModelAgreement);
        run("bsdf", testBsdfSampling);
        run("cut", testCutFace);
        run("water", testWater);
        run("point", testPoint);
        run("coat", testCoat);
        run("glass", testGlass);
        run("rect", testRect);
        run("sphere", testSphere);
        run("disk", testDiskAndTube);
        run("shadow", testShadow);
        run("sunparts", testSunParts);
        run("sun", testSunAbsorbing);
        run("caustic", testSunCaustic);
        run("skythin", testSkySingleScatter);
        run("sky", testSkyEstimators);
        logf(g_gpu ? "reference tests passed (GPU tracer)\n" : "reference tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAILED: %s\n", e.what());
        return 1;
    }
}
