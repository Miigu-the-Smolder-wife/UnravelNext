// M shading correctness (no GPU lock). Builds without S and R: sun without atmosphere or shadows, no indirect light.
//   1. the f0-split specular albedo table: A + B = the model's E table;
//   2. the solar-disk specular integral (shSunSpecular) against a dense CPU quadrature of the model BRDF over the disk,
//      for roughness 0.01 .. 0.5 and mirror directions inside, on and outside the disk edge;
//   3. a lit scene end to end (stand-in V, M resolve, M shading, linear output): every sampled pixel against the model
//      evaluated on the CPU from the read-back G-buffer and material word;
//   4. the solar disk in the sky: per-pixel coverage against 32 x 32 supersampling and the total flux against E;
//   5. the display encoding (PBR Neutral + sRGB OETF, RGB10A2) against the CPU on the linear image.
//   unx_test_shading_shadingtests [--no-debug-layer]
#include "../../Material/Tests/MTestFrame.h"

#include "unx/scene/MaterialModel.h"
#include "unx/shading/ShadingSystem.h"

#include <cstdio>

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
    for (float r : { 0.01f, 0.03f, 0.04f, 0.06f, 0.1f, 0.15f, 0.19f, 0.2f, 0.3f, 0.5f })
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
    double worstPoint = 0, worstQuad = 0, worstMirror = 0;
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
        const char* regime = alpha >= 16 * thetaS ? "point" : alpha >= thetaS / 3 ? "quadrature" : "narrow lobe";
        if (e > 5e-3) logf("  r %.2f offset %.2f (%s): gpu %.5e ref %.5e (peak %.5e) err %.2e\n", cases[i].roughness, cases[i].offset, regime, g.y, ref.y, peak.y, e);
        if (alpha >= 16 * thetaS) worstPoint = std::max(worstPoint, e);
        else if (alpha >= thetaS / 3) worstQuad = std::max(worstQuad, e);
        else worstMirror = std::max(worstMirror, e);
    }
    report(worstPoint < 5e-3, "sun specular [point, alpha >= 16 theta_s] vs dense quadrature (rel. to peak)", worstPoint, 5e-3);
    report(worstQuad < 1e-2, "sun specular [disk quadrature] vs dense quadrature (rel. to peak)", worstQuad, 1e-2);
    report(worstMirror < 1e-2, "sun specular [narrow lobe, alpha < theta_s / 3] vs lobe-sampled reference (rel. to peak)", worstMirror, 1e-2);
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
            }
            else disp = tf.readback(fc, v.color);
        });
    }
    tf.frame.outputLinearHdr = false;
    const double exposure = 1.0 / (1.2 * std::exp2(13.0));
    const float3 E = s.sun.color * s.sun.illuminance;
    const double cap = 2 / (1 + std::cos(thetaS));
    double worst = 0;
    uint32_t checked = 0, glint = 0, backlit = 0;
    for (uint32_t y = 0; y < H; y += 3)
        for (uint32_t x = 0; x < W; x += 3)
        {
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
                float3 sun{};
                if (NoV > 0 && NoL > 0)
                {
                    const float3 front = su.cls == scene::MaterialClass::Foliage ? diffuse * (1 - su.transmission) : diffuse;
                    const double alpha = model::alphaFromRoughness(su.roughness);
                    const float3 spec = alpha >= 16 * thetaS ? (model::evaluate(su, n, v, l0) - front) * NoL * (float)cap : sunSpecularReference(su, n, v, l0, thetaS, 24, 48);
                    sun = front * NoL * (float)cap + spec;
                    if (alpha < 16 * thetaS && spec.y > 10 * front.y * NoL) ++glint;
                }
                else if (su.cls == scene::MaterialClass::Foliage && NoV * NoL < 0)
                {
                    sun = diffuse * su.transmission * std::fabs(NoL) * (float)cap;
                    ++backlit;
                }
                expected = (sun * E + mat.emissive) * (float)exposure;
            }
            const double scale = std::max({ expected.x, expected.y, expected.z, 1e-3f });
            const double e = std::max({ std::abs(got.x - expected.x), std::abs(got.y - expected.y), std::abs(got.z - expected.z) }) / scale;
            if (e > worst && e > 5e-3) logf("  px (%u,%u) word %08x got (%.5f %.5f %.5f) expected (%.5f %.5f %.5f)\n", x, y, word, got.x, got.y, got.z, expected.x, expected.y, expected.z);
            worst = std::max(worst, e);
            ++checked;
        }
    logf("scene: %u pixels checked, %u sun-glint pixels (disk regimes), %u back-lit leaf pixels\n", checked, glint, backlit);
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
        logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
        return report.failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
