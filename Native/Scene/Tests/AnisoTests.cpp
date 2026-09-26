// A9 anisotropy (MATERIAL_LAYERS 1.5), CPU: the (A, B) table against an independent estimator at its grid points and
// between them, the metal white furnace of the full model (energy 1 up to the table's error), the lobe's reciprocity,
// the isotropic limit, the frame's rules, the cooked-tangent versus uv-derivative-tangent continuity study
// (--study <path>: Results/C/Aniso/tangent_continuity.txt), and the .unxscene anisotropy block with its validation.
#include "unx/core/Log.h"
#include "unx/scene/MaterialModel.h"
#include "unx/scene/SceneData.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::scene;
using namespace unx::scene::model;

namespace
{
uint32_t g_failures = 0;
#define CHECK(c) \
    do { if (!(c)) { std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); ++g_failures; } } while (0)

constexpr double kPiD = 3.14159265358979323846;

float3 viewAt(float mu, float phi)
{
    const float s = std::sqrt(std::max(0.0f, 1 - mu * mu));
    return { s * std::cos(phi), s * std::sin(phi), mu };
}

// Independent: the cosine-weighted lobe D V cos (F = 1) over the hemisphere, sampled in the lobe's own stretched solid
// angle by a Fibonacci set of microfacet normals (h uniform in the projected stretched disk; the Jacobian dl/dh and the
// density are applied explicitly, no visible-normal sampling as the table uses).
double albedoIndependent(float3 v, float at, float ab)
{
    const uint32_t count = 1u << 21;
    double sum = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        // h from the GGX slope distribution's inverse CDF (radius ~ tan theta_h in stretched slope space): its pdf is
        // D(h) h.n exactly, so the weight is D V cos / pdf(l) = V cos 4 (v.h) / (h.n)
        const double u1 = (i + 0.5) / count, u2 = std::fmod(i * 0.6180339887498949, 1.0);
        const double r = std::sqrt(u1 / (1 - u1)), ph = 2 * kPiD * u2;
        double hx = -at * r * std::cos(ph), hy = -ab * r * std::sin(ph), hz = 1;
        const double hl = std::sqrt(hx * hx + hy * hy + hz * hz);
        hx /= hl, hy /= hl, hz /= hl;
        const double voh = v.x * hx + v.y * hy + v.z * hz;
        if (voh <= 0) continue;
        const float3 l{ (float)(2 * voh * hx - v.x), (float)(2 * voh * hy - v.y), (float)(2 * voh * hz - v.z) };
        if (l.z <= 0) continue;
        const double w = visibilitySmithGgxAniso(v, l, at, ab) * l.z * 4 * voh / hz;
        sum += w;
    }
    return sum / count;
}

bool fails(const Scene& s)
{
    try
    {
        validate(s);
    }
    catch (const std::exception&)
    {
        return true;
    }
    return false;
}

Scene sceneWithAnisotropy(float strength, float rotation, bool tangents)
{
    Scene s;
    s.name = "aniso";
    Material m;
    m.name = "brushed";
    m.metallic = 1;
    m.roughness = 0.3f;
    m.anisotropy = strength;
    m.anisotropyRotation = rotation;
    s.materials.push_back(m);
    Mesh mesh;
    mesh.name = "quad";
    mesh.positions = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 } };
    mesh.normals = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 } };
    mesh.uv0 = { { 0, 0 }, { 1, 0 }, { 0, 1 } };
    if (tangents) mesh.tangents = { { 1, 0, 0, 1 }, { 1, 0, 0, 1 }, { 1, 0, 0, 1 } };
    mesh.indices = { 0, 2, 1 };
    mesh.submeshes.push_back({ 0, 3, 0 });
    s.meshes.push_back(mesh);
    Instance in;
    in.mesh = 0;
    s.instances.push_back(in);
    return s;
}

// Tangent study: a flat disc (normal +y) of K fan triangles, brushed radially (a knob, a pan bottom: uv = (radius,
// angle), the tangent d/du points outwards). The cooked tangent is the per-vertex radial direction, interpolated, so it
// turns smoothly about the normal; the uv-derivative tangent is dp/du of each triangle, the radial direction at the
// triangle's mid-angle, constant per triangle. At a rim point of a shared edge the two triangles' uv-derivative tangents
// differ by 360/K degrees; the cooked tangent there is one shared vertex tangent. Views and lights over the hemisphere
// where the lobe is lit (f cos > 5 % of the max) give the radiance jump across the edge: the visible seam. (Brushing
// along a cylinder's circumference shows no seam either way: the chords project onto the same direction.)
void tangentStudy(FILE* out)
{
    std::fprintf(out, "# cooked vs uv-derivative tangent across a fan edge of a radially brushed disc (A9 anisotropy; AnisoTests --study)\n");
    std::fprintf(out, "# K segments, s, r: max relative jump of f cos over views/lights with f cos > 5 %% of the max\n");
    for (uint32_t K : { 16u, 32u, 64u })
        for (float strength : { 0.5f, 0.9f })
        {
            const float r = 0.3f;
            const double dth = 2 * kPiD / K;
            const float3 n{ 0, 1, 0 };
            const float3 cooked{ 1, 0, 0 };  // radial at the edge's angle 0
            const float3 uvA{ (float)std::cos(-dth / 2), 0, (float)std::sin(-dth / 2) };  // mid-angles of the two triangles
            const float3 uvB{ (float)std::cos(dth / 2), 0, (float)std::sin(dth / 2) };
            Surface s;
            s.baseColor = { 1, 1, 1 };
            s.metallic = 1;
            s.roughness = r;
            double worstUv = 0, peak = 0;
            struct Pair { float3 v, l; };
            std::vector<Pair> pairs;
            for (float el = 0.05f; el <= 1.45f; el += 0.1f)
                for (float az = 0; az < 6.28f; az += 0.1f)
                {
                    const float3 v = normalize(float3{ std::sin(el) * std::cos(az), std::cos(el), std::sin(el) * std::sin(az) });
                    const float3 l = normalize(float3{ -std::sin(el * 0.8f) * std::cos(az + 0.4f), std::cos(el * 0.8f), -std::sin(el * 0.8f) * std::sin(az + 0.4f) });
                    if (v.y > 0.05f && l.y > 0.05f) pairs.push_back({ v, l });
                }
            auto fcos = [&](float3 tangent, const Pair& p) {
                Anisotropy a;
                a.strength = strength;
                float3 t, b;
                anisoFrame(tangent, 1, n, 0, n, t, b);
                a.t = t, a.b = b;
                return evaluateAnisotropic(s, a, n, p.v, p.l).x * p.l.y;
            };
            for (const Pair& p : pairs) peak = std::max(peak, (double)fcos(cooked, p));
            for (const Pair& p : pairs)
            {
                const double a = fcos(uvA, p), b = fcos(uvB, p);
                if (std::max(a, b) < 0.05 * peak) continue;
                worstUv = std::max(worstUv, std::abs(a - b) / std::max(a, b));
            }
            // the cooked tangent is one vertex tangent shared by both triangles at the edge: no jump
            std::fprintf(out, "K %3u  s %.1f  r %.1f  cooked jump 0  uv-derivative jump %.3g  (tangent turn %.2f deg)\n", K, strength, r, worstUv,
                         dth * 180 / kPiD);
        }
}
} // namespace

int main(int argc, char** argv)
{
    std::string studyPath;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--study" && i + 1 < argc) studyPath = argv[++i];
    try
    {
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<float>& table = anisoAlbedoTable();
        const double buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("aniso table: %zu floats built in %.0f ms\n", table.size(), buildMs);
        CHECK(table.size() == kAnisoTableSize);
        bool bounded = true;
        for (size_t i = 0; i < table.size(); i += 2) bounded &= table[i] >= 0 && table[i + 1] >= 0 && table[i] + table[i + 1] <= 1.0001f;
        CHECK(bounded);

        // 1. grid points (no interpolation) against the independent estimator, alphas >= (4/15)^2
        double nodeErr = 0;
        for (uint32_t jt : { 4u, 8u, 15u })
            for (uint32_t jb : { 4u, 6u, 11u })
                for (uint32_t k : { 0u, 5u, 12u })
                    for (uint32_t i : { 6u, 16u, 31u })
                    {
                        const float at = std::pow((float)jt / 15, 2.0f), ab = std::pow((float)jb / 15, 2.0f);
                        const float mu = std::pow((float)i / 31, 2.0f), phi = 0.5f * 3.14159265f * k / 12;
                        const float3 v = viewAt(mu, phi);
                        const float2 e = anisoSpecularAlbedo(v, at, ab);
                        nodeErr = std::max(nodeErr, std::abs((double)e.x + e.y - albedoIndependent(v, at, ab)));
                    }
        std::printf("table at grid points vs independent estimator: %.2e (abs.)\n", nodeErr);
        CHECK(nodeErr < 3e-3);

        // 2. between grid points (the interpolation error the header states: 0.0087 worst, 0.0058 at n.v >= 0.05)
        double midErr = 0, midErrFacing = 0;
        uint32_t state = 12345;
        auto rnd = [&]() { state = state * 1664525u + 1013904223u; return (state >> 8) * (1.0f / 16777216.0f); };
        for (int q = 0; q < 120; ++q)
        {
            const float mu = std::max(rnd() * rnd(), 0.01f), phi = rnd() * 1.5707963f;
            const float at = std::max(std::pow(0.25f + 0.75f * rnd(), 2.0f), 1e-4f), ab = std::max(std::pow(0.25f + 0.75f * rnd(), 2.0f), 1e-4f);
            const float3 v = viewAt(mu, phi);
            const float2 e = anisoSpecularAlbedo(v, at, ab);
            const double d = std::abs((double)e.x + e.y - albedoIndependent(v, at, ab));
            midErr = std::max(midErr, d);
            if (mu >= 0.05f) midErrFacing = std::max(midErrFacing, d);
        }
        std::printf("table between grid points vs independent: %.2e (n.v >= 0.05: %.2e)\n", midErr, midErrFacing);
        CHECK(midErr < 1.2e-2);
        CHECK(midErrFacing < 8e-3);

        // 3. white furnace: a white metal (f0 = 1) reflects all the energy: integral of f cos = E_a (1 / E_a) = 1 up to
        //    the table's error at the view (the estimator integrates D V cos; the compensation is the same table value)
        double furnace = 0;
        for (float mu : { 0.1f, 0.4f, 0.8f, 1.0f })
            for (float s : { 0.5f, 0.9f })
                for (float r : { 0.3f, 0.6f })
                {
                    const float2 al = anisoAlphas(r, s);
                    const float3 v = viewAt(mu, 0.6f);
                    const float2 e = anisoSpecularAlbedo(v, al.x, al.y);
                    const double integral = albedoIndependent(v, al.x, al.y) * (1 + (1 / ((double)e.x + e.y) - 1));
                    furnace = std::max(furnace, std::abs(integral - 1));
                }
        std::printf("metal white furnace: |energy - 1| <= %.2e\n", furnace);
        CHECK(furnace < 1.5e-2);

        // 4. reciprocity of the lobe, and the isotropic limit (s = 0: the isotropic model up to the two tables' error)
        Surface metal;
        metal.baseColor = { 0.9f, 0.6f, 0.3f };
        metal.metallic = 1;
        metal.roughness = 0.4f;
        double recip = 0, isoLimit = 0;
        for (int q = 0; q < 400; ++q)
        {
            const float3 v = viewAt(0.05f + 0.95f * rnd(), 6.2831853f * rnd()), l = viewAt(0.05f + 0.95f * rnd(), 6.2831853f * rnd());
            Anisotropy a;
            a.strength = 0.8f;
            float3 t, b;
            CHECK(anisoFrame({ 1, 0.2f, 0 }, 1, { 0, 0, 1 }, 0.7f, { 0, 0, 1 }, t, b));
            a.t = t, a.b = b;
            const float2 al = anisoAlphas(metal.roughness, a.strength);
            const float3 vl{ dot(v, t), dot(v, b), v.z }, ll{ dot(l, t), dot(l, b), l.z };
            const float3 h = normalize(v + l);
            const double dv1 = distributionGgxAniso(dot(h, t), dot(h, b), h.z, al.x, al.y) * visibilitySmithGgxAniso(vl, ll, al.x, al.y);
            const double dv2 = distributionGgxAniso(dot(h, t), dot(h, b), h.z, al.x, al.y) * visibilitySmithGgxAniso(ll, vl, al.x, al.y);
            recip = std::max(recip, std::abs(dv1 - dv2) / std::max(dv1, 1e-6));
            Anisotropy zero = a;
            zero.strength = 0;
            const float3 fa = evaluateAnisotropic(metal, zero, { 0, 0, 1 }, v, l), fi = evaluate(metal, { 0, 0, 1 }, v, l);
            isoLimit = std::max(isoLimit, (double)std::abs(fa.x - fi.x) / std::max(fi.x, 1e-3f));
        }
        std::printf("lobe reciprocity %.2e, s = 0 vs the isotropic model %.2e (relative; the two albedo tables' difference)\n", recip, isoLimit);
        CHECK(recip < 1e-5);
        CHECK(isoLimit < 2e-2);

        // 5. the frame: orthonormal about n, rotation pi/2 turns the tangent to the bitangent, degenerate input refused
        {
            float3 t, b;
            const float3 n = normalize(float3{ 0.2f, 0.1f, 1 });
            CHECK(anisoFrame({ 2, 0, 0.3f }, -1, { 0, 0, 1 }, 0, n, t, b));
            CHECK(std::abs(dot(t, n)) < 1e-6f && std::abs(dot(b, n)) < 1e-6f && std::abs(length(t) - 1) < 1e-6f && std::abs(dot(t, b)) < 1e-6f);
            float3 t2, b2;
            CHECK(anisoFrame({ 1, 0, 0 }, -1, { 0, 0, 1 }, 1.5707963f, { 0, 0, 1 }, t2, b2));
            CHECK(std::abs(t2.y + 1) < 1e-6f);  // sign -1: B = -(z x x) = -y
            CHECK(!anisoFrame({ 0, 0, 1 }, 1, { 0, 0, 1 }, 0, { 0, 0, 1 }, t, b));
        }

        // 6. .unxscene block and validation
        {
            const Scene s = sceneWithAnisotropy(0.7f, 0.4f, true);
            validate(s);
            const Scene r = deserialize(serialize(s));
            CHECK(r.materials[0].anisotropy == 0.7f && r.materials[0].anisotropyRotation == 0.4f);
            const Scene plain = sceneWithAnisotropy(0, 0, true);
            CHECK(serialize(plain).size() + 4 + 8 + 12 == serialize(s).size());  // the block only when anisotropic
            CHECK(fails(sceneWithAnisotropy(1.5f, 0, true)));
            CHECK(fails(sceneWithAnisotropy(0.5f, 0, false)));  // no tangents
            Scene foliage = sceneWithAnisotropy(0.5f, 0, true);
            foliage.materials[0].cls = MaterialClass::Foliage;
            CHECK(fails(foliage));
        }

        if (!studyPath.empty())
        {
            FILE* f = nullptr;
            if (fopen_s(&f, studyPath.c_str(), "w") || !f) fail("cannot write %s", studyPath.c_str());
            tangentStudy(f);
            std::fclose(f);
            std::printf("tangent study written to %s\n", studyPath.c_str());
        }
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    std::printf(g_failures ? "FAIL (%u)\n" : "PASS\n", g_failures);
    return g_failures ? 1 : 0;
}
