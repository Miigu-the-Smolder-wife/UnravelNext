// M shading correctness (no GPU lock). Builds without S and R: sun without atmosphere or shadows, no indirect light.
//   1. the f0-split specular albedo table: A + B = the model's E table;
//   2. the solar-disk specular integral (shSunSpecular) against a dense CPU quadrature of the model BRDF over the disk,
//      for roughness 0.01 .. 0.5 and mirror directions inside, on and outside the disk edge;
//   3. a lit scene end to end (stand-in V, M resolve, M shading, linear output): every sampled pixel against the model
//      evaluated on the CPU from the read-back G-buffer and material word;
//   4. the solar disk in the sky: per-pixel coverage against 32 x 32 supersampling and the total flux against E;
//   5. the display encoding (PBR Neutral + sRGB OETF, RGB10A2) against the CPU on the linear image;
//   6. local lights (froxel list, shadow slots) against the CPU;
//   7. the edge composite's exact triangle area (edgeTriangleArea) against double-precision clipping, and the composite
//      of two overlapping quads against their exact visible areas;
//   8. the edge composite over an alpha-tested (cut-out) quad against the supersampled cut shape (the pixel's share
//      where the bilinear alpha passes the cutoff), so cut edges keep their width;
//   9. area lights (AreaLight.hlsli): the diffuse integrals (front, back) and the LTC integral over the true light shape
//      against surface-grid integration in double (exactness of the region integration), and the LTC specular against
//      the model BRDF integrated over the light (the fit error, reported by roughness).
//  10. area lights: the closed forms against each light's outline as a dense polygon in double, 6,000 cases (random
//      shapes, the scenes' sizes, the horizon through the light): P99 and worst relative error of every integral.
//  11. coverage composite: band B triangles as V's coverage records (exact area, subsample masks, centroid depth) over
//      a band A ground, each pixel with fragments against a CPU composite of CPU-shaded fragments, the rest unchanged.
//  12. glass over the translucent layer (A10, TranslucentComposite.hlsl): a two-sided Glass pane given to the shading
//      track as V's translucent layer (vis ids and classes from a CPU ray cast) over the stand-in band A scene; every
//      class 1 pixel against R_p x (single-scatter GGX sun specular with F = 1) x exposure + T_p x (the same pixel
//      shaded without the layer), with the exact dielectric Fresnel in double; every other pixel unchanged; the
//      statistics count the pane's pixels.
//  13. the Subsurface class's scatter pass (shading.subsurface_scatter): the switch off, on without a pixel scattering,
//      and on, against each other and against the estimate's limit on the CPU (testSubsurfaceScatter; --subsurface: alone);
//      then the same three frames in a planar reflection view of the scene (the class's PLANAR = 1 kernels and the
//      view's own scatter pass): the split kernels without scattering against the unsplit one, the scatter changing the
//      class's pixels and no other.
//   unx_test_shading_shadingtests [--no-debug-layer] [--gbv] [--glass] [--set key=value ...]   (--gbv: GPU-based validation;
//   --set output.band_pixels=65536 runs the banded passes with 8 bands at the tests' 960 x 540)
#include "FilmCurve.h"
#include "AreaQuadReplica.h"
#include "../../Material/Tests/MTestFrame.h"

#include "unx/scene/MaterialModel.h"
#include "unx/shading/ShadingSystem.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <cstdlib>
#include <random>
#include <span>
#include <thread>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;
namespace model = unx::scene::model;

namespace
{
constexpr double kPi = 3.14159265358979323846;
bool g_areaDump = false;  // --area-dump: print test 10's failing cases in full precision

struct Report
{
    int failures = 0;
    void operator()(bool ok, const char* what, double value, double limit)
    {
        logf("%-70s %.3e (limit %.1e) %s\n", what, value, limit, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }
};

float3 rotateTowards(float3 a, float3 axis, double angle)  // Rodrigues, axis unit and perpendicular to a
{
    const double c = std::cos(angle), s = std::sin(angle);
    const float3 k = axis;
    return normalize(a * (float)c + cross(k, a) * (float)s + k * (float)(dot(k, a) * (1 - c)));
}

// Dense CPU reference of the solar-disk specular integral, E = 1 lux: int_cap f_s cos dw x L_sun.
//  - alpha >= theta_s: uniform grid over the cap (uniform in cos theta x phi), mean of f_s cos times L Omega;
//  - alpha < theta_s: the lobe is narrower than the disk, so the cap grid would not resolve it; instead a stratified
//    grid over the GGX distribution of normals (D sampling), keeping the directions inside the cap:
//    f_s cos / pdf_l = 4 V F comp NoL VoH / NoH (D cancels), times L_sun = 1 / (pi sin^2 theta_s).
float3 sunSpecularReference(const model::Surface& s, float3 n, float3 v, float3 l0, double thetaS, uint32_t nr, uint32_t na)
{
    const float3 diffuse = s.baseColor * ((1 - s.metallic) / model::kPi);
    const double cosS = std::cos(thetaS);
    const double alpha = model::alphaFromRoughness(s.roughness);
    double sum[3] = {};
    if (alpha >= thetaS)
    {
        const float3 t = normalize(std::fabs(l0.y) < 0.99f ? cross(float3{ 0, 1, 0 }, l0) : cross(float3{ 1, 0, 0 }, l0));
        const float3 b = cross(l0, t);
        for (uint32_t i = 0; i < nr; ++i)
        {
            const double c = 1 - (i + 0.5) / nr * (1 - cosS), sn = std::sqrt(std::max(1 - c * c, 0.0));
            for (uint32_t k = 0; k < na; ++k)
            {
                const double ph = (k + 0.5) / na * 2 * kPi;
                const float3 l = normalize(l0 * (float)c + (t * (float)std::cos(ph) + b * (float)std::sin(ph)) * (float)sn);
                const float NoL = dot(n, l), NoV = dot(n, v);
                if (NoL <= 0 || NoV <= 0) continue;
                const float3 f = model::evaluate(s, n, v, l) - diffuse;
                sum[0] += f.x * NoL;
                sum[1] += f.y * NoL;
                sum[2] += f.z * NoL;
            }
        }
        const double scale = 2 / (1 + cosS) / ((double)nr * na);
        return { (float)(sum[0] * scale), (float)(sum[1] * scale), (float)(sum[2] * scale) };
    }
    const float3 t = normalize(std::fabs(n.y) < 0.99f ? cross(float3{ 0, 1, 0 }, n) : cross(float3{ 1, 0, 0 }, n));
    const float3 b = cross(n, t);
    const float NoV = dot(n, v);
    const float3 f0 = model::f0(s);
    const float e = model::directionalAlbedo(NoV, s.roughness);
    const uint32_t N = 1536;
    for (uint32_t i = 0; i < N; ++i)
    {
        const double u = (i + 0.5) / N, tan2 = alpha * alpha * u / (1 - u), ch = 1 / std::sqrt(1 + tan2), sh = std::sqrt(std::max(0.0, 1 - ch * ch));
        for (uint32_t k = 0; k < N; ++k)
        {
            const double ph = (k + 0.5) / N * 2 * kPi;
            const float3 h = normalize(n * (float)ch + (t * (float)std::cos(ph) + b * (float)std::sin(ph)) * (float)sh);
            const float VoH = dot(v, h);
            if (VoH <= 0) continue;
            const float3 l = h * (2 * VoH) - v;
            if (dot(l, l0) < cosS) continue;
            const float NoL = dot(n, l), NoH = dot(n, h);
            if (NoL <= 0) continue;
            const float V = model::visibilitySmithGgxCorrelated(NoV, NoL, (float)alpha);
            const float3 F = model::fresnelSchlick(f0, VoH);
            const double w = 4.0 * V * NoL * VoH / NoH;
            for (int c = 0; c < 3; ++c) sum[c] += w * (&F.x)[c] * (1 + (&f0.x)[c] * (1 / e - 1));
        }
    }
    const double L = 1 / (kPi * std::sin(thetaS) * std::sin(thetaS)), scale = L / ((double)N * N);
    return { (float)(sum[0] * scale), (float)(sum[1] * scale), (float)(sum[2] * scale) };
}

double srgbToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }
double oetf(double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055; }

void pixelRay(const ViewDesc& v, double px, double py, double D[3], double Dx[3])
{
    const auto& P = v.proj.m;
    const double ndcx = px / v.width * 2 - 1, ndcy = 1 - py / v.height * 2;
    const double vx = (ndcx + P[0][2] - P[0][3]) / P[0][0], vy = (ndcy + P[1][2] - P[1][3]) / P[1][1];
    for (int k = 0; k < 3; ++k)
    {
        D[k] = v.view.m[0][k] * vx + v.view.m[1][k] * vy - v.view.m[2][k];
        Dx[k] = v.view.m[0][k] * (2.0 / (v.width * P[0][0]));
    }
}

float3 octDecode(uint32_t p)
{
    const float ex = (int16_t)(p & 0xFFFF) / 32767.0f, ey = (int16_t)(p >> 16) / 32767.0f;
    float3 n{ ex, ey, 1 - std::fabs(ex) - std::fabs(ey) };
    if (n.z < 0)
    {
        const float ox = (1 - std::fabs(n.y)) * (n.x >= 0 ? 1.f : -1.f), oy = (1 - std::fabs(n.x)) * (n.y >= 0 ? 1.f : -1.f);
        n.x = ox;
        n.y = oy;
    }
    return normalize(n);
}

uint32_t addPlane(scene::Scene& s, float size, uint32_t material)
{
    scene::Mesh m;
    m.name = "plane";
    const float h = size / 2;
    m.positions = { { -h, 0, -h }, { h, 0, -h }, { -h, 0, h }, { h, 0, h } };
    m.normals.assign(4, { 0, 1, 0 });
    m.tangents.assign(4, { 1, 0, 0, 1 });
    m.uv0 = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    m.indices = { 0, 2, 1, 1, 2, 3 };
    m.submeshes.push_back({ 0, 6, material });
    s.meshes.push_back(std::move(m));
    return (uint32_t)s.meshes.size() - 1;
}

uint32_t addSphere(scene::Scene& s, float radius, uint32_t rings, uint32_t sectors, uint32_t material)
{
    scene::Mesh m;
    m.name = "sphere";
    for (uint32_t r = 0; r <= rings; ++r)
        for (uint32_t k = 0; k <= sectors; ++k)
        {
            const double th = kPi * r / rings, ph = 2 * kPi * k / sectors;
            const float3 n{ (float)(std::sin(th) * std::cos(ph)), (float)std::cos(th), (float)(std::sin(th) * std::sin(ph)) };
            m.positions.push_back(n * radius);
            m.normals.push_back(n);
            m.tangents.push_back({ (float)-std::sin(ph), 0, (float)std::cos(ph), 1 });
            m.uv0.push_back({ (float)k / sectors, (float)r / rings });
        }
    for (uint32_t r = 0; r < rings; ++r)
        for (uint32_t k = 0; k < sectors; ++k)
        {
            const uint32_t a = r * (sectors + 1) + k, b = a + 1, c = a + sectors + 1, d = c + 1;
            for (uint32_t v : { a, b, c, b, d, c }) m.indices.push_back(v);
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), material });
    s.meshes.push_back(std::move(m));
    return (uint32_t)s.meshes.size() - 1;
}

// ---------------------------------------------------------------- edge pixels on the CPU (Edge.hlsli)
// The same relation as the GPU, slightly stricter (cos of 0.9 x the angle, 0.9 x the tolerances) so a pixel near a
// threshold that the GPU may classify either way counts as an edge here: the centre-sample model checks skip it.
struct CpuEdgeSample
{
    bool sky = true;
    uint32_t material = 0;
    double pos[3] = {}, n[3] = {};
    double footprint = 0;
};
CpuEdgeSample cpuEdgeSample(const ViewDesc& desc, const std::vector<uint8_t>& words, const std::vector<uint8_t>& gb, const std::vector<uint8_t>& depth,
                            uint32_t W, uint32_t x, uint32_t y)
{
    CpuEdgeSample e;
    const uint32_t word = texelOf<uint32_t>(words, W, x, y);
    e.material = word & 0xFFFF;
    e.sky = e.material == 0xFFFF;
    if (e.sky) return e;
    double D[3], Dx[3];
    pixelRay(desc, x + 0.5, y + 0.5, D, Dx);
    const double z = desc.nearPlane / std::max((double)texelOf<float>(depth, W, x, y), 1e-30);
    for (int k = 0; k < 3; ++k) e.pos[k] = D[k] * z;
    const float3 n = octDecode(texelOf<uint2>(gb, W, x, y).x);
    e.n[0] = n.x;
    e.n[1] = n.y;
    e.n[2] = n.z;
    e.footprint = std::sqrt(Dx[0] * Dx[0] + Dx[1] * Dx[1] + Dx[2] * Dx[2]) * z;
    return e;
}
bool cpuSameSurface(const CpuEdgeSample& a, const CpuEdgeSample& b, const QualityConfig& q)
{
    if (a.sky || b.sky) return a.sky && b.sky;
    if (a.material != b.material) return false;
    const double cosA = std::cos(0.9 * q.number("shading.edge_normal_angle_deg") * kPi / 180);
    if (a.n[0] * b.n[0] + a.n[1] * b.n[1] + a.n[2] * b.n[2] < cosA) return false;
    const double d[3] = { b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2] };
    const double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const double tol = 0.9 * std::max(q.number("shading.edge_footprint_tolerance") * std::max(a.footprint, b.footprint), q.number("shading.edge_distance_tolerance") * len);
    return std::fabs(d[0] * a.n[0] + d[1] * a.n[1] + d[2] * a.n[2]) <= tol && std::fabs(d[0] * b.n[0] + d[1] * b.n[1] + d[2] * b.n[2]) <= tol;
}
bool cpuIsEdge(const ViewDesc& desc, const std::vector<uint8_t>& words, const std::vector<uint8_t>& gb, const std::vector<uint8_t>& depth, uint32_t W, uint32_t H,
               uint32_t x, uint32_t y, const QualityConfig& q, const std::vector<uint8_t>* vis = nullptr)
{
    const CpuEdgeSample c = cpuEdgeSample(desc, words, gb, depth, W, x, y);
    const uint32_t vc = vis ? texelOf<uint32_t>(*vis, W, x, y) : 0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int qx = (int)x + dx, qy = (int)y + dy;
            if ((dx == 0 && dy == 0) || qx < 0 || qy < 0 || qx >= (int)W || qy >= (int)H) continue;
            if (vis && texelOf<uint32_t>(*vis, W, (uint32_t)qx, (uint32_t)qy) == vc) continue;
            if (!cpuSameSurface(c, cpuEdgeSample(desc, words, gb, depth, W, (uint32_t)qx, (uint32_t)qy), q)) return true;
        }
    return false;
}

// ---------------------------------------------------------------- 2D convex polygons (edge composite reference)
using Poly = std::vector<std::array<double, 2>>;
double polyArea(const Poly& p)
{
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        const auto& u = p[i];
        const auto& v = p[(i + 1) % p.size()];
        a += u[0] * v[1] - v[0] * u[1];
    }
    return 0.5 * std::fabs(a);
}
// Clips 'p' by the convex polygon 'c' (either winding).
Poly polyClip(Poly p, const Poly& c)
{
    double orient = 0;
    for (size_t i = 0; i < c.size(); ++i) orient += c[i][0] * c[(i + 1) % c.size()][1] - c[(i + 1) % c.size()][0] * c[i][1];
    const double sgn = orient >= 0 ? 1 : -1;
    for (size_t e = 0; e < c.size() && !p.empty(); ++e)
    {
        const auto& a = c[e];
        const auto& b = c[(e + 1) % c.size()];
        auto side = [&](const std::array<double, 2>& q) { return sgn * ((b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0])); };
        Poly o;
        for (size_t i = 0; i < p.size(); ++i)
        {
            const auto& u = p[i];
            const auto& v = p[(i + 1) % p.size()];
            const double su = side(u), sv = side(v);
            if (su >= 0) o.push_back(u);
            if ((su >= 0) != (sv >= 0))
            {
                const double t = su / (su - sv);
                o.push_back({ u[0] + (v[0] - u[0]) * t, u[1] + (v[1] - u[1]) * t });
            }
        }
        p = o;
    }
    return p;
}
std::array<double, 2> projectPixel(const ViewDesc& v, float3 w)
{
    const double p[4] = { w.x, w.y, w.z, 1 };
    double vs[4], cl[4];
    for (int r = 0; r < 4; ++r)
    {
        vs[r] = 0;
        for (int k = 0; k < 4; ++k) vs[r] += v.view.m[r][k] * p[k];
    }
    for (int r = 0; r < 4; ++r)
    {
        cl[r] = 0;
        for (int k = 0; k < 4; ++k) cl[r] += v.proj.m[r][k] * vs[k];
    }
    return { (cl[0] / cl[3] * 0.5 + 0.5) * v.width, (0.5 - cl[1] / cl[3] * 0.5) * v.height };
}

// ---------------------------------------------------------------- 1
void testTable(Report& report)
{
    const std::vector<float>& ab = shading::specularAlbedoTable();
    const std::vector<float>& e = model::directionalAlbedoTable();
    double worst = 0;
    for (size_t i = 0; i < e.size(); ++i) worst = std::max(worst, std::abs((double)ab[2 * i] + ab[2 * i + 1] - e[i]) / std::max((double)e[i], 1e-6));
    report(worst < 1e-5, "specular albedo: A + B = model E table (rel.)", worst, 1e-5);
}

// ---------------------------------------------------------------- 2
void testSunSpecular(TestFrame& tf, Report& report)
{
    const float3 l0 = normalize(tf.sceneData.sun.direction);
    const double thetaS = tf.sceneData.sun.angularRadius;
    const float3 n{ 0, 1, 0 };
    const float3 axis = normalize(cross(l0, float3{ 0.3f, 0.2f, 1 }));
    struct Case
    {
        float roughness, offset;  // offset of the mirror direction from the disk centre, in disk radii
        float3 f0;
    };
    std::vector<Case> cases;
    for (float r : { 0.01f, 0.03f, 0.04f, 0.05f, 0.06f, 0.07f, 0.08f, 0.09f, 0.1f, 0.13f, 0.14f, 0.15f, 0.17f, 0.19f, 0.22f, 0.25f, 0.27f, 0.3f, 0.5f })
        for (float o : { 0.0f, 0.5f, 0.95f, 1.0f, 1.05f, 2.0f, 6.0f }) cases.push_back({ r, o, { 0.04f, 0.04f, 0.04f } });
    for (float r : { 0.03f, 0.1f }) cases.push_back({ r, 0.3f, { 0.95f, 0.64f, 0.54f } });
    std::vector<float4> q;
    std::vector<model::Surface> surfaces;
    std::vector<float3> views;
    for (const Case& c : cases)
    {
        const float3 refl = rotateTowards(l0, axis, c.offset * thetaS);
        const float3 v = normalize(n * (2 * dot(n, refl)) - refl);
        const float alpha = model::alphaFromRoughness(c.roughness);
        q.push_back({ n.x, n.y, n.z, alpha });
        q.push_back({ v.x, v.y, v.z, c.roughness });
        q.push_back({ c.f0.x, c.f0.y, c.f0.z, 0.0f });  // no pixel footprint: the reference integrates the disk only
        model::Surface s;
        s.roughness = c.roughness;
        s.metallic = 1;  // f0 = baseColor, no diffuse
        s.baseColor = c.f0;
        surfaces.push_back(s);
        views.push_back(v);
    }
    std::shared_ptr<std::vector<uint8_t>> out;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, 64, 64);
        (void)v;
        BufferRef in = fc.graph.createBuffer({ "m.test.queries", q.size() * 16, 16 });
        BufferRef res = fc.graph.createBuffer({ "m.test.results", cases.size() * 16, 16 });
        ComPtr<ID3D12Resource> staging = makeBuffer(tf.device, q.size() * 16, D3D12_HEAP_TYPE_UPLOAD);
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(staging->Map(0, &none, &p), "map");
        std::memcpy(p, q.data(), q.size() * 16);
        staging->Unmap(0, nullptr);
        tf.keep(staging);
        fc.graph.addPass("m.test.upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(in, Use::CopyDst); },
                         [staging, in, bytes = q.size() * 16](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(in), 0, staging.Get(), 0, bytes); });
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/Tests/ShadingProbe");
        const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
        const uint32_t count = (uint32_t)cases.size();
        fc.graph.addPass("m.test.probe", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(in, Use::SrvCompute);
                             b.use(res, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             const uint32_t k[4] = { c.srv(in), c.uav(res), count, 0 };  // LUT: frame constant
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 4);
                             c.cmd->Dispatch((count + 63) / 64, 1, 1);
                         });
        out = tf.readbackBuffer(fc, res, cases.size() * 16);
    });
    double worstPoint = 0, worst4 = 0, worstMirror = 0;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        float4 g;
        std::memcpy(&g, out->data() + i * 16, 16);
        const float3 ref = sunSpecularReference(surfaces[i], n, views[i], l0, thetaS, 400, 1200);
        const double alpha = model::alphaFromRoughness(cases[i].roughness);
        // Relative to the peak of this roughness (the value with the mirror direction at the disk centre), so tails far
        // outside the disk do not dominate through tiny denominators.
        const float3 peak = sunSpecularReference(surfaces[i], n, normalize(n * (2 * dot(n, l0)) - l0), l0, thetaS, 200, 600);
        const double e = std::max({ std::abs(g.x - ref.x), std::abs(g.y - ref.y), std::abs(g.z - ref.z) }) / std::max({ peak.x, peak.y, peak.z, 1e-30f });
        const char* regime = alpha >= 16 * thetaS ? "point" : alpha >= 2 * thetaS ? "4-point" : "narrow lobe";
        if (e > 5e-3) logf("  r %.2f offset %.2f (%s): gpu %.5e ref %.5e (peak %.5e) err %.2e\n", cases[i].roughness, cases[i].offset, regime, g.y, ref.y, peak.y, e);
        if (alpha >= 16 * thetaS) worstPoint = std::max(worstPoint, e);
        else if (alpha >= 2 * thetaS) worst4 = std::max(worst4, e);

        else worstMirror = std::max(worstMirror, e);
    }
    report(worstPoint < 5e-3, "sun specular [point, alpha >= 16 theta_s] vs dense quadrature (rel. to peak)", worstPoint, 5e-3);
    report(worst4 < 5e-3, "sun specular [4-point rule, 2..16 theta_s] vs dense quadrature (rel. to peak)", worst4, 5e-3);
    report(worstMirror < 1e-2, "sun specular [narrow lobe, alpha < 2 theta_s] vs dense / lobe-sampled reference (rel. to peak)", worstMirror, 1e-2);
}

// ---------------------------------------------------------------- 3-5
// Sun radiance of a surface point by the CPU model for a unit-illuminance sun (times E by the caller): cap averages of
// the clipped cosines (the point value away from the terminator, a dense disk quadrature within two disk radii of it:
// the model integrated over the disk, as the reference path tracer does), the specular by the model at the disk centre
// for wide lobes and by sunSpecularReference otherwise, Foliage transmission from the other side. Counts glint (narrow
// lobe dominating) and back-lit pixels when asked.
float3 cpuSun(const model::Surface& su, float3 n, float3 v, float3 l0, double thetaS, uint32_t* glint = nullptr, uint32_t* backlit = nullptr)
{
    const double cap = 2 / (1 + std::cos(thetaS));
    const float NoV = dot(n, v), NoL = dot(n, l0);
    const float3 diffuse = su.baseColor * ((1 - su.metallic) / model::kPi);
    const float3 front = su.cls == scene::MaterialClass::Foliage ? diffuse * (1 - su.transmission) : diffuse;
    const float3 back = su.cls == scene::MaterialClass::Foliage ? diffuse * su.transmission : float3{};
    auto capCos = [&](float sign) {
        const double nl = sign * NoL;
        if (nl >= 2 * std::sin(thetaS)) return nl;
        if (nl <= -2 * std::sin(thetaS)) return 0.0;
        const float3 t = normalize(std::fabs(l0.y) < 0.99f ? cross(float3{ 0, 1, 0 }, l0) : cross(float3{ 1, 0, 0 }, l0));
        const float3 b = cross(l0, t);
        double sum = 0;
        const uint32_t nr = 64, na = 128;
        for (uint32_t i = 0; i < nr; ++i)
        {
            const double c = 1 - (i + 0.5) / nr * (1 - std::cos(thetaS)), sn = std::sqrt(std::max(1 - c * c, 0.0));
            for (uint32_t k = 0; k < na; ++k)
            {
                const double ph = (k + 0.5) / na * 2 * kPi;
                const float3 l = l0 * (float)c + (t * (float)std::cos(ph) + b * (float)std::sin(ph)) * (float)sn;
                sum += std::max(0.0, (double)sign * dot(n, l));
            }
        }
        return sum / (nr * na);
    };
    float3 sun{};
    const double above = capCos(1), below = capCos(-1);
    if (NoV > 0)
    {
        if (above > 0)
        {
            const double alpha = model::alphaFromRoughness(su.roughness);
            const bool terminator = std::fabs(NoL) < 2 * std::sin(thetaS);
            const float3 spec = (alpha >= 16 * thetaS && !terminator) ? (model::evaluate(su, n, v, l0) - front) * NoL * (float)cap
                                                                       : sunSpecularReference(su, n, v, l0, thetaS, terminator ? 200 : 24, terminator ? 400 : 48);
            sun = front * (float)(above * cap) + spec;
            if (glint && alpha < 16 * thetaS && spec.y > 10 * front.y * NoL) ++*glint;
        }
        if (su.cls == scene::MaterialClass::Foliage && below > 0)
        {
            sun = sun + back * (float)(below * cap);
            if (backlit) ++*backlit;
        }
    }
    else if (su.cls == scene::MaterialClass::Foliage && above > 0)
    {
        sun = back * (float)(above * cap);
        if (backlit) ++*backlit;
    }
    return sun;
}

void testScene(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "shading test";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { 0.5f, 0.45f, 0.4f };
    ground.roughness = 0.5f;
    s.materials.push_back(ground);
    scene::Material metal;
    metal.name = "copper";
    metal.baseColor = { 0.95f, 0.64f, 0.54f };
    metal.roughness = 0.12f;
    metal.metallic = 1;
    s.materials.push_back(metal);
    scene::Material leaf;
    leaf.name = "leaf";
    leaf.cls = scene::MaterialClass::Foliage;
    leaf.baseColor = { 0.2f, 0.5f, 0.1f };
    leaf.roughness = 0.45f;
    leaf.transmission = 0.4f;
    leaf.twoSided = true;
    leaf.emissive = { 0.5f, 0.2f, 0.1f };
    s.materials.push_back(leaf);
    const uint32_t plane = addPlane(s, 40, 0);
    const uint32_t sphere = addSphere(s, 1, 48, 96, 1);
    const uint32_t card = addPlane(s, 1.5f, 2);
    scene::Instance a;
    a.mesh = plane;
    s.instances.push_back(a);
    scene::Instance b;
    b.mesh = sphere;
    b.transform = float3x4::translation({ 0, 1, 0 });
    s.instances.push_back(b);
    scene::Instance c;  // upright leaf card facing +z (away from the sun's z < 0 side is lit from behind)
    c.mesh = card;
    c.transform.m[1][1] = 0;
    c.transform.m[1][2] = -1;
    c.transform.m[2][1] = 1;
    c.transform.m[2][2] = 0;
    c.transform.m[0][3] = -2;
    c.transform.m[1][3] = 1;
    s.instances.push_back(c);
    s.sun.direction = normalize(float3{ 0.3f, 0.6f, -0.75f });
    scene::Camera cam;
    cam.name = "scene";
    cam.position = { 0.5f, 1.6f, 4.5f };
    cam.forward = normalize(float3{ -0.1f, -0.15f, -1 });
    cam.ev100 = 13;
    s.cameras.push_back(cam);
    scene::Camera sunCam;
    sunCam.name = "sun";
    sunCam.position = { 0, 1, 10 };
    sunCam.forward = s.sun.direction;
    sunCam.verticalFov = 0.0872665f;  // 5 deg: the disk spans ~58 px
    s.cameras.push_back(sunCam);
    tf.setScene(s);

    const uint32_t W = 960, H = 540;
    const float3 l0 = normalize(s.sun.direction);
    const double thetaS = s.sun.angularRadius;
    auto sceneCamera = [&]() {
    // ---- scene camera, linear and display outputs
    std::shared_ptr<std::vector<uint8_t>> gb, words, lin, disp, depthRb;
    ViewDesc desc;
    // The display pass checks the writers' own encoding (ShadingCommon.hlsli shEncodeExposed: film curve, sRGB OETF),
    // which they use when no post term is on. The chain's terms that are on by default (bloom, vignette, local exposure;
    // Post.cpp postActive) are PostTests' subject and are off for this pass.
    const double bloomDefault = tf.quality.number("shading.post_bloom_strength"), vignetteDefault = tf.quality.number("shading.post_vignette");
    const bool localExposureDefault = tf.quality.boolean("shading.post_local_exposure");
    for (int pass = 0; pass < 2; ++pass)
    {
        if (pass == 1)
        {
            tf.quality.applyOverride("shading.post_bloom_strength=0.0");
            tf.quality.applyOverride("shading.post_vignette=0.0");
            tf.quality.applyOverride("shading.post_local_exposure=false");
        }
        tf.frame.outputLinearHdr = pass == 0;
        tf.run([&](FramePassContext& fc) {
            ViewResources v = tf.mainView(fc, W, H, 0);
            desc = v.view;
            v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, pass == 0 ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM });
            tf.vis.record(fc, v);
            tracks::materialResolve(fc, v);
            tracks::shading(fc, v);
            if (pass == 0)
            {
                gb = tf.readback(fc, v.gbuffer);
                words = tf.readback(fc, material::resolveOutputs(fc, v).materialWord);
                lin = tf.readback(fc, v.color);
                depthRb = tf.readback(fc, v.depth);
            }
            else disp = tf.readback(fc, v.color);
        });
    }
    tf.frame.outputLinearHdr = false;
    tf.quality.applyOverride(unx::format("shading.post_bloom_strength=%.9g", bloomDefault));
    tf.quality.applyOverride(unx::format("shading.post_vignette=%.9g", vignetteDefault));
    tf.quality.applyOverride(localExposureDefault ? "shading.post_local_exposure=true" : "shading.post_local_exposure=false");
    const double exposure = 1.0 / (1.2 * std::exp2(13.0));
    const float3 E = s.sun.color * s.sun.illuminance;
    double worst = 0;
    uint32_t checked = 0, glint = 0, backlit = 0, edgeSkipped = 0;
    for (uint32_t y = 0; y < H; y += 3)
        for (uint32_t x = 0; x < W; x += 3)
        {
            if (cpuIsEdge(desc, *words, *gb, *depthRb, W, H, x, y, tf.quality))
            {
                ++edgeSkipped;
                continue;  // edge composite (testEdgeComposite)
            }
            const uint32_t word = texelOf<uint32_t>(*words, W, x, y);
            const float4 got = texelOf<float4>(*lin, W, x, y);
            float3 expected{};
            if ((word & 0xFFFF) != 0xFFFF)
            {
                const scene::Material& mat = s.materials[word & 0xFFFF];
                const uint2 p = texelOf<uint2>(*gb, W, x, y);
                model::Surface su;
                su.cls = mat.cls;
                su.baseColor = { (float)srgbToLinear((p.y & 0xFF) / 255.0), (float)srgbToLinear(((p.y >> 8) & 0xFF) / 255.0), (float)srgbToLinear(((p.y >> 16) & 0xFF) / 255.0) };
                su.roughness = (p.y >> 24) / 255.0f;
                su.metallic = ((word >> 16) & 0xFF) / 255.0f;
                su.specular = mat.specular;
                su.transmission = mat.transmission;
                const float3 n = octDecode(p.x);
                double D[3], Dx[3];
                pixelRay(desc, x + 0.5, y + 0.5, D, Dx);
                const float3 v = normalize(float3{ (float)-D[0], (float)-D[1], (float)-D[2] });
                expected = (cpuSun(su, n, v, l0, thetaS, &glint, &backlit) * E + mat.emissive) * (float)exposure;
            }
            // Relative to the pixel, floored at 1 % of a sunlit surface (~1 after exposure here): the disk integrators are exact
            // relative to the lobe's peak, so at grazing light (values 1e-3 of lit) their error is judged on that scale.
            const double scale = std::max({ expected.x, expected.y, expected.z, 1e-2f });
            const double e = std::max({ std::abs(got.x - expected.x), std::abs(got.y - expected.y), std::abs(got.z - expected.z) }) / scale;
            if (e > worst && e > 5e-3) logf("  px (%u,%u) word %08x got (%.5f %.5f %.5f) expected (%.5f %.5f %.5f)\n", x, y, word, got.x, got.y, got.z, expected.x, expected.y, expected.z);
            worst = std::max(worst, e);
            ++checked;
        }
    logf("scene: %u pixels checked, %u sun-glint pixels (disk regimes), %u back-lit leaf pixels, %u edge pixels left to the composite test\n", checked, glint, backlit, edgeSkipped);
    report(worst < 5e-3, "scene [linear]: pixel radiance vs CPU model on the G-buffer (rel.)", worst, 5e-3);
    report(glint > 20 && backlit > 100, "scene: glint and back-lit transmission pixels present", std::min(glint, backlit), 20);

    // Display encoding of the same frame.
    double worstLsb = 0;
    for (uint32_t y = 0; y < H; y += 2)
        for (uint32_t x = 0; x < W; x += 2)
        {
            const float4 l = texelOf<float4>(*lin, W, x, y);
            const uint32_t d = texelOf<uint32_t>(*disp, W, x, y);
            float tc[3] = { std::max(l.x, 0.f), std::max(l.y, 0.f), std::max(l.z, 0.f) };
            unx::test::filmCurve(tc, 1.0f);
            const float3 t{ tc[0], tc[1], tc[2] };
            const double want[3] = { oetf(std::clamp(t.x, 0.f, 1.f)) * 1023, oetf(std::clamp(t.y, 0.f, 1.f)) * 1023, oetf(std::clamp(t.z, 0.f, 1.f)) * 1023 };
            for (int k = 0; k < 3; ++k) worstLsb = std::max(worstLsb, std::abs((double)((d >> (10 * k)) & 0x3FF) - want[k]));
        }
    report(worstLsb <= 1.0, "display: RGB10A2 = sRGB OETF(film curve(linear)) (10-bit LSB)", worstLsb, 1.0);

    };
    auto sunCamera = [&]() {
    // ---- solar disk in the sky (5 deg camera), linear output
    tf.frame.outputLinearHdr = true;
    std::shared_ptr<std::vector<uint8_t>> sky;
    ViewDesc sunDesc;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 1);
        sunDesc = v.view;
        v.color = fc.graph.createTexture({ "m.test.sky", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        tracks::shading(fc, v);
        sky = tf.readback(fc, v.color);
    });
    tf.frame.outputLinearHdr = false;
    const double expo = 1.0 / (1.2 * std::exp2(14.0));
    const double Lsun = s.sun.illuminance / (kPi * std::sin(thetaS) * std::sin(thetaS));
    // Angles to the sun in double from the chord (a float-normalised direction's norm error would move a cosine test
    // by ~1e-5 rad at this radius).
    const double sl = std::sqrt((double)l0.x * l0.x + (double)l0.y * l0.y + (double)l0.z * l0.z);
    const double sd[3] = { l0.x / sl, l0.y / sl, l0.z / sl };
    auto angleTo = [&](const double* d) {
        const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const double c0 = d[0] / dl - sd[0], c1 = d[1] / dl - sd[1], c2 = d[2] / dl - sd[2];
        return 2 * std::asin(0.5 * std::sqrt(c0 * c0 + c1 * c1 + c2 * c2));
    };
    double worstCov = 0, flux = 0, cg[3] = {}, cc[3] = {};
    uint32_t edge = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            const float4 g = texelOf<float4>(*sky, W, x, y);
            double inside = 0;
            double D[3], Dx[3];
            pixelRay(sunDesc, x + 0.5, y + 0.5, D, Dx);
            const double dc = angleTo(D);
            if (dc > thetaS + 0.001) continue;  // far from the disk
            for (int j = 0; j < 128; ++j)
                for (int i = 0; i < 128; ++i)
                {
                    double d[3], dx[3];
                    pixelRay(sunDesc, x + (i + 0.5) / 128, y + (j + 0.5) / 128, d, dx);
                    inside += angleTo(d) <= thetaS ? 1 : 0;
                }
            inside /= 16384;
            const double got = g.y / (Lsun * expo);
            if (inside > 0 && inside < 1) ++edge;
            cg[0] += got * x; cg[1] += got * y; cg[2] += got;
            cc[0] += inside * x; cc[1] += inside * y; cc[2] += inside;
            if (std::abs(got - inside) > worstCov && std::abs(got - inside) > 0.01)
                logf("  disk px (%u,%u) angle/theta_s %.5f got %.4f supersampled %.4f\n", x, y, dc / thetaS, got, inside);
            worstCov = std::max(worstCov, std::abs(got - inside));
            // Pixel solid angle ~ |Dx|^2 / |D|^2 (square pixels at the disk, 5 deg field).
            const double dl2 = D[0] * D[0] + D[1] * D[1] + D[2] * D[2];
            const double omega = (Dx[0] * Dx[0] + Dx[1] * Dx[1] + Dx[2] * Dx[2]) / dl2 * (1 / std::sqrt(dl2));
            flux += got * Lsun * omega;
        }
    const double fluxWant = s.sun.illuminance * 2 / (1 + std::cos(thetaS));
    logf("sun disk centroid: gpu (%.4f, %.4f) area %.3f px, supersampled (%.4f, %.4f) area %.3f px\n", cg[0] / cg[2], cg[1] / cg[2], cg[2], cc[0] / cc[2], cc[1] / cc[2], cc[2]);
    logf("sun disk: %u edge pixels\n", edge);
    report(worstCov < 0.02, "sun disk: pixel coverage vs 128x128 supersampling (abs.)", worstCov, 0.02);
    report(std::abs(flux / fluxWant - 1) < 3e-3, "sun disk: total flux vs L_sun x disk solid angle (rel.)", std::abs(flux / fluxWant - 1), 3e-3);
    };
    sceneCamera();
    sunCamera();
}

// ---------------------------------------------------------------- 6. local lights through a froxel list
// S's list format (FroxelCommon.hlsli) with one froxel holding every light, and a shadow-visibility target whose slots
// differ (slot 1 = 128/255, slot 2 = 1, slot 3 = 64/255): the kernel must give the n-th shadow-casting light of the
// list slot n and leave the fourth caster unshadowed (7.3). Sun off. Every sampled pixel against the CPU model.
void testLocalLights(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "local lights test";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { 0.6f, 0.55f, 0.5f };
    ground.roughness = 0.4f;
    s.materials.push_back(ground);
    scene::Material metal;
    metal.name = "steel";
    metal.baseColor = { 0.56f, 0.57f, 0.58f };
    metal.roughness = 0.25f;
    metal.metallic = 1;
    s.materials.push_back(metal);
    scene::Material leaf;
    leaf.name = "leaf";
    leaf.cls = scene::MaterialClass::Foliage;
    leaf.baseColor = { 0.25f, 0.5f, 0.1f };
    leaf.roughness = 0.5f;
    leaf.transmission = 0.35f;
    leaf.twoSided = true;
    s.materials.push_back(leaf);
    scene::Instance plane;
    plane.mesh = addPlane(s, 30, 0);
    s.instances.push_back(plane);
    scene::Instance ball;
    ball.mesh = addSphere(s, 0.8f, 40, 80, 1);
    ball.transform = float3x4::translation({ 0.3f, 0.8f, 0 });
    s.instances.push_back(ball);
    scene::Instance card;
    card.mesh = addPlane(s, 1.2f, 2);
    card.transform.m[1][1] = 0;
    card.transform.m[1][2] = -1;
    card.transform.m[2][1] = 1;
    card.transform.m[2][2] = 0;
    card.transform.m[0][3] = -1.6f;
    card.transform.m[1][3] = 0.8f;
    s.instances.push_back(card);
    s.sun.illuminance = 0;
    auto light = [&](scene::LightType t, float3 p, float3 fwd, float intensity, float range, bool shadow) {
        scene::Light l;
        l.type = t;
        l.position = p;
        l.forward = normalize(fwd);
        l.intensity = intensity;
        l.range = range;
        l.castShadow = shadow;
        l.spotInner = 0.3f;
        l.spotOuter = 0.65f;
        l.color = { 1.0f, 0.9f, 0.8f };
        s.lights.push_back(l);
    };
    light(scene::LightType::Point, { 1, 2, 1 }, { 0, -1, 0 }, 500, 10, true);             // caster 1 -> slot 1
    light(scene::LightType::Spot, { -1.5f, 3, 0.5f }, { 0.2f, -1, -0.1f }, 2000, 12, true);  // caster 2 -> slot 2
    light(scene::LightType::Point, { 0, 0.4f, 2.5f }, { 0, -1, 0 }, 80, 6, false);
    light(scene::LightType::Spot, { 2, 1.5f, -1 }, { -1, -0.3f, 0.5f }, 1500, 8, true);   // caster 3 -> slot 3
    light(scene::LightType::Point, { -1.6f, 1.2f, -1.5f }, { 0, -1, 0 }, 300, 7, true);   // casters 4-6: S's overflow list
    light(scene::LightType::Point, { 1.8f, 0.9f, 1.4f }, { 0, -1, 0 }, 250, 7, true);
    light(scene::LightType::Spot, { -0.4f, 2.6f, 2.2f }, { 0.1f, -1, -0.6f }, 1200, 9, true);
    scene::Camera cam;
    cam.name = "lights";
    cam.position = { 0.5f, 2.2f, 5 };
    cam.forward = normalize(float3{ -0.1f, -0.35f, -1 });
    cam.ev100 = 5;
    s.cameras.push_back(cam);
    tf.setScene(s);

    // S's froxel list buffer: header, one froxel (first entry 0, count 7: two words), indices 0..6 as 16-bit pairs.
    std::vector<uint32_t> list(64, 0);
    const float nearM = 0.01f, farM = 1000.0f, logRatio = std::log2(farM / nearM);
    list[0] = 1;
    list[1] = 1;
    list[2] = 1;
    list[3] = 4096;
    std::memcpy(&list[4], &nearM, 4);
    std::memcpy(&list[5], &farM, 4);
    std::memcpy(&list[6], &logRatio, 4);
    list[8] = 64;   // headerBase
    list[9] = 128;  // indexBase
    list[10] = 64;  // capacity (entries)
    list[11] = 7;   // indexCount
    list[16] = 0;   // first entry
    list[17] = 7;   // count
    list[32] = 0 | (1u << 16);
    list[33] = 2 | (3u << 16);
    list[34] = 4 | (5u << 16);
    list[35] = 6;
    ComPtr<ID3D12Resource> listBuffer = uploadStatic(tf.device, list.data(), list.size() * 4, L"test froxel list");
    const uint32_t slot[4] = { 255, 128, 255, 64 };
    // Per light, list order: slots 1-3, a light without shadows, then the three casters past the third (overflow runs).
    const uint32_t overflowRun[3] = { 200, 100, 30 };
    const double slotVisibility[7] = { 128 / 255.0, 1.0, 1.0, 64 / 255.0, 200 / 255.0, 100 / 255.0, 30 / 255.0 };

    const uint32_t W = 960, H = 540;
    // S's overflow list (INTERFACES 7.3, v1.20): every tile has the three casters past the third; every fifth tile column is
    // over the list's capacity and goes to the fallback kernel (no VSM in this build: those lights shade unshadowed there).
    const uint32_t tilesX = (W + 7) / 8, tilesY = (H + 7) / 8;
    auto fallbackTile = [](uint32_t tx) { return tx % 5 == 2; };
    std::vector<uint32_t> heads((size_t)TestFrame::rowPitch(tilesX, 4) / 4 * tilesY, 0), overflowWords, fallbackList(4, 0);
    for (uint32_t ty = 0; ty < tilesY; ++ty)
        for (uint32_t tx = 0; tx < tilesX; ++tx)
        {
            uint32_t& head = heads[(size_t)ty * (TestFrame::rowPitch(tilesX, 4) / 4) + tx];
            if (fallbackTile(tx))
            {
                head = 0xFFFFFFFFu;
                fallbackList.push_back((ty << 16) | tx);
                continue;
            }
            head = 1 + (uint32_t)overflowWords.size();
            for (uint32_t p = 0; p < 64; ++p) overflowWords.push_back((3u << 24) | 64u);
            overflowWords.push_back(overflowRun[0] | (overflowRun[1] << 8) | (overflowRun[2] << 16));
        }
    fallbackList[0] = fallbackList[1] = (uint32_t)(fallbackList.size() - 4);
    fallbackList[2] = fallbackList[3] = 1;
    ComPtr<ID3D12Resource> overflowBuffer = uploadStatic(tf.device, overflowWords.data(), overflowWords.size() * 4, L"test shadow overflow");
    ComPtr<ID3D12Resource> fallbackBuffer = uploadStatic(tf.device, fallbackList.data(), fallbackList.size() * 4, L"test shadow overflow fallback");
    ComPtr<ID3D12Resource> headsStaging = makeBuffer(tf.device, heads.size() * 4, D3D12_HEAP_TYPE_UPLOAD);
    {
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(headsStaging->Map(0, &none, &p), "map overflow heads staging");
        std::memcpy(p, heads.data(), heads.size() * 4);
        headsStaging->Unmap(0, nullptr);
    }
    const uint32_t packed = slot[0] | (slot[1] << 8) | (slot[2] << 16) | (slot[3] << 24);
    std::vector<uint32_t> shadowTexels((size_t)TestFrame::rowPitch(W, 4) / 4 * H, packed);
    ComPtr<ID3D12Resource> shadowStaging = makeBuffer(tf.device, shadowTexels.size() * 4, D3D12_HEAP_TYPE_UPLOAD);
    {
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(shadowStaging->Map(0, &none, &p), "map shadow staging");
        std::memcpy(p, shadowTexels.data(), shadowTexels.size() * 4);
        shadowStaging->Unmap(0, nullptr);
    }
    std::shared_ptr<std::vector<uint8_t>> gb, words, depth, lin;
    ViewDesc desc;
    tf.frame.outputLinearHdr = true;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 0);
        desc = v.view;
        v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        fc.resources.froxelLights = fc.graph.importBuffer(listBuffer.Get(), { "test froxel lists", list.size() * 4, 0 });
        v.froxelLights = fc.resources.froxelLights;  // the main view's lists are the frame's (FrameRenderer, v1.22)
        v.shadowVisibility = fc.graph.createTexture({ "test shadow visibility", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
        const TextureRef sv = v.shadowVisibility;
        fc.graph.addPass("m.test.shadow.upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(sv, Use::CopyDst); },
                         [&, sv, W, H](PassContext& c) {
                             D3D12_TEXTURE_COPY_LOCATION dst{ c.resource(sv), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                             dst.SubresourceIndex = 0;
                             D3D12_TEXTURE_COPY_LOCATION src{ shadowStaging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                             src.PlacedFootprint.Offset = 0;
                             src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, W, H, 1, TestFrame::rowPitch(W, 4) };
                             c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                         });
        v.shadowOverflowTiles = fc.graph.createTexture({ "test shadow overflow tiles", tilesX, tilesY, 1, 1, DXGI_FORMAT_R32_UINT });
        v.shadowOverflow = fc.graph.importBuffer(overflowBuffer.Get(), { "test shadow overflow", overflowWords.size() * 4, 0 });
        v.shadowOverflowFallbackTiles = fc.graph.importBuffer(fallbackBuffer.Get(), { "test shadow overflow fallback", fallbackList.size() * 4, 0 });
        const TextureRef ht = v.shadowOverflowTiles;
        fc.graph.addPass("m.test.overflow.upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(ht, Use::CopyDst); },
                         [&, ht, tilesX, tilesY](PassContext& c) {
                             D3D12_TEXTURE_COPY_LOCATION dst{ c.resource(ht), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                             dst.SubresourceIndex = 0;
                             D3D12_TEXTURE_COPY_LOCATION src{ headsStaging.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                             src.PlacedFootprint.Offset = 0;
                             src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, tilesX, tilesY, 1, TestFrame::rowPitch(tilesX, 4) };
                             c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                         });
        tracks::shading(fc, v);
        gb = tf.readback(fc, v.gbuffer);
        words = tf.readback(fc, material::resolveOutputs(fc, v).materialWord);
        depth = tf.readback(fc, v.depth);
        lin = tf.readback(fc, v.color);
    });
    tf.frame.outputLinearHdr = false;

    const double exposure = 1.0 / (1.2 * std::exp2(5.0));
    uint32_t fallbackPixels = 0;
    double worst = 0;
    uint32_t checked = 0, lit = 0, back = 0;
    for (uint32_t y = 0; y < H; y += 3)
        for (uint32_t x = 0; x < W; x += 3)
        {
            const uint32_t word = texelOf<uint32_t>(*words, W, x, y);
            if ((word & 0xFFFF) == 0xFFFF) continue;
            if (cpuIsEdge(desc, *words, *gb, *depth, W, H, x, y, tf.quality)) continue;  // edge composite (testEdgeComposite)
            const scene::Material& mat = s.materials[word & 0xFFFF];
            const uint2 p = texelOf<uint2>(*gb, W, x, y);
            model::Surface su;
            su.cls = mat.cls;
            su.baseColor = { (float)srgbToLinear((p.y & 0xFF) / 255.0), (float)srgbToLinear(((p.y >> 8) & 0xFF) / 255.0), (float)srgbToLinear(((p.y >> 16) & 0xFF) / 255.0) };
            su.roughness = (p.y >> 24) / 255.0f;
            su.metallic = ((word >> 16) & 0xFF) / 255.0f;
            su.specular = mat.specular;
            su.transmission = mat.transmission;
            const float3 n = octDecode(p.x);
            double D[3], Dx[3];
            pixelRay(desc, x + 0.5, y + 0.5, D, Dx);
            const float3 v = normalize(float3{ (float)-D[0], (float)-D[1], (float)-D[2] });
            const double z = desc.nearPlane / std::max((double)texelOf<float>(*depth, W, x, y), 1e-30);
            const double P[3] = { desc.position.x + D[0] * z, desc.position.y + D[1] * z, desc.position.z + D[2] * z };
            double sum[3] = {};
            const bool inFallback = fallbackTile(x / 8);
            if (inFallback) ++fallbackPixels;
            for (size_t i = 0; i < s.lights.size(); ++i)
            {
                const scene::Light& l = s.lights[i];
                const double visibility = (inFallback && i >= 4) ? 1.0 : slotVisibility[i];
                const double t[3] = { l.position.x - P[0], l.position.y - P[1], l.position.z - P[2] };
                const double d2 = t[0] * t[0] + t[1] * t[1] + t[2] * t[2], d = std::sqrt(d2);
                const float3 L{ (float)(t[0] / d), (float)(t[1] / d), (float)(t[2] / d) };
                const double r = d / l.range, w = std::pow(std::clamp(1 - r * r * r * r, 0.0, 1.0), 2);
                double I = l.intensity * w / d2;
                if (l.type == scene::LightType::Spot)
                {
                    const double ci = std::cos(l.spotInner), co = std::cos(l.spotOuter), scale = 1 / std::max(ci - co, 1e-4), off = -co * scale;
                    const double sp = std::clamp(-(L.x * l.forward.x + L.y * l.forward.y + L.z * l.forward.z) * scale + off, 0.0, 1.0);
                    I *= sp * sp;
                }
                const float cosL = dot(n, L), NoV = dot(n, v);
                if (!(NoV * cosL > 0) && !(su.cls == scene::MaterialClass::Foliage && NoV * cosL < 0)) continue;
                if (NoV <= 0 && su.cls != scene::MaterialClass::Foliage) continue;
                const float3 f = model::evaluate(su, n, v, L);
                if (NoV * cosL < 0) ++back;
                else ++lit;
                for (int k = 0; k < 3; ++k) sum[k] += (&f.x)[k] * (&l.color.x)[k] * I * std::fabs(cosL) * visibility;
            }
            const float4 got = texelOf<float4>(*lin, W, x, y);
            const double e[3] = { sum[0] * exposure, sum[1] * exposure, sum[2] * exposure };
            const double scale = std::max({ e[0], e[1], e[2], 1e-2 });
            const double err = std::max({ std::abs(got.x - e[0]), std::abs(got.y - e[1]), std::abs(got.z - e[2]) }) / scale;
            if (err > worst && err > 5e-3) logf("  lights px (%u,%u) got (%.5f %.5f %.5f) expected (%.5f %.5f %.5f)\n", x, y, got.x, got.y, got.z, e[0], e[1], e[2]);
            worst = std::max(worst, err);
            ++checked;
        }
    logf("local lights: %u pixels (%u in fallback tiles), %u light-surface pairs reflected, %u transmitted through leaves \n", checked, fallbackPixels, lit, back);
    report(worst < 5e-3, "local lights [point, spot, shadow slots 1-3, overflow list, fallback tiles] vs CPU model (rel.)", worst, 5e-3);
    report(fallbackPixels > 100, "local lights: pixels in overflow fallback tiles checked", fallbackPixels, 100);
    report(back > 50, "local lights: back-lit leaf pairs present", back, 50);
}

// ---------------------------------------------------------------- 7. edge composite
// Two emissive planar quads at different depths, the nearer overlapping the farther, over a black sky (sun off): every
// pixel's exposed radiance must equal the exact visible areas of the quads in the pixel square (convex clipping of the
// projected quads; the farther one minus its overlap with the nearer), within the 32-subsample overlap resolution where
// both quads' edges cross the pixel.
// ---------------------------------------------------------------- 7
// edgeTriangleArea (closed-form column integrals) against double-precision clipping of the float inputs, over small
// triangles around the pixel, triangles whose long edges cross it, triangles far larger than it, slivers, degenerate
// ones and edges on the pixel's boundary. V's coverageTriangleArea (Sutherland-Hodgman) is reported alongside.
void testEdgeArea(TestFrame& tf, Report& report)
{
    struct Case
    {
        std::array<float, 2> a, b, c, pixel;
        bool large;
    };
    std::vector<Case> cases;
    std::mt19937 rng(20260925);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    const std::array<float, 2> pxA = { 731, 402 }, pxB = { 3517, 2011 };
    auto at = [](const std::array<float, 2>& px, double x, double y) { return std::array<float, 2>{ (float)(px[0] + x), (float)(px[1] + y) }; };
    for (int i = 0; i < 4000; ++i)  // vertices within 1.5 px of the pixel
    {
        const auto& px = i & 1 ? pxB : pxA;
        cases.push_back({ at(px, 4 * u01(rng) - 1.5, 4 * u01(rng) - 1.5), at(px, 4 * u01(rng) - 1.5, 4 * u01(rng) - 1.5),
                          at(px, 4 * u01(rng) - 1.5, 4 * u01(rng) - 1.5), px, false });
    }
    for (int i = 0; i < 2000; ++i)  // a long edge through the pixel, the triangle spreading to one side (up to 800 px)
    {
        const auto& px = i & 1 ? pxB : pxA;
        const double mx = u01(rng), my = u01(rng), phi = 2 * kPi * u01(rng), len = 2 + 400 * u01(rng) * u01(rng);
        const double dx = std::cos(phi), dy = std::sin(phi), side = i & 2 ? 1 : -1;
        cases.push_back({ at(px, mx + dx * len, my + dy * len), at(px, mx - dx * len, my - dy * len), at(px, mx - dy * len * side, my + dx * len * side), px, true });
    }
    for (int i = 0; i < 500; ++i)  // large random triangles (covering, missing or cutting the pixel)
        cases.push_back({ at(pxA, 600 * u01(rng) - 300, 600 * u01(rng) - 300), at(pxA, 600 * u01(rng) - 300, 600 * u01(rng) - 300),
                          at(pxA, 600 * u01(rng) - 300, 600 * u01(rng) - 300), pxA, true });
    for (int i = 0; i < 500; ++i)  // slivers and tiny triangles inside the pixel
    {
        const double x = u01(rng), y = u01(rng), s = i & 1 ? 1e-3 : 1.0, t = 1e-4 * u01(rng);
        cases.push_back({ at(pxA, x, y), at(pxA, x + s * (u01(rng) - 0.5), y + s * (u01(rng) - 0.5)), at(pxA, x + t, y + s * (u01(rng) - 0.5)), pxA, false });
    }
    // Edges on the pixel boundary, vertical and horizontal edges, degenerate triangles.
    const double fixed[][6] = { { 0, 0, 1, 0, 0, 1 },     { 0, 0, 1, 0, 1, 1 },    { 0, 0, 0, 1, 1, 1 },     { 0, -1, 0, 2, 2, 0.5 },  { 1, -1, 1, 2, -1, 0.5 },
                                { -1, 0, 2, 0, 0.5, 2 },  { -1, 1, 2, 1, 0.5, -1 }, { 0.5, -5, 0.5, 5, 9, 0 }, { 0, 0, 0.5, 0.5, 1, 1 }, { -3, -3, 0.5, 0.5, 4, 4 },
                                { 0.3, 0.3, 0.3, 0.3, 0.7, 0.2 }, { -2, -2, 3, -2, 0.5, 3 }, { 0.25, 0.25, 0.75, 0.25, 0.5, 0.75 } };
    for (const auto& f : fixed) cases.push_back({ at(pxA, f[0], f[1]), at(pxA, f[2], f[3]), at(pxA, f[4], f[5]), pxA, false });

    std::vector<float4> q;
    for (const Case& c : cases)
    {
        q.push_back({ c.a[0], c.a[1], c.b[0], c.b[1] });
        q.push_back({ c.c[0], c.c[1], c.pixel[0], c.pixel[1] });
    }
    std::shared_ptr<std::vector<uint8_t>> out;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, 64, 64);
        BufferRef in = fc.graph.createBuffer({ "m.test.area queries", q.size() * 16, 16 });
        BufferRef res = fc.graph.createBuffer({ "m.test.area results", cases.size() * 8, 8 });
        ComPtr<ID3D12Resource> staging = makeBuffer(tf.device, q.size() * 16, D3D12_HEAP_TYPE_UPLOAD);
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(staging->Map(0, &none, &p), "map");
        std::memcpy(p, q.data(), q.size() * 16);
        staging->Unmap(0, nullptr);
        tf.keep(staging);
        fc.graph.addPass("m.test.area upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(in, Use::CopyDst); },
                         [staging, in, bytes = q.size() * 16](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(in), 0, staging.Get(), 0, bytes); });
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/Tests/EdgeAreaProbe");
        const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
        const uint32_t count = (uint32_t)cases.size();
        fc.graph.addPass("m.test.area", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(in, Use::SrvCompute);
                             b.use(res, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             const uint32_t k[4] = { c.srv(in), c.uav(res), count, 0 };
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 4);
                             c.cmd->Dispatch((count + 63) / 64, 1, 1);
                         });
        out = tf.readbackBuffer(fc, res, cases.size() * 8);
    });
    double worstSmall = 0, worstLarge = 0, worstVSmall = 0, worstVLarge = 0;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const Case& c = cases[i];
        float g[2];
        std::memcpy(g, out->data() + i * 8, 8);
        const double x = c.pixel[0], y = c.pixel[1];
        const Poly px = { { x, y }, { x + 1, y }, { x + 1, y + 1 }, { x, y + 1 } };
        const Poly tri = { { c.a[0], c.a[1] }, { c.b[0], c.b[1] }, { c.c[0], c.c[1] } };
        const Poly in = polyArea(tri) > 0 ? polyClip(px, tri) : Poly{};  // the square clipped by the (convex) triangle
        const double ref = in.empty() ? 0 : polyArea(in);
        const double e = std::fabs(g[0] - ref), ev = std::fabs(g[1] - ref);
        if (e > (c.large ? 2e-4 : 2e-6))
            logf("  area (%.6f %.6f) (%.6f %.6f) (%.6f %.6f) px (%g %g): M %.7f V %.7f exact %.7f\n", c.a[0], c.a[1], c.b[0], c.b[1], c.c[0], c.c[1], x, y, g[0], g[1], ref);
        double& w = c.large ? worstLarge : worstSmall;
        double& wv = c.large ? worstVLarge : worstVSmall;
        w = std::max(w, e);
        wv = std::max(wv, ev);
    }
    logf("edge area: %zu triangles; V's coverageTriangleArea worst abs. error %.2e (near the pixel) / %.2e (long edges)\n", cases.size(), worstVSmall, worstVLarge);
    report(worstSmall < 2e-6, "edge area: triangles near the pixel vs double clipping (abs.)", worstSmall, 2e-6);
    report(worstLarge < 2e-4, "edge area: long edges up to 800 px vs double clipping (abs.)", worstLarge, 2e-4);
}

void testEdgeComposite(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "edge composite test";
    auto emissive = [&](const char* name, float3 e) {
        scene::Material m;
        m.name = name;
        m.baseColor = { 0, 0, 0 };
        m.specular = 0;
        m.emissive = e;
        s.materials.push_back(m);
        return (uint32_t)s.materials.size() - 1;
    };
    const uint32_t red = emissive("red", { 1.2f, 0, 0 }), green = emissive("green", { 0, 1.2f, 0 });
    const uint32_t quadB = addPlane(s, 1, red), quadC = addPlane(s, 1, green);
    // Rotation about x by +90 deg turns the plane's +y normal to +z (towards the camera), then scale, spin, place.
    auto place = [&](uint32_t mesh, float sx, float sz, float spin, float tilt, float3 at) {
        const double cs = std::cos(spin), sn = std::sin(spin), ct = std::cos(tilt), st = std::sin(tilt);
        // Object (x, y, z) -> face (x, -z, y) spun about z by 'spin', tilted about y by 'tilt'. Non-uniform scale is not
        // allowed on instances (INTERFACES 6.1), so the quad's size goes into the mesh positions instead.
        scene::Mesh& m = s.meshes[mesh];
        for (float3& p : m.positions) p = float3{ p.x * sx, p.y, p.z * sz };
        float3x4 t;
        const double r[3][3] = { { cs * ct, 0, -sn * ct }, { sn, 0, cs }, { -cs * st, -1, sn * st } };
        // columns: object x -> (cs ct, sn, -cs st); object y -> (0,0,-1)... keep a proper rotation: build from basis.
        const float3 ex = normalize(float3{ (float)(cs * ct), (float)sn, (float)(-cs * st) });
        const float3 ez = normalize(float3{ (float)st, 0, (float)ct });       // face normal (object +y) -> ez
        const float3 ey = cross(ez, ex);
        const float3 ox = normalize(cross(ey, ez));                            // orthonormal x
        const float3 oz = cross(ox, ez);                                       // object z so that (x, y=normal, z) is right-handed
        (void)r;
        t.m[0][0] = ox.x; t.m[1][0] = ox.y; t.m[2][0] = ox.z;
        t.m[0][1] = ez.x; t.m[1][1] = ez.y; t.m[2][1] = ez.z;
        t.m[0][2] = oz.x; t.m[1][2] = oz.y; t.m[2][2] = oz.z;
        t.m[0][3] = at.x; t.m[1][3] = at.y; t.m[2][3] = at.z;
        scene::Instance in;
        in.mesh = mesh;
        in.transform = t;
        s.instances.push_back(in);
        return t;
    };
    const float3x4 tB = place(quadB, 2.2f, 1.6f, 0.35f, 0.3f, { -0.2f, 0.1f, -6 });
    const float3x4 tC = place(quadC, 1.0f, 1.0f, 0.9f, -0.25f, { 0.5f, 0.3f, -4 });
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "front";
    cam.position = { 0, 0, 0 };
    cam.forward = { 0, 0, -1 };
    cam.ev100 = 0;
    s.cameras.push_back(cam);
    tf.setScene(s);

    const uint32_t W = 960, H = 540;
    std::shared_ptr<std::vector<uint8_t>> lin;
    ViewDesc desc;
    tf.frame.outputLinearHdr = true;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 0);
        desc = v.view;
        v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        tracks::shading(fc, v);
        lin = tf.readback(fc, v.color);
    });
    tf.frame.outputLinearHdr = false;

    auto screenQuad = [&](const float3x4& t, uint32_t mesh) {
        const scene::Mesh& m = s.meshes[mesh];  // corners: indices 0, 1, 3, 2 of the 2 x 2 vertex grid
        Poly p;
        for (int i : { 0, 1, 3, 2 }) p.push_back(projectPixel(desc, t.transformPoint(m.positions[i])));
        return p;
    };
    const Poly pB = screenQuad(tB, quadB), pC = screenQuad(tC, quadC);
    double worst = 0, worstSingle = 0, sumEdge = 0;
    uint32_t edgePixels = 0, overlapPixels = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            const Poly px = { { (double)x, (double)y }, { x + 1.0, (double)y }, { x + 1.0, y + 1.0 }, { (double)x, y + 1.0 } };
            const Poly bIn = polyClip(px, pB), cIn = polyClip(px, pC);
            const double aC = cIn.empty() ? 0 : polyArea(cIn);
            const double aBall = bIn.empty() ? 0 : polyArea(bIn);
            const Poly bc = bIn.empty() ? Poly{} : polyClip(bIn, pC);
            const double aB = aBall - (bc.empty() ? 0 : polyArea(bc));
            const float4 g = texelOf<float4>(*lin, W, x, y);
            const double err = std::max({ std::fabs(g.x - aB), std::fabs(g.y - aC), std::fabs((double)g.z) });
            const bool partial = (aB > 1e-6 && aB < 1 - 1e-6) || (aC > 1e-6 && aC < 1 - 1e-6);
            const bool overlap = !bc.empty() && polyArea(bc) > 1e-6 && (aBall < 1 - 1e-6 || aC < 1 - 1e-6) && aC < 1 - 1e-6;
            if (partial)
            {
                ++edgePixels;
                sumEdge += err;
            }
            if (overlap) ++overlapPixels;
            else worstSingle = std::max(worstSingle, err);
            if (err > worst && err > 2e-3) logf("  edge px (%u,%u) got (%.4f %.4f) exact (%.4f %.4f)%s\n", x, y, g.x, g.y, aB, aC, overlap ? " overlap" : "");
            worst = std::max(worst, err);
        }
    logf("edge composite: %u pixels with partial coverage, %u where both quads' edges meet; mean error over edge pixels %.2e \n", edgePixels, overlapPixels,
         sumEdge / std::max(edgePixels, 1u));
    report(worstSingle < 2e-3, "edge composite: pixels outside overlaps vs exact visible areas (abs.)", worstSingle, 2e-3);
    report(worst < 1.0 / 32 + 2e-3, "edge composite: all pixels (overlaps within 1/32) vs exact visible areas (abs.)", worst, 1.0 / 32 + 2e-3);
}
// ---------------------------------------------------------------- 8
// A red emissive quad cut out by a smooth alpha disk (radius 11 of 32 texels, alpha ramp over two texels, cutoff 0.5,
// about 4.4 pixels per texel) in front of a green emissive wall. Truth per pixel: the share of 32 x 32 subsamples where
// the quad's bilinear alpha (mip 0, wrapped, the stand-in V's test) passes the cutoff. The composite must give that
// share in red and the rest in green; the geometric area alone (no cut-out coverage) would paint every pixel the card
// covers red, a one-pixel dilation of the disk.
void testEdgeCutout(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "edge cut-out test";
    auto emissive = [&](const char* name, float3 e) {
        scene::Material m;
        m.name = name;
        m.baseColor = { 0, 0, 0 };
        m.specular = 0;
        m.emissive = e;
        s.materials.push_back(m);
        return (uint32_t)s.materials.size() - 1;
    };
    const uint32_t wall = emissive("wall", { 0, 1.2f, 0 }), leaf = emissive("cut-out", { 1.2f, 0, 0 });
    constexpr uint32_t T = 32;
    scene::Texture tex;
    tex.name = "alpha disk";
    tex.width = tex.height = T;
    tex.format = scene::TextureFormat::Rgba8Srgb;
    tex.wrap = true;
    tex.texels.resize(T * T * 4);
    for (uint32_t y = 0; y < T; ++y)
        for (uint32_t x = 0; x < T; ++x)
        {
            const double r = std::hypot(x + 0.5 - T / 2.0, y + 0.5 - T / 2.0);
            const double a = std::clamp(0.5 + (11.0 - r) / 2.0, 0.0, 1.0);
            uint8_t* t = &tex.texels[(y * T + x) * 4];
            t[0] = t[1] = t[2] = 128;
            t[3] = (uint8_t)std::lround(a * 255);
        }
    s.textures.push_back(tex);
    s.materials[leaf].baseColorTexture = 0;
    s.materials[leaf].alphaCutoff = 0.5f;
    const float h = 0.6f;
    const uint32_t quadWall = addPlane(s, 20, wall), quadLeaf = addPlane(s, 2 * h, leaf);
    // Object y (the plane's normal) -> world +z (towards the camera), object z -> world -y: uv (0, 0) at the top left.
    auto facing = [&](uint32_t mesh, float3 at) {
        float3x4 t;  // rows: world x, y, z; columns: object x, y, z, translation
        const float r[3][3] = { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } };
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) t.m[i][j] = r[i][j];
        t.m[0][3] = at.x;
        t.m[1][3] = at.y;
        t.m[2][3] = at.z;
        scene::Instance in;
        in.mesh = mesh;
        in.transform = t;
        s.instances.push_back(in);
    };
    const float3 leafAt = { 0.07f, -0.03f, -4 };
    facing(quadWall, { 0, 0, -6 });
    facing(quadLeaf, leafAt);
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "front";
    cam.position = { 0, 0, 0 };
    cam.forward = { 0, 0, -1 };
    cam.ev100 = 0;
    s.cameras.push_back(cam);
    tf.setScene(s);

    const uint32_t W = 960, H = 540;
    std::shared_ptr<std::vector<uint8_t>> lin;
    ViewDesc desc;
    tf.frame.outputLinearHdr = true;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 0);
        desc = v.view;
        v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        tracks::shading(fc, v);
        lin = tf.readback(fc, v.color);
    });
    tf.frame.outputLinearHdr = false;

    auto alphaAt = [&](double u, double v) {
        const double px = u * T - 0.5, py = v * T - 0.5;
        const double fx = std::floor(px), fy = std::floor(py);
        auto texel = [&](double x, double y) {
            const uint32_t ix = (uint32_t)(((int64_t)x % T + T) % T), iy = (uint32_t)(((int64_t)y % T + T) % T);
            return tex.texels[(iy * T + ix) * 4 + 3] / 255.0;
        };
        const double ax = px - fx, ay = py - fy;
        return (texel(fx, fy) * (1 - ax) + texel(fx + 1, fy) * ax) * (1 - ay) + (texel(fx, fy + 1) * (1 - ax) + texel(fx + 1, fy + 1) * ax) * ay;
    };
    double worst = 0, sumErr = 0, sumTruth = 0, sumRed = 0;
    uint32_t partial = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            uint32_t pass = 0;
            constexpr uint32_t N = 32;
            for (uint32_t j = 0; j < N; ++j)
                for (uint32_t i = 0; i < N; ++i)
                {
                    double D[3], Dx[3];
                    pixelRay(desc, x + (i + 0.5) / N, y + (j + 0.5) / N, D, Dx);
                    const double t = leafAt.z / D[2];
                    const double ox = D[0] * t - leafAt.x, oz = -(D[1] * t - leafAt.y);
                    const double u = (ox + h) / (2 * h), v = (oz + h) / (2 * h);
                    if (u >= 0 && u <= 1 && v >= 0 && v <= 1 && alphaAt(u, v) >= 0.5) ++pass;
                }
            const double truth = pass / double(N * N);
            const float4 g = texelOf<float4>(*lin, W, x, y);
            const double err = std::max(std::fabs(g.x - truth), std::fabs(g.y - (1 - truth)));
            sumTruth += truth;
            sumRed += g.x;
            if (truth > 0 && truth < 1)
            {
                ++partial;
                sumErr += err;
            }
            if (err > worst && err > 0.15) logf("  cut-out px (%u,%u) got (%.4f %.4f) truth %.4f\n", x, y, g.x, g.y, truth);
            worst = std::max(worst, err);
        }
    logf("edge cut-out: %u pixels partly covered by the cut shape; covered area %.2f px (truth %.2f px)\n", partial, sumRed, sumTruth);
    report(sumErr / std::max(partial, 1u) < 0.02, "edge cut-out: mean error over partly covered pixels (abs.)", sumErr / std::max(partial, 1u), 0.02);
    report(worst < 0.15, "edge cut-out: every pixel vs supersampled cut shape (abs.)", worst, 0.15);
    report(std::fabs(sumRed - sumTruth) / sumTruth < 3e-3, "edge cut-out: total covered area vs truth (rel.)", std::fabs(sumRed - sumTruth) / sumTruth, 3e-3);
}

// ---------------------------------------------------------------- 9
struct AlVec
{
    double x = 0, y = 0, z = 0;
};
AlVec operator+(AlVec a, AlVec b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
AlVec operator-(AlVec a, AlVec b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
AlVec operator*(AlVec a, double k) { return { a.x * k, a.y * k, a.z * k }; }
double alDot(AlVec a, AlVec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
AlVec alCross(AlVec a, AlVec b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
AlVec alNorm(AlVec a) { return a * (1 / std::sqrt(alDot(a, a))); }
AlVec alVec(float3 v) { return { v.x, v.y, v.z }; }
float3 alF(AlVec v) { return { (float)v.x, (float)v.y, (float)v.z }; }

struct AlMat  // rows
{
    AlVec r[3];
    AlVec operator*(AlVec v) const { return { alDot(r[0], v), alDot(r[1], v), alDot(r[2], v) }; }
    double det() const { return alDot(r[0], alCross(r[1], r[2])); }
};

struct AlCase
{
    uint32_t type;  // LIGHT_RECT 2, DISK 3, SPHERE 4, TUBE 5
    AlVec p, forward, right;
    double sx, sy;
    AlVec n, v;
    float roughness, f0;
};

// The shading kernel's frame (AreaLight.hlsli shShadingFrame) and LTC inverse (shLtcInverse: bilinear in float).
AlMat alFrame(AlVec n, AlVec v)
{
    const double NoV = alDot(n, v);
    AlVec t = v - n * NoV;
    if (alDot(t, t) < 1e-12) t = std::fabs(n.x) < 0.9 ? alCross(n, { 1, 0, 0 }) : alCross(n, { 0, 1, 0 });
    t = alNorm(t);
    return { { t, alCross(n, t), n } };
}
AlMat alLtcInverse(double NoV, double roughness)
{
    const std::vector<float>& t = shading::ltcTable();
    const float cx = std::sqrt(std::clamp(1.0f - (float)NoV, 0.0f, 1.0f)) * 63, cy = std::clamp((float)roughness, 0.0f, 1.0f) * 63;
    const uint32_t ix = std::min((uint32_t)cx, 62u), iy = std::min((uint32_t)cy, 62u);
    const float fx = cx - ix, fy = cy - iy;
    float m[4];
    for (int k = 0; k < 4; ++k)
    {
        auto at = [&](uint32_t x, uint32_t y) { return t[(y * 64 + x) * 4 + k]; };
        m[k] = (at(ix, iy) * (1 - fx) + at(ix + 1, iy) * fx) * (1 - fy) + (at(ix, iy + 1) * (1 - fx) + at(ix + 1, iy + 1) * fx) * fy;
    }
    return { { { m[0], 0, m[1] }, { 0, 1, 0 }, { m[2], 0, m[3] } } };
}
AlMat alMul(const AlMat& a, const AlMat& b)
{
    AlMat r;
    for (int i = 0; i < 3; ++i)
    {
        const AlVec& row = a.r[i];
        r.r[i] = b.r[0] * row.x + b.r[1] * row.y + b.r[2] * row.z;
    }
    return r;
}

// Visits the light's surface as patches (point relative to the shading point, outward normal x area); N x N per part.
template <typename F>
void alSurface(const AlCase& c, int N, F visit)
{
    const AlVec up = alCross(c.forward, c.right);
    auto orthonormal = [](AlVec d, AlVec& e1, AlVec& e2) {
        e1 = alNorm(std::fabs(d.x) < 0.9 ? alCross(d, { 1, 0, 0 }) : alCross(d, { 0, 1, 0 }));
        e2 = alCross(d, e1);
    };
    if (c.type == 2)
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i)
            {
                const double a = (i + 0.5) / N - 0.5, b = (j + 0.5) / N - 0.5;
                visit(c.p + c.right * (a * c.sx) + up * (b * c.sy), c.forward * (c.sx * c.sy / (double(N) * N)));
            }
    else if (c.type == 3)
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i)
            {
                const double rho = (i + 0.5) / N, phi = 2 * kPi * (j + 0.5) / N, R = c.sx;
                visit(c.p + (c.right * std::cos(phi) + up * std::sin(phi)) * (R * rho), c.forward * (R * R * rho * (1.0 / N) * (2 * kPi / N)));
            }
    else
    {
        // Sphere (radius sx) or capsule (axis along right, length sx, radius sy): cylinder + end hemispheres.
        const bool tube = c.type == 5;
        const double r = tube ? c.sy : c.sx;
        const AlVec ax = c.right, a = c.p - c.right * (tube ? 0.5 * c.sx : 0), b = c.p + c.right * (tube ? 0.5 * c.sx : 0);
        AlVec e1, e2;
        orthonormal(ax, e1, e2);
        auto cap = [&](AlVec centre, AlVec pole, double thetaMax) {
            for (int j = 0; j < N; ++j)
                for (int i = 0; i < N; ++i)
                {
                    const double th = thetaMax * (i + 0.5) / N, phi = 2 * kPi * (j + 0.5) / N;
                    const AlVec s = e1 * (std::sin(th) * std::cos(phi)) + e2 * (std::sin(th) * std::sin(phi)) + pole * std::cos(th);
                    visit(centre + s * r, s * (r * r * std::sin(th) * (thetaMax / N) * (2 * kPi / N)));
                }
        };
        if (!tube)
        {
            cap(c.p, ax, kPi);
            return;
        }
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i)
            {
                const double t = (i + 0.5) / N, phi = 2 * kPi * (j + 0.5) / N;
                const AlVec s = e1 * std::cos(phi) + e2 * std::sin(phi);
                visit(a + ax * (c.sx * t) + s * r, s * (r * c.sx * (1.0 / N) * (2 * kPi / N)));
            }
        cap(b, ax, kPi / 2);
        cap(a, ax * -1, kPi / 2);
    }
}

// I of the light under T: sum over front-facing patches of |det T| (N.(-y)) dA (Ty).z_+ / (pi |Ty|^4).
double alIntegral(const AlCase& c, const AlMat& T, int N)
{
    const double det = std::fabs(T.det());
    double sum = 0;
    alSurface(c, N, [&](AlVec y, AlVec ndA) {
        const double facing = -alDot(ndA, y);
        if (facing <= 0) return;
        const AlVec x = T * y;
        if (x.z <= 0) return;
        const double l2 = alDot(x, x);
        sum += det * facing * x.z / (kPi * l2 * l2);
    });
    return sum;
}

// int f_s cos dw of the model BRDF (metallic, baseColor f0) over the light.
double alBrdfIntegral(const AlCase& c, int N)
{
    model::Surface surf;
    surf.metallic = 1;
    surf.baseColor = { c.f0, c.f0, c.f0 };
    surf.roughness = c.roughness;
    const float3 n = alF(c.n), v = alF(c.v);
    double sum = 0;
    alSurface(c, N, [&](AlVec y, AlVec ndA) {
        const double facing = -alDot(ndA, y);
        if (facing <= 0) return;
        const double d2 = alDot(y, y), d = std::sqrt(d2);
        const AlVec l = y * (1 / d);
        const double cosN = alDot(c.n, l);
        if (cosN <= 0) return;
        sum += model::evaluate(surf, n, v, alF(l)).x * cosN * facing / (d2 * d);
    });
    return sum;
}

// Runs Tests/AreaLightProbe.hlsl over the cases: per case (I front diffuse, I specular LTC, I back diffuse,
// shSpecularAlbedo(f0)).
std::vector<float4> alProbe(TestFrame& tf, const std::vector<AlCase>& cases)
{
    std::vector<gpu::Light> lights;
    std::vector<float4> q;
    for (const AlCase& c : cases)
    {
        gpu::Light l{};
        l.position = alF(c.p);
        l.typeFlags = c.type;
        l.forward = alF(c.forward);
        l.range = 1e6f;
        l.right = alF(c.right);
        l.intensity = 1;
        l.color = { 1, 1, 1 };
        l.size = { (float)c.sx, (float)c.sy };
        lights.push_back(l);
        q.push_back({ (float)c.n.x, (float)c.n.y, (float)c.n.z, c.roughness });
        q.push_back({ (float)c.v.x, (float)c.v.y, (float)c.v.z, c.f0 });
    }
    const std::vector<float>& ltc = shading::ltcTable();
    ComPtr<ID3D12Resource> ltcBuf = uploadStatic(tf.device, ltc.data(), ltc.size() * 4, L"test LTC table");
    ComPtr<ID3D12Resource> lightBuf = uploadStatic(tf.device, lights.data(), lights.size() * sizeof(gpu::Light), L"test area lights");
    ComPtr<ID3D12Resource> queryBuf = uploadStatic(tf.device, q.data(), q.size() * 16, L"test area queries");
    auto srvOf = [&](ID3D12Resource* r, uint32_t count, uint32_t stride) {
        const uint32_t srv = tf.device.descriptors().allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = count;
        sd.Buffer.StructureByteStride = stride;
        tf.device.d3d()->CreateShaderResourceView(r, &sd, tf.device.descriptors().resourceCpu(srv));
        return srv;
    };
    const uint32_t ltcSrv = srvOf(ltcBuf.Get(), (uint32_t)(ltc.size() / 4), 16);
    const uint32_t lightSrv = srvOf(lightBuf.Get(), (uint32_t)lights.size(), sizeof(gpu::Light)), querySrv = srvOf(queryBuf.Get(), (uint32_t)q.size(), 16);
    std::shared_ptr<std::vector<uint8_t>> out;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, 64, 64);
        BufferRef res = fc.graph.createBuffer({ "m.test.area light results", cases.size() * 16, 16 });
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/Tests/AreaLightProbe");
        const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
        const uint32_t count = (uint32_t)cases.size();
        fc.graph.addPass("m.test.area lights", QueueType::Graphics, [&](PassBuilder& b) { b.use(res, Use::UavCompute); },
                         [=](PassContext& c) {
                             const uint32_t k[8] = { lightSrv, querySrv, c.uav(res), count, ltcSrv, 0, 0, 0 };
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 8);
                             c.cmd->Dispatch((count + 63) / 64, 1, 1);
                         });
        out = tf.readbackBuffer(fc, res, cases.size() * 16);
    });
    for (uint32_t srv : { ltcSrv, lightSrv, querySrv }) tf.device.descriptors().freeResource(srv);
    std::vector<float4> r(cases.size());
    std::memcpy(r.data(), out->data(), cases.size() * 16);
    return r;
}

void testAreaLights(TestFrame& tf, Report& report)
{
    std::mt19937 rng(8202);
    std::uniform_real_distribution<double> u01(0, 1);
    auto unit = [&]() {
        for (;;)
        {
            const AlVec d{ 2 * u01(rng) - 1, 2 * u01(rng) - 1, 2 * u01(rng) - 1 };
            const double l = alDot(d, d);
            if (l > 1e-4 && l <= 1) return d * (1 / std::sqrt(l));
        }
    };
    const float roughnesses[] = { 0.1f, 0.2f, 0.35f, 0.5f, 0.7f, 1.0f };
    std::vector<AlCase> cases;
    for (int k = 0; k < 240; ++k)
    {
        AlCase c;
        c.type = 2 + k % 4;
        c.n = unit();
        do c.v = unit();
        while (alDot(c.n, c.v) < 0.05);
        const AlVec dir = unit();
        const double dist = 0.4 + 5.6 * u01(rng) * u01(rng);
        c.p = dir * dist;
        c.forward = alNorm(dir * -1 + unit() * 0.8);
        c.right = alNorm(alCross(c.forward, unit()));
        if (c.type == 2) c.sx = 0.2 + 2.3 * u01(rng), c.sy = 0.2 + 2.3 * u01(rng);
        if (c.type == 3) c.sx = 0.1 + 1.1 * u01(rng), c.sy = 0;
        if (c.type == 4) c.sx = std::min(0.05 + 0.95 * u01(rng), 0.8 * dist), c.sy = 0;
        if (c.type == 5)
        {
            c.sx = 0.3 + 2.2 * u01(rng);
            c.sy = 0.02 + 0.13 * u01(rng);
            // Keep the shading point outside the capsule.
            const AlVec a = c.p - c.right * (0.5 * c.sx);
            const double t = std::clamp(alDot(a * -1, c.right) / c.sx, 0.0, 1.0);
            const AlVec q = a + c.right * (t * c.sx);
            if (alDot(q, q) < 4 * c.sy * c.sy) c.p = c.p + alNorm(q) * (3 * c.sy);
        }
        c.roughness = roughnesses[k % 6];
        c.f0 = 0.9f;
        cases.push_back(c);
    }

    const std::vector<float4> gpuOut = alProbe(tf, cases);

    // CPU references, cases in parallel.
    struct Ref
    {
        double front, back, ltc, brdf;
    };
    std::vector<Ref> refs(cases.size());
    std::atomic<size_t> next{ 0 };
    auto worker = [&]() {
        for (size_t i; (i = next++) < cases.size();)
        {
            const AlCase& c = cases[i];
            constexpr int N = 400;
            const AlMat frame = alFrame(c.n, c.v);
            const AlMat back{ { frame.r[0], frame.r[1] * -1, frame.r[2] * -1 } };
            const AlMat spec = alMul(alLtcInverse(std::max(alDot(c.n, c.v), 1e-4), c.roughness), frame);
            refs[i] = { alIntegral(c, frame, N), alIntegral(c, back, N), alIntegral(c, spec, N), alBrdfIntegral(c, N) };
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back(worker);  // 4 threads: the machine is shared
    for (auto& t : threads) t.join();

    const char* names[] = { "rect", "disk", "sphere", "tube" };
    double worstExact[4] = {}, worstLtc[4] = {}, fitSum[6] = {}, fitWorst[6] = {};
    int fitCount[6] = {};
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const AlCase& c = cases[i];
        const float4 g = gpuOut[i];
        const Ref& r = refs[i];
        auto rel = [](double got, double ref) { return std::fabs(got - ref) / std::max(ref, 1e-3); };
        const double eDiffuse = std::max(rel(g.x, r.front), rel(g.z, r.back)), eLtc = rel(g.y, r.ltc);
        const int t = (int)c.type - 2;
        if (eDiffuse > 5e-3 || eLtc > 5e-3)
            logf("  %s (%.2f %.2f) at %.2f m, rough %.2f: diffuse %.5f / %.5f back %.5f / %.5f ltc %.5f / %.5f\n", names[t], c.sx, c.sy, std::sqrt(alDot(c.p, c.p)),
                 c.roughness, g.x, r.front, g.z, r.back, g.y, r.ltc);
        worstExact[t] = std::max(worstExact[t], eDiffuse);
        worstLtc[t] = std::max(worstLtc[t], eLtc);
        const int ri = (int)(std::find(std::begin(roughnesses), std::end(roughnesses), c.roughness) - std::begin(roughnesses));
        if (r.brdf > 1e-3)
        {
            const double e = std::fabs(g.w * g.y - r.brdf) / r.brdf;
            fitSum[ri] += e;
            fitWorst[ri] = std::max(fitWorst[ri], e);
            ++fitCount[ri];
        }
    }
    for (int t = 0; t < 4; ++t)
        logf("area lights, %-6s: diffuse (front, back) worst %.2e, LTC integral over the shape worst %.2e (rel. to max(I, 1e-3))\n", names[t], worstExact[t], worstLtc[t]);
    for (int ri = 0; ri < 6; ++ri)
        logf("area lights, LTC fit vs model BRDF over the light, roughness %.2f: mean %.3f, worst %.3f (%d cases, rel.)\n", roughnesses[ri], fitSum[ri] / std::max(fitCount[ri], 1),
             fitWorst[ri], fitCount[ri]);
    report(std::max({ worstExact[0], worstExact[1], worstExact[2] }) < 5e-3, "area lights: diffuse integrals, rect/disk/sphere vs surface grid (rel.)",
           std::max({ worstExact[0], worstExact[1], worstExact[2] }), 5e-3);
    report(std::max({ worstLtc[0], worstLtc[1], worstLtc[2] }) < 5e-3, "area lights: LTC integral over rect/disk/sphere vs surface grid (rel.)",
           std::max({ worstLtc[0], worstLtc[1], worstLtc[2] }), 5e-3);
    report(std::max(worstExact[3], worstLtc[3]) < 5e-3, "area lights: tube (exact outline) vs the capsule (rel.)", std::max(worstExact[3], worstLtc[3]), 5e-3);
}

// ---------------------------------------------------------------- 10
// The kernel's closed forms against the light's outline as a dense polygon in double (the quality definition: the form
// factor is exact up to float rounding, design revision 1 12.4). The outline: rect corners; the disk's circle; the circle
// a sphere subtends; the capsule's generators and end-sphere silhouette arcs (the end sphere's circle when seen end-on);
// curves inscribed with n points, transformed, clipped to z >= 0 and closed on the horizon (I = |sum| / 2 pi).
double alEdge(AlVec a, AlVec b)
{
    const AlVec c = alCross(a, b);
    const double s = std::sqrt(alDot(c, c));
    return s > 0 ? std::atan2(s, alDot(a, b)) * (c.z / s) : 0;
}

double alPolygon(const std::vector<AlVec>& P)
{
    double sum = 0;
    AlVec exitPoint, entryPoint;
    bool exits = false, enters = false;
    for (size_t i = 0; i < P.size(); ++i)
    {
        const AlVec a = P[i], b = P[(i + 1) % P.size()];
        if (a.z < 0 && b.z < 0) continue;
        AlVec p = a, q = b;
        if (a.z < 0)
        {
            p = a + (b - a) * (a.z / (a.z - b.z));
            p.z = 0;
            entryPoint = p;
            enters = true;
        }
        else if (b.z < 0)
        {
            q = a + (b - a) * (a.z / (a.z - b.z));
            q.z = 0;
            exitPoint = q;
            exits = true;
        }
        sum += alEdge(p, q);
    }
    if (exits && enters) sum += alEdge(exitPoint, entryPoint);
    return std::fabs(sum) / (2 * kPi);
}

// The outline relative to the shading point (empty: behind a one-sided light or inside the emitter).
std::vector<AlVec> alOutline(const AlCase& c, int n)
{
    std::vector<AlVec> P;
    const AlVec up = alCross(c.forward, c.right);
    if ((c.type == 2 || c.type == 3) && alDot(c.p * -1, c.forward) <= 0) return P;
    if (c.type == 2)
    {
        const AlVec ex = c.right * (0.5 * c.sx), ey = up * (0.5 * c.sy);
        return { c.p - ex - ey, c.p + ex - ey, c.p + ex + ey, c.p - ex + ey };
    }
    auto circle = [&](AlVec centre, AlVec u, AlVec w) {
        for (int i = 0; i < n; ++i)
            P.push_back(centre + u * std::cos(2 * kPi * i / n) + w * std::sin(2 * kPi * i / n));
    };
    auto subtended = [&](AlVec q, double r) {
        const double d2 = alDot(q, q), k = r * r / d2, rc = r * std::sqrt(1 - k);
        const AlVec dir = q * (1 / std::sqrt(d2));
        const AlVec t1 = alNorm(std::fabs(dir.x) < 0.9 ? alCross(dir, { 1, 0, 0 }) : alCross(dir, { 0, 1, 0 }));
        circle(q * (1 - k), t1 * rc, alCross(dir, t1) * rc);
    };
    if (c.type == 3)
    {
        circle(c.p, c.right * c.sx, up * c.sx);
        return P;
    }
    const double r = c.type == 5 ? c.sy : c.sx;
    if (c.type == 4)
    {
        if (alDot(c.p, c.p) > r * r) subtended(c.p, r);
        return P;
    }
    const AlVec a = c.p - c.right * (0.5 * c.sx), b = c.p + c.right * (0.5 * c.sx);
    const AlVec aPerp = a - c.right * alDot(a, c.right);
    const double dPerp = std::sqrt(alDot(aPerp, aPerp));
    if (dPerp <= r * 1.0001)
    {
        const AlVec e = alDot(a, a) < alDot(b, b) ? a : b;
        if (alDot(e, e) > r * r) subtended(e, r);
        return P;
    }
    if (std::min(alDot(a, a), alDot(b, b)) <= r * r) return P;
    const AlVec ah = aPerp * (1 / dPerp), uh = alNorm(alCross(c.right, ah));
    const double along = std::sqrt(1 - r * r / (dPerp * dPerp));
    const AlVec mp = ah * (-r / dPerp) + uh * along, mm = ah * (-r / dPerp) - uh * along;
    const AlVec a0 = a + mp * r, b0 = b + mp * r, b1 = b + mm * r, a1 = a + mm * r;
    auto arc = [&](AlVec end, AlVec p0, AlVec p1, AlVec outward) {  // interior points of the outer arc p0 -> p1
        const double d2 = alDot(end, end), k = r * r / d2, rc = r * std::sqrt(1 - k);
        const AlVec cc = end * (1 - k), e1 = alNorm(p0 - cc);
        AlVec e2 = alNorm(alCross(end, e1));
        if (alDot(e2, outward) < 0) e2 = e2 * -1;
        const AlVec q = p1 - cc;
        double span = std::atan2(alDot(q, e2), alDot(q, e1));
        if (span <= 0) span += 2 * kPi;
        for (int i = 1; i < n; ++i) P.push_back(cc + (e1 * std::cos(span * i / n) + e2 * std::sin(span * i / n)) * rc);
    };
    P.push_back(a0);
    P.push_back(b0);
    arc(b, b0, b1, c.right);
    P.push_back(b1);
    P.push_back(a1);
    arc(a, a1, a0, c.right * -1);
    return P;
}

double alContour(const AlCase& c, const AlMat& T, int n)
{
    std::vector<AlVec> P = alOutline(c, n);
    for (AlVec& x : P) x = T * x;
    return P.empty() ? 0 : alPolygon(P);
}

void testAreaLightContours(TestFrame& tf, Report& report)
{
    std::mt19937 rng(1224);
    std::uniform_real_distribution<double> u01(0, 1);
    auto unit = [&]() {
        for (;;)
        {
            const AlVec d{ 2 * u01(rng) - 1, 2 * u01(rng) - 1, 2 * u01(rng) - 1 };
            const double l = alDot(d, d);
            if (l > 1e-4 && l <= 1) return d * (1 / std::sqrt(l));
        }
    };
    const float roughnesses[] = { 0.1f, 0.2f, 0.35f, 0.5f, 0.7f, 1.0f };
    // Three populations of 2,000: random shapes as in 9; the scenes' sizes (interior, city_night) out to 15 m; and the
    // horizon through the light (the shading normal tilted from the light's direction by at most its angular radius).
    std::vector<AlCase> cases;
    for (int k = 0; k < 6000; ++k)
    {
        AlCase c;
        c.type = 2 + k % 4;
        c.roughness = roughnesses[(k / 4) % 6];
        c.f0 = 0.9f;
        const int mode = k / 2000;
        const double dist = mode == 1 ? 0.3 + 14.7 * u01(rng) * u01(rng) : 0.4 + 5.6 * u01(rng) * u01(rng);
        const AlVec dir = unit();
        c.p = dir * dist;
        c.forward = alNorm(dir * -1 + unit() * 0.8);
        c.right = alNorm(alCross(c.forward, unit()));
        if (mode == 1)
        {
            if (c.type == 2) c.sx = u01(rng) < 0.5 ? 1.2 : 1.6, c.sy = c.sx == 1.2 ? 0.6 : 1.8;
            if (c.type == 3) c.sx = 0.1, c.sy = 0;
            if (c.type == 4) c.sx = 0.05 + 0.01 * u01(rng), c.sy = 0;
            if (c.type == 5) c.sx = 1.2 + 0.4 * u01(rng), c.sy = 0.015 + 0.005 * u01(rng);
        }
        else
        {
            if (c.type == 2) c.sx = 0.2 + 2.3 * u01(rng), c.sy = 0.2 + 2.3 * u01(rng);
            if (c.type == 3) c.sx = 0.1 + 1.1 * u01(rng), c.sy = 0;
            if (c.type == 4) c.sx = std::min(0.05 + 0.95 * u01(rng), 0.8 * dist), c.sy = 0;
            if (c.type == 5) c.sx = 0.3 + 2.2 * u01(rng), c.sy = 0.02 + 0.13 * u01(rng);
        }
        if (c.type == 5)
        {
            // Keep the shading point outside the capsule.
            const AlVec a = c.p - c.right * (0.5 * c.sx);
            const double t = std::clamp(alDot(a * -1, c.right) / c.sx, 0.0, 1.0);
            const AlVec q = a + c.right * (t * c.sx);
            if (alDot(q, q) < 4 * c.sy * c.sy) c.p = c.p + alNorm(q) * (3 * c.sy);
        }
        if (mode == 2)
        {
            const AlVec d = alNorm(c.p);
            const double bound = c.type == 2 ? 0.5 * std::sqrt(c.sx * c.sx + c.sy * c.sy) : (c.type == 5 ? 0.5 * c.sx + c.sy : c.sx);
            const double theta = std::asin(std::min(bound / std::sqrt(alDot(c.p, c.p)), 1.0));
            const AlVec side = alNorm(alCross(d, unit()));
            c.n = alNorm(side + d * std::tan(theta * (2 * u01(rng) - 1)));
        }
        else c.n = unit();
        do c.v = unit();
        while (alDot(c.n, c.v) < 0.05);
        cases.push_back(c);
    }
    const std::vector<float4> gpuOut = alProbe(tf, cases);

    std::vector<std::array<double, 3>> refs(cases.size());
    std::atomic<size_t> next{ 0 };
    auto worker = [&]() {
        for (size_t i; (i = next++) < cases.size();)
        {
            const AlCase& c = cases[i];
            const AlMat frame = alFrame(c.n, c.v);
            const AlMat back{ { frame.r[0], frame.r[1] * -1, frame.r[2] * -1 } };
            const AlMat spec = alMul(alLtcInverse(std::max(alDot(c.n, c.v), 1e-4), c.roughness), frame);
            refs[i] = { alContour(c, frame, 8192), alContour(c, spec, 8192), alContour(c, back, 8192) };
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 4; ++t) threads.emplace_back(worker);  // 4 threads: the machine is shared
    for (auto& t : threads) t.join();

    // Errors of each integral: relative where I >= 1e-3 (P99), and against max(I, 1e-3) for all (as 9: slivers and lights
    // below the horizon count by their absolute error). The LTC transform's condition number kappa (M^-1: its xz block's
    // singular values and 1) amplifies fp32 rounding in the transformed geometry: kappa <= 100 everywhere at roughness
    // >= 0.2 (NoV >= 0.15) and >= 0.3, up to ~1000 at roughness 0.1 and grazing view, where the LTC fit's own L1 error
    // reaches 0.9; cases above 100 are reported apart.
    auto kappa = [&](const AlCase& c) {
        const AlMat m = alLtcInverse(std::max(alDot(c.n, c.v), 1e-4), c.roughness);
        const double a = m.r[0].x, b = m.r[0].z, cc = m.r[2].x, d = m.r[2].z;
        const double f = a * a + b * b + cc * cc + d * d, det = std::fabs(a * d - b * cc);
        const double smax = std::sqrt(0.5 * (f + std::sqrt(std::max(f * f - 4 * det * det, 0.0)))), smin = det / smax;
        return std::max(smax, 1.0) / std::min(smin, 1.0);
    };
    const char* names[] = { "rect", "disk", "sphere", "tube" };
    const char* kinds[] = { "diffuse", "LTC", "back" };
    double p99All = 0, worstAll = 0, worstIll = 0;
    size_t ill = 0;
    for (int t = 0; t < 4; ++t)
        for (int j = 0; j < 3; ++j)
        {
            std::vector<double> e;
            double worst = 0, worstK = 0;
            for (size_t i = 0; i < cases.size(); ++i)
            {
                const AlCase& c = cases[i];
                if ((int)c.type != t + 2) continue;
                const double g = j == 0 ? gpuOut[i].x : (j == 1 ? gpuOut[i].y : gpuOut[i].z), r = refs[i][j];
                const double err = std::fabs(g - r) / std::max(r, 1e-3);
                const bool illConditioned = j == 1 && kappa(c) > 100;
                if (g_areaDump && err > 1e-3)
                    logf("DUMP %d %d %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", t + 2, j, c.p.x, c.p.y, c.p.z, c.forward.x,
                         c.forward.y, c.forward.z, c.right.x, c.right.y, c.right.z, c.sx, c.sy, c.n.x, c.n.y, c.n.z, c.v.x, c.v.y, c.v.z, (double)c.roughness, g, r);
                if (illConditioned)
                {
                    ++ill;
                    worstK = std::max(worstK, err);
                    continue;
                }
                if (r >= 1e-3) e.push_back(std::fabs(g - r) / r);
                worst = std::max(worst, err);
                if (err > 1e-3)
                    logf("  %s %s (%.3f %.3f) at %.2f m, rough %.2f, n.l %.3f: %.7f / %.7f\n", names[t], kinds[j], c.sx, c.sy, std::sqrt(alDot(c.p, c.p)), c.roughness,
                         alDot(c.n, alNorm(c.p)), g, r);
            }
            std::sort(e.begin(), e.end());
            const double p50 = e.empty() ? 0 : e[e.size() / 2], p99 = e.empty() ? 0 : e[std::min(e.size() - 1, e.size() * 99 / 100)];
            logf("area light contours, %-6s %-7s: %4zu with I >= 1e-3, rel. error p50 %.1e p99 %.1e max %.1e; all (kappa <= 100) vs max(I, 1e-3) worst %.1e", names[t], kinds[j],
                 e.size(), p50, p99, e.empty() ? 0 : e.back(), worst);
            logf(j == 1 ? "; kappa > 100 worst %.1e\n" : "\n", worstK);
            p99All = std::max(p99All, p99);
            worstAll = std::max(worstAll, worst);
            worstIll = std::max(worstIll, worstK);
        }
    logf("area light contours: %zu LTC integrals with kappa(M^-1) > 100 (roughness 0.1, grazing), worst vs max(I, 1e-3) %.1e (reported, not gated)\n", ill, worstIll);
    report(p99All < 1e-4, "area lights: closed forms vs dense outline, P99 where I >= 1e-3 (rel.)", p99All, 1e-4);
    report(worstAll < 1e-3, "area lights: closed forms vs dense outline, worst for kappa <= 100 (rel. to max(I, 1e-3))", worstAll, 1e-3);
}

// V's coverage layer in its v1.41 layout (CoverageTiles.hlsli, INTERFACES 7.1) from records per tile ({ visId, depth
// bits, mask, packed }, the pixel in packed >> 26): each listed tile's records pixel-major (a pixel's in the given
// order), tiles listed in index order, uploaded for a view.
struct TestCoverageLayer
{
    std::vector<uint32_t> headers, list, records, tilePixels;
    uint32_t listed = 0, stored = 0;
    ComPtr<ID3D12Resource> headerBuf, listBuf, recordBuf, pixelBuf;

    TestCoverageLayer(Device& device, uint32_t tilesX, uint32_t tilesY, const std::vector<std::vector<std::array<uint32_t, 4>>>& tileRecords)
    {
        const uint32_t tiles = tilesX * tilesY;
        headers.assign((size_t)tiles * 8, 0);
        list.assign(16, 0);
        uint32_t blocks = 0;
        for (uint32_t tile = 0; tile < tiles; ++tile)
        {
            const auto& fr = tileRecords[tile];
            if (fr.empty()) continue;
            const uint32_t base = (uint32_t)(records.size() / 4), n = (uint32_t)fr.size();
            headers[tile * 8] = n;
            headers[tile * 8 + 1] = base;
            headers[tile * 8 + 2] = listed + 1;
            list.insert(list.end(), { tile, n, base, blocks });
            blocks += (n + 1023) / 1024;
            std::vector<std::vector<uint32_t>> byPixel(64);
            for (uint32_t i = 0; i < n; ++i) byPixel[fr[i][3] >> 26].push_back(i);
            uint32_t offset = 0;
            for (uint32_t px = 0; px < 64; ++px)
            {
                tilePixels.push_back(offset);
                for (uint32_t i : byPixel[px]) records.insert(records.end(), fr[i].begin(), fr[i].end());
                offset += (uint32_t)byPixel[px].size();
            }
            ++listed;
        }
        stored = (uint32_t)(records.size() / 4);
        list[0] = listed;
        list[1] = 1;
        list[2] = 1;
        list[3] = listed;
        list[4] = stored;
        list[5] = blocks;
        list[6] = std::max(stored, 1u);
        list[7] = tilesX;
        list[8] = std::min(blocks, 65535u);
        list[9] = blocks ? (blocks + 65534) / 65535 : 0;
        list[10] = 1;
        if (records.empty()) records.assign(4, 0);
        if (tilePixels.empty()) tilePixels.assign(64, 0);
        headerBuf = uploadStatic(device, headers.data(), headers.size() * 4, L"test coverage tiles");
        listBuf = uploadStatic(device, list.data(), list.size() * 4, L"test coverage tile list");
        recordBuf = uploadStatic(device, records.data(), records.size() * 4, L"test coverage records");
        pixelBuf = uploadStatic(device, tilePixels.data(), tilePixels.size() * 4, L"test coverage tile pixels");
    }

    void install(RenderGraph& g, ViewResources& v) const
    {
        v.coverageTiles = g.importBuffer(headerBuf.Get(), { "test coverage tiles", headers.size() * 4, 0 });
        v.coverageTileList = g.importBuffer(listBuf.Get(), { "test coverage tile list", list.size() * 4, 0 });
        v.coverageRecords = g.importBuffer(recordBuf.Get(), { "test coverage records", records.size() * 4, 16 });
        v.coverageTilePixels = g.importBuffer(pixelBuf.Get(), { "test coverage tile pixels", tilePixels.size() * 4, 0 });
    }
};

// ---------------------------------------------------------------- 11. coverage composite (CoverageComposite.hlsl)
// A ground plane in band A and 90 small triangles (diffuse, rough metal, two-sided leaf with transmission and emission)
// in band B: the stand-in raster leaves them out of band A, and their coverage records are made here as V defines them
// (INTERFACES 7.1 v1.38): exact area of the pixel square clipped by the projected triangle, the 32 subsamples of
// coverageSample inside it, the device depth at the covered region's centroid (z/w is affine in screen space), tile
// chunks of 64 records. The frame is rendered without the layer (band A alone) and with it; every pixel with fragments
// is checked against a CPU composite (front to back, mask union, band A taking the rest with the first frame's value)
// whose fragments are shaded by the CPU model at the centroid (cpuSun), and every other pixel must be unchanged.
void testCoverageComposite(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "coverage composite test";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { 0.5f, 0.45f, 0.4f };
    ground.roughness = 0.6f;
    s.materials.push_back(ground);
    scene::Material grass;
    grass.name = "grass";
    grass.baseColor = { 0.25f, 0.55f, 0.15f };
    grass.roughness = 0.5f;
    s.materials.push_back(grass);
    scene::Material metal;
    metal.name = "rough metal";
    metal.baseColor = { 0.9f, 0.8f, 0.6f };
    metal.roughness = 0.35f;
    metal.metallic = 1;
    s.materials.push_back(metal);
    scene::Material leaf;
    leaf.name = "leaf";
    leaf.cls = scene::MaterialClass::Foliage;
    leaf.baseColor = { 0.2f, 0.5f, 0.1f };
    leaf.roughness = 0.45f;
    leaf.transmission = 0.4f;
    leaf.twoSided = true;
    leaf.emissive = { 0.3f, 0.1f, 0.05f };
    s.materials.push_back(leaf);
    const uint32_t plane = addPlane(s, 40, 0);
    // Band B: 90 triangles over three materials in a box in front of the camera, some against the sky.
    std::mt19937 rng(4501);
    std::uniform_real_distribution<float> u01(0, 1);
    scene::Mesh bm;
    bm.name = "blades";
    for (uint32_t m = 1; m <= 3; ++m)
    {
        const uint32_t first = (uint32_t)bm.indices.size();
        for (uint32_t k = 0; k < 30; ++k)
        {
            const float3 c{ -1.6f + 3.2f * u01(rng), 0.15f + 1.9f * u01(rng), -1.5f - 2.5f * u01(rng) };
            const float size = 0.04f + 0.25f * u01(rng);
            float3 p[3];
            for (int i = 0; i < 3; ++i) p[i] = c + float3{ size * (2 * u01(rng) - 1), size * (2 * u01(rng) - 1), size * (2 * u01(rng) - 1) };
            const float3 fn = normalize(cross(p[1] - p[0], p[2] - p[0]));
            for (int i = 0; i < 3; ++i)
            {
                bm.indices.push_back((uint32_t)bm.positions.size());
                bm.positions.push_back(p[i]);
                bm.normals.push_back(fn);
                bm.tangents.push_back({ 1, 0, 0, 1 });
                bm.uv0.push_back({ 0, 0 });
            }
        }
        bm.submeshes.push_back({ first, (uint32_t)bm.indices.size() - first, m });
    }
    // A fourth submesh (grass): 30 slivers ~0.3 px wide through one screen point at depths 2..4 m along the view axis, in
    // random directions, so pixels there hold up to ~30 fragments of a few subsamples each: the walk needs several
    // windows of 8 before the mask union fills.
    {
        const uint32_t first = (uint32_t)bm.indices.size();
        const float3 eye{ 0, 1.1f, 1.5f }, fwd = normalize(float3{ 0, -0.12f, -1 });
        const float3 right = normalize(cross(fwd, float3{ 0, 1, 0 })), up = cross(right, fwd);
        for (uint32_t k = 0; k < 30; ++k)
        {
            const float t = 2.0f + 2.0f * k / 29.0f, th = 6.2831853f * u01(rng);
            const float3 c = eye + fwd * t, u = right * std::cos(th) + up * std::sin(th), w = cross(fwd, u);
            const float half = 0.06f * t, width = 0.3f * t * 1.0472f / 180;  // ~0.3 px at 180 rows over 60 deg
            const float3 p[3] = { c - u * half, c + u * half, c + w * width };
            const float3 fn = normalize(cross(p[1] - p[0], p[2] - p[0]));
            for (int i = 0; i < 3; ++i)
            {
                bm.indices.push_back((uint32_t)bm.positions.size());
                bm.positions.push_back(p[i]);
                bm.normals.push_back(fn);
                bm.tangents.push_back({ 1, 0, 0, 1 });
                bm.uv0.push_back({ 0, 0 });
            }
        }
        bm.submeshes.push_back({ first, (uint32_t)bm.indices.size() - first, 1 });
    }
    // Heavy stacks (CoverageHeavy*): tiny triangles (a few hundredths of a pixel, most without a subsample) facing the
    // camera at random depths 2..4 m inside one target pixel each, in submeshes of 30. Stack 1: 120 rough-metal
    // triangles, so the walk goes through every one (several rounds of 32). Stack 2: 1,110 leaf triangles, so the pixel
    // holds more than one sorted run of 1,024 and its nearest fragments come from both runs.
    auto addStack = [&](float px, float py, uint32_t count, float side, uint32_t material) {
        const float3 eye{ 0, 1.1f, 1.5f }, fwd = normalize(float3{ 0, -0.12f, -1 });
        const float3 right = normalize(cross(fwd, float3{ 0, 1, 0 })), up = cross(right, fwd);
        const float pixelAngle = 60.0f / 180 * 3.14159265f / 180;  // ~ one pixel at the centre (180 rows over 60 deg)
        for (uint32_t k0 = 0; k0 < count; k0 += 30)
        {
            const uint32_t first = (uint32_t)bm.indices.size();
            for (uint32_t k = 0; k < 30; ++k)
            {
                const float t = 2.0f + 2.0f * u01(rng), size = side * t * pixelAngle;
                const float dx = px - 160 + 0.2f + 0.6f * u01(rng), dy = py - 90 + 0.2f + 0.6f * u01(rng);
                const float3 c = eye + fwd * t + right * (dx * t * pixelAngle) - up * (dy * t * pixelAngle);
                const float th = 6.2831853f * u01(rng);
                const float3 a = right * std::cos(th) + up * std::sin(th), b = cross(fwd, a);
                float3 p[3] = { c + a * size, c + b * size, c - (a + b) * (0.7f * size) };
                if (dot(cross(p[1] - p[0], p[2] - p[0]), fwd) > 0) std::swap(p[1], p[2]);  // front faces toward the camera
                const float3 fn = normalize(cross(p[1] - p[0], p[2] - p[0]));
                for (int i = 0; i < 3; ++i)
                {
                    bm.indices.push_back((uint32_t)bm.positions.size());
                    bm.positions.push_back(p[i]);
                    bm.normals.push_back(fn);
                    bm.tangents.push_back({ 1, 0, 0, 1 });
                    bm.uv0.push_back({ 0, 0 });
                }
            }
            bm.submeshes.push_back({ first, (uint32_t)bm.indices.size() - first, material });
        }
    };
    addStack(100, 40, 120, 0.12f, 2);
    addStack(220, 40, 1110, 0.3f, 3);
    s.meshes.push_back(bm);
    const uint32_t blades = (uint32_t)s.meshes.size() - 1;
    scene::Instance a;
    a.mesh = plane;
    s.instances.push_back(a);
    scene::Instance b;
    b.mesh = blades;
    s.instances.push_back(b);
    s.sun.direction = normalize(float3{ 0.3f, 0.6f, -0.75f });
    scene::Camera cam;
    cam.name = "coverage";
    cam.position = { 0, 1.1f, 1.5f };
    cam.forward = normalize(float3{ 0, -0.12f, -1 });
    cam.ev100 = 13;
    s.cameras.push_back(cam);
    tf.setScene(s, { 1 });

    const uint32_t W = 320, H = 180, tilesX = (W + 7) / 8, tilesY = (H + 7) / 8;
    ViewDesc desc;
    tf.run([&](FramePassContext& fc) { desc = tf.mainView(fc, W, H, 0).view; });
    // Screen position (pixels) and device depth of a world point.
    auto project = [&](float3 w, double& px, double& py, double& z) {
        const double o[3] = { (double)w.x - desc.position.x, (double)w.y - desc.position.y, (double)w.z - desc.position.z };
        double vv[3];
        for (int r = 0; r < 3; ++r) vv[r] = desc.view.m[r][0] * o[0] + desc.view.m[r][1] * o[1] + desc.view.m[r][2] * o[2];
        double clip[4];
        for (int r = 0; r < 4; ++r) clip[r] = desc.proj.m[r][0] * vv[0] + desc.proj.m[r][1] * vv[1] + desc.proj.m[r][2] * vv[2] + desc.proj.m[r][3];
        px = (clip[0] / clip[3] * 0.5 + 0.5) * W;
        py = (0.5 - clip[1] / clip[3] * 0.5) * H;
        z = clip[2] / clip[3];
    };
    auto sample = [](uint32_t i) {
        uint32_t r = 0;
        for (int bit = 0; bit < 5; ++bit) r |= ((i >> bit) & 1u) << (4 - bit);
        return std::array<double, 2>{ (i + 0.5) / 32.0, r / 32.0 + 1.0 / 64.0 };
    };
    auto encodeNormal = [](float3 n) {
        const float l1 = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
        float ex = n.x / l1, ey = n.y / l1;
        if (n.z < 0)
        {
            const float ox = (1 - std::fabs(ey)) * (ex >= 0 ? 1.f : -1.f), oy = (1 - std::fabs(ex)) * (ey >= 0 ? 1.f : -1.f);
            ex = ox;
            ey = oy;
        }
        const uint32_t qx = (uint32_t)std::lround(std::clamp(ex * 0.5f + 0.5f, 0.f, 1.f) * 255), qy = (uint32_t)std::lround(std::clamp(ey * 0.5f + 0.5f, 0.f, 1.f) * 255);
        return qx | (qy << 8);
    };

    // Records per tile. The blades' cluster follows the ground's in the visible list (FakeVisibility, not rasterised).
    struct Frag
    {
        uint32_t tri, visId, depthBits, mask, packed;
        double cx, cy;  // centroid (pixels)
    };
    std::vector<std::vector<Frag>> tileFrags(tilesX * tilesY);
    const uint32_t visibleBlades = tf.vis.rasterised;
    M_CHECK(tf.vis.visible.size() == visibleBlades + bm.submeshes.size(), "blades expected in one cluster per submesh after the ground's");
    uint32_t fragments = 0;
    for (uint32_t t = 0; t < (uint32_t)bm.indices.size() / 3; ++t)
    {
        double sx[3], sy[3], sz[3];
        for (int i = 0; i < 3; ++i) project(bm.positions[bm.indices[3 * t + i]], sx[i], sy[i], sz[i]);
        const Poly tri{ { sx[0], sy[0] }, { sx[1], sy[1] }, { sx[2], sy[2] } };
        const double det = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (std::fabs(det) < 1e-9) continue;
        const int x0 = std::max(0, (int)std::floor(std::min({ sx[0], sx[1], sx[2] }))), x1 = std::min((int)W - 1, (int)std::floor(std::max({ sx[0], sx[1], sx[2] })));
        const int y0 = std::max(0, (int)std::floor(std::min({ sy[0], sy[1], sy[2] }))), y1 = std::min((int)H - 1, (int)std::floor(std::max({ sy[0], sy[1], sy[2] })));
        // chunkClusters: one cluster per 30-triangle submesh (<= 64), triangles in index order.
        const uint32_t sub = t / 30, inSub = t % 30;
        const uint32_t visId = (((visibleBlades + sub) << 7) | inSub) + 1;
        const float3 fn = bm.normals[bm.indices[3 * t]];
        for (int py = y0; py <= y1; ++py)
            for (int px = x0; px <= x1; ++px)
            {
                const Poly square{ { (double)px, (double)py }, { px + 1.0, (double)py }, { px + 1.0, py + 1.0 }, { (double)px, py + 1.0 } };
                const Poly in = polyClip(square, tri);
                if (in.size() < 3) continue;
                const double area = polyArea(in);
                if (area <= 1e-9) continue;
                double cx = 0, cy = 0, a2 = 0;
                for (size_t i = 0; i < in.size(); ++i)
                {
                    const auto& u = in[i];
                    const auto& v = in[(i + 1) % in.size()];
                    const double cr = u[0] * v[1] - v[0] * u[1];
                    a2 += cr;
                    cx += (u[0] + v[0]) * cr;
                    cy += (u[1] + v[1]) * cr;
                }
                cx /= 3 * a2;
                cy /= 3 * a2;
                uint32_t mask = 0;
                for (uint32_t i = 0; i < 32; ++i)
                {
                    const auto sp = sample(i);
                    const double qx = px + sp[0], qy = py + sp[1];
                    double w[3];
                    for (int e = 0; e < 3; ++e)
                    {
                        const int e1 = (e + 1) % 3, e2 = (e + 2) % 3;
                        w[e] = ((sx[e2] - sx[e1]) * (qy - sy[e1]) - (sy[e2] - sy[e1]) * (qx - sx[e1])) / det;
                    }
                    if (w[0] >= 0 && w[1] >= 0 && w[2] >= 0) mask |= 1u << i;
                }
                // Screen barycentrics of the centroid: z/w is affine in screen space.
                double bc[3];
                for (int e = 0; e < 3; ++e)
                {
                    const int e1 = (e + 1) % 3, e2 = (e + 2) % 3;
                    bc[e] = ((sx[e2] - sx[e1]) * (cy - sy[e1]) - (sy[e2] - sy[e1]) * (cx - sx[e1])) / det;
                }
                const float depth = (float)(bc[0] * sz[0] + bc[1] * sz[1] + bc[2] * sz[2]);
                if (!(depth > 0)) continue;
                Frag f;
                f.tri = t;
                f.visId = visId;
                std::memcpy(&f.depthBits, &depth, 4);
                f.mask = mask;
                const uint32_t pixelInTile = (px % 8) + 8 * (py % 8);
                f.packed = encodeNormal(fn) | ((uint32_t)std::lround(std::min(area, 1.0) * 1023) << 16) | (pixelInTile << 26);
                f.cx = cx;
                f.cy = cy;
                tileFrags[(py / 8) * tilesX + px / 8].push_back(f);
                ++fragments;
            }
    }
    // V's layer (v1.41 layout).
    std::vector<std::vector<std::array<uint32_t, 4>>> tileRecords(tilesX * tilesY);
    for (uint32_t tile = 0; tile < tilesX * tilesY; ++tile)
        for (const Frag& f : tileFrags[tile]) tileRecords[tile].push_back({ f.visId, f.depthBits, f.mask, f.packed });
    const TestCoverageLayer layer(tf.device, tilesX, tilesY, tileRecords);
    const uint32_t listed = layer.listed;

    // Band A alone, then with the layer (linear output).
    std::shared_ptr<std::vector<uint8_t>> base, withLayer, depthRb;
    tf.frame.outputLinearHdr = true;
    for (int pass = 0; pass < 2; ++pass)
        tf.run([&](FramePassContext& fc) {
            ViewResources v = tf.mainView(fc, W, H, 0);
            v.color = fc.graph.createTexture({ "m.test.coverage color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
            tf.vis.record(fc, v);
            tracks::materialResolve(fc, v);
            if (pass == 1) layer.install(fc.graph, v);
            tracks::shading(fc, v);
            if (pass == 0)
            {
                base = tf.readback(fc, v.color);
                depthRb = tf.readback(fc, v.depth);
            }
            else withLayer = tf.readback(fc, v.color);
        });
    tf.frame.outputLinearHdr = false;

    // CPU composite.
    auto bitCount = [](uint32_t v) {
        uint32_t c = 0;
        for (; v; v &= v - 1) ++c;
        return c;
    };
    const double exposure = 1.0 / (1.2 * std::exp2(13.0));
    const float3 l0 = normalize(s.sun.direction);
    const float3 E = s.sun.color * s.sun.illuminance;
    double worst = 0, worstOther = 0;
    uint32_t checked = 0, layered = 0, hidden = 0, sky = 0, heavyPixels = 0, rounds = 0, runs = 0, most = 0;
    uint32_t failedHeavy = 0, overLimit = 0;
    std::vector<bool> hasFrags(W * H, false);
    for (uint32_t tile = 0; tile < tilesX * tilesY; ++tile)
        for (const Frag& f : tileFrags[tile])
            hasFrags[((tile / tilesX) * 8 + (f.packed >> 26) / 8) * W + (tile % tilesX) * 8 + (f.packed >> 26) % 8] = true;
    for (uint32_t tile = 0; tile < tilesX * tilesY; ++tile)
    {
        const auto& fr = tileFrags[tile];
        if (fr.empty()) continue;
        for (uint32_t p = 0; p < 64; ++p)
        {
            const uint32_t x = (tile % tilesX) * 8 + p % 8, y = (tile / tilesX) * 8 + p / 8;
            if (x >= W || y >= H) continue;
            std::vector<const Frag*> px;
            for (const Frag& f : fr)
                if ((f.packed >> 26) == p) px.push_back(&f);
            if (px.empty()) continue;
            std::sort(px.begin(), px.end(), [](const Frag* a, const Frag* b) { return a->depthBits > b->depthBits || (a->depthBits == b->depthBits && a->visId < b->visId); });
            const float bandDepth = texelOf<float>(*depthRb, W, x, y);
            if (bandDepth == 0) ++sky;
            double sum[3] = {}, used = 0;
            uint32_t covered = 0, visible = 0, walked = 0, complete = 0;  // complete: fragments walked until used reached 1
            // The heavy rounds take at most kRoundLimit fragments (CoverageShade.hlsli COV_ROUNDS x COV_ROUND): the
            // composite at that point, for pixels that need more (the frame then reports COV_M_ERROR_ROUNDS).
            constexpr uint32_t kRoundLimit = 256;
            double sumAtLimit[3] = {}, usedAtLimit = 0;
            bool behind = false;
            most = std::max(most, (uint32_t)px.size());
            for (const Frag* f : px)
            {
                ++walked;
                float d;
                std::memcpy(&d, &f->depthBits, 4);
                if (d < bandDepth)
                {
                    ++hidden;
                    behind = true;
                    break;
                }
                const uint32_t bits = bitCount(f->mask);
                const double seen = bits > 0 ? bitCount(f->mask & ~covered) / (double)bits : 1 - bitCount(covered) / 32.0;
                const double w = std::min(((f->packed >> 16) & 0x3FF) / 1023.0 * seen, std::max(1 - used, 0.0));
                if (w > 0)
                {
                    ++visible;
                    // The fragment's point: the centroid ray on the triangle's plane; interpolated (flat) normal.
                    double D[3], Dx[3];
                    pixelRay(desc, f->cx, f->cy, D, Dx);
                    const float3 fn = bm.normals[bm.indices[3 * f->tri]];
                    const scene::Material& mat = s.materials[bm.submeshes[f->tri / 30].material];
                    const double nD = fn.x * D[0] + fn.y * D[1] + fn.z * D[2];
                    float3 n = fn;
                    if (!(nD < 0) && mat.twoSided) n = n * -1.0f;
                    model::Surface su;
                    su.cls = mat.cls;
                    su.baseColor = mat.baseColor;
                    su.roughness = mat.roughness;
                    su.metallic = mat.metallic;
                    su.specular = mat.specular;
                    su.transmission = mat.transmission;
                    const float3 v = normalize(float3{ (float)-D[0], (float)-D[1], (float)-D[2] });
                    const float3 L = (cpuSun(su, n, v, l0, s.sun.angularRadius) * E + mat.emissive) * (float)exposure;
                    sum[0] += w * L.x;
                    sum[1] += w * L.y;
                    sum[2] += w * L.z;
                }
                used += w;
                if (walked == kRoundLimit)
                {
                    std::copy(sum, sum + 3, sumAtLimit);
                    usedAtLimit = used;
                }
                if (complete == 0 && used >= 1 - 1e-6) complete = walked;
                covered |= f->mask;
                if (covered == 0xFFFFFFFFu) break;
            }
            // Fragments the composite has to take: up to the one that fills the union or the weights, or all in front
            // of the band A surface.
            uint32_t needed = behind ? walked - 1 : walked;
            if (complete > 0) needed = std::min(needed, complete);
            const bool truncated = px.size() > 16 && needed > kRoundLimit;
            overLimit += truncated;
            if (truncated)
            {
                std::copy(sumAtLimit, sumAtLimit + 3, sum);
                used = usedAtLimit;
            }
            layered += visible > 1;
            heavyPixels += px.size() > 16;            // CoverageHeavy* (more than COV_LIGHT)
            rounds += px.size() > 16 && walked > 32;  // several heavy rounds of 32
            runs += px.size() > 1024;                 // several sorted runs of 1,024
            const float4 A = texelOf<float4>(*base, W, x, y);
            const double wA = std::max(1 - used, 0.0);
            const double want[3] = { sum[0] + wA * A.x, sum[1] + wA * A.y, sum[2] + wA * A.z };
            const float4 got = texelOf<float4>(*withLayer, W, x, y);
            const double scale = std::max({ want[0], want[1], want[2], 1e-2 });
            const double e = std::max({ std::fabs(got.x - want[0]), std::fabs(got.y - want[1]), std::fabs(got.z - want[2]) }) / scale;
            if (e > 5e-3 && (e > worst || (px.size() > 16 && failedHeavy++ < 12)))
                logf("  coverage px (%u,%u): %zu fragments (%u needed%s, %u visible), used %.4f: got (%.5f %.5f %.5f) want (%.5f %.5f %.5f), band A (%.5f %.5f %.5f)\n", x, y,
                     px.size(), needed, truncated ? ", truncated" : "", visible, used, got.x, got.y, got.z, want[0], want[1], want[2], A.x, A.y, A.z);
            worst = std::max(worst, e);
            ++checked;
        }
    }
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
            if (!hasFrags[y * W + x])
            {
                const float4 g = texelOf<float4>(*withLayer, W, x, y), a0 = texelOf<float4>(*base, W, x, y);
                worstOther = std::max({ worstOther, (double)std::fabs(g.x - a0.x), (double)std::fabs(g.y - a0.y), (double)std::fabs(g.z - a0.z) });
            }
    const uint32_t errors = shading::latestStats(tf.trackState).coverageErrors;
    logf("coverage composite: %u fragments in %u tiles, %u pixels checked (%u with several visible fragments, %u over the sky, %u heavy, %u walked "
         "past one round, %u with several runs, %u needing more than the rounds take, at most %u in a pixel), %u walks ended by the band A surface; "
         "error bits 0x%x\n",
         fragments, listed, checked, layered, sky, heavyPixels, rounds, runs, overLimit, most, hidden, errors);
    report(heavyPixels >= 10 && rounds >= 1 && runs >= 1 && overLimit >= 1,
           "coverage composite: heavy pixels, walks of several rounds, pixels of several runs and over the round limit present",
           std::min({ heavyPixels / 10, rounds, runs, overLimit }), 1);
    // 0x800 exactly when a pixel needs more fragments than the rounds take (compared above at the limit); nothing else.
    report(errors == (overLimit > 0 ? 0x800u : 0u), "coverage composite: error bits = COV_M_ERROR_ROUNDS iff pixels over the round limit", errors ^ (overLimit > 0 ? 0x800u : 0u), 0);
    report(checked > 500 && layered > 50, "coverage composite: pixels with fragments and with overlapping fragments present", std::min(checked / 10, layered), 50);
    report(worst < 5e-3, "coverage composite: pixels with fragments vs CPU composite of CPU-shaded fragments (rel.)", worst, 5e-3);
    report(worstOther == 0, "coverage composite: pixels without fragments unchanged (abs.)", worstOther, 0);

    // L2c (14.1c): the same layer under local lights, shading.coverage_tile_lights off (every light per record) vs on
    // (the tile x depth-interval FAR field; NEAR, shadowed and horizon-straddling lights per record): every pixel within
    // the 1e-3 rule (P99), the gate of 14.8 L2c. The sun is off so the local lights carry the whole direct term.
    {
        scene::Scene sl = s;
        std::mt19937 rngL(31);
        auto uniL = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rngL); };
        for (int i = 0; i < 24; ++i)
        {
            scene::Light L;
            const int kind = i % 3;
            L.type = kind == 0 ? scene::LightType::Point : (kind == 1 ? scene::LightType::Spot : scene::LightType::Rect);
            L.position = { uniL(-4, 4), uniL(0.8f, 4.5f), uniL(-5, 1.2f) };
            L.forward = normalize(float3{ uniL(-0.5f, 0.5f), -1, uniL(-0.5f, 0.5f) });
            L.right = normalize(cross(L.forward, float3{ 0, 0, 1 }));
            L.intensity = kind == 2 ? uniL(200, 1200) : uniL(200, 2500);
            L.range = uniL(6, 25);
            L.size = kind == 2 ? float2{ uniL(0.2f, 0.5f), uniL(0.2f, 0.4f) } : float2{ 0, 0 };
            L.spotInner = 0.4f;
            L.spotOuter = 0.7f;
            sl.lights.push_back(L);
        }
        sl.sun.illuminance = 0;
        tf.setScene(sl, { 1 });
        const uint32_t N = (uint32_t)sl.lights.size();
        std::vector<uint32_t> llist(64 + N, 0);
        const float nearM = 0.01f, farM = 1000.0f, logRatio = std::log2(farM / nearM);
        llist[0] = 1, llist[1] = 1, llist[2] = 1, llist[3] = 4096;
        std::memcpy(&llist[4], &nearM, 4);
        std::memcpy(&llist[5], &farM, 4);
        std::memcpy(&llist[6], &logRatio, 4);
        llist[8] = 64, llist[9] = 128, llist[10] = (N + 1) & ~1u, llist[11] = N;
        llist[16] = 0, llist[17] = N;
        for (uint32_t i = 0; i < N; i += 2) llist[32 + i / 2] = i | ((i + 1 < N ? i + 1 : 0) << 16);
        ComPtr<ID3D12Resource> llistBuffer = uploadStatic(tf.device, llist.data(), llist.size() * 4, L"test froxel list (coverage tile lights)");
        std::shared_ptr<std::vector<uint8_t>> offL, onL;
        tf.frame.outputLinearHdr = true;
        for (int pass = 0; pass < 2; ++pass)
        {
            tf.quality.applyOverride(pass ? "shading.coverage_tile_lights=true" : "shading.coverage_tile_lights=false");
            tf.run([&](FramePassContext& fc) {
                ViewResources v = tf.mainView(fc, W, H, 0);
                v.color = fc.graph.createTexture({ "m.test.coverage color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
                tf.vis.record(fc, v);
                tracks::materialResolve(fc, v);
                fc.resources.froxelLights = fc.graph.importBuffer(llistBuffer.Get(), { "test froxel lists", llist.size() * 4, 0 });
                v.froxelLights = fc.resources.froxelLights;
                layer.install(fc.graph, v);
                tracks::shading(fc, v);
                (pass ? onL : offL) = tf.readback(fc, v.color);
            });
        }
        tf.quality.applyOverride("shading.coverage_tile_lights=false");
        tf.frame.outputLinearHdr = false;
        std::vector<double> errsL;
        double worstL = 0, largestL = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                const float4 pa = texelOf<float4>(*offL, W, x, y), pb = texelOf<float4>(*onL, W, x, y);
                const double scale = std::max({ (double)pa.x, (double)pa.y, (double)pa.z, 1e-3 });
                const double e = std::max({ std::abs(pa.x - pb.x), std::abs(pa.y - pb.y), std::abs(pa.z - pb.z) }) / scale;
                if (e > worstL && e > 5e-3) logf("  coverage tile lights px (%u,%u): per-record (%.5f %.5f %.5f) field (%.5f %.5f %.5f)\n", x, y, pa.x, pa.y, pa.z, pb.x, pb.y, pb.z);
                worstL = std::max(worstL, e);
                largestL = std::max(largestL, (double)pa.x);
                errsL.push_back(e);
            }
        std::sort(errsL.begin(), errsL.end());
        const double p99L = errsL.empty() ? 1 : errsL[std::min(errsL.size() - 1, (size_t)(errsL.size() * 0.99))];
        logf("coverage tile lights: %zu pixels, largest exposed radiance %.4f, |dE|/E P99 %.3g, worst %.3g\n", errsL.size(), largestL, p99L, worstL);
        report(largestL > 0.02, "coverage tile lights: the local lights light the frame (exposed radiance)", largestL, 0.02);
        report(p99L <= 1e-3, "coverage tile lights: FAR field vs per-record loop, |dE|/E P99 (14.8 L2c gate)", p99L, 1e-3);
        report(worstL <= 1e-2, "coverage tile lights: FAR field vs per-record loop, worst pixel", worstL, 1e-2);
    }
}

} // namespace

// ---------------------------------------------------------------- coverage composite growth (--coverage-growth)
// One tile over the sky holds N synthetic coverage records (v1.41 layout; triangles of a real cluster, masks empty, area
// 1/1023, random depths), spread over the tile's 64 pixels or all in one pixel; every coverage stage is timed per dispatch. Tiny areas
// make every heavy pixel walk all COV_ROUNDS rounds (the rounds' worst case; COV_M_ERROR_ROUNDS is then expected).
// The growth stops before a dispatch could come near the TDR limit: when the slowest dispatch passes 50 ms, the next
// size is not run (COVERAGE_REDESIGN validation order (1), coordinator 2026-09-26).
int testCoverageGrowth(TestFrame& tf)
{
    scene::Scene s;
    s.name = "coverage growth";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { 0.5f, 0.45f, 0.4f };
    ground.roughness = 0.6f;
    s.materials.push_back(ground);
    const uint32_t plane = addPlane(s, 40, 0);
    scene::Mesh bm;
    bm.name = "blades";
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> u01(0, 1);
    for (uint32_t k = 0; k < 30; ++k)
    {
        const float3 c{ -1.0f + 2.0f * u01(rng), 1.5f + u01(rng), -3.0f - u01(rng) };
        const float3 p[3] = { c, c + float3{ 0.1f, 0, 0 }, c + float3{ 0, 0.1f, 0 } };
        for (int i = 0; i < 3; ++i)
        {
            bm.indices.push_back((uint32_t)bm.positions.size());
            bm.positions.push_back(p[i]);
            bm.normals.push_back({ 0, 0, 1 });
            bm.tangents.push_back({ 1, 0, 0, 1 });
            bm.uv0.push_back({ 0, 0 });
        }
    }
    bm.submeshes.push_back({ 0, (uint32_t)bm.indices.size(), 0 });
    s.meshes.push_back(bm);
    scene::Instance a;
    a.mesh = plane;
    s.instances.push_back(a);
    scene::Instance b;
    b.mesh = (uint32_t)s.meshes.size() - 1;
    s.instances.push_back(b);
    s.sun.direction = normalize(float3{ 0.3f, 0.6f, -0.75f });
    scene::Camera cam;
    cam.name = "growth";
    cam.position = { 0, 1.1f, 1.5f };
    cam.forward = normalize(float3{ 0, -0.12f, -1 });
    cam.ev100 = 13;
    s.cameras.push_back(cam);
    tf.setScene(s, { 1 });
    const uint32_t visibleBlades = tf.vis.rasterised;

    GpuProfiler profiler(tf.device, 1, 1024);
    tf.profiler = &profiler;
    const uint32_t W = 256, H = 256, tilesX = W / 8, tiles = tilesX * (H / 8), tile = 1 * tilesX + 16;  // row 1: sky
    int failures = 0;
    for (int onePixel = 0; onePixel < 2; ++onePixel)
    {
        double slowest = 0;
        for (uint32_t n : { 10000u, 30000u, 100000u, 300000u })
        {
            if (slowest > 50)
            {
                logf("coverage growth %s: %u records not run (slowest dispatch %.1f ms > 50 ms)\n", onePixel ? "one pixel" : "64 pixels", n, slowest);
                continue;
            }
            std::vector<std::vector<std::array<uint32_t, 4>>> tileRecords(tiles);
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t tri = rng() % 30, pixel = onePixel ? 9 : i % 64;
                const float depth = 0.01f + 0.5f * u01(rng);
                uint32_t bits;
                std::memcpy(&bits, &depth, 4);
                // No subsample (seen = what the union left), normal +z, area 1/1023.
                tileRecords[tile].push_back({ ((visibleBlades << 7) | tri) + 1, bits, 0u, 0x8080u | (1u << 16) | (pixel << 26) });
            }
            const TestCoverageLayer layer(tf.device, tilesX, H / 8, tileRecords);
            FrameTiming timing;
            for (int pass = 0; pass < 2; ++pass)  // the first frame compiles the pipelines
            {
                tf.run([&](FramePassContext& fc) {
                    ViewResources v = tf.mainView(fc, W, H, 0);
                    v.color = fc.graph.createTexture({ "m.test.growth color", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
                    tf.vis.record(fc, v);
                    tracks::materialResolve(fc, v);
                    layer.install(fc.graph, v);
                    tracks::shading(fc, v);
                });
                timing = tf.lastTiming;
            }
            const uint32_t errors = shading::latestStats(tf.trackState).coverageErrors;
            std::string line;
            double total = 0, slowestHere = 0, rounds = 0;
            for (const PassTiming& p : timing.passes)
            {
                if (p.name.rfind("m.coverage", 0) != 0) continue;
                const double ms = p.durationMs();
                total += ms;
                slowestHere = std::max(slowestHere, ms);
                if (p.name == "m.coverage.round") rounds += ms;
                else if (p.name != "m.coverage.round args")
                {
                    char buf[96];
                    std::snprintf(buf, sizeof buf, " %s %.3f", p.name.size() > 11 ? p.name.c_str() + 11 : "light", ms);
                    line += buf;
                }
            }
            slowest = std::max(slowest, slowestHere);
            logf("coverage growth %s, %6u records: total %.3f ms, slowest dispatch %.3f ms, rounds %.3f ms;%s; error bits 0x%x\n", onePixel ? "one pixel" : "64 pixels", n, total,
                 slowestHere, rounds, line.c_str(), errors);
            // Tiny areas: a pixel walks all its fragments, so COV_M_ERROR_ROUNDS exactly when one holds more than the
            // rounds take (COV_ROUNDS x COV_ROUND = 256), nothing else.
            const uint32_t perPixel = onePixel ? n : (n + 63) / 64, expected = perPixel > 256 ? 0x800u : 0u;
            if (errors != expected)
            {
                logf("  unexpected error bits 0x%x (expected 0x%x)\n", errors, expected);
                ++failures;
            }
        }
    }
    tf.profiler = nullptr;
    return failures;
}

// ---------------------------------------------------------------- planar view products (v1.22)
// A planar reflection view shades with S's own froxel lists and air volume. When the frame's main view has lists and a
// planar view has none, the shading fails instead of leaving local lights and air out of the reflection.
void testPlanarProducts(TestFrame& tf, Report& report)
{
    const std::vector<uint32_t> words(64, 0);
    ComPtr<ID3D12Resource> lists = uploadStatic(tf.device, words.data(), words.size() * 4, L"test main froxel lists");
    std::string error;
    tf.run([&](FramePassContext& fc) {
        const ViewResources mainView = tf.mainView(fc, 64, 64, 0);
        fc.resources.froxelLights = fc.graph.importBuffer(lists.Get(), { "test main froxel lists", words.size() * 4, 0 });
        ViewResources v;
        v.view = ViewDesc::planarReflection(mainView.view, float4{ 0, 1, 0, 0 }, 0, 0, 32, 32);
        v.frameConstants = fc.frameConstantsFor(v.view);
        v.color = fc.graph.createTexture({ "m.test.planar.color", 32, 32, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        try
        {
            tracks::shading(fc, v);
        }
        catch (const std::exception& e)
        {
            error = e.what();
        }
    });
    logf("planar products: %s\n", error.empty() ? "no failure" : error.c_str());
    report(error.find("froxel lists") != std::string::npos, "planar view without S's froxel lists while the main view has them fails", error.empty() ? 0 : 1, 1);
}

// ---------------------------------------------------------------- 12
double dielectricFresnel(double cosI, double eta)  // unpolarised, eta = n1 / n2; 1 under total internal reflection
{
    cosI = std::clamp(cosI, 0.0, 1.0);
    const double sin2T = eta * eta * (1 - cosI * cosI);
    if (sin2T >= 1) return 1;
    const double cosT = std::sqrt(1 - sin2T);
    const double rs = (eta * cosI - cosT) / (eta * cosI + cosT), rp = (eta * cosT - cosI) / (eta * cosT + cosI);
    return 0.5 * (rs * rs + rp * rp);
}

// service: 0 = no refraction service (JOBS=0), 1 = FakeRefraction.hlsl traces every job (R-1 / R-2 through
// TranslucentComposite JOBS=1 and TranslucentApply), 2 = a service that traces nothing (every job's fallback).
double secondsSinceStart()
{
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void testGlassComposite(TestFrame& tf, Report& report, int service = 0)
{
    const uint32_t W = 480, H = 270;
    struct Case
    {
        const char* name;
        float ior, roughness;
        float3 tint, sun;
        bool solid;
    };
    const Case plain[] = {
        { "index 1 (T_p = tint, R_p = 0)", 1.0f, 0.05f, { 0.6f, 0.3f, 0.9f }, { 0.3f, 0.6f, -0.75f }, false },
        { "index 1.5, sun behind the pane (R_p, T_p)", 1.5f, 0.05f, { 0.8f, 0.5f, 0.2f }, { 0.3f, 0.6f, -0.75f }, false },
        { "index 1.5, rough, sun in front (R_p x sun specular)", 1.5f, 0.4f, { 0.9f, 0.9f, 0.7f }, { 0.6f, 0.5f, 0.6f }, false },
    };
    // (the sun behind the glass for the mirror cases: the sun specular is the plain cases' check, and the CPU disk integral of
    // a mirror lobe takes minutes)
    const Case jobbed[] = {
        { "jobs: mirror pane (R-1)", 1.5f, 0.05f, { 0.8f, 0.5f, 0.2f }, { 0.3f, 0.6f, -0.75f }, false },
        { "jobs: mirror solid (R-1 + R-2)", 1.5f, 0.05f, { 0.8f, 0.5f, 0.2f }, { 0.3f, 0.6f, -0.75f }, true },
        { "jobs: rough solid (straight path)", 1.5f, 0.4f, { 0.9f, 0.9f, 0.7f }, { 0.6f, 0.5f, 0.6f }, true },
    };
    const float mirrorMax = (float)tf.quality.number("reflection.mirror_roughness_max");
    for (const Case& k : service == 0 ? std::span<const Case>(plain) : std::span<const Case>(jobbed))
    {
        scene::Scene s;
        s.name = "glass composite";
        scene::Material ground;
        ground.name = "ground";
        ground.baseColor = { 0.5f, 0.45f, 0.4f };
        ground.roughness = 0.5f;
        scene::Material ball;
        ball.name = "ball";
        ball.baseColor = { 0.2f, 0.4f, 0.7f };
        ball.roughness = 0.3f;
        scene::Material glass;
        glass.name = "glass";
        glass.cls = scene::MaterialClass::Glass;
        glass.twoSided = !k.solid;
        glass.baseColor = k.tint;
        glass.roughness = k.roughness;
        glass.ior = k.ior;
        s.materials = { ground, ball, glass };
        const uint32_t plane = addPlane(s, 40, 0), sphere = addSphere(s, 1, 48, 96, 1), pane = addPlane(s, 2, 2);
        s.instances.resize(3);
        s.instances[0].mesh = plane;
        s.instances[1].mesh = sphere;
        s.instances[1].transform = float3x4::translation({ 0, 1, 0 });
        s.instances[2].mesh = pane;
        {
            // upright (plane normal +y -> +z), turned 40 deg about y, 3.3 m in front of the camera
            const float c = std::cos(0.698132f), sn = std::sin(0.698132f);
            float3x4& m = s.instances[2].transform;
            m = float3x4::translation({ 0.3f, 1.3f, 2.2f });
            m.m[0][0] = c, m.m[0][1] = sn, m.m[0][2] = 0;
            m.m[1][0] = 0, m.m[1][1] = 0, m.m[1][2] = -1;
            m.m[2][0] = -sn, m.m[2][1] = c, m.m[2][2] = 0;
        }
        s.sun.direction = normalize(k.sun);
        scene::Camera cam;
        cam.name = "glass";
        cam.position = { 0.5f, 1.6f, 5.5f };
        cam.forward = normalize(float3{ -0.1f, -0.15f, -1 });
        cam.ev100 = 13;
        s.cameras.push_back(cam);
        tf.setScene(s, { 2 });  // the pane is not in band A: its clusters are listed for its vis ids only
        logf("glass [%s]: scene set (%.1f s)\n", k.name, secondsSinceStart());

        uint32_t element = UINT32_MAX;
        for (uint32_t e = 0; e < (uint32_t)tf.vis.visible.size(); ++e)
            if (tf.vis.visible[e].instance == 2) element = e;
        M_CHECK(element != UINT32_MAX, "pane cluster listed");
        const scene::Mesh& pm = s.meshes[pane];
        float3 corner[4];
        for (int i = 0; i < 4; ++i) corner[i] = s.instances[2].transform.transformPoint(pm.positions[i]);
        const float3 n0 = normalize(s.instances[2].transform.transformVector(float3{ 0, 1, 0 }));

        // CPU ray cast of the pane per pixel centre: vis id (element, triangle) and class (1: all of the 3 x 3 block's
        // centres on the pane, 2: the pane's outline)
        const ViewDesc desc = ViewDesc::fromCamera(cam, W, H, float4x4{});
        std::vector<int8_t> hitTri((size_t)W * H, -1);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                double D[3], Dx[3];
                pixelRay(desc, x + 0.5, y + 0.5, D, Dx);
                for (uint32_t t = 0; t < 2; ++t)
                {
                    const uint32_t* ix = &pm.indices[3 * t];
                    const float3 a = corner[ix[0]], b = corner[ix[1]], c = corner[ix[2]];
                    const double e1[3] = { b.x - a.x, b.y - a.y, b.z - a.z }, e2[3] = { c.x - a.x, c.y - a.y, c.z - a.z };
                    const double p[3] = { D[1] * e2[2] - D[2] * e2[1], D[2] * e2[0] - D[0] * e2[2], D[0] * e2[1] - D[1] * e2[0] };
                    const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
                    if (std::fabs(det) < 1e-12) continue;
                    const double o[3] = { cam.position.x - a.x, cam.position.y - a.y, cam.position.z - a.z };
                    const double u = (o[0] * p[0] + o[1] * p[1] + o[2] * p[2]) / det;
                    const double q[3] = { o[1] * e1[2] - o[2] * e1[1], o[2] * e1[0] - o[0] * e1[2], o[0] * e1[1] - o[1] * e1[0] };
                    const double v = (D[0] * q[0] + D[1] * q[1] + D[2] * q[2]) / det, dist = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
                    if (u >= 0 && v >= 0 && u + v <= 1 && dist > 0)
                    {
                        hitTri[(size_t)y * W + x] = (int8_t)t;
                        break;
                    }
                }
            }
        std::vector<uint32_t> tvis((size_t)W * H, 0);
        std::vector<uint8_t> tcls((size_t)W * H, 0);
        uint32_t class1 = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                const size_t i = (size_t)y * W + x;
                bool all = true, any = false;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int xx = (int)x + dx, yy = (int)y + dy;
                        const bool on = xx >= 0 && yy >= 0 && xx < (int)W && yy < (int)H && hitTri[(size_t)yy * W + xx] >= 0;
                        all = all && on;
                        any = any || on;
                    }
                if (hitTri[i] >= 0) tvis[i] = (element << 7) + (uint32_t)hitTri[i] + 1;
                tcls[i] = all ? 1 : any ? 2 : 0;
                class1 += tcls[i] == 1;
            }

        auto upload = [&](FramePassContext& fc, const char* name, DXGI_FORMAT format, uint32_t bytesPerTexel, const void* data) {
            const TextureRef t = fc.graph.createTexture({ name, W, H, 1, 1, format });
            const uint32_t pitch = TestFrame::rowPitch(W, bytesPerTexel);
            ComPtr<ID3D12Resource> staging = makeBuffer(tf.device, (uint64_t)pitch * H, D3D12_HEAP_TYPE_UPLOAD);
            uint8_t* p = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(staging->Map(0, &none, reinterpret_cast<void**>(&p)), "map");
            for (uint32_t y = 0; y < H; ++y) std::memcpy(p + (size_t)y * pitch, static_cast<const uint8_t*>(data) + (size_t)y * W * bytesPerTexel, (size_t)W * bytesPerTexel);
            staging->Unmap(0, nullptr);
            ID3D12Resource* src = staging.Get();
            tf.keep(staging);
            fc.graph.addPass("m.test.upload layer", QueueType::Graphics, [&](PassBuilder& b) { b.use(t, Use::CopyDst); },
                             [=](PassContext& c) {
                                 D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                                 to.pResource = c.resource(t);
                                 to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                 from.pResource = src;
                                 from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                 from.PlacedFootprint.Footprint = { format, W, H, 1, pitch };
                                 c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                             });
            return t;
        };
        std::shared_ptr<std::vector<uint8_t>> base, with;
        tf.frame.outputLinearHdr = true;
        for (int pass = 0; pass < 2; ++pass)
            tf.run([&](FramePassContext& fc) {
                ViewResources v = tf.mainView(fc, W, H, 0);
                v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
                tf.vis.record(fc, v);
                if (pass == 1)
                {
                    v.translucentVis = upload(fc, "m.test.translucent vis", DXGI_FORMAT_R32_UINT, 4, tvis.data());
                    v.translucentClass = upload(fc, "m.test.translucent class", DXGI_FORMAT_R8_UINT, 1, tcls.data());
                }
                if (service != 0)
                {
                    ID3D12PipelineState* fake = fc.shaders.compute("Passes/Shading/Tests/FakeRefraction");
                    fc.services.traceRefractions = [fake, service](FramePassContext& c, BufferRef jobs, BufferRef results, uint32_t maxJobs) {
                        if (service != 1) return;
                        c.graph.addPass("m.test.fake refraction", QueueType::Graphics,
                                        [&](PassBuilder& b) {
                                            b.use(jobs, Use::SrvCompute);
                                            b.use(results, Use::UavCompute);
                                        },
                                        [=](PassContext& pc) {
                                            const uint32_t k[4] = { pc.srv(jobs), pc.uav(results), maxJobs, 0 };
                                            pc.cmd->SetPipelineState(fake);
                                            pc.computeConstants(k, 4);
                                            pc.cmd->Dispatch((maxJobs + 63) / 64, 1, 1);
                                        });
                    };
                }
                tracks::materialResolve(fc, v);
                tracks::shading(fc, v);
                (pass == 0 ? base : with) = tf.readback(fc, v.color);
            });
        tf.frame.outputLinearHdr = false;
        const shading::Stats st = shading::latestStats(tf.trackState);
        logf("glass [%s]: rendered (%.1f s)\n", k.name, secondsSinceStart());

        const double exposure = 1.0 / (1.2 * std::exp2(13.0)), thetaS = s.sun.angularRadius;
        const float3 E = s.sun.color * s.sun.illuminance, l0 = normalize(s.sun.direction);
        model::Surface mirror;  // F = 1: the GGX lobe of the pane's specular, compensation removed below
        mirror.baseColor = { 1, 1, 1 };
        mirror.metallic = 1;
        mirror.roughness = k.roughness;
        double worst = 0, worstOther = 0, minF = 1, maxF = 0, maxSpec = 0;
        uint32_t checked = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                const size_t i = (size_t)y * W + x;
                const float4 a = texelOf<float4>(*base, W, x, y), g = texelOf<float4>(*with, W, x, y);
                if (tcls[i] != 1)
                {
                    worstOther = std::max({ worstOther, (double)std::fabs(g.x - a.x), (double)std::fabs(g.y - a.y), (double)std::fabs(g.z - a.z) });
                    continue;
                }
                double D[3], Dx[3];
                pixelRay(desc, x + 0.5, y + 0.5, D, Dx);
                const float3 v = normalize(float3{ (float)-D[0], (float)-D[1], (float)-D[2] });
                const float3 n = dot(n0, v) >= 0 ? n0 : n0 * -1.0f;
                const double NoV = std::max(0.0, (double)dot(n, v));
                const double F = dielectricFresnel(NoV, 1.0 / std::max(k.ior, 1.0001f));
                minF = std::min(minF, F);
                maxF = std::max(maxF, F);
                float3 spec{};
                if (dot(n, l0) > 0)
                    spec = cpuSun(mirror, n, v, l0, thetaS) * model::directionalAlbedo((float)NoV, k.roughness) * E * (float)exposure;
                // JOBS=1: a mirror pixel's reflection job, and a solid body's refraction job (front face: F and 1 - F)
                const bool mirrorJob = service != 0 && k.roughness < mirrorMax, refractJob = mirrorJob && k.solid;
                const float3 dr = v * -1.0f + n * (2 * (float)NoV);
                float3 fakeReflect = { std::fabs(dr.x) * 0.05f + 0.003f, std::fabs(dr.y) * 0.05f + 0.003f, std::fabs(dr.z) * 0.05f + 0.003f }, fakeRefract{};
                if (refractJob)
                {
                    const float eta = 1 / k.ior, c = (float)NoV, k2 = 1 - eta * eta * (1 - c * c);
                    const float3 dt = v * -eta + n * (eta * c - std::sqrt(k2));
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        const float sigma = -std::log(std::max((&k.tint.x)[ch], 1e-4f)) / 0.01f;
                        (&fakeRefract.x)[ch] = std::fabs((&dt.x)[ch]) * 0.05f + sigma * 1e-3f + k.ior * 0.01f + 0.002f;
                    }
                }
                double want[3];
                for (int c = 0; c < 3; ++c)
                {
                    const double t = std::clamp((double)(&k.tint.x)[c], 0.0, 1.0), d = 1 - F * F * t * t;
                    const double Rp = F + (1 - F) * (1 - F) * F * t * t / d, Tp = (1 - F) * (1 - F) * t / d;
                    const double Rs = refractJob ? F : Rp;
                    want[c] = Rs * (&spec.x)[c];
                    if (service == 1 && mirrorJob) want[c] += Rs * (&fakeReflect.x)[c];
                    want[c] += service == 1 && refractJob ? (1 - F) / ((double)k.ior * k.ior) * (&fakeRefract.x)[c] : Tp * (&a.x)[c];
                }
                maxSpec = std::max(maxSpec, (double)spec.y);
                const double scale = std::max({ want[0], want[1], want[2], 1e-2 });
                const double e = std::max({ std::fabs(g.x - want[0]), std::fabs(g.y - want[1]), std::fabs(g.z - want[2]) }) / scale;
                if (e > 5e-3 && e > worst)
                {
                    logf("  glass px (%u,%u): got (%.5f %.5f %.5f) want (%.5f %.5f %.5f), behind (%.5f %.5f %.5f), F %.5f\n", x, y, g.x, g.y, g.z, want[0], want[1], want[2], a.x, a.y, a.z, F);
                    if (service == 1)
                        logf("    mirror %d refract %d: dr (%.4f %.4f %.4f), fake reflect (%.5f %.5f %.5f), fake refract (%.5f %.5f %.5f)\n", mirrorJob, refractJob, dr.x, dr.y,
                             dr.z, fakeReflect.x, fakeReflect.y, fakeReflect.z, fakeRefract.x, fakeRefract.y, fakeRefract.z);
                }
                worst = std::max(worst, e);
                ++checked;
            }
        logf("glass [%s]: %u class 1 pixels checked, F %.4f .. %.4f, largest sun specular %.4f (exposed); statistics: %u pane, %u solid, %u unlit\n", k.name, checked, minF,
             maxF, maxSpec, st.glassPanePixels, st.glassSolidPixels, st.glassUnlitPixels);
        report(checked > 5000 && worst < 5e-3, unx::format("glass [%s]: class 1 pixels vs R_p x sun specular + T_p x behind (rel.)", k.name).c_str(), worst, 5e-3);
        report(worstOther == 0, unx::format("glass [%s]: pixels outside class 1 unchanged", k.name).c_str(), worstOther, 0);
        logf("glass [%s]: evaluated (%.1f s)\n", k.name, secondsSinceStart());
        const bool straight = k.solid && !(service != 0 && k.roughness < mirrorMax);  // solid glass without refraction
        const uint32_t exact = straight ? st.glassSolidPixels : st.glassPanePixels, other = straight ? st.glassPanePixels : st.glassSolidPixels;
        report(exact == class1 && other == 0 && st.glassUnlitPixels == 0, unx::format("glass [%s]: statistics count the class 1 pixels", k.name).c_str(),
               std::fabs((double)exact - class1), 0);
    }
}

// ---------------------------------------------------------------- 13. pre-shaded coverage records (CoverageSpecial.hlsl)
// v1.75: band B records of an M pre-shaded class (kind 5, COV_PRESHADE_ID) are shaded before the composite into
// ViewResources::coverageRecordRadiance - the class's material into a scratch per list entry, then the lighting of the
// record at the pixel found from its element (tile list search) - and the composite reads the value. A ground plane in
// band A and 120 blades in band B (records from edge functions at 32 subsamples, area = covered share, depth at the pixel
// centre), rendered twice with the same records: blades of a Standard material, then of a Terrain material with one layer
// equal to it (splat weight 1: the same base colour, roughness and normal), its records marked kind 5 and listed in the
// special list. Every pixel must match the first image within the record radiance's f16 rounding (relative 2^-11 of a
// record's share, plus the float sum's order), and the special list must hold every record.
void testPreshadedRecords(TestFrame& tf, Report& report)
{
    auto buildScene = [](bool terrain, float coat = 0, float sheen = 0) {
        scene::Scene s;
        s.name = "preshade test";
        scene::Material ground;
        ground.name = "ground";
        ground.baseColor = { 0.5f, 0.45f, 0.4f };
        ground.roughness = 0.6f;
        s.materials.push_back(ground);
        scene::Material blade;
        blade.name = "blade";
        blade.baseColor = { 0.3f, 0.55f, 0.2f };
        blade.roughness = 0.45f;
        blade.metallic = 0.2f;
        blade.clearcoat = coat;  // run 2 (A9): a coat of cover 0.001, pre-shaded through MODE 4 / 5 / 6
        blade.sheenColor = { sheen, sheen, sheen };  // runs 3, 4 (A9): a sheen of colour 0.01 and 0.02, the same path
        if (!terrain)
            s.materials.push_back(blade);
        else
        {
            scene::Texture splat;
            splat.name = "splat";
            splat.width = splat.height = 2;
            splat.format = scene::TextureFormat::Rgba8Linear;
            for (int i = 0; i < 4; ++i) splat.texels.insert(splat.texels.end(), { 255, 0, 0, 0 });
            s.textures.push_back(splat);
            scene::Material t;
            t.name = "terrain";
            t.cls = scene::MaterialClass::Terrain;
            t.terrainSplat[0] = 0;
            t.terrainLayers = { { 2, { 1, 1 }, { 0, 0 } } };
            s.materials.push_back(t);  // index 1, as the Standard blade
            s.materials.push_back(blade);  // the layer
        }
        const uint32_t plane = addPlane(s, 40, 0);
        std::mt19937 rng(7702);
        std::uniform_real_distribution<float> u01(0, 1);
        scene::Mesh bm;
        bm.name = "blades";
        for (uint32_t sub = 0; sub < 4; ++sub)
        {
            const uint32_t first = (uint32_t)bm.indices.size();
            for (uint32_t k = 0; k < 30; ++k)
            {
                const float3 c{ -1.6f + 3.2f * u01(rng), 0.15f + 1.6f * u01(rng), -1.5f - 2.5f * u01(rng) };
                const float size = 0.02f + 0.2f * u01(rng);
                float3 q[3];
                for (int i = 0; i < 3; ++i) q[i] = c + float3{ size * (2 * u01(rng) - 1), size * (2 * u01(rng) - 1), size * (2 * u01(rng) - 1) };
                if (dot(cross(q[1] - q[0], q[2] - q[0]), float3{ 0, 0.12f, 1 }) < 0) std::swap(q[1], q[2]);  // facing the camera
                const float3 fn = normalize(cross(q[1] - q[0], q[2] - q[0]));
                for (int i = 0; i < 3; ++i)
                {
                    bm.indices.push_back((uint32_t)bm.positions.size());
                    bm.positions.push_back(q[i]);
                    bm.normals.push_back(fn);
                    bm.tangents.push_back({ 1, 0, 0, 1 });
                    bm.uv0.push_back({ u01(rng), u01(rng) });
                }
            }
            bm.submeshes.push_back({ first, (uint32_t)bm.indices.size() - first, 1 });
        }
        s.meshes.push_back(bm);
        scene::Instance a;
        a.mesh = plane;
        s.instances.push_back(a);
        scene::Instance b;
        b.mesh = (uint32_t)s.meshes.size() - 1;
        s.instances.push_back(b);
        s.sun.direction = normalize(float3{ 0.3f, 0.6f, -0.75f });
        scene::Camera cam;
        cam.name = "preshade";
        cam.position = { 0, 1.1f, 1.5f };
        cam.forward = normalize(float3{ 0, -0.12f, -1 });
        cam.ev100 = 13;
        s.cameras.push_back(cam);
        return s;
    };

    const uint32_t W = 320, H = 180, tilesX = (W + 7) / 8, tilesY = (H + 7) / 8;
    std::shared_ptr<std::vector<uint8_t>> images[5];
    uint32_t recordCount = 0, listedSpecial = 0;
    for (int run = 0; run < 5; ++run)
    {
        const scene::Scene s = buildScene(run == 1, run == 2 ? 0.001f : 0.0f, run == 3 ? 0.01f : run == 4 ? 0.02f : 0.0f);
        tf.setScene(s, { 1 });
        ViewDesc desc;
        tf.run([&](FramePassContext& fc) { desc = tf.mainView(fc, W, H, 0).view; });
        auto project = [&](float3 w, double& px, double& py, double& z) {
            const double o[3] = { (double)w.x - desc.position.x, (double)w.y - desc.position.y, (double)w.z - desc.position.z };
            double vv[3];
            for (int r = 0; r < 3; ++r) vv[r] = desc.view.m[r][0] * o[0] + desc.view.m[r][1] * o[1] + desc.view.m[r][2] * o[2];
            double clip[4];
            for (int r = 0; r < 4; ++r) clip[r] = desc.proj.m[r][0] * vv[0] + desc.proj.m[r][1] * vv[1] + desc.proj.m[r][2] * vv[2] + desc.proj.m[r][3];
            px = (clip[0] / clip[3] * 0.5 + 0.5) * W;
            py = (0.5 - clip[1] / clip[3] * 0.5) * H;
            z = clip[2] / clip[3];
        };
        const scene::Mesh& bm = s.meshes.back();
        const uint32_t visibleBlades = tf.vis.rasterised;
        std::vector<std::vector<std::array<uint32_t, 4>>> tileRecords(tilesX * tilesY);
        for (uint32_t t = 0; t < (uint32_t)bm.indices.size() / 3; ++t)
        {
            double sx[3], sy[3], sz[3];
            for (int i = 0; i < 3; ++i) project(bm.positions[bm.indices[3 * t + i]], sx[i], sy[i], sz[i]);
            const double det = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
            if (std::fabs(det) < 1e-9) continue;
            const int x0 = std::max(0, (int)std::floor(std::min({ sx[0], sx[1], sx[2] }))), x1 = std::min((int)W - 1, (int)std::floor(std::max({ sx[0], sx[1], sx[2] })));
            const int y0 = std::max(0, (int)std::floor(std::min({ sy[0], sy[1], sy[2] }))), y1 = std::min((int)H - 1, (int)std::floor(std::max({ sy[0], sy[1], sy[2] })));
            const uint32_t visId = ((((visibleBlades + t / 30) << 7) | (t % 30)) + 1) | (run >= 1 ? 0x40000000u : 0u);  // COV_PRESHADE_ID
            for (int py = y0; py <= y1; ++py)
                for (int px = x0; px <= x1; ++px)
                {
                    uint32_t mask = 0;
                    for (uint32_t i = 0; i < 32; ++i)
                    {
                        uint32_t r = 0;
                        for (int bit = 0; bit < 5; ++bit) r |= ((i >> bit) & 1u) << (4 - bit);
                        const double qx = px + (i + 0.5) / 32.0, qy = py + r / 32.0 + 1.0 / 64.0;
                        double w[3];
                        for (int e = 0; e < 3; ++e)
                        {
                            const int e1 = (e + 1) % 3, e2 = (e + 2) % 3;
                            w[e] = ((sx[e2] - sx[e1]) * (qy - sy[e1]) - (sy[e2] - sy[e1]) * (qx - sx[e1])) / det;
                        }
                        if (w[0] >= 0 && w[1] >= 0 && w[2] >= 0) mask |= 1u << i;
                    }
                    if (!mask) continue;
                    double bc[3];
                    for (int e = 0; e < 3; ++e)
                    {
                        const int e1 = (e + 1) % 3, e2 = (e + 2) % 3;
                        bc[e] = ((sx[e2] - sx[e1]) * (py + 0.5 - sy[e1]) - (sy[e2] - sy[e1]) * (px + 0.5 - sx[e1])) / det;
                    }
                    const float depth = (float)(bc[0] * sz[0] + bc[1] * sz[1] + bc[2] * sz[2]);
                    if (!(depth > 0)) continue;
                    uint32_t bits = 0;
                    for (uint32_t m = mask; m; m &= m - 1) ++bits;
                    uint32_t depthBits;
                    std::memcpy(&depthBits, &depth, 4);
                    const uint32_t packed = ((uint32_t)std::lround(bits / 32.0 * 1023) << 16) | ((uint32_t)((px % 8) + 8 * (py % 8)) << 26) | 0x8080u;
                    tileRecords[(py / 8) * tilesX + px / 8].push_back({ visId, depthBits, mask, packed });
                }
        }
        const TestCoverageLayer layer(tf.device, tilesX, tilesY, tileRecords);
        recordCount = layer.stored;
        // V's special list (v1.73): header { count, groups, 1, 1 }, then { element, kind 5 } per pre-shaded record.
        std::vector<uint32_t> special = { 0, 0, 1, 1 };
        for (uint32_t e = 0; e < layer.stored; ++e)
            if ((layer.records[4 * e] >> 30) == 1u) special.insert(special.end(), { e, 5u });
        const uint32_t count = (uint32_t)(special.size() - 4) / 2;
        special[0] = count;
        special[1] = std::max((count + 63) / 64, 1u);
        if (run == 1) listedSpecial = count;
        special.resize(special.size() + 2, 0);  // (one spare entry: the capacity is above the count, as V sizes it)
        const ComPtr<ID3D12Resource> specialBuf = uploadStatic(tf.device, special.data(), special.size() * 4, L"test coverage special");
        tf.frame.outputLinearHdr = true;
        tf.run([&](FramePassContext& fc) {
            ViewResources v = tf.mainView(fc, W, H, 0);
            v.color = fc.graph.createTexture({ "m.test.preshade color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
            tf.vis.record(fc, v);
            tracks::materialResolve(fc, v);
            layer.install(fc.graph, v);
            v.coverageSpecial = fc.graph.importBuffer(specialBuf.Get(), { "test coverage special", special.size() * 4, 0 });
            tracks::shading(fc, v);
            images[run] = tf.readback(fc, v.color);
        });
        tf.frame.outputLinearHdr = false;
    }
    double worst = 0, worstCoat = 0, sheenChange = 0, sheenLinear = 0;
    uint32_t differing = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            const float4 a = texelOf<float4>(*images[0], W, x, y), b = texelOf<float4>(*images[1], W, x, y), c3 = texelOf<float4>(*images[2], W, x, y);
            const float4 c4 = texelOf<float4>(*images[3], W, x, y), c5 = texelOf<float4>(*images[4], W, x, y);
            const double da[3] = { a.x, a.y, a.z }, db[3] = { b.x, b.y, b.z }, dc[3] = { c3.x, c3.y, c3.z }, ds[3] = { c4.x, c4.y, c4.z };
            const double ds2[3] = { c5.x, c5.y, c5.z };
            for (int c = 0; c < 3; ++c)
            {
                const double rel = std::abs(da[c] - db[c]) / std::max(std::abs(da[c]), 1e-6);
                differing += rel > 0;
                worst = std::max(worst, rel);
                worstCoat = std::max(worstCoat, std::abs(da[c] - dc[c]) / std::max(std::abs(da[c]), 1e-6));
                // the sheen's change is linear in C (keepS and the lobe both are): (C 0.02) - base = 2 x ((C 0.01) - base)
                const double scale = std::max(std::abs(da[c]), 1e-3);
                sheenChange = std::max(sheenChange, std::abs(ds[c] - da[c]) / scale);
                // C2 + C0 - 2*C1 has four rounding weights, each at most 2^-11 for
                // an f16 record. Normalize by the propagated bound, not three weights.
                const double roundingBound = (std::abs(ds2[c]) + std::abs(da[c]) + 2 * std::abs(ds[c])) / 2048 + 1e-6 * scale;
                sheenLinear = std::max(sheenLinear, std::isfinite(ds[c] + ds2[c]) ? std::abs((ds2[c] - da[c]) - 2 * (ds[c] - da[c])) / std::max(roundingBound, 1e-12) : 1e9);
            }
        }
    logf("preshade: %u records, %u listed as kind 5, %u channel values differ\n", recordCount, listedSpecial, differing);
    report(recordCount > 1000 && listedSpecial == recordCount, "preshade: every blade record listed as an M pre-shaded record", (double)(recordCount - listedSpecial), 0);
    // f16 radiance: 2^-11 relative per record share (the band A remainder is the same texel in both)
    report(worst <= 1.0e-3, "preshade: Terrain(1 layer = Standard) records pre-shaded = Standard records shaded in the composite", worst, 1.0e-3);
    report(differing > 0, "preshade: the pre-shaded path ran (its f16 rounding shows)", differing > 0 ? 0.0 : 1.0, 0);
    // A9: cover 0.001 moves the radiance by at most ~0.001 of it (the coat's terms are bounded by the base's here), plus f16
    report(worstCoat <= 3e-3, "preshade: clearcoat records (MODE 4 / 5 / 6, cover 0.001) = Standard records in the composite", worstCoat, 3e-3);
    // A9 sheen through MODE 4 / 5 / 6: the change from the Standard records is linear in the sheen colour (the model's
    // values are the band A path's, ShadingTests --sheen); four f16 rounding weights.
    logf("preshade: sheen colour 0.01 changes the records by up to %.3g of their value\n", sheenChange);
    report(sheenChange > 1e-3, "preshade: sheen records (MODE 4 / 5 / 6) carry the sheen", sheenChange, 1e-3);
    report(sheenLinear <= 1, "preshade: sheen linearity residual / propagated f16 rounding bound", sheenLinear, 1);
}

// ---------------------------------------------------------------- 14. clearcoat sun lobe (A9, CoatSunProbe.hlsl)
// The coat lobe's sun term as ShadeOpaque LAYERED forms it (shSunSpecular with F = 1 at the coat's roughness times
// shCoatSunWeight) against a dense quadrature of scene::model::evaluateCoatLobe x cos over the disk, for both coats, with
// the mirror direction at 0 .. 6 disk radii from the centre; relative to the peak of the case's roughness (as test 2).
// A9 sheen (MATERIAL_LAYERS 1.4): the HLSL mirror against the C++ model on the GPU - the lobe and E_sh against
// evaluateSheenLobe / sheenAlbedo, and the 4-point sun term against the same rule in double and against a dense
// quadrature of the disk (the rule's own error, measured on the CPU at 1.5e-3).
void testSheen(TestFrame& tf, Report& report)
{
    {
        scene::Scene s;  // the scene's tables (coat + sheen in one buffer)
        s.name = "sheen test";
        s.materials.push_back(scene::Material{});
        scene::Instance plane;
        plane.mesh = addPlane(s, 10, 0);
        s.instances.push_back(plane);
        scene::Camera cam;
        cam.name = "sheen";
        cam.position = { 0, 1, 3 };
        cam.forward = normalize(float3{ 0, -0.3f, -1 });
        s.cameras.push_back(cam);
        tf.setScene(s);
    }
    struct Case { float3 n, v, l; float r, rho; };
    std::vector<Case> cases;
    const float rho = 0.2725f * 3.14159265f / 180;
    for (float r : { 0.1f, 0.2f, 0.35f, 0.6f, 1.0f })
        for (float mu : { 0.03f, 0.2f, 0.5f, 0.9f })
            for (float el : { -0.15f, 0.0f, 0.12f, 3.0f, 20.0f, 60.0f })
                for (float az : { 0.0f, 100.0f, 180.0f })
                {
                    const float t = el * 3.14159265f / 180, ph = az * 3.14159265f / 180;
                    cases.push_back({ { 0, 0, 1 }, { std::sqrt(1 - mu * mu), 0, mu }, { std::cos(t) * std::cos(ph), std::cos(t) * std::sin(ph), std::sin(t) }, r, rho });
                }
    std::vector<float4> q;
    for (const Case& c : cases)
    {
        q.push_back({ c.n.x, c.n.y, c.n.z, c.r });
        q.push_back({ c.v.x, c.v.y, c.v.z, 0 });
        q.push_back({ c.l.x, c.l.y, c.l.z, c.rho });
    }
    std::shared_ptr<std::vector<uint8_t>> out;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, 64, 64);
        BufferRef in = fc.graph.createBuffer({ "m.test.sheen queries", q.size() * 16, 16 });
        BufferRef res = fc.graph.createBuffer({ "m.test.sheen results", cases.size() * 16, 16 });
        ComPtr<ID3D12Resource> staging = makeBuffer(tf.device, q.size() * 16, D3D12_HEAP_TYPE_UPLOAD);
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(staging->Map(0, &none, &p), "map");
        std::memcpy(p, q.data(), q.size() * 16);
        staging->Unmap(0, nullptr);
        tf.keep(staging);
        fc.graph.addPass("m.test.upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(in, Use::CopyDst); },
                         [staging, in, bytes = q.size() * 16](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(in), 0, staging.Get(), 0, bytes); });
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/Tests/SheenProbe");
        const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
        const uint32_t count = (uint32_t)cases.size();
        fc.graph.addPass("m.test.sheen probe", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(in, Use::SrvCompute);
                             b.use(res, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             const uint32_t k[4] = { c.srv(in), c.uav(res), count, 0 };
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 4);
                             c.cmd->Dispatch((count + 63) / 64, 1, 1);
                         });
        out = tf.readbackBuffer(fc, res, cases.size() * 16);
    });
    double lobeErr = 0, albedoErr = 0, ruleErr = 0, diskErr = 0;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const Case& c = cases[i];
        float g[4];
        std::memcpy(g, out->data() + i * 16, 16);
        const float lobe = model::evaluateSheenLobe(c.r, c.n, c.v, c.l), albedo = model::sheenAlbedo(c.v.z, c.r);
        const double scale = std::max((double)albedo / 3.14159265, 1e-3);  // the lobe's cosine-weighted scale
        if (c.l.z > 0.01f) lobeErr = std::max(lobeErr, std::abs(g[0] - lobe) * std::max((double)c.l.z, 0.0) / scale);
        albedoErr = std::max(albedoErr, (double)std::abs(g[1] - albedo));
        // the 4-point rule in double, and the disk average (polar midpoint, 16 x 32)
        const float3 du = normalize(cross(std::fabs(c.l.z) < 0.9f ? float3{ 0, 0, 1 } : float3{ 1, 0, 0 }, c.l)), dw = cross(c.l, du);
        const double rule = model::sheenSunRule(c.r, c.n, c.v, c.l, c.rho);
        double disk = 0;
        for (int a = 0; a < 16; ++a)
            for (int b = 0; b < 32; ++b)
            {
                const float rr = c.rho * std::sqrt((a + 0.5f) / 16), ph = 2 * 3.14159265f * (b + 0.5f) / 32;
                const float3 lk = normalize(c.l + du * (rr * std::cos(ph)) + dw * (rr * std::sin(ph)));
                disk += model::evaluateSheenLobe(c.r, c.n, c.v, lk) * std::max(lk.z, 0.0f) / 512;
            }
        ruleErr = std::max(ruleErr, std::abs(g[2] - rule) / std::max(rule, scale));
        diskErr = std::max(diskErr, std::abs(g[2] - disk) / std::max(disk, scale));
    }
    logf("sheen probe: %zu cases; lobe x cos vs C++ %.2e, E_sh %.2e, sun rule vs double %.2e, vs disk average %.2e (relative to max(value, E_sh / pi))\n",
         cases.size(), lobeErr, albedoErr, ruleErr, diskErr);
    report(lobeErr < 1e-3, "sheen: modelSheenLobe x cos vs evaluateSheenLobe (rel. to E_sh / pi)", lobeErr, 1e-3);
    report(albedoErr < 1e-5, "sheen: modelSheenAlbedo vs sheenAlbedo (abs.)", albedoErr, 1e-5);
    report(ruleErr < 1e-3, "sheen: modelSheenSun vs the 4-point rule in double (rel.)", ruleErr, 1e-3);
    report(diskErr < 3e-3, "sheen: modelSheenSun vs the disk average (rel.)", diskErr, 3e-3);
}

// A9 sheen in a frame (shade class Sheen, ShadeOpaque LAYERED=2): a sphere of cloth (sheen C (0.8, 0.6, 0.4), r_sh 0.4
// over a Standard base) on the ground under the sun alone (the test frame has no GI, shadows or air). Every non-edge pixel
// of the cloth against keepS x the base's sun term + C x the 4-point sheen sun term (C++ model, the G-buffer's normal and
// base, the word's footprint-filtered sheen roughness); the ground as the plain model.
void testSheenFrame(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "sheen frame test";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { 0.5f, 0.45f, 0.4f };
    ground.roughness = 0.5f;
    scene::Material cloth;
    cloth.name = "cloth";
    cloth.baseColor = { 0.25f, 0.1f, 0.3f };
    cloth.roughness = 0.7f;
    cloth.sheenColor = { 0.8f, 0.6f, 0.4f };
    cloth.sheenRoughness = 0.4f;
    s.materials = { ground, cloth };
    scene::Instance a;
    a.mesh = addPlane(s, 40, 0);
    s.instances.push_back(a);
    scene::Instance b;
    b.mesh = addSphere(s, 1, 48, 96, 1);
    b.transform = float3x4::translation({ 0, 1, 0 });
    s.instances.push_back(b);
    s.sun.direction = normalize(float3{ -0.4f, 0.35f, -0.85f });  // low and behind the sphere: grazing sheen on its rim
    scene::Camera cam;
    cam.name = "sheen";
    cam.position = { 0.5f, 1.6f, 4.5f };
    cam.forward = normalize(float3{ -0.1f, -0.15f, -1 });
    cam.ev100 = 13;
    s.cameras.push_back(cam);
    tf.setScene(s);

    const uint32_t W = 960, H = 540;
    std::shared_ptr<std::vector<uint8_t>> gb, words, lin, depthRb;
    ViewDesc desc;
    tf.frame.outputLinearHdr = true;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 0);
        desc = v.view;
        v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        tracks::shading(fc, v);
        gb = tf.readback(fc, v.gbuffer);
        words = tf.readback(fc, material::resolveOutputs(fc, v).materialWord);
        lin = tf.readback(fc, v.color);
        depthRb = tf.readback(fc, v.depth);
    });
    tf.frame.outputLinearHdr = false;
    const double exposure = 1.0 / (1.2 * std::exp2(13.0)), thetaS = s.sun.angularRadius;
    const float3 E = s.sun.color * s.sun.illuminance, l0 = normalize(s.sun.direction);
    const float cap = (float)(2 / (1 + std::cos(thetaS)));
    const float3 du = normalize(cross(std::fabs(l0.z) < 0.9f ? float3{ 0, 0, 1 } : float3{ 1, 0, 0 }, l0)), dw = cross(l0, du);
    double worst = 0, largestSheen = 0;
    uint32_t checked = 0, clothPixels = 0, glint = 0, backlit = 0, terminator = 0;
    for (uint32_t y = 0; y < H; y += 2)
        for (uint32_t x = 0; x < W; x += 2)
        {
            if (cpuIsEdge(desc, *words, *gb, *depthRb, W, H, x, y, tf.quality)) continue;
            const uint32_t word = texelOf<uint32_t>(*words, W, x, y);
            if ((word & 0xFFFF) == 0xFFFF) continue;
            const scene::Material& mat = s.materials[word & 0xFFFF];
            const uint2 pk = texelOf<uint2>(*gb, W, x, y);
            model::Surface su;
            su.cls = mat.cls;
            su.baseColor = { (float)srgbToLinear((pk.y & 0xFF) / 255.0), (float)srgbToLinear(((pk.y >> 8) & 0xFF) / 255.0), (float)srgbToLinear(((pk.y >> 16) & 0xFF) / 255.0) };
            su.roughness = (pk.y >> 24) / 255.0f;
            su.metallic = ((word >> 16) & 0xFF) / 255.0f;
            su.specular = mat.specular;
            const float3 n = octDecode(pk.x);
            double D[3], Dx[3];
            pixelRay(desc, x + 0.5, y + 0.5, D, Dx);
            const float3 v = normalize(float3{ (float)-D[0], (float)-D[1], (float)-D[2] });
            // Pixels whose sun disk straddles the horizon (|n.l| < 1.5 theta_S) are left to the probe and the CPU test (same
            // inputs): there the value is ill-conditioned in the G-buffer's 16-bit normal (CPU and GPU decodes differ by
            // ~1e-5 in n.l against a band 4.8e-3 wide).
            if ((word & 0xFFFF) == 1 && std::fabs(dot(n, l0)) < 1.5f * (float)thetaS)
            {
                ++terminator;
                continue;
            }
            float3 expected = cpuSun(su, n, v, l0, thetaS, &glint, &backlit) * E;
            if ((word & 0xFFFF) == 1 && dot(n, v) > 0)
            {
                ++clothPixels;
                const float r = std::max((word >> 24) / 255.0f, 0.1f);
                const float cmax = std::max(mat.sheenColor.x, std::max(mat.sheenColor.y, mat.sheenColor.z));
                const float keep = 1 - cmax * model::sheenAlbedo(std::max(dot(n, v), 1e-4f), r);
                const double rule = model::sheenSunRule(r, n, v, l0, (float)thetaS);
                const float3 sheen = mat.sheenColor * (float)(rule * cap) * E;
                largestSheen = std::max(largestSheen, (double)sheen.x * exposure);
                expected = expected * keep + sheen;
            }
            expected = expected * (float)exposure;
            const float4 got = texelOf<float4>(*lin, W, x, y);
            const double scale = std::max({ expected.x, expected.y, expected.z, 1e-2f });
            const double e = std::max({ std::abs(got.x - expected.x), std::abs(got.y - expected.y), std::abs(got.z - expected.z) }) / scale;
            if (e > worst && e > 5e-3) logf("  sheen px (%u,%u) word %08x n.v %.4f n.l %.4f got (%.5f %.5f %.5f) expected (%.5f %.5f %.5f)\n", x, y, word, dot(n, v), dot(n, l0), got.x, got.y, got.z, expected.x, expected.y, expected.z);
            worst = std::max(worst, e);
            ++checked;
        }
    const shading::Stats st = shading::latestStats(tf.trackState);
    logf("sheen frame: %u pixels checked (%u cloth, %u cloth terminator pixels left to the probe), largest sheen sun term %.4f (exposed)\n", checked,
         clothPixels, terminator, largestSheen);
    report(clothPixels > 5000 && worst < 5e-3, "sheen frame: pixels vs keepS x base sun + C x 4-point sheen sun (rel.)", worst, 5e-3);
    report(largestSheen > 0.05, "sheen frame: the sheen term is visible (largest exposed sun sheen)", largestSheen, 0.05);
    (void)st;
}

// A9 thin film in a frame (MATERIAL_LAYERS 1.2; MaterialModel.h Film): a soap film sphere (black base, water film 1.33
// of 480 nm over air) and an oxide on copper (film 1.5 of 260 nm over the spectral copper preset, cover 0.9) under the
// sun, lobes wider than 16 theta_S (the point rule). Each pixel against the C++ model evaluateFilm (the exact method (c),
// not the renderer's table: the difference includes the table's interpolation) with the pixel's G-buffer inputs.
// --film-image <path.ppm>: the exposed frame (sRGB, x 4) for the record.
std::string g_filmImage;
void testFilmFrame(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "thin film frame test";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { 0.05f, 0.05f, 0.06f };
    ground.roughness = 0.6f;
    scene::Material soap;
    soap.name = "soap film";
    soap.baseColor = { 0, 0, 0 };
    soap.roughness = 0.3f;
    soap.thinFilmThickness = 480, soap.thinFilmIor = 1.33f, soap.substrateIor = 1, soap.substrateExtinction = 0;
    scene::Material oxide;
    oxide.name = "oxide on copper";
    oxide.baseColor = { 0.95f, 0.64f, 0.54f };
    oxide.metallic = 1;
    oxide.roughness = 0.3f;
    oxide.thinFilmThickness = 260, oxide.thinFilmIor = 1.5f, oxide.thinFilmCoverage = 0.9f, oxide.thinFilmSubstrate = 2;
    s.materials = { ground, soap, oxide };
    scene::Instance a;
    a.mesh = addPlane(s, 40, 0);
    s.instances.push_back(a);
    for (uint32_t k = 0; k < 2; ++k)
    {
        scene::Instance b;
        b.mesh = addSphere(s, 1, 64, 128, 1 + k);
        b.transform = float3x4::translation({ k ? 1.2f : -1.2f, 1, 0 });
        s.instances.push_back(b);
    }
    s.sun.direction = normalize(float3{ -0.3f, 0.6f, 0.75f });
    scene::Camera cam;
    cam.name = "film";
    cam.position = { 0, 1.4f, 5.2f };
    cam.forward = normalize(float3{ 0, -0.08f, -1 });
    cam.ev100 = 13;
    s.cameras.push_back(cam);
    tf.setScene(s);

    const uint32_t W = 960, H = 540;
    std::shared_ptr<std::vector<uint8_t>> gb, words, lin, depthRb;
    ViewDesc desc;
    tf.frame.outputLinearHdr = true;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 0);
        desc = v.view;
        v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        tracks::shading(fc, v);
        gb = tf.readback(fc, v.gbuffer);
        words = tf.readback(fc, material::resolveOutputs(fc, v).materialWord);
        lin = tf.readback(fc, v.color);
        depthRb = tf.readback(fc, v.depth);
    });
    tf.frame.outputLinearHdr = false;
    const double exposure = 1.0 / (1.2 * std::exp2(13.0)), thetaS = s.sun.angularRadius;
    const float3 E = s.sun.color * s.sun.illuminance, l0 = normalize(s.sun.direction);
    const float cap = (float)(2 / (1 + std::cos(thetaS)));
    double worst[3] = { 0, 0, 0 };
    uint32_t count[3] = { 0, 0, 0 };
    double spread[2][3] = { { 1e9, 1e9, 1e9 }, { 0, 0, 0 } };  // the soap film's chromaticity range (its colours vary with angle)
    for (uint32_t y = 0; y < H; y += 2)
        for (uint32_t x = 0; x < W; x += 2)
        {
            if (cpuIsEdge(desc, *words, *gb, *depthRb, W, H, x, y, tf.quality)) continue;
            const uint32_t word = texelOf<uint32_t>(*words, W, x, y);
            const uint32_t mi = word & 0xFFFF;
            if (mi > 2) continue;
            const scene::Material& mat = s.materials[mi];
            const uint2 pk = texelOf<uint2>(*gb, W, x, y);
            model::Surface su;
            su.cls = mat.cls;
            su.baseColor = { (float)srgbToLinear((pk.y & 0xFF) / 255.0), (float)srgbToLinear(((pk.y >> 8) & 0xFF) / 255.0), (float)srgbToLinear(((pk.y >> 16) & 0xFF) / 255.0) };
            su.roughness = (pk.y >> 24) / 255.0f;
            su.metallic = ((word >> 16) & 0xFF) / 255.0f;
            su.specular = mat.specular;
            const float3 n = octDecode(pk.x);
            double D[3], Dx[3];
            pixelRay(desc, x + 0.5, y + 0.5, D, Dx);
            const float3 v = normalize(float3{ (float)-D[0], (float)-D[1], (float)-D[2] });
            const float NoL = dot(n, l0), NoV = dot(n, v);
            if (NoV <= 0 || NoL < 4 * (float)thetaS) continue;  // the lit side away from the terminator band (point rule)
            if (model::alphaFromRoughness(su.roughness) < 16 * thetaS) continue;
            const float3 f = mi == 0 ? model::evaluate(su, n, v, l0) : model::evaluateFilm(su, model::filmOf(mat), n, v, l0);
            const float3 expected = f * (NoL * cap) * E * (float)exposure;
            const float4 got = texelOf<float4>(*lin, W, x, y);
            const double scale = std::max({ expected.x, expected.y, expected.z, 1e-2f });
            const double e = std::max({ std::abs(got.x - expected.x), std::abs(got.y - expected.y), std::abs(got.z - expected.z) }) / scale;
            if (e > worst[mi] && e > 1e-2)
                logf("  film px (%u,%u) mat %u n.v %.4f got (%.5f %.5f %.5f) expected (%.5f %.5f %.5f)\n", x, y, mi, NoV, got.x, got.y, got.z, expected.x,
                     expected.y, expected.z);
            worst[mi] = std::max(worst[mi], e);
            ++count[mi];
            if (mi == 1 && expected.y > 0.02f)
            {
                const double sum = got.x + got.y + got.z;
                const double c[3] = { got.x / sum, got.y / sum, got.z / sum };
                for (int k = 0; k < 3; ++k) spread[0][k] = std::min(spread[0][k], c[k]), spread[1][k] = std::max(spread[1][k], c[k]);
            }
        }
    logf("film frame: ground %u px (worst %.2e), soap %u px (worst %.2e), oxide on copper %u px (worst %.2e); soap chromaticity r %.3f-%.3f g %.3f-%.3f "
         "b %.3f-%.3f\n",
         count[0], worst[0], count[1], worst[1], count[2], worst[2], spread[0][0], spread[1][0], spread[0][1], spread[1][1], spread[0][2], spread[1][2]);
    // 1e-2: the renderer's F table (<= 0.30 dE76 against the model, unx_test_scene_film) and the 8-bit G-buffer inputs
    report(count[1] > 5000 && count[2] > 5000 && std::max(worst[1], worst[2]) < 1e-2, "film frame: soap and oxide pixels vs evaluateFilm (rel.)",
           std::max(worst[1], worst[2]), 1e-2);
    report(spread[1][2] - spread[0][2] > 0.05, "film frame: the soap film's colour changes with angle (blue chromaticity range)", spread[1][2] - spread[0][2], 0.05);
    if (!g_filmImage.empty())
    {
        std::vector<unsigned char> px(3 * W * H);
        for (uint32_t i = 0; i < W * H; ++i)
        {
            const float4 c = texelOf<float4>(*lin, W, i % W, i / W);
            const float ch[3] = { c.x, c.y, c.z };
            for (int k = 0; k < 3; ++k)
            {
                const double l = std::clamp(4.0 * ch[k], 0.0, 1.0);
                px[3 * i + k] = (unsigned char)std::lround(255 * (l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1 / 2.4) - 0.055));
            }
        }
        std::ofstream o(g_filmImage, std::ios::binary);
        o << "P6\n" << W << " " << H << "\n255\n";
        o.write((const char*)px.data(), px.size());
    }
}

// A9 anisotropy in a frame (MATERIAL_LAYERS 1.5; render C's lobe and word, render A's shading join): two brushed metal
// spheres under the sun - r 0.3 s 0.8 (both axes wider than the disk) and r 0.08 s 1 (alpha_b 0.0064 narrower than the
// disk, alpha_t wide: a streak) - each pixel against a dense disk quadrature (48 area-uniform radii x 192 angles) of the
// C++ lobe (distributionGgxAniso, visibilitySmithGgxAniso, anisoSpecularAlbedo) with the pixel's word (the frame about the
// decoded G-buffer normal, the band-limited alphas; the pixel footprint widens the slope density's alphas by p / 4 as the
// rule does). The terminator band is the per-point quadrature's (skipped here, as for the sheen frame).
void testAnisoFrame(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "aniso frame test";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { 0.5f, 0.45f, 0.4f };
    ground.roughness = 0.5f;
    scene::Material brushed;
    brushed.name = "brushed";
    brushed.baseColor = { 0.95f, 0.93f, 0.88f };
    brushed.metallic = 1;
    brushed.roughness = 0.3f;
    brushed.anisotropy = 0.8f;
    brushed.anisotropyRotation = 0.4f;
    scene::Material streak = brushed;
    streak.name = "streak";
    streak.roughness = 0.08f;
    streak.anisotropy = 1;
    streak.anisotropyRotation = 0;
    s.materials = { ground, brushed, streak };
    scene::Instance a;
    a.mesh = addPlane(s, 40, 0);
    s.instances.push_back(a);
    for (uint32_t k = 0; k < 2; ++k)
    {
        scene::Instance b;
        b.mesh = addSphere(s, 1, 48, 96, 1 + k);
        b.transform = float3x4::translation({ k ? 1.15f : -1.15f, 1, 0 });
        s.instances.push_back(b);
    }
    s.sun.direction = normalize(float3{ 0.25f, 0.55f, 0.8f });  // in front of the spheres: the highlights face the camera
    scene::Camera cam;
    cam.name = "aniso";
    cam.position = { 0, 1.4f, 5.0f };
    cam.forward = normalize(float3{ 0, -0.08f, -1 });
    cam.ev100 = 15;
    s.cameras.push_back(cam);
    tf.setScene(s);

    const uint32_t W = 960, H = 540;
    std::shared_ptr<std::vector<uint8_t>> gb, words, aw, lin, depthRb;
    ViewDesc desc;
    tf.frame.outputLinearHdr = true;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 0);
        desc = v.view;
        v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        tracks::shading(fc, v);
        const material::ResolveOutputs o = material::resolveOutputs(fc, v);
        gb = tf.readback(fc, v.gbuffer);
        words = tf.readback(fc, o.materialWord);
        aw = tf.readback(fc, o.anisoWord);
        lin = tf.readback(fc, v.color);
        depthRb = tf.readback(fc, v.depth);
    });
    tf.frame.outputLinearHdr = false;
    const double pi = 3.14159265358979323846;
    const double exposure = 1.0 / (1.2 * std::exp2(15.0)), thetaS = s.sun.angularRadius;
    const float3 E = s.sun.color * s.sun.illuminance, l0 = normalize(s.sun.direction);
    const double Lsun = 1 / (pi * std::sin(thetaS) * std::sin(thetaS));
    const float3 dt = normalize(std::fabs(l0.y) < 0.99f ? cross(float3{ 0, 1, 0 }, l0) : cross(float3{ 1, 0, 0 }, l0)), db = cross(l0, dt);
    double worst[2] = { 0, 0 }, largest = 0;
    uint32_t checked[2] = { 0, 0 }, streakNarrow = 0;
    for (uint32_t y = 0; y < H; y += 2)
        for (uint32_t x = 0; x < W; x += 2)
        {
            if (cpuIsEdge(desc, *words, *gb, *depthRb, W, H, x, y, tf.quality)) continue;
            const uint32_t word = texelOf<uint32_t>(*words, W, x, y);
            const uint32_t mi = word & 0xFFFF;
            if (mi != 1 && mi != 2) continue;
            const uint2 pk = texelOf<uint2>(*gb, W, x, y);
            const float3 n = octDecode(pk.x);
            double D[3], Dx[3];
            pixelRay(desc, x + 0.5, y + 0.5, D, Dx);
            const float3 v = normalize(float3{ (float)-D[0], (float)-D[1], (float)-D[2] });
            const float NoV = dot(n, v), NoL0 = dot(n, l0);
            if (NoV < 0.05f || NoL0 < 2 * (float)std::sin(thetaS)) continue;
            // the word (Aniso.hlsli anisoUnpackWord): the angle about the decoded normal's Duff basis, sqrt(alpha) unorm8
            const uint32_t aword = texelOf<uint32_t>(*aw, W, x, y);
            const float sg = n.z >= 0 ? 1.0f : -1.0f, ia = -1 / (sg + n.z), c = n.x * n.y * ia;
            const float3 b1{ 1 + sg * n.x * n.x * ia, sg * c, -sg * n.x }, b2{ c, sg + n.y * n.y * ia, -n.y };
            const double psi = (aword & 0xFFFF) * (pi / 65536.0);
            const float3 d = b1 * (float)std::cos(psi) + b2 * (float)std::sin(psi);
            const float3 t = normalize(d - n * dot(n, d)), bt = cross(n, t);
            const float rt = ((aword >> 16) & 0xFF) / 255.0f, rb = (aword >> 24) / 255.0f;
            const float at = std::max(rt * rt, 1e-4f), ab = std::max(rb * rb, 1e-4f);
            const double p = std::sqrt(Dx[0] * Dx[0] + Dx[1] * Dx[1] + Dx[2] * Dx[2]) / std::sqrt(D[0] * D[0] + D[1] * D[1] + D[2] * D[2]);
            const float atw = (float)std::sqrt(at * at + p * p / 16), abw = (float)std::sqrt(ab * ab + p * p / 16);
            if (mi == 2 && ab < 2 * thetaS) ++streakNarrow;
            const float3 vL{ dot(v, t), dot(v, bt), NoV };
            const float2 AB = model::anisoSpecularAlbedo(vL, at, ab);
            const float3 f0{ (float)srgbToLinear((pk.y & 0xFF) / 255.0), (float)srgbToLinear(((pk.y >> 8) & 0xFF) / 255.0), (float)srgbToLinear(((pk.y >> 16) & 0xFF) / 255.0) };
            double sum[3] = { 0, 0, 0 };
            const int NR = 48, NA = 192;
            for (int i = 0; i < NR; ++i)
            {
                const double cc = 1 - (i + 0.5) / NR * (1 - std::cos(thetaS)), ss = std::sqrt(std::max(0.0, 1 - cc * cc));
                for (int k = 0; k < NA; ++k)
                {
                    const double ph = (k + 0.5) * 2 * pi / NA;
                    const float3 l = normalize(l0 * (float)cc + (dt * (float)std::cos(ph) + db * (float)std::sin(ph)) * (float)ss);
                    const float NoL = dot(n, l);
                    if (NoL <= 0) continue;
                    const float3 h = normalize(v + l);
                    const float3 lL{ dot(l, t), dot(l, bt), NoL };
                    const float Dh = model::distributionGgxAniso(dot(h, t), dot(h, bt), dot(h, n), atw, abw);
                    const float Vv = model::visibilitySmithGgxAniso(vL, lL, at, ab);
                    const float3 F = model::fresnelSchlick(f0, dot(v, h));
                    for (int q = 0; q < 3; ++q)
                        sum[q] += (&F.x)[q] * Dh * Vv * (1 + (&f0.x)[q] * (1 / (AB.x + AB.y) - 1)) * NoL;
                }
            }
            const double omega = 2 * pi * (1 - std::cos(thetaS));
            float3 expected;
            for (int q = 0; q < 3; ++q) (&expected.x)[q] = (float)(sum[q] / (NR * NA) * omega * Lsun * (&E.x)[q] * exposure);
            const float4 got = texelOf<float4>(*lin, W, x, y);
            largest = std::max(largest, (double)expected.x);
            const double scale = std::max({ expected.x, expected.y, expected.z, 1e-2f });
            const double e = std::max({ std::abs(got.x - expected.x), std::abs(got.y - expected.y), std::abs(got.z - expected.z) }) / scale;
            if (e > worst[mi - 1] && e > 5e-3) logf("  aniso px (%u,%u) mat %u n.v %.4f n.l %.4f alpha (%.4f %.4f) got (%.5f %.5f %.5f) expected (%.5f %.5f %.5f)\n", x, y, mi, NoV, NoL0, at, ab, got.x, got.y, got.z, expected.x, expected.y, expected.z);
            worst[mi - 1] = std::max(worst[mi - 1], e);
            ++checked[mi - 1];
        }
    logf("aniso frame: %u + %u pixels checked (%u streak pixels with alpha_b < 2 theta_s), largest exposed sun specular %.4f\n", checked[0], checked[1], streakNarrow, largest);
    report(checked[0] > 2000 && worst[0] < 5e-3, "aniso frame: r 0.3 s 0.8 sun (disk rule) vs dense disk quadrature (rel.)", worst[0], 5e-3);
    report(checked[1] > 2000 && streakNarrow > 500 && worst[1] < 1e-2, "aniso frame: r 0.08 s 1 streak sun vs dense disk quadrature (rel.)", worst[1], 1e-2);
    report(largest > 0.05, "aniso frame: the highlight is visible (largest exposed sun specular)", largest, 0.05);
}

// A9 area lights for the sheen and anisotropic lobes (AreaQuadrature.hlsli): the GPU mirror against the double replica
// (Tests/AreaQuadReplica.h; its study AreaQuad.cpp measures the method against independent references) over the study's
// placements - 4 shapes x 5 directions (one partly below the horizon) x 3 sizes x 3 views - for sheen r 0.2 / 0.5 and
// anisotropic (r, s) (0.3, 0.8), (0.08, 1).
void testAreaQuadrature(TestFrame& tf, Report& report)
{
    namespace aq = unx::aqr;
    {
        scene::Scene s;  // the scene's tables (the sheen table in the coat buffer)
        s.name = "area quadrature test";
        scene::Material m;
        m.sheenColor = { 0.5f, 0.5f, 0.5f };
        s.materials = { m };
        scene::Instance a;
        a.mesh = addPlane(s, 4, 0);
        s.instances.push_back(a);
        scene::Camera cam;
        cam.position = { 0, 1, 3 };
        cam.forward = { 0, 0, -1 };
        s.cameras.push_back(cam);
        tf.setScene(s);
    }
    struct Case
    {
        aq::Light L;
        aq::V3 v;
        double rSheen;
        aq::Aniso a;
        double refSheen = 0, refAniso = 0;    // the replica
        double truthSheen = 0, truthAniso = 0;  // the independent references (1024^2 grids)
    };
    std::vector<Case> cases;
    const double thetas[] = { 0.3, 0.9, 1.35, 1.55, 1.7 }, sizes[] = { 0.05, 0.2, 0.6 }, views[] = { 0.2, 0.9, 1.35 };
    const double anisoR[2][2] = { { 0.3, 0.8 }, { 0.08, 1.0 } };
    int idx = 0;
    for (int sh = 0; sh < 4; ++sh)
        for (double th : thetas)
            for (double hs : sizes)
                for (double vt : views)
                {
                    Case c;
                    c.L.shape = (aq::Shape)sh;
                    const double phi = 0.7 + sh;
                    c.L.p = aq::V3{ std::sin(th) * std::cos(phi), std::sin(th) * std::sin(phi), std::cos(th) };
                    c.L.forward = aq::normalize(c.L.p * -1 + aq::V3{ 0.2, -0.1, 0.1 });
                    c.L.right = aq::normalize(aq::cross(c.L.forward, aq::V3{ 0.3, 0.9, 0.1 }));
                    const double ext = 2 * std::tan(hs);
                    c.L.sx = sh == aq::Rect ? ext : sh == aq::Tube ? 2.5 * ext : 0.5 * ext;
                    c.L.sy = sh == aq::Rect ? 0.6 * ext : sh == aq::Tube ? 0.15 * ext : 0;
                    if (sh == aq::Tube) c.L.right = aq::normalize(aq::cross(c.L.p, aq::V3{ 0.1, 0.2, 1 }));
                    c.v = aq::V3{ std::sin(vt), 0, std::cos(vt) };
                    c.rSheen = idx % 2 ? 0.5 : 0.2;
                    const int k = (idx / 2) % 2;
                    const float2 al = model::anisoAlphas((float)anisoR[k][0], (float)anisoR[k][1]);
                    c.a.at = al.x, c.a.ab = al.y;
                    const double rot = 0.5 + k;
                    c.a.t = aq::V3{ std::cos(rot), std::sin(rot), 0 }, c.a.b = aq::V3{ -std::sin(rot), std::cos(rot), 0 };
                    c.a.f0 = { 0.9, 0.9, 0.9 };
                    const float2 AB = model::anisoSpecularAlbedo({ (float)aq::dot(c.v, c.a.t), (float)aq::dot(c.v, c.a.b), (float)c.v.z }, (float)c.a.at, (float)c.a.ab);
                    c.a.Ea = AB.x + AB.y;
                    std::vector<aq::Elem> loop, region;
                    if (aq::outline(c.L, loop) && aq::clipHorizon(loop, region))
                    {
                        c.refSheen = aq::sheenQuad(region, c.v, c.rSheen, 6);
                        c.refAniso = aq::anisoQuad(region, c.v, c.a, 6);
                    }
                    c.truthSheen = aq::coneReference(c.L, 1024, [&](aq::V3 l) {
                        return (double)model::evaluateSheenLobe((float)c.rSheen, { 0, 0, 1 }, { (float)c.v.x, (float)c.v.y, (float)c.v.z }, { (float)l.x, (float)l.y, (float)l.z }) * l.z;
                    });
                    c.truthAniso = aq::lobeReference(c.L, c.v, c.a, 1024);
                    cases.push_back(c);
                    ++idx;
                }
    std::vector<gpu::Light> lights;
    std::vector<float4> q;
    auto f3 = [](aq::V3 x) { return float3{ (float)x.x, (float)x.y, (float)x.z }; };
    for (const Case& c : cases)
    {
        gpu::Light l{};
        l.position = f3(c.L.p);
        l.typeFlags = 2 + (uint32_t)c.L.shape;
        l.forward = f3(c.L.forward);
        l.range = 1e6f;
        l.right = f3(c.L.right);
        l.intensity = 1;
        l.color = { 1, 1, 1 };
        l.size = { (float)c.L.sx, (float)c.L.sy };
        lights.push_back(l);
        q.push_back({ 0, 0, 1, (float)c.rSheen });
        q.push_back({ (float)c.v.x, (float)c.v.y, (float)c.v.z, (float)c.a.f0.x });
        q.push_back({ (float)c.a.t.x, (float)c.a.t.y, (float)c.a.t.z, (float)c.a.at });
        q.push_back({ (float)c.a.b.x, (float)c.a.b.y, (float)c.a.b.z, (float)c.a.ab });
        q.push_back({ (float)(1 + c.a.f0.x * (1 / c.a.Ea - 1)), 0, 0, 0 });
    }
    ComPtr<ID3D12Resource> lightBuf = uploadStatic(tf.device, lights.data(), lights.size() * sizeof(gpu::Light), L"test aq lights");
    ComPtr<ID3D12Resource> queryBuf = uploadStatic(tf.device, q.data(), q.size() * 16, L"test aq queries");
    auto srvOf = [&](ID3D12Resource* r, uint32_t count, uint32_t stride) {
        const uint32_t srv = tf.device.descriptors().allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = count;
        sd.Buffer.StructureByteStride = stride;
        tf.device.d3d()->CreateShaderResourceView(r, &sd, tf.device.descriptors().resourceCpu(srv));
        return srv;
    };
    const uint32_t lightSrv = srvOf(lightBuf.Get(), (uint32_t)lights.size(), sizeof(gpu::Light)), querySrv = srvOf(queryBuf.Get(), (uint32_t)q.size(), 16);
    std::shared_ptr<std::vector<uint8_t>> out;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, 64, 64);
        BufferRef res = fc.graph.createBuffer({ "m.test.aq results", cases.size() * 16, 16 });
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/Tests/AreaQuadProbe");
        const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
        const uint32_t count = (uint32_t)cases.size();
        fc.graph.addPass("m.test.aq", QueueType::Graphics, [&](PassBuilder& b) { b.use(res, Use::UavCompute); },
                         [=](PassContext& c) {
                             const uint32_t k[4] = { lightSrv, querySrv, c.uav(res), count };
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 4);
                             c.cmd->Dispatch((count + 63) / 64, 1, 1);
                         });
        out = tf.readbackBuffer(fc, res, cases.size() * 16);
    });
    for (uint32_t srv : { lightSrv, querySrv }) tf.device.descriptors().freeResource(srv);
    std::vector<float4> r(cases.size());
    std::memcpy(r.data(), out->data(), cases.size() * 16);
    double peakS = 0, peakA = 0, worstS = 0, worstA = 0;
    for (const Case& c : cases) peakS = std::max(peakS, c.refSheen), peakA = std::max(peakA, c.refAniso);
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const double es = std::abs(r[i].x - cases[i].refSheen) / std::max(cases[i].refSheen, 1e-3 * peakS);
        const double ea = std::abs(r[i].y - cases[i].refAniso) / std::max(cases[i].refAniso, 1e-3 * peakA);
        if ((es > 2e-3 && es > worstS) || (ea > 2e-3 && ea > worstA))
            logf("  aq case %zu shape %d: sheen gpu %.6g replica %.6g, aniso gpu %.6g replica %.6g\n", i, (int)cases[i].L.shape, r[i].x, cases[i].refSheen, r[i].y, cases[i].refAniso);
        worstS = std::max(worstS, es), worstA = std::max(worstA, ea);
    }
    // accuracy against the independent references, energy-weighted per lobe (the study's measure): sum |gpu - truth| / sum truth
    double eS = 0, tS = 0, eA = 0, tA = 0, eAn = 0, tAn = 0;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        eS += std::abs(r[i].x - cases[i].truthSheen), tS += cases[i].truthSheen;
        const bool narrow = cases[i].a.ab < 0.01;
        (narrow ? eAn : eA) += std::abs(r[i].y - cases[i].truthAniso);
        (narrow ? tAn : tA) += cases[i].truthAniso;
    }
    logf("area quadrature: %zu placements (4 shapes); GPU vs double replica worst: sheen %.3g, anisotropic %.3g (rel. to max(value, 1e-3 peak))\n", cases.size(), worstS, worstA);
    logf("area quadrature: GPU vs independent references, energy-weighted: sheen %.4f, anisotropic r 0.3 s 0.8 %.4f, r 0.08 s 1 %.4f\n", eS / tS, eA / tA, eAn / tAn);
    report(worstS <= 2e-3, "area quadrature: sheen over the light, GPU vs double replica (rel.)", worstS, 2e-3);
    report(eS / tS <= 5e-3, "area quadrature: sheen vs reference, energy-weighted over 180 placements", eS / tS, 5e-3);
    report(eA / tA <= 5e-3, "area quadrature: anisotropic r 0.3 s 0.8 vs reference, energy-weighted", eA / tA, 5e-3);
    // (MATERIAL_LAYERS 1.5 quality: area-light terms carry the method's error, as the isotropic lobe carries its LTC fit's -
    // measured here in "LTC fit vs model BRDF": mean 3.5-50 % by roughness; the narrow anisotropic lobe's 2.7 % [measured]
    // is bounded at 3 % as a regression gate)
    report(eAn / tAn <= 3e-2, "area quadrature: anisotropic r 0.08 s 1 (alpha_b 0.0064) vs reference, energy-weighted", eAn / tAn, 3e-2);
}

// A9 area-light lobes in a frame (AreaLobes.hlsl before the layered kernel): a brushed metal sphere (metallic 1: no
// diffuse; r 0.3 s 0.8, rotation 0.4) under one rect area light, no sun, no probes in the test frame - each pixel is the
// anisotropic lobe over the light, against the double replica (AreaQuadReplica.h) on the pixel's word (frame about the
// decoded normal, band-limited alphas) and the pixel's position from the depth, times the light's radiance.
void testAreaLobesFrame(TestFrame& tf, Report& report)
{
    namespace aq = unx::aqr;
    scene::Scene s;
    s.name = "area lobes frame test";
    scene::Material ground;
    ground.baseColor = { 0.3f, 0.3f, 0.3f };
    scene::Material brushed;
    brushed.baseColor = { 0.95f, 0.93f, 0.88f };
    brushed.metallic = 1;
    brushed.roughness = 0.3f;
    brushed.anisotropy = 0.8f;
    brushed.anisotropyRotation = 0.4f;
    s.materials = { ground, brushed };
    scene::Instance a;
    a.mesh = addPlane(s, 40, 0);
    s.instances.push_back(a);
    scene::Instance b;
    b.mesh = addSphere(s, 1, 48, 96, 1);
    b.transform = float3x4::translation({ 0, 1, 0 });
    s.instances.push_back(b);
    s.sun.illuminance = 0;
    scene::Light L;
    L.type = scene::LightType::Rect;
    L.position = { 0.6f, 2.6f, 2.2f };
    L.forward = normalize(float3{ -0.6f, -1.6f, -2.2f });
    L.right = normalize(cross(L.forward, float3{ 0, 1, 0 }));
    L.size = { 1.2f, 0.5f };
    L.intensity = 2000;
    L.range = 1000;
    s.lights.push_back(L);
    scene::Camera cam;
    cam.position = { 0, 1.4f, 4.5f };
    cam.forward = normalize(float3{ 0, -0.08f, -1 });
    cam.ev100 = 9;
    s.cameras.push_back(cam);
    tf.setScene(s);

    // S's froxel list (the local-lights test's format): one froxel holding the one light
    std::vector<uint32_t> list(64, 0);
    const float nearM = 0.01f, farM = 1000.0f, logRatio = std::log2(farM / nearM);
    list[0] = 1, list[1] = 1, list[2] = 1, list[3] = 4096;
    std::memcpy(&list[4], &nearM, 4);
    std::memcpy(&list[5], &farM, 4);
    std::memcpy(&list[6], &logRatio, 4);
    list[8] = 64, list[9] = 128, list[10] = 64, list[11] = 1;
    list[16] = 0, list[17] = 1;  // header (first entry, count)
    list[32] = 0;
    ComPtr<ID3D12Resource> listBuffer = uploadStatic(tf.device, list.data(), list.size() * 4, L"test froxel list (area lobes)");
    const uint32_t W = 640, H = 360;
    std::shared_ptr<std::vector<uint8_t>> gb, words, aw, lin, depthRb;
    ViewDesc desc;
    tf.frame.outputLinearHdr = true;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 0);
        desc = v.view;
        v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        fc.resources.froxelLights = fc.graph.importBuffer(listBuffer.Get(), { "test froxel lists", list.size() * 4, 0 });
        v.froxelLights = fc.resources.froxelLights;
        tracks::shading(fc, v);
        const material::ResolveOutputs o = material::resolveOutputs(fc, v);
        gb = tf.readback(fc, v.gbuffer);
        words = tf.readback(fc, o.materialWord);
        aw = tf.readback(fc, o.anisoWord);
        lin = tf.readback(fc, v.color);
        depthRb = tf.readback(fc, v.depth);
    });
    tf.frame.outputLinearHdr = false;
    const double pi = 3.14159265358979323846, exposure = 1.0 / (1.2 * std::exp2(9.0));
    double worst = 0, largest = 0, sumE = 0, sumR = 0;
    uint32_t checked = 0;
    for (uint32_t y = 0; y < H; y += 3)
        for (uint32_t x = 0; x < W; x += 3)
        {
            if (cpuIsEdge(desc, *words, *gb, *depthRb, W, H, x, y, tf.quality)) continue;
            const uint32_t word = texelOf<uint32_t>(*words, W, x, y);
            if ((word & 0xFFFF) != 1) continue;
            const uint2 pk = texelOf<uint2>(*gb, W, x, y);
            const float3 n = octDecode(pk.x);
            double D[3], Dx[3];
            pixelRay(desc, x + 0.5, y + 0.5, D, Dx);
            const float3 v = normalize(float3{ (float)-D[0], (float)-D[1], (float)-D[2] });
            const float NoV = dot(n, v);
            if (NoV < 0.05f) continue;
            const uint32_t aword = texelOf<uint32_t>(*aw, W, x, y);
            const float sg = n.z >= 0 ? 1.0f : -1.0f, ia = -1 / (sg + n.z), c = n.x * n.y * ia;
            const float3 b1{ 1 + sg * n.x * n.x * ia, sg * c, -sg * n.x }, b2{ c, sg + n.y * n.y * ia, -n.y };
            const double psi = (aword & 0xFFFF) * (pi / 65536.0);
            const float3 d = b1 * (float)std::cos(psi) + b2 * (float)std::sin(psi);
            const float3 t = normalize(d - n * dot(n, d)), bt = cross(n, t);
            const float rt = ((aword >> 16) & 0xFF) / 255.0f, rb = (aword >> 24) / 255.0f;
            // the pixel's position (linear depth along the view axis; D has unit depth) and the light in the frame (t, bt, n)
            const float lz = (float)(desc.nearPlane / std::max((double)texelOf<float>(*depthRb, W, x, y), 1e-30));
            const float3 pos = desc.position + float3{ (float)D[0], (float)D[1], (float)D[2] } * lz;
            auto loc = [&](float3 w) { return aq::V3{ dot(w, t), dot(w, bt), dot(w, n) }; };
            aq::Light al;
            al.shape = aq::Rect;
            al.p = loc(L.position - pos), al.forward = loc(L.forward), al.right = loc(L.right), al.sx = L.size.x, al.sy = L.size.y;
            aq::Aniso an;
            an.t = { 1, 0, 0 }, an.b = { 0, 1, 0 };
            an.at = std::max(rt * rt, 1e-4f), an.ab = std::max(rb * rb, 1e-4f);
            const float3 f0{ (float)srgbToLinear((pk.y & 0xFF) / 255.0), (float)srgbToLinear(((pk.y >> 8) & 0xFF) / 255.0), (float)srgbToLinear(((pk.y >> 16) & 0xFF) / 255.0) };
            const aq::V3 vl = loc(v);
            const float2 AB = model::anisoSpecularAlbedo({ (float)vl.x, (float)vl.y, (float)vl.z }, (float)an.at, (float)an.ab);
            an.Ea = AB.x + AB.y;
            std::vector<aq::Elem> loop, region;
            float3 expected{ 0, 0, 0 };
            if (aq::outline(al, loop) && aq::clipHorizon(loop, region))
            {
                const double dist = std::sqrt(aq::dot(al.p, al.p)) / L.range, w = std::pow(std::max(0.0, 1 - std::pow(dist, 4)), 2);
                for (int q = 0; q < 3; ++q)
                {
                    an.f0 = { (&f0.x)[q], (&f0.x)[q], (&f0.x)[q] };
                    (&expected.x)[q] = (float)(aq::anisoQuad(region, vl, an, 6) * L.intensity * w * exposure);
                }
            }
            const float4 got = texelOf<float4>(*lin, W, x, y);
            const double scale = std::max({ expected.x, expected.y, expected.z, 1e-2f });
            const double e = std::max({ std::abs(got.x - expected.x), std::abs(got.y - expected.y), std::abs(got.z - expected.z) }) / scale;
            if (e > worst && e > 1e-2) logf("  lobes px (%u,%u) n.v %.3f got (%.5f %.5f %.5f) expected (%.5f %.5f %.5f)\n", x, y, NoV, got.x, got.y, got.z, expected.x, expected.y, expected.z);
            worst = std::max(worst, e);
            largest = std::max(largest, (double)expected.x);
            sumE += std::abs(got.x - expected.x), sumR += expected.x;
            ++checked;
        }
    logf("area lobes frame: %u sphere pixels, largest exposed radiance %.4f, energy-weighted error %.4g, worst %.4g\n", checked, largest, sumR > 0 ? sumE / sumR : 0, worst);
    report(checked > 1000 && sumR > 0 && sumE / sumR < 5e-3, "area lobes frame: anisotropic sphere under a rect light vs replica (energy-weighted)", sumR > 0 ? sumE / sumR : 1, 5e-3);
    report(largest > 0.05, "area lobes frame: the area-light highlight is visible", largest, 0.05);
}

void testCoatSun(TestFrame& tf, Report& report)
{
    {
        scene::Scene s;  // the frame constants' sun (defaults) and the coat tables
        s.name = "coat sun test";
        s.materials.push_back(scene::Material{});
        scene::Instance plane;
        plane.mesh = addPlane(s, 10, 0);
        s.instances.push_back(plane);
        scene::Camera cam;
        cam.name = "coat";
        cam.position = { 0, 1, 3 };
        cam.forward = normalize(float3{ 0, -0.3f, -1 });
        s.cameras.push_back(cam);
        tf.setScene(s);
    }
    const float3 l0 = normalize(tf.sceneData.sun.direction);
    const double thetaS = tf.sceneData.sun.angularRadius, cosS = std::cos(thetaS);
    const float3 n{ 0, 1, 0 };
    const float3 axis = normalize(cross(l0, float3{ 0.3f, 0.2f, 1 }));
    struct Case
    {
        float roughness, offset;
        uint32_t coat;
    };
    std::vector<Case> cases;
    for (uint32_t coat : { 0u, 1u })
        for (float r : { 0.07f, 0.1f, 0.15f, 0.2f, 0.3f, 0.5f })
            for (float o : { 0.0f, 0.5f, 1.0f, 2.0f, 6.0f }) cases.push_back({ r, o, coat });
    std::vector<float4> q;
    std::vector<float3> views;
    for (const Case& c : cases)
    {
        const float3 refl = rotateTowards(l0, axis, c.offset * thetaS);
        const float3 v = normalize(n * (2 * dot(n, refl)) - refl);
        q.push_back({ n.x, n.y, n.z, c.roughness });
        q.push_back({ v.x, v.y, v.z, (float)c.coat });
        views.push_back(v);
    }
    std::shared_ptr<std::vector<uint8_t>> out;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, 64, 64);
        BufferRef in = fc.graph.createBuffer({ "m.test.coat queries", q.size() * 16, 16 });
        BufferRef res = fc.graph.createBuffer({ "m.test.coat results", cases.size() * 16, 16 });
        ComPtr<ID3D12Resource> staging = makeBuffer(tf.device, q.size() * 16, D3D12_HEAP_TYPE_UPLOAD);
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(staging->Map(0, &none, &p), "map");
        std::memcpy(p, q.data(), q.size() * 16);
        staging->Unmap(0, nullptr);
        tf.keep(staging);
        fc.graph.addPass("m.test.upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(in, Use::CopyDst); },
                         [staging, in, bytes = q.size() * 16](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(in), 0, staging.Get(), 0, bytes); });
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shading/Tests/CoatSunProbe");
        const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
        const uint32_t count = (uint32_t)cases.size();
        fc.graph.addPass("m.test.coat probe", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(in, Use::SrvCompute);
                             b.use(res, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             const uint32_t k[4] = { c.srv(in), c.uav(res), count, 0 };
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 4);
                             c.cmd->Dispatch((count + 63) / 64, 1, 1);
                         });
        out = tf.readbackBuffer(fc, res, cases.size() * 16);
    });
    auto reference = [&](const Case& c, float3 v) {
        model::Coat coat;
        coat.cover = 1;
        coat.roughness = c.roughness;
        coat.eta = model::kCoatEtas[c.coat];
        const float3 t = normalize(std::fabs(l0.y) < 0.99f ? cross(float3{ 0, 1, 0 }, l0) : cross(float3{ 1, 0, 0 }, l0));
        const float3 b = cross(l0, t);
        const uint32_t nr = 400, na = 1200;
        double sum = 0;
        for (uint32_t i = 0; i < nr; ++i)
        {
            const double cc = 1 - (i + 0.5) / nr * (1 - cosS), sn = std::sqrt(std::max(1 - cc * cc, 0.0));
            for (uint32_t k = 0; k < na; ++k)
            {
                const double ph = (k + 0.5) / na * 2 * kPi;
                const float3 l = normalize(l0 * (float)cc + (t * (float)std::cos(ph) + b * (float)std::sin(ph)) * (float)sn);
                const float NoL = dot(n, l);
                if (NoL > 0) sum += model::evaluateCoatLobe(coat, n, v, l) * NoL;
            }
        }
        return sum * 2 / (1 + cosS) / ((double)nr * na);
    };
    double worst4 = 0, worstNarrow = 0;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        float4 g;
        std::memcpy(&g, out->data() + i * 16, 16);
        const double ref = reference(cases[i], views[i]);
        const double peak = reference({ cases[i].roughness, 0.0f, cases[i].coat }, normalize(n * (2 * dot(n, l0)) - l0));
        const double e = std::abs(g.y - ref) / std::max(peak, 1e-30);
        const double alpha = model::alphaFromRoughness(cases[i].roughness);
        if (e > 5e-3) logf("  coat %u r %.2f offset %.2f: gpu %.5e ref %.5e (peak %.5e) err %.2e\n", cases[i].coat, cases[i].roughness, cases[i].offset, g.y, ref, peak, e);
        if (alpha >= 2 * thetaS) worst4 = std::max(worst4, e);
        else worstNarrow = std::max(worstNarrow, e);
    }
    report(worst4 < 5e-3, "coat sun lobe [alpha >= 2 theta_s] vs dense quadrature of evaluateCoatLobe (rel. to peak)", worst4, 5e-3);
    report(worstNarrow < 1e-2, "coat sun lobe [narrow, alpha < 2 theta_s] vs dense quadrature (rel. to peak)", worstNarrow, 1e-2);
}

// 14.1b (L2b): a constant-emission panel as quadtree area lights (shading.emissive_area_lights) against the same
// panel authored as a RECT light (the LTC closed form the kernel uses for area lights): the ground's diffuse radiance
// must agree pixel by pixel (the quadtree evaluates the panel as a few squares, the rect light as one polygon; the same
// edge integral, the window w(d) of the rect light is 1 - (d / 1000)^4 ~ 1). The panel's own pixels (its material)
// are skipped (its direct view is the material's emission in one run and black in the other).
// The quadtree lights add the diffuse term alone (the emitters' specular is the reflection path's), while the kernel
// shades a rect light's specular lobe too: the rect light's diffuse radiance is its frame minus the same frame over a
// black ground (base colour 0: no diffuse term; f0 = 0.08 x specular and the lobe are those of the grey ground).
void testEmissivePanel(TestFrame& tf, Report& report)
{
    const float3 L{ 3000, 2800, 2500 };  // nits
    const float3 panelPos{ 0.4f, 2.6f, 0.5f };
    const float2 panelSize{ 1.2f, 0.8f };
    auto renderGround = [&](bool quadtree, bool blackGround, std::shared_ptr<std::vector<uint8_t>>& lin, std::shared_ptr<std::vector<uint8_t>>& words, uint32_t W,
                            uint32_t H) {
        scene::Scene s;
        s.name = quadtree ? "emissive panel (quadtree)" : "emissive panel (rect light)";
        scene::Material ground;
        ground.baseColor = blackGround ? float3{ 0, 0, 0 } : float3{ 0.5f, 0.5f, 0.5f };
        scene::Material panel;
        panel.baseColor = { 0.02f, 0.02f, 0.02f };
        if (quadtree) panel.emissive = L;
        s.materials = { ground, panel };
        scene::Instance a;
        a.mesh = addPlane(s, 40, 0);
        s.instances.push_back(a);
        // the panel: addPlane's quad (normal +y, counter-clockwise from above) turned to face down and scaled
        scene::Mesh pm;
        pm.name = "panel";
        const float hx = panelSize.x / 2, hz = panelSize.y / 2;
        pm.positions = { { -hx, 0, -hz }, { hx, 0, -hz }, { -hx, 0, hz }, { hx, 0, hz } };
        pm.normals.assign(4, { 0, -1, 0 });
        pm.tangents.assign(4, { 1, 0, 0, 1 });
        pm.uv0 = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
        pm.indices = { 0, 1, 2, 1, 3, 2 };  // counter-clockwise seen from below
        pm.submeshes.push_back({ 0, 6, 1 });
        s.meshes.push_back(std::move(pm));
        scene::Instance b;
        b.mesh = (uint32_t)s.meshes.size() - 1;
        b.transform = float3x4::translation(panelPos);
        s.instances.push_back(b);
        s.sun.illuminance = 0;
        if (!quadtree)
        {
            scene::Light rl;
            rl.type = scene::LightType::Rect;
            rl.position = panelPos;
            rl.forward = { 0, -1, 0 };
            rl.right = { 1, 0, 0 };
            rl.size = panelSize;
            rl.intensity = 1;
            rl.color = L;  // (colour x intensity = rgb nits, as the kernel's Lw)
            rl.range = 1000;
            s.lights.push_back(rl);
        }
        scene::Camera cam;
        cam.position = { 0, 1.6f, 4.0f };
        cam.forward = normalize(float3{ 0, -0.35f, -1 });
        cam.ev100 = 9;
        s.cameras.push_back(cam);
        tf.setScene(s);
        tf.quality.applyOverride(quadtree ? "shading.emissive_area_lights=true" : "shading.emissive_area_lights=false");
        std::vector<uint32_t> list(64, 0);
        const float nearM = 0.01f, farM = 1000.0f, logRatio = std::log2(farM / nearM);
        list[0] = 1, list[1] = 1, list[2] = 1, list[3] = 4096;
        std::memcpy(&list[4], &nearM, 4);
        std::memcpy(&list[5], &farM, 4);
        std::memcpy(&list[6], &logRatio, 4);
        list[8] = 64, list[9] = 128, list[10] = 64, list[11] = 1;
        list[16] = 0, list[17] = 1;  // header (first entry, count): the one rect light
        list[32] = 0;
        ComPtr<ID3D12Resource> listBuffer = uploadStatic(tf.device, list.data(), list.size() * 4, L"test froxel list (emissive panel)");
        tf.frame.outputLinearHdr = true;
        tf.run([&](FramePassContext& fc) {
            ViewResources v = tf.mainView(fc, W, H, 0);
            v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
            tf.vis.record(fc, v);
            tracks::materialResolve(fc, v);
            if (!quadtree)
            {
                fc.resources.froxelLights = fc.graph.importBuffer(listBuffer.Get(), { "test froxel lists", list.size() * 4, 0 });
                v.froxelLights = fc.resources.froxelLights;
            }
            tracks::shading(fc, v);
            const material::ResolveOutputs o = material::resolveOutputs(fc, v);
            words = tf.readback(fc, o.materialWord);
            lin = tf.readback(fc, v.color);
        });
        tf.frame.outputLinearHdr = false;
        tf.quality.applyOverride("shading.emissive_area_lights=false");
    };
    const uint32_t W = 640, H = 360;
    std::shared_ptr<std::vector<uint8_t>> linQ, wordsQ, linR, wordsR, linS, wordsS;
    renderGround(true, false, linQ, wordsQ, W, H);
    renderGround(false, false, linR, wordsR, W, H);
    renderGround(false, true, linS, wordsS, W, H);  // the rect light's specular lobe alone
    double worst = 0, sumE = 0, sumR = 0, largest = 0, sumSpecular = 0, sumTotal = 0, largestSpecularShare = 0;
    uint32_t checked = 0;
    for (uint32_t y = 0; y < H; y += 2)
        for (uint32_t x = 0; x < W; x += 2)
        {
            if ((texelOf<uint32_t>(*wordsQ, W, x, y) & 0xFFFF) != 0 || (texelOf<uint32_t>(*wordsR, W, x, y) & 0xFFFF) != 0) continue;  // ground pixels alone
            const float4 q = texelOf<float4>(*linQ, W, x, y), total = texelOf<float4>(*linR, W, x, y), specular = texelOf<float4>(*linS, W, x, y);
            const float3 r{ total.x - specular.x, total.y - specular.y, total.z - specular.z };  // the rect light's diffuse radiance
            const double scale = std::max({ (double)r.x, (double)r.y, (double)r.z, 1e-3 });
            const double e = std::max({ std::abs(q.x - r.x), std::abs(q.y - r.y), std::abs(q.z - r.z) }) / scale;
            if (e > worst && e > 2e-3)
                logf("  emissive panel px (%u,%u): quadtree (%.5f %.5f %.5f) rect light diffuse (%.5f %.5f %.5f) specular (%.5f %.5f %.5f)\n", x, y, q.x, q.y, q.z, r.x, r.y,
                     r.z, specular.x, specular.y, specular.z);
            worst = std::max(worst, e);
            largest = std::max(largest, (double)r.x);
            sumE += std::abs(q.x - r.x), sumR += r.x;
            sumSpecular += specular.x, sumTotal += total.x;
            if (total.x > 1e-4) largestSpecularShare = std::max(largestSpecularShare, (double)specular.x / total.x);
            ++checked;
        }
    logf("emissive panel: %u ground pixels, largest exposed radiance %.4f, energy-weighted error %.4g, worst %.4g; the rect light's specular lobe is %.4g of its "
         "radiance (largest share at a pixel %.3g)\n",
         checked, largest, sumR > 0 ? sumE / sumR : 0, worst, sumTotal > 0 ? sumSpecular / sumTotal : 0, largestSpecularShare);
    report(checked > 10000 && largest > 0.02, "emissive panel: the panel lights the ground (exposed radiance)", largest, 0.02);
    report(sumR > 0 && sumE / sumR < 1e-3, "emissive panel: quadtree area lights vs the rect light (energy-weighted rel.)", sumR > 0 ? sumE / sumR : 1, 1e-3);
    report(worst < 5e-3, "emissive panel: quadtree area lights vs the rect light (worst pixel rel.)", worst, 5e-3);
}

// L2 (14.1/14.2): the tile lights' FAR term against the per-pixel evaluation. The same frame shaded with
// shading.tile_lights off (every light per pixel) and on (FAR lights' diffuse from the tile corners' vector
// irradiance, NEAR lights and every specular per pixel): the gate of 14.8 L2, |dE| / E P99 <= 1e-3 over the surface
// pixels (edge tiles keep the per-pixel path by design). The lights sit at 3..30 m from the ground and the boxes, so
// most are FAR for most tiles; the one S-style froxel list of the tests holds them all.
void testTileLights(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "tile lights";
    scene::Material ground;
    ground.baseColor = { 0.5f, 0.5f, 0.5f };
    ground.roughness = 0.6f;
    scene::Material boxMat;
    boxMat.baseColor = { 0.7f, 0.4f, 0.3f };
    boxMat.roughness = 0.3f;
    s.materials = { ground, boxMat };
    scene::Instance a;
    a.mesh = addPlane(s, 60, 0);
    s.instances.push_back(a);
    for (int i = 0; i < 3; ++i)
    {
        scene::Instance b;
        b.mesh = addSphere(s, 0.6f, 24, 48, 1);
        b.transform = float3x4::translation({ -2.5f + 2.5f * i, 0.6f, -1.0f + 0.7f * i });
        s.instances.push_back(b);
    }
    s.sun.illuminance = 0;
    std::mt19937 rng(2026);
    auto uni = [&](float lo, float hi) { return std::uniform_real_distribution<float>(lo, hi)(rng); };
    for (int i = 0; i < 28; ++i)
    {
        scene::Light L;
        const int kind = i % 4;
        L.type = kind == 0 ? scene::LightType::Point : (kind == 1 ? scene::LightType::Spot : (kind == 2 ? scene::LightType::Rect : scene::LightType::Sphere));
        L.position = { uni(-12, 12), uni(2, 9), uni(-14, 6) };
        L.forward = normalize(float3{ uni(-0.5f, 0.5f), -1, uni(-0.5f, 0.5f) });
        L.right = normalize(cross(L.forward, float3{ 0, 0, 1 }));
        L.intensity = kind >= 2 ? uni(300, 1500) : uni(500, 4000);
        L.range = uni(12, 40);
        L.size = kind == 2 ? float2{ uni(0.2f, 0.6f), uni(0.2f, 0.5f) } : float2{ uni(0.05f, 0.2f), 0 };
        L.spotInner = 0.4f;
        L.spotOuter = 0.7f;
        s.lights.push_back(L);
    }
    scene::Camera cam;
    cam.position = { 0, 1.7f, 5.5f };
    cam.forward = normalize(float3{ 0, -0.3f, -1 });
    cam.ev100 = 9;
    s.cameras.push_back(cam);
    tf.setScene(s);
    // one froxel holding every light (the tests' S-style list; FroxelCommon.hlsli layout: header, one (first, count), entries)
    const uint32_t N = (uint32_t)s.lights.size();
    std::vector<uint32_t> list(64 + N, 0);
    const float nearM = 0.01f, farM = 1000.0f, logRatio = std::log2(farM / nearM);
    list[0] = 1, list[1] = 1, list[2] = 1, list[3] = 4096;
    std::memcpy(&list[4], &nearM, 4);
    std::memcpy(&list[5], &farM, 4);
    std::memcpy(&list[6], &logRatio, 4);
    list[8] = 64, list[9] = 128, list[10] = (N + 1) & ~1u, list[11] = N;
    list[16] = 0, list[17] = N;
    for (uint32_t i = 0; i < N; i += 2) list[32 + i / 2] = i | ((i + 1 < N ? i + 1 : 0) << 16);
    ComPtr<ID3D12Resource> listBuffer = uploadStatic(tf.device, list.data(), list.size() * 4, L"test froxel list (tile lights)");
    const uint32_t W = 640, H = 360;
    auto render = [&](bool tiles, std::shared_ptr<std::vector<uint8_t>>& lin, std::shared_ptr<std::vector<uint8_t>>& words, std::shared_ptr<std::vector<uint8_t>>& gb,
                      std::shared_ptr<std::vector<uint8_t>>& depthRb, ViewDesc& desc) {
        tf.quality.applyOverride(tiles ? "shading.tile_lights=true" : "shading.tile_lights=false");
        tf.frame.outputLinearHdr = true;
        tf.run([&](FramePassContext& fc) {
            ViewResources v = tf.mainView(fc, W, H, 0);
            desc = v.view;
            v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
            tf.vis.record(fc, v);
            tracks::materialResolve(fc, v);
            fc.resources.froxelLights = fc.graph.importBuffer(listBuffer.Get(), { "test froxel lists", list.size() * 4, 0 });
            v.froxelLights = fc.resources.froxelLights;
            tracks::shading(fc, v);
            const material::ResolveOutputs o = material::resolveOutputs(fc, v);
            words = tf.readback(fc, o.materialWord);
            gb = tf.readback(fc, v.gbuffer);
            depthRb = tf.readback(fc, v.depth);
            lin = tf.readback(fc, v.color);
        });
        tf.frame.outputLinearHdr = false;
        tf.quality.applyOverride("shading.tile_lights=false");
    };
    std::shared_ptr<std::vector<uint8_t>> linOff, wordsOff, gbOff, depthOff, linOn, wordsOn, gbOn, depthOn;
    ViewDesc descOff, descOn;
    render(false, linOff, wordsOff, gbOff, depthOff, descOff);
    render(true, linOn, wordsOn, gbOn, depthOn, descOn);
    std::vector<double> errs;
    double worst = 0, sumE = 0, sumR = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            if (texelOf<float>(*depthOff, W, x, y) <= 0) continue;  // sky
            if (cpuIsEdge(descOff, *wordsOff, *gbOff, *depthOff, W, H, x, y, tf.quality)) continue;
            const float4 pa = texelOf<float4>(*linOff, W, x, y), pb = texelOf<float4>(*linOn, W, x, y);
            const double scale = std::max({ (double)pa.x, (double)pa.y, (double)pa.z, 1e-3 });
            const double e = std::max({ std::abs(pa.x - pb.x), std::abs(pa.y - pb.y), std::abs(pa.z - pb.z) }) / scale;
            if (e > worst && e > 5e-3) logf("  tile lights px (%u,%u): per-pixel (%.5f %.5f %.5f) tiles (%.5f %.5f %.5f)\n", x, y, pa.x, pa.y, pa.z, pb.x, pb.y, pb.z);
            worst = std::max(worst, e);
            errs.push_back(e);
            sumE += std::abs(pa.x - pb.x), sumR += pa.x;
        }
    std::sort(errs.begin(), errs.end());
    const double p99 = errs.empty() ? 1 : errs[std::min(errs.size() - 1, (size_t)(errs.size() * 0.99))];
    logf("tile lights: %zu surface pixels, |dE|/E P99 %.3g, worst %.3g, energy-weighted %.3g\n", errs.size(), p99, worst, sumR > 0 ? sumE / sumR : 0);
    report(errs.size() > 50000 && p99 <= 1e-3, "tile lights: tile FAR term vs per-pixel, |dE|/E P99 (14.8 L2 gate)", p99, 1e-3);
    report(worst <= 1e-2, "tile lights: tile FAR term vs per-pixel, worst pixel", worst, 1e-2);
}

// Subsurface class, stage B in a frame (shading.subsurface_scatter; SubsurfaceScatter.hlsli): a Subsurface sphere and a
// Subsurface plate over a Standard ground under the sun (this build: no shadows and no indirect light, so a pixel's
// diffuse light is the sun's cosine - known on the CPU from its G-buffer normal - and the sphere's far side is black
// without the pass). Four frames of one view:
//   off     the switch off: stage A's picture;
//   kept    the switch on with a footprint limit no pixel reaches: every pixel keeps its own diffuse light, so the
//           class's pixels equal 'off' up to the diffuse texture's f16 rounding (the separation into the diffuse texture
//           and f_d back after it), and every other pixel bit for bit;
//   on      the defaults: the other classes' pixels bit for bit; the plate (uniformly lit) keeps its radiance; the
//           sphere's pixels just behind the terminator are lit, red most; pixels far behind it stay black;
//   on 64   against the estimate's limit on the CPU - for sampled pixels of the sphere the same plane from the depth,
//           sample test, weights and footprint share (SubsurfaceScatter.hlsli sssScatter, replicated here in double) with
//           a dense polar grid in place of the kernel's 64 samples: the mean difference (a wrong projection, plane or
//           weight shows here) and its spread (the 64 samples' noise).
float halfRoundTrip(float f)  // an R16_FLOAT store and load of a positive value in the format's normal range (nearest, ties to even)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    const uint32_t rest = u & 0x1FFFu;
    u &= ~0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (u & 0x2000u) != 0)) u += 0x2000u;
    std::memcpy(&f, &u, 4);
    return f;
}

void testSubsurfaceScatter(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "subsurface scatter test";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { 0.5f, 0.45f, 0.4f };
    ground.roughness = 0.5f;
    scene::Material skin;
    skin.name = "skin";
    skin.cls = scene::MaterialClass::Subsurface;
    skin.baseColor = { 0.80f, 0.56f, 0.45f };
    skin.roughness = 0.45f;
    skin.specular = 0.35f;
    skin.transmission = 0.0f;  // (no light through thin parts: the far side is black without the pass)
    skin.subsurfaceMeanFreePath = { 0.0120f, 0.0064f, 0.0045f };  // (a wax's: the scatter spans several pixels at this view's distance)
    s.materials = { ground, skin };
    scene::Instance a;
    a.mesh = addPlane(s, 40, 0);
    s.instances.push_back(a);
    scene::Instance b;
    b.mesh = addSphere(s, 0.25f, 64, 128, 1);
    b.transform = float3x4::translation({ 0, 0.6f, 0 });
    s.instances.push_back(b);
    scene::Instance c;
    c.mesh = addPlane(s, 0.4f, 1);  // the plate: 0.4 m, facing up
    c.transform = float3x4::translation({ 0.62f, 0.38f, 0.1f });
    s.instances.push_back(c);
    s.sun.direction = normalize(float3{ -0.85f, 0.45f, 0.3f });  // from the left: the terminator runs down the sphere's visible side
    scene::Camera cam;
    cam.name = "skin";
    cam.position = { 0.3f, 0.78f, 1.35f };
    cam.forward = normalize(float3{ -0.06f, -0.17f, -1 });
    cam.verticalFov = 0.6f;
    cam.ev100 = 13;
    s.cameras.push_back(cam);
    tf.setScene(s);

    const uint32_t W = 960, H = 540;
    struct Shot
    {
        std::shared_ptr<std::vector<uint8_t>> gb, words, lin, depth;
    };
    ViewDesc desc;
    // (the configuration's own values come back after each shot)
    const std::vector<std::string> restore = { std::string("shading.subsurface_scatter=") + (tf.quality.boolean("shading.subsurface_scatter") ? "true" : "false"),
                                               "shading.subsurface_scatter_samples=" + std::to_string(tf.quality.integer("shading.subsurface_scatter_samples")),
                                               "shading.subsurface_scatter_min_px=" + std::to_string(tf.quality.number("shading.subsurface_scatter_min_px")) };
    auto shoot = [&](std::initializer_list<const char*> settings) {
        for (const char* o : settings) tf.quality.applyOverride(o);
        Shot shot;
        tf.frame.outputLinearHdr = true;
        tf.run([&](FramePassContext& fc) {
            ViewResources v = tf.mainView(fc, W, H, 0);
            desc = v.view;
            v.color = fc.graph.createTexture({ "m.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
            tf.vis.record(fc, v);
            tracks::materialResolve(fc, v);
            tracks::shading(fc, v);
            shot.gb = tf.readback(fc, v.gbuffer);
            shot.words = tf.readback(fc, material::resolveOutputs(fc, v).materialWord);
            shot.lin = tf.readback(fc, v.color);
            shot.depth = tf.readback(fc, v.depth);
        });
        tf.frame.outputLinearHdr = false;
        for (const std::string& o : restore) tf.quality.applyOverride(o);
        return shot;
    };
    const Shot off = shoot({ "shading.subsurface_scatter=false" });
    const Shot kept = shoot({ "shading.subsurface_scatter=true", "shading.subsurface_scatter_min_px=1000000000.0" });
    const Shot on = shoot({ "shading.subsurface_scatter=true" });
    const Shot on64 = shoot({ "shading.subsurface_scatter=true", "shading.subsurface_scatter_samples=64" });

    // ---- the view's pixels on the CPU: class, position, the sun's diffuse light E_d (lux, per unit f_d), f_d
    const double exposure = 1.0 / (1.2 * std::exp2(13.0)), thetaS = s.sun.angularRadius;
    const float3 l0f = normalize(s.sun.direction);
    const AlVec l0 = alVec(l0f);
    // E_d of a surface facing the sun, per channel: the unit of the comparisons below (a pixel's E_d is its cap cosine x this)
    const double lit[3] = { (double)s.sun.illuminance * s.sun.color.x * 2 / (1 + std::cos(thetaS)), (double)s.sun.illuminance * s.sun.color.y * 2 / (1 + std::cos(thetaS)),
                            (double)s.sun.illuminance * s.sun.color.z * 2 / (1 + std::cos(thetaS)) };
    auto capCosine = [&](double NoL) {  // ShadingCommon.hlsli shCapCosine
        const double kR = std::sqrt(std::max(1 - NoL * NoL, 0.0)) * thetaS;
        if (NoL >= kR) return NoL;
        if (NoL <= -kR) return 0.0;
        const double u = -NoL / kR, w = std::sqrt(std::max(1 - u * u, 0.0));
        return (2 * kR / kPi) * (w * w * w / 3 - 0.5 * u * (std::acos(u) - u * w));
    };
    struct Px
    {
        bool skin = false;
        double z = 0, zHalf = 0;  // view depth; as the diffuse texture's alpha holds it
        AlVec n, fd;              // shading normal towards the viewer; f_d
        double Ed = 0;            // the sun's diffuse light per unit f_d, in units of 'lit' (the disk's clipped cosine)
    };
    std::vector<Px> px((size_t)W * H);
    AlVec Dx, Dy, back;  // the ray's change per pixel, the camera's back axis (MaterialSurface.hlsli mPixelRay)
    {
        const auto& P = desc.proj.m;
        Dx = AlVec{ desc.view.m[0][0], desc.view.m[0][1], desc.view.m[0][2] } * (2.0 / (W * P[0][0]));
        Dy = AlVec{ desc.view.m[1][0], desc.view.m[1][1], desc.view.m[1][2] } * (-2.0 / (H * P[1][1]));
        back = AlVec{ desc.view.m[2][0], desc.view.m[2][1], desc.view.m[2][2] };
    }
    auto rayOf = [&](double x, double y) {
        double D[3], unused[3];
        pixelRay(desc, x, y, D, unused);
        return AlVec{ D[0], D[1], D[2] };
    };
    uint32_t skinPixels = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            Px& p = px[(size_t)y * W + x];
            const uint32_t word = texelOf<uint32_t>(*off.words, W, x, y);
            p.z = desc.nearPlane / std::max((double)texelOf<float>(*off.depth, W, x, y), 1e-30);
            if ((word & 0xFFFF) != 1) continue;
            p.skin = true;
            ++skinPixels;
            p.zHalf = halfRoundTrip((float)p.z);
            const uint2 pk = texelOf<uint2>(*off.gb, W, x, y);
            const AlVec v = alNorm(rayOf(x + 0.5, y + 0.5) * -1.0);
            AlVec n = alVec(octDecode(pk.x));
            if (alDot(n, v) < 1e-4) n = alNorm(n + v * (1e-4 - alDot(n, v)));  // MaterialInternal.hlsli mNormalTowardsViewer
            p.n = n;
            const double albedoScale = (1 - ((word >> 16) & 0xFF) / 255.0) / kPi;
            p.fd = AlVec{ srgbToLinear((pk.y & 0xFF) / 255.0), srgbToLinear(((pk.y >> 8) & 0xFF) / 255.0), srgbToLinear(((pk.y >> 16) & 0xFF) / 255.0) } * albedoScale;
            p.Ed = capCosine(alDot(n, l0));
        }
    auto colour = [&](const Shot& shot, uint32_t x, uint32_t y) {
        const float4 v = texelOf<float4>(*shot.lin, W, x, y);
        return AlVec{ v.x, v.y, v.z };
    };

    // ---- the pictures against 'off'
    uint32_t otherBits = 0, otherPixels = 0, sameWords = 0;
    double worstKept = 0, worstPlate = 0;
    uint32_t platePixels = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            sameWords += texelOf<uint32_t>(*off.words, W, x, y) != texelOf<uint32_t>(*on.words, W, x, y);
            const Px& p = px[(size_t)y * W + x];
            const float4 o = texelOf<float4>(*off.lin, W, x, y);
            if (cpuIsEdge(desc, *off.words, *off.gb, *off.depth, W, H, x, y, tf.quality)) continue;  // (the edge composite mixes classes)
            if (!p.skin)
            {
                ++otherPixels;
                for (const Shot* shot : { &kept, &on, &on64 })
                    otherBits += std::memcmp(&o, shot->lin->data() + (size_t)y * TestFrame::rowPitch(W, 16) + (size_t)x * 16, 12) != 0;
                continue;
            }
            const AlVec k = colour(kept, x, y), n = colour(on, x, y), base = { o.x, o.y, o.z };
            const double scale = std::max({ (double)o.x, (double)o.y, (double)o.z, 1e-3 });
            worstKept = std::max({ worstKept, std::fabs(k.x - base.x) / scale, std::fabs(k.y - base.y) / scale, std::fabs(k.z - base.z) / scale });
            // the plate: every pixel of it lies in one plane under one light
            if (std::fabs(p.n.y - 1) < 1e-3 && x > W / 2)
            {
                ++platePixels;
                worstPlate = std::max({ worstPlate, std::fabs(n.x - base.x) / scale, std::fabs(n.y - base.y) / scale, std::fabs(n.z - base.z) / scale });
            }
        }
    logf("subsurface scatter: %u pixels of the class (%u of the plate), %u of other classes\n", skinPixels, platePixels, otherPixels);
    report(sameWords == 0 && skinPixels > 40000 && platePixels > 2000, "subsurface scatter: the view holds the sphere and the plate (material words differing)", sameWords, 0);
    report(otherBits == 0, "subsurface scatter: other classes' pixels differ from the switch off (kept, on, on 64)", otherBits, 0);
    // [measured: 9.6e-4 and 5.9e-4 - one f16 step of the diffuse texture is 9.8e-4 of the value]
    report(worstKept < 1.5e-3, "subsurface scatter: no pixel scattering = the switch off (rel., f16 diffuse texture)", worstKept, 1.5e-3);
    report(worstPlate < 1.5e-3, "subsurface scatter: a uniformly lit plate keeps its radiance (rel.)", worstPlate, 1.5e-3);

    // ---- the estimate's limit on the CPU (SubsurfaceScatter.hlsli sssScatter with a dense polar grid)
    auto surfaceNormal = [&](uint32_t x, uint32_t y, AlVec D, const Px& p) {
        const double beyond = 1e30;
        const double zl = x > 0 ? px[(size_t)y * W + x - 1].z : beyond, zr = x + 1 < W ? px[(size_t)y * W + x + 1].z : beyond;
        const double zu = y > 0 ? px[(size_t)(y - 1) * W + x].z : beyond, zd = y + 1 < H ? px[(size_t)(y + 1) * W + x].z : beyond;
        const AlVec P0 = D * p.z;
        const AlVec dx = std::fabs(zl - p.z) < std::fabs(zr - p.z) ? P0 - (D - Dx) * zl : (D + Dx) * zr - P0;
        const AlVec dy = std::fabs(zu - p.z) < std::fabs(zd - p.z) ? P0 - (D - Dy) * zu : (D + Dy) * zd - P0;
        AlVec g = alCross(dx, dy);
        const double len = std::sqrt(alDot(g, g));
        if (!(len > 0) || !(len < beyond)) return p.n;
        g = g * (1 / len);
        if (alDot(g, D) > 0) g = g * -1.0;
        return alDot(g, D) < -0.05 * std::sqrt(alDot(D, D)) ? g : p.n;
    };
    const float minPixels = (float)tf.quality.number("shading.subsurface_scatter_min_px");
    auto scatteredLimit = [&](uint32_t x, uint32_t y, double out[3]) {
        const Px& p = px[(size_t)y * W + x];
        const AlVec D = rayOf(x + 0.5, y + 0.5);
        const float3 d = model::subsurfaceDistance(skin.subsurfaceMeanFreePath, alF(p.fd * kPi));
        const double dc[3] = { d.x, d.y, d.z }, dS = std::max({ d.x, d.y, d.z });
        const double footprint = p.z * std::sqrt(alDot(Dx, Dx));
        const double strength = std::clamp(model::kSubsurfaceMeanRadius * dS / (footprint * minPixels) - 1, 0.0, 1.0);
        const AlVec plane = surfaceNormal(x, y, D, p);
        const AlVec t = alNorm(std::fabs(plane.z) < 0.9 ? alCross(AlVec{ 0, 0, 1 }, plane) : alCross(AlVec{ 1, 0, 0 }, plane)), bt = alCross(plane, t);
        const AlVec P0 = D * p.z;
        const double rc = footprint * 0.5642 / std::sqrt(std::max(-alDot(plane, D) / std::sqrt(alDot(D, D)), 0.25));
        auto cdf = [](double dd, double r) {
            const double yy = std::exp(-r / (3 * dd));
            return 1 - 0.25 * yy * yy * yy - 0.75 * yy;
        };
        auto pdf = [](double dd, double r) {
            const double yy = std::exp(-r / (3 * dd));
            return (yy * yy * yy + yy) / (4 * dd);
        };
        const double centreS = cdf(dS, rc), reach = 2.0 * dS + 2 * footprint + p.z / 512.0;
        const int K = 96, A = 96;
        double sum[3] = {}, weight[3] = {};
        for (int i = 0; i < K; ++i)
        {
            const double r = model::subsurfaceRadius((float)dS, (float)(centreS + (1 - centreS) * (i + 0.5) / K)), density = pdf(dS, r);
            for (int j = 0; j < A; ++j)
            {
                const double angle = 2 * kPi * (j + 0.5) / A;
                const AlVec Q = P0 + (t * std::cos(angle) + bt * std::sin(angle)) * r;
                const double depthQ = -alDot(Q, back);
                if (!(depthQ > desc.nearPlane)) continue;
                const AlVec u = Q * (1 / depthQ) - D;
                const int sx = (int)std::floor(x + 0.5 + alDot(u, Dx) / alDot(Dx, Dx)), sy = (int)std::floor(y + 0.5 + alDot(u, Dy) / alDot(Dy, Dy));
                if (sx < 0 || sy < 0 || sx >= (int)W || sy >= (int)H) continue;
                const Px& q = px[(size_t)sy * W + sx];
                if (!q.skin) continue;
                const double h = alDot((D + Dx * (double)(sx - (int)x) + Dy * (double)(sy - (int)y)) * q.zHalf - P0, plane);
                if (std::fabs(h) > reach) continue;
                const double rr = std::sqrt(r * r + h * h);
                for (int ch = 0; ch < 3; ++ch)
                {
                    const double w = pdf(dc[ch], rr) / density;
                    sum[ch] += q.Ed * w;
                    weight[ch] += w;
                }
            }
        }
        for (int ch = 0; ch < 3; ++ch)
        {
            const double tail = weight[ch] > 0 ? sum[ch] / weight[ch] : p.Ed, centre = cdf(dc[ch], rc);
            out[ch] = p.Ed + ((tail + (p.Ed - tail) * centre) - p.Ed) * strength;
        }
    };
    // sampled pixels of the sphere (the plate is uniform): the kernel's scattered light from the two pictures,
    // E_d + (on - off) / (f_d x exposure), against the limit; all in units of the lit level
    double mean[3] = {}, square[3] = {}, worst = 0, behindRed = 0, behindBlue = 0, farBehind = 0, defaultSpread = 0;
    uint32_t compared = 0, behind = 0, farPixels = 0;
    for (uint32_t y = 2; y + 2 < H; y += 5)
        for (uint32_t x = 2; x + 2 < W / 2 + 60; x += 5)
        {
            const Px& p = px[(size_t)y * W + x];
            if (!p.skin || std::fabs(p.n.y - 1) < 1e-3) continue;
            if (cpuIsEdge(desc, *off.words, *off.gb, *off.depth, W, H, x, y, tf.quality)) continue;
            double limit[3];
            scatteredLimit(x, y, limit);
            const AlVec o = colour(off, x, y), n64 = colour(on64, x, y), n16 = colour(on, x, y);
            const double fd[3] = { p.fd.x, p.fd.y, p.fd.z }, d64[3] = { n64.x - o.x, n64.y - o.y, n64.z - o.z }, d16[3] = { n16.x - o.x, n16.y - o.y, n16.z - o.z };
            for (int ch = 0; ch < 3; ++ch)
            {
                const double unit = fd[ch] * exposure * lit[ch], e = p.Ed + d64[ch] / unit - limit[ch];
                mean[ch] += e;
                square[ch] += e * e;
                worst = std::max(worst, std::fabs(e));
                defaultSpread += std::pow(p.Ed + d16[ch] / unit - limit[ch], 2) / 3;
            }
            ++compared;
            // behind the terminator (no sun on the pixel): lit by the pass within reach of it, black far from it
            const double NoL = alDot(p.n, l0);
            if (NoL < -0.02 && NoL > -0.12)
            {
                ++behind;
                behindRed += d16[0] / (fd[0] * exposure * lit[0]);
                behindBlue += d16[2] / (fd[2] * exposure * lit[2]);
            }
            if (NoL < -0.75)
            {
                ++farPixels;
                farBehind = std::max({ farBehind, std::fabs(d16[0]) / (fd[0] * exposure * lit[0]), std::fabs(d16[2]) / (fd[2] * exposure * lit[2]) });
            }
        }
    double bias = 0, spread = 0;
    for (int ch = 0; ch < 3; ++ch)
    {
        mean[ch] /= std::max(compared, 1u);
        bias = std::max(bias, std::fabs(mean[ch]));
        spread = std::max(spread, std::sqrt(std::max(square[ch] / std::max(compared, 1u) - mean[ch] * mean[ch], 0.0)));
    }
    defaultSpread = std::sqrt(defaultSpread / std::max(compared, 1u));
    behindRed /= std::max(behind, 1u);
    behindBlue /= std::max(behind, 1u);
    logf("subsurface scatter: %u pixels of the sphere against the dense grid: mean (%.5f %.5f %.5f), spread %.5f, worst %.5f of the lit level (64 samples); the defaults' "
         "spread %.5f; %u pixels just behind the terminator gain red %.5f, blue %.5f; %u far behind it at most %.2e\n",
         compared, mean[0], mean[1], mean[2], spread, worst, defaultSpread, behind, behindRed, behindBlue, farPixels, farBehind);
    // [measured, 3258 pixels: mean -1.1e-4 in every channel, spread 2.3e-3 (worst pixel 8.9e-3) at 64 samples, 3.3e-3 at the
    // default 16; 222 pixels behind the terminator gain 1.2e-2 in red and 6e-4 in blue]
    report(compared > 1500 && bias < 5e-4, "subsurface scatter: the kernel's mean vs the dense grid (of the lit level)", bias, 5e-4);
    report(spread < 4e-3 && worst < 0.02, "subsurface scatter: 64 samples' spread around the dense grid (of the lit level)", spread, 4e-3);
    report(defaultSpread < 6e-3, "subsurface scatter: the default sample count's spread around the dense grid (of the lit level)", defaultSpread, 6e-3);
    report(behind > 30 && behindRed > 5e-3 && behindRed > 4 * behindBlue, "subsurface scatter: light behind the terminator, red most (mean red, of the lit level)", behindRed, 5e-3);
    report(farPixels > 30 && farBehind < 1e-4, "subsurface scatter: no light far behind the terminator (of the lit level)", farBehind, 1e-4);

    // ---- the class in a planar reflection view: a mirror plane x = -1.2 left of the sphere (the mirrored camera sees its
    // lit side). SubsurfaceDirect / SubsurfaceIndirect with PLANAR = 1, m.sss.clear.planar and m.sss.scatter.planar are this
    // view's passes; the stand-in raster draws the whole scene (it does not clip at the mirror plane: the ground behind it
    // is in the picture, which the comparisons below do not mind).
    struct PlanarShot
    {
        std::shared_ptr<std::vector<uint8_t>> words, lin;
    };
    auto shootPlanar = [&](std::initializer_list<const char*> settings) {
        for (const char* o : settings) tf.quality.applyOverride(o);
        PlanarShot shot;
        tf.frame.outputLinearHdr = true;
        tf.run([&](FramePassContext& fc) {
            ViewResources mainV = tf.mainView(fc, W, H, 0);
            // Aim at the virtual sphere behind x=-1.2; the original narrow-FOV camera aimed at
            // the real sphere, leaving it outside the reflected view entirely.
            scene::Camera mirrorCamera = cam;
            mirrorCamera.forward = normalize(float3{-2.4f, 0.6f, 0} - cam.position);
            mainV.view = ViewDesc::fromCamera(mirrorCamera, W, H, float4x4{});
            ViewResources v;
            v.view = ViewDesc::planarReflection(mainV.view, float4{ 1, 0, 0, 1.2f }, 0, 0, W, H);
            v.frameConstants = fc.frameConstantsFor(v.view);
            v.color = fc.graph.createTexture({ "m.test.planar.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
            tf.vis.record(fc, v);
            tracks::materialResolve(fc, v);
            tracks::shading(fc, v);
            shot.words = tf.readback(fc, material::resolveOutputs(fc, v).materialWord);
            shot.lin = tf.readback(fc, v.color);
        });
        tf.frame.outputLinearHdr = false;
        for (const std::string& o : restore) tf.quality.applyOverride(o);
        return shot;
    };
    const PlanarShot pOff = shootPlanar({ "shading.subsurface_scatter=false" });
    const PlanarShot pKept = shootPlanar({ "shading.subsurface_scatter=true", "shading.subsurface_scatter_min_px=1000000000.0" });
    const PlanarShot pOn = shootPlanar({ "shading.subsurface_scatter=true" });
    uint32_t planarSkin = 0, planarOther = 0, planarOtherBits = 0, planarChanged = 0;
    double planarWorstKept = 0;
    for (uint32_t y = 1; y + 1 < H; ++y)
        for (uint32_t x = 1; x + 1 < W; ++x)
        {
            // (pixels whose 3 x 3 neighbourhood is one material: the edge composite mixes classes at the others)
            const uint32_t word = texelOf<uint32_t>(*pOff.words, W, x, y) & 0xFFFF;
            bool interior = true;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) interior = interior && (texelOf<uint32_t>(*pOff.words, W, x + dx, y + dy) & 0xFFFF) == word;
            if (!interior) continue;
            const float4 o = texelOf<float4>(*pOff.lin, W, x, y), k = texelOf<float4>(*pKept.lin, W, x, y), n = texelOf<float4>(*pOn.lin, W, x, y);
            if (word != 1)
            {
                ++planarOther;
                planarOtherBits += std::memcmp(&o, &k, 12) != 0 || std::memcmp(&o, &n, 12) != 0;
                continue;
            }
            ++planarSkin;
            const double scale = std::max({ (double)o.x, (double)o.y, (double)o.z, 1e-3 });
            planarWorstKept = std::max({ planarWorstKept, std::fabs(k.x - o.x) / scale, std::fabs(k.y - o.y) / scale, std::fabs(k.z - o.z) / scale });
            planarChanged += std::fabs(n.x - o.x) > 0.01 * scale;
        }
    logf("subsurface scatter, planar view: %u pixels of the class, %u of other classes; without scattering vs the switch off: worst %.2e; %u pixels changed by the "
         "scattering (red, over 1 %%)\n", planarSkin, planarOther, planarWorstKept, planarChanged);
    report(planarSkin > 5000 && planarOther > 5000, "subsurface scatter, planar view: the view holds the class and others", planarSkin, 5000);
    report(planarOtherBits == 0, "subsurface scatter, planar view: other classes' pixels differ from the switch off", planarOtherBits, 0);
    report(planarWorstKept < 1.5e-3, "subsurface scatter, planar view: no pixel scattering = the switch off (rel.)", planarWorstKept, 1.5e-3);
    report(planarChanged > 200, "subsurface scatter, planar view: the scattering changes the class's pixels", planarChanged, 200);
}

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, gpuValidation = false, growth = false, glassOnly = false, glassJobsOnly = false, preshadeOnly = false, coatOnly = false, sheenOnly = false, anisoOnly = false, filmOnly = false, areaQuadOnly = false, emissiveOnly = false, warp = false;
        bool subsurfaceOnly = false;
        std::vector<std::string> overrides;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--gbv") gpuValidation = true;
            if (std::string(argv[i]) == "--area-dump") g_areaDump = true;
            if (std::string(argv[i]) == "--set" && i + 1 < argc) overrides.push_back(argv[++i]);
            if (std::string(argv[i]) == "--coverage-growth") growth = true;
            if (std::string(argv[i]) == "--glass") glassOnly = true;
            if (std::string(argv[i]) == "--glass-jobs") glassJobsOnly = true;
            if (std::string(argv[i]) == "--preshade") preshadeOnly = true;
            if (std::string(argv[i]) == "--coat") coatOnly = true;
            if (std::string(argv[i]) == "--sheen") sheenOnly = true;
            if (std::string(argv[i]) == "--aniso") anisoOnly = true;
            if (std::string(argv[i]) == "--film") filmOnly = true;
            if (std::string(argv[i]) == "--film-image" && i + 1 < argc) g_filmImage = argv[i + 1];
            if (std::string(argv[i]) == "--area-quad") areaQuadOnly = true;
            if (std::string(argv[i]) == "--subsurface") subsurfaceOnly = true;
            if (std::string(argv[i]) == "--emissive") emissiveOnly = true;
            if (std::string(argv[i]) == "--warp") warp = true;  // compute probes only (full frames render black on WARP)
        }
        ComPtr<ID3D12Device> warpDevice;
        if (warp)
        {
            ComPtr<IDXGIFactory6> factory;
            check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&warpDevice))))
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&warpDevice)), "WARP device");
        }
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--log") logOpenFile(argv[i + 1]);  // a flushed copy of the log (runs under GpuLock)
        secondsSinceStart();
        Report report;
        testTable(report);
        TestFrame tf(debugLayer, gpuValidation, warpDevice.Get());
        if (subsurfaceOnly)  // --subsurface: the Subsurface class's scatter frames alone
        {
            testSubsurfaceScatter(tf, report);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        if (emissiveOnly)  // --emissive: the emissive panel (14.1b) alone
        {
            testEmissivePanel(tf, report);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        if (areaQuadOnly)  // --area-quad: the sheen / anisotropic area-light probe alone
        {
            testAreaQuadrature(tf, report);
            testAreaLobesFrame(tf, report);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        if (filmOnly)  // --film: the thin film frame, after a scene with anisotropy (the layer tables' re-placement across scenes)
        {
            testAnisoFrame(tf, report);
            testFilmFrame(tf, report);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        if (anisoOnly)  // --aniso: the anisotropy frame alone
        {
            testAnisoFrame(tf, report);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        if (sheenOnly)  // --sheen: the sheen probe alone
        {
            testSheen(tf, report);
            testSheenFrame(tf, report);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        if (coatOnly)  // --coat: test 14 alone
        {
            testCoatSun(tf, report);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        if (preshadeOnly)  // --preshade: test 13 alone
        {
            testPreshadedRecords(tf, report);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        for (const std::string& o : overrides) tf.quality.applyOverride(o);
        if (growth)
        {
            const int failures = testCoverageGrowth(tf);
            logf("%s: %d failure(s)\n", failures ? "FAILED" : "passed", failures);
            return failures ? 1 : 0;
        }
        if (glassJobsOnly)  // --glass-jobs: test 12's refraction-service cases alone
        {
            testGlassComposite(tf, report, 1);
            testGlassComposite(tf, report, 2);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        if (glassOnly)  // --glass: test 12 alone
        {
            testGlassComposite(tf, report);
            testGlassComposite(tf, report, 1);
            testGlassComposite(tf, report, 2);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        testScene(tf, report);
        testSunSpecular(tf, report);
        testLocalLights(tf, report);
        testEdgeArea(tf, report);
        testEdgeComposite(tf, report);
        testEdgeCutout(tf, report);
        testAreaLights(tf, report);
        testAreaLightContours(tf, report);
        testCoverageComposite(tf, report);
        testPlanarProducts(tf, report);
        testGlassComposite(tf, report);
        testPreshadedRecords(tf, report);
        testCoatSun(tf, report);
        testSheen(tf, report);
        testSheenFrame(tf, report);
        testAnisoFrame(tf, report);
        testFilmFrame(tf, report);
        testAreaQuadrature(tf, report);
        testAreaLobesFrame(tf, report);
        testEmissivePanel(tf, report);  // 14.1b (L2b)
        testTileLights(tf, report);  // 14.1/14.2 (L2)
        testSubsurfaceScatter(tf, report);  // the Subsurface class's scatter pass
        logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
        return report.failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
