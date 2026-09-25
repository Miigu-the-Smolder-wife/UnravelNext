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
//      where the bilinear alpha passes the cutoff), so cut edges keep their width.
//   unx_test_shading_shadingtests [--no-debug-layer]
#include "../../Material/Tests/MTestFrame.h"

#include "unx/scene/MaterialModel.h"
#include "unx/shading/ShadingSystem.h"

#include <array>
#include <cstdio>
#include <random>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;
namespace model = unx::scene::model;

namespace
{
constexpr double kPi = 3.14159265358979323846;

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

float3 pbrNeutral(float3 c)
{
    const float startCompression = 0.8f - 0.04f, desaturation = 0.15f;
    const float x = std::min(c.x, std::min(c.y, c.z));
    const float offset = x < 0.08f ? x - 6.25f * x * x : 0.04f;
    c = c - float3{ offset, offset, offset };
    const float peak = std::max(c.x, std::max(c.y, c.z));
    if (peak < startCompression) return c;
    const float d = 1 - startCompression;
    const float newPeak = 1 - d * d / (peak + d - startCompression);
    c = c * (newPeak / peak);
    const float g = 1 - 1 / (desaturation * (peak - newPeak) + 1);
    return c + (float3{ newPeak, newPeak, newPeak } - c) * g;
}

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
    // The same LUT the shading pass uploads (ShadingSystem.cpp), for the probe kernel.
    const std::vector<float>& table = shading::specularAlbedoTable();
    ComPtr<ID3D12Resource> lut = uploadStatic(tf.device, table.data(), table.size() * 4, L"test specular LUT");
    const uint32_t lutSrv = tf.device.descriptors().allocateResource();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = (UINT)(table.size() / 2);
        sd.Buffer.StructureByteStride = 8;
        tf.device.d3d()->CreateShaderResourceView(lut.Get(), &sd, tf.device.descriptors().resourceCpu(lutSrv));
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
                             const uint32_t k[4] = { c.srv(in), c.uav(res), count, lutSrv };
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 4);
                             c.cmd->Dispatch((count + 63) / 64, 1, 1);
                         });
        out = tf.readbackBuffer(fc, res, cases.size() * 16);
    });
    tf.device.descriptors().freeResource(lutSrv);
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
    for (int pass = 0; pass < 2; ++pass)
    {
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
    const double exposure = 1.0 / (1.2 * std::exp2(13.0));
    const float3 E = s.sun.color * s.sun.illuminance;
    const double cap = 2 / (1 + std::cos(thetaS));
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
                const float NoV = dot(n, v), NoL = dot(n, l0);
                const float3 diffuse = su.baseColor * ((1 - su.metallic) / model::kPi);
                const float3 front = su.cls == scene::MaterialClass::Foliage ? diffuse * (1 - su.transmission) : diffuse;
                const float3 back = su.cls == scene::MaterialClass::Foliage ? diffuse * su.transmission : float3{};
                // Cap averages of the clipped cosines: the point value away from the terminator, a dense disk quadrature
                // within two disk radii of it (the model integrated over the disk, as the reference path tracer does).
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
                        if (alpha < 16 * thetaS && spec.y > 10 * front.y * NoL) ++glint;
                    }
                    if (su.cls == scene::MaterialClass::Foliage && below > 0)
                    {
                        sun = sun + back * (float)(below * cap);
                        ++backlit;
                    }
                }
                else if (su.cls == scene::MaterialClass::Foliage && above > 0)
                {
                    sun = back * (float)(above * cap);
                    ++backlit;
                }
                expected = (sun * E + mat.emissive) * (float)exposure;
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
            const float3 t = pbrNeutral({ std::max(l.x, 0.f), std::max(l.y, 0.f), std::max(l.z, 0.f) });
            const double want[3] = { oetf(std::clamp(t.x, 0.f, 1.f)) * 1023, oetf(std::clamp(t.y, 0.f, 1.f)) * 1023, oetf(std::clamp(t.z, 0.f, 1.f)) * 1023 };
            for (int k = 0; k < 3; ++k) worstLsb = std::max(worstLsb, std::abs((double)((d >> (10 * k)) & 0x3FF) - want[k]));
        }
    report(worstLsb <= 1.0, "display: RGB10A2 = sRGB OETF(PBR Neutral(linear)) (10-bit LSB)", worstLsb, 1.0);

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
    light(scene::LightType::Point, { -1.6f, 1.2f, -1.5f }, { 0, -1, 0 }, 300, 7, true);   // caster 4: beyond the slots
    scene::Camera cam;
    cam.name = "lights";
    cam.position = { 0.5f, 2.2f, 5 };
    cam.forward = normalize(float3{ -0.1f, -0.35f, -1 });
    cam.ev100 = 5;
    s.cameras.push_back(cam);
    tf.setScene(s);

    // S's froxel list buffer: header, one froxel (first entry 0, count 5), indices 0..4 as 16-bit pairs.
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
    list[10] = 64;  // indexStride
    list[11] = 5;   // indexCount
    list[16] = (0u << 6) | 5u;
    list[32] = 0 | (1u << 16);
    list[33] = 2 | (3u << 16);
    list[34] = 4;
    ComPtr<ID3D12Resource> listBuffer = uploadStatic(tf.device, list.data(), list.size() * 4, L"test froxel list");
    const uint32_t slot[4] = { 255, 128, 255, 64 };
    const double slotVisibility[5] = { 128 / 255.0, 1.0, 1.0, 64 / 255.0, 1.0 };  // per light, list order

    const uint32_t W = 960, H = 540;
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
        tracks::shading(fc, v);
        gb = tf.readback(fc, v.gbuffer);
        words = tf.readback(fc, material::resolveOutputs(fc, v).materialWord);
        depth = tf.readback(fc, v.depth);
        lin = tf.readback(fc, v.color);
    });
    tf.frame.outputLinearHdr = false;

    const double exposure = 1.0 / (1.2 * std::exp2(5.0));
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
            for (size_t i = 0; i < s.lights.size(); ++i)
            {
                const scene::Light& l = s.lights[i];
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
                for (int k = 0; k < 3; ++k) sum[k] += (&f.x)[k] * (&l.color.x)[k] * I * std::fabs(cosL) * slotVisibility[i];
            }
            const float4 got = texelOf<float4>(*lin, W, x, y);
            const double e[3] = { sum[0] * exposure, sum[1] * exposure, sum[2] * exposure };
            const double scale = std::max({ e[0], e[1], e[2], 1e-2 });
            const double err = std::max({ std::abs(got.x - e[0]), std::abs(got.y - e[1]), std::abs(got.z - e[2]) }) / scale;
            if (err > worst && err > 5e-3) logf("  lights px (%u,%u) got (%.5f %.5f %.5f) expected (%.5f %.5f %.5f)\n", x, y, got.x, got.y, got.z, e[0], e[1], e[2]);
            worst = std::max(worst, err);
            ++checked;
        }
    logf("local lights: %u pixels, %u light-surface pairs reflected, %u transmitted through leaves \n", checked, lit, back);
    report(worst < 5e-3, "local lights [point, spot, shadow slots by list order] vs CPU model (rel.)", worst, 5e-3);
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

} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
        Report report;
        testTable(report);
        TestFrame tf(debugLayer);
        testScene(tf, report);
        testSunSpecular(tf, report);
        testLocalLights(tf, report);
        testEdgeArea(tf, report);
        testEdgeComposite(tf, report);
        testEdgeCutout(tf, report);
        logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
        return report.failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
