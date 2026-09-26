// A9 anisotropy on the GPU (Passes/Material/Aniso.hlsli, MATERIAL_LAYERS 1.5), against the C++ model and a double replica:
//   1. mirror: the lobe (anisoSpecular vs scene::model::evaluateAnisotropic on a metal), E_a from the table, the frame
//      (anisoFrame), the per-pixel word's round trip about the G-buffer's decoded normal, the sampling pdf;
//   2. V: the frame of the vis-buffer surface (MaterialSurface.hlsli interpolants: the cooked tangent, rotated, about the
//      interpolated normal) and the per-axis band limit, at every pixel of a brushed sphere, against a double replica of
//      the pixel-centre barycentrics and derivatives.
// Run with --warp for the software adapter (no GPU lock needed); on hardware take the GPU lock (correctness).
#include "MTestFrame.h"

#include "unx/scene/MaterialModel.h"

#include <cstdio>
#include <dxgi1_6.h>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

namespace
{
constexpr double kPi = 3.14159265358979323846;

struct D3
{
    double x = 0, y = 0, z = 0;
};
D3 d3(float3 v) { return { v.x, v.y, v.z }; }
D3 operator+(D3 a, D3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
D3 operator-(D3 a, D3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
D3 operator*(D3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double len(D3 a) { return std::sqrt(dot(a, a)); }
D3 norm(D3 a) { return a * (1.0 / len(a)); }

// GPU vertex normal/tangent storage: octahedral snorm16 (GpuScene.cpp, Scene.hlsli).
uint32_t octEncode(float3 n)
{
    float s = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    float ex = n.x / s, ey = n.y / s;
    if (n.z < 0)
    {
        const float ox = (1 - std::fabs(ey)) * (ex >= 0 ? 1.f : -1.f), oy = (1 - std::fabs(ex)) * (ey >= 0 ? 1.f : -1.f);
        ex = ox;
        ey = oy;
    }
    auto q = [](float v) { return (uint32_t)(int32_t)std::lround(std::clamp(v, -1.f, 1.f) * 32767.f) & 0xFFFFu; };
    return q(ex) | (q(ey) << 16);
}
D3 octDecode(uint32_t p)
{
    const double ex = (int16_t)(p & 0xFFFF) / 32767.0, ey = (int16_t)(p >> 16) / 32767.0;
    D3 n{ ex, ey, 1 - std::fabs(ex) - std::fabs(ey) };
    if (n.z < 0)
    {
        const double ox = (1 - std::fabs(n.y)) * (n.x >= 0 ? 1 : -1), oy = (1 - std::fabs(n.x)) * (n.y >= 0 ? 1 : -1);
        n.x = ox;
        n.y = oy;
    }
    return norm(n);
}
D3 quantised(float3 n) { return octDecode(octEncode(n)); }
float3 f3(D3 a) { return { (float)a.x, (float)a.y, (float)a.z }; }

struct Report
{
    int failures = 0;
    void operator()(bool ok, const char* what, double value, double limit)
    {
        logf("%-72s %.3e (limit %.1e) %s\n", what, value, limit, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }
};

// A sphere with the circumferential tangent (addSphere of MaterialTests: uv = (longitude, latitude)).
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
            const float3 t = normalize(float3{ (float)-std::sin(ph), 0.0f, (float)std::cos(ph) } + float3{ 1e-6f, 0, 0 });
            m.tangents.push_back({ t.x, t.y, t.z, (r & 1) ? -1.0f : 1.0f });  // both bitangent signs occur
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

// Double replica of the vis-buffer surface's interpolants at a pixel centre (MaterialSurface.hlsli), with the tangent.
struct Replica
{
    D3 T, N;       // interpolated tangent, normal (not normalised)
    D3 dndx, dndy; // screen derivatives of the normalised normal
    float sign = 1;
};

Replica replicate(const TestFrame& tf, const ViewDesc& v, uint32_t visId, uint32_t px, uint32_t py)
{
    const gpu::VisibleCluster vc = tf.vis.visible.at((visId - 1) >> 7);
    const gpu::Cluster& c = tf.vis.clusters.clusters.at(vc.cluster);
    const uint32_t packed = tf.vis.clusters.clusterTriangles.at(c.triangleOffset + ((visId - 1) & 127));
    const scene::Instance& inst = tf.sceneData.instances.at(vc.instance);
    const scene::Mesh& mesh = tf.sceneData.meshes.at(inst.mesh);
    D3 P[3], N[3], T[3];
    float sign = 1;
    for (int k = 0; k < 3; ++k)
    {
        const uint32_t mv = tf.vis.clusters.clusterVertexIndices.at(c.vertexOffset + ((packed >> (8 * k)) & 0xFF));
        P[k] = d3(inst.transform.transformPoint(mesh.positions[mv]));
        N[k] = norm(d3(inst.transform.transformVector(f3(quantised(mesh.normals[mv])))));
        const float4 t = mesh.tangents[mv];
        T[k] = norm(d3(inst.transform.transformVector(f3(quantised({ t.x, t.y, t.z })))));
        if (k == 0) sign = t.w < 0 ? -1.0f : 1.0f;
    }
    const auto& Pm = v.proj.m;
    const double ndcx = (px + 0.5) / v.width * 2 - 1, ndcy = 1 - (py + 0.5) / v.height * 2;
    const double vx = (ndcx + Pm[0][2] - Pm[0][3]) / Pm[0][0], vy = (ndcy + Pm[1][2] - Pm[1][3]) / Pm[1][1];
    const D3 ax{ v.view.m[0][0], v.view.m[0][1], v.view.m[0][2] }, ay{ v.view.m[1][0], v.view.m[1][1], v.view.m[1][2] }, az{ v.view.m[2][0], v.view.m[2][1], v.view.m[2][2] };
    const D3 D = ax * vx + ay * vy - az, Dx = ax * (2.0 / (v.width * Pm[0][0])), Dy = ay * (-2.0 / (v.height * Pm[1][1]));
    const D3 C = d3(v.position);
    const D3 r0 = P[0] - C, e1 = P[1] - P[0], e2 = P[2] - P[0], n = cross(e1, e2);
    const double nD = dot(n, D), t = dot(n, r0) / nD, inv = 1 / dot(n, n);
    const D3 r = D * t - r0;
    const D3 rx = (Dx - D * (dot(n, Dx) / nD)) * t, ry = (Dy - D * (dot(n, Dy) / nD)) * t;
    const double b1 = dot(n, cross(r, e2)) * inv, b2 = dot(n, cross(e1, r)) * inv;
    const double b1x = dot(n, cross(rx, e2)) * inv, b2x = dot(n, cross(e1, rx)) * inv, b1y = dot(n, cross(ry, e2)) * inv, b2y = dot(n, cross(e1, ry)) * inv;
    const double b[3] = { 1 - b1 - b2, b1, b2 }, bx[3] = { -b1x - b2x, b1x, b2x }, by[3] = { -b1y - b2y, b1y, b2y };
    Replica out;
    out.sign = sign;
    D3 nx{}, ny{};
    for (int k = 0; k < 3; ++k)
    {
        out.N = out.N + N[k] * b[k];
        out.T = out.T + T[k] * b[k];
        nx = nx + N[k] * bx[k];
        ny = ny + N[k] * by[k];
    }
    const double l = len(out.N);
    const D3 nh = out.N * (1 / l);
    out.dndx = (nx - nh * dot(nh, nx)) * (1 / l);
    out.dndy = (ny - nh * dot(nh, ny)) * (1 / l);
    return out;
}

// the angle between two lines (t and -t are the same lobe direction)
double lineAngle(D3 a, D3 b) { return std::acos(std::clamp(std::fabs(dot(norm(a), norm(b))), 0.0, 1.0)); }

void run(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "anisotropy test";
    scene::Material brushed;
    brushed.name = "brushed";
    brushed.baseColor = { 0.95f, 0.64f, 0.54f };
    brushed.metallic = 1;
    brushed.roughness = 0.3f;
    brushed.anisotropy = 0.8f;
    brushed.anisotropyRotation = 0.6f;
    s.materials.push_back(brushed);
    const uint32_t sphere = addSphere(s, 1.0f, 32, 64, 0);
    scene::Instance a;
    a.mesh = sphere;
    s.instances.push_back(a);
    scene::Instance skewed;  // rotated about z and scaled 1.3 (instances are rotation + uniform scale)
    skewed.mesh = sphere;
    skewed.transform = float3x4::translation({ 2.4f, 0.2f, -0.5f });
    skewed.transform.m[0][0] = 1.3f * 0.8f;
    skewed.transform.m[0][1] = -1.3f * 0.6f;
    skewed.transform.m[1][0] = 1.3f * 0.6f;
    skewed.transform.m[1][1] = 1.3f * 0.8f;
    skewed.transform.m[2][2] = 1.3f;
    s.instances.push_back(skewed);
    scene::Camera cam;
    cam.name = "near";
    cam.position = { 1.2f, 0.8f, 4.5f };
    cam.forward = normalize(float3{ 1.2f, 0, 0 } - cam.position);
    s.cameras.push_back(cam);
    tf.setScene(s);

    // 1. mirror queries
    struct Case { float3 v, l, T, N, n; float r, st, sign, theta; };
    std::vector<Case> cases;
    uint32_t state = 777;
    auto rnd = [&]() { state = state * 1664525u + 1013904223u; return (state >> 8) * (1.0f / 16777216.0f); };
    auto hemi = [&](float minMu) {
        const float mu = minMu + (1 - minMu) * rnd(), ph = 6.2831853f * rnd(), sn = std::sqrt(1 - mu * mu);
        return float3{ sn * std::cos(ph), sn * std::sin(ph), mu };
    };
    for (int k = 0; k < 2048; ++k)
    {
        Case c;
        c.v = hemi(0.02f);
        c.l = hemi(0.02f);
        c.r = std::max(0.02f, rnd());
        c.st = rnd();
        c.T = normalize(float3{ rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f } + float3{ 1e-3f, 0, 0 });
        c.N = normalize(float3{ rnd() - 0.5f, rnd() - 0.5f, rnd() - 0.5f } + float3{ 0, 0, 1e-3f });
        c.n = normalize(c.N + float3{ 0.3f * (rnd() - 0.5f), 0.3f * (rnd() - 0.5f), 0.3f * (rnd() - 0.5f) });
        c.sign = rnd() < 0.5f ? -1.0f : 1.0f;
        c.theta = 6.2831853f * rnd();
        cases.push_back(c);
    }
    std::vector<float4> q;
    for (const Case& c : cases)
    {
        q.push_back({ c.v.x, c.v.y, c.v.z, c.r });
        q.push_back({ c.l.x, c.l.y, c.l.z, c.st });
        q.push_back({ c.T.x, c.T.y, c.T.z, c.sign });
        q.push_back({ c.N.x, c.N.y, c.N.z, c.theta });
        q.push_back({ c.n.x, c.n.y, c.n.z, 0 });
    }
    const uint32_t W = 640, H = 360;
    std::shared_ptr<std::vector<uint8_t>> mirror, pixels, vis;
    ViewDesc viewDesc;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H);
        viewDesc = v.view;
        tf.vis.record(fc, v);
        BufferRef in = fc.graph.createBuffer({ "m.test.aniso queries", q.size() * 16, 16 });
        BufferRef res = fc.graph.createBuffer({ "m.test.aniso mirror", cases.size() * 64, 16 });
        BufferRef px = fc.graph.createBuffer({ "m.test.aniso pixels", (uint64_t)W * H * 32, 16 });
        ComPtr<ID3D12Resource> staging = makeBuffer(tf.device, q.size() * 16, D3D12_HEAP_TYPE_UPLOAD);
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(staging->Map(0, &none, &p), "map");
        std::memcpy(p, q.data(), q.size() * 16);
        staging->Unmap(0, nullptr);
        tf.keep(staging);
        fc.graph.addPass("m.test.aniso upload", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(in, Use::CopyDst); },
                         [staging, in, bytes = q.size() * 16](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(in), 0, staging.Get(), 0, bytes); });
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Material/Tests/AnisoProbe");
        const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
        const uint32_t count = (uint32_t)cases.size();
        const TextureRef visId = v.visId;
        const BufferRef vcl = v.visibleClusters;
        fc.graph.addPass("m.test.aniso probe", QueueType::Graphics,
                         [&](PassBuilder& pb) {
                             pb.use(in, Use::SrvCompute);
                             pb.use(res, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             const uint32_t k[8] = { c.srv(in), c.uav(res), count, 0, 0, 0, 0, 0 };
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 8);
                             c.cmd->Dispatch((count + 63) / 64, 1, 1);
                         });
        fc.graph.addPass("m.test.aniso frame", QueueType::Graphics,
                         [&](PassBuilder& pb) {
                             pb.use(visId, Use::SrvCompute);
                             pb.use(vcl, Use::SrvCompute);
                             pb.use(px, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             const uint32_t k[8] = { 0, c.uav(px), W * H, 1, c.srv(visId), c.srv(vcl), W, H };
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 8);
                             c.cmd->Dispatch((W * H + 63) / 64, 1, 1);
                         });
        mirror = tf.readbackBuffer(fc, res, cases.size() * 64);
        pixels = tf.readbackBuffer(fc, px, (uint64_t)W * H * 32);
        vis = tf.readback(fc, v.visId);
    });

    double eLobe = 0, eAlbedo = 0, eFrame = 0, eWord = 0, ePdf = 0;
    uint32_t okMismatch = 0;
    for (size_t i = 0; i < cases.size(); ++i)
    {
        const Case& c = cases[i];
        float4 g[4];
        std::memcpy(g, mirror->data() + i * 64, 64);
        scene::model::Surface metal;
        metal.baseColor = { 1, 0.5f, 0.04f };
        metal.metallic = 1;
        metal.roughness = c.r;
        scene::model::Anisotropy an;
        an.strength = c.st;
        const float3 f = scene::model::evaluateAnisotropic(metal, an, { 0, 0, 1 }, c.v, c.l);
        const float2 al = scene::model::anisoAlphas(c.r, c.st);
        const float2 ab = scene::model::anisoSpecularAlbedo(c.v, al.x, al.y);
        // lobe x cos relative to the lobe's cosine-weighted scale E / pi (the sheen probe's measure)
        const double scale = std::max((ab.x + ab.y) / kPi, 1e-3);
        eLobe = std::max(eLobe, std::abs((double)g[0].x - f.x) * c.l.z / scale);
        eAlbedo = std::max(eAlbedo, std::abs((double)g[0].w - (ab.x + ab.y)));
        float3 t, b;
        const bool ok = scene::model::anisoFrame(c.T, c.sign, c.N, c.theta, c.n, t, b);
        if (ok != (g[1].w > 0.5f)) ++okMismatch;
        if (ok)
        {
            eFrame = std::max(eFrame, lineAngle(d3(t), { g[1].x, g[1].y, g[1].z }));
            eWord = std::max(eWord, lineAngle({ g[1].x, g[1].y, g[1].z }, { g[2].x, g[2].y, g[2].z }));
        }
        // pdf = G1(v) D / (4 v.z), D in double
        const float3 h = normalize(c.v + c.l);
        const double x = h.x / al.x, y = h.y / al.y, d = x * x + y * y + (double)h.z * h.z;
        const double D = 1 / (kPi * al.x * al.y * d * d);
        const double lv = std::sqrt((double)al.x * al.x * c.v.x * c.v.x + (double)al.y * al.y * c.v.y * c.v.y + (double)c.v.z * c.v.z);
        const double pdf = 2 * c.v.z / (c.v.z + lv) * D / (4 * c.v.z);
        ePdf = std::max(ePdf, std::abs(g[3].x - pdf) / std::max(pdf, 1e-3));
    }
    report(eLobe < 1e-3, "mirror: anisoSpecular x cos vs evaluateAnisotropic (rel. to E / pi)", eLobe, 1e-3);
    // float atan2 / sqrt on the table coordinates (the azimuth axis spans 12 cells over pi / 2): 2.5e-5 measured on WARP,
    // 350x below the table's own interpolation error (8.7e-3)
    report(eAlbedo < 5e-5, "mirror: anisoSpecularAlbedo (A + B) vs C++ (abs.)", eAlbedo, 5e-5);
    report(okMismatch == 0, "mirror: anisoFrame degenerate flags", okMismatch, 0);
    report(eFrame < 1e-5, "mirror: anisoFrame t vs C++ (radians)", eFrame, 1e-5);
    report(eWord < 1e-4, "mirror: word round trip about the decoded normal (radians)", eWord, 1e-4);
    report(ePdf < 1e-4, "mirror: anisoPdf vs double (rel., floor 1e-3)", ePdf, 1e-4);

    // 2. V: the frame of the vis-buffer surface at every pixel
    double eT = 0, eAlpha = 0, meanT = 0;
    uint32_t surfaces = 0;
    const scene::Material& mat = s.materials[0];
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            const uint32_t id = texelOf<uint32_t>(*vis, W, x, y);
            if (id == 0) continue;
            ++surfaces;
            float4 g[2];
            std::memcpy(g, pixels->data() + ((size_t)y * W + x) * 32, 32);
            const Replica r = replicate(tf, viewDesc, id, x, y);
            const D3 n = norm(r.N);
            float3 t, b;
            if (!scene::model::anisoFrame(f3(r.T), r.sign, f3(r.N), mat.anisotropyRotation, f3(n), t, b)) continue;
            const double e = lineAngle(d3(t), { g[0].x, g[0].y, g[0].z });
            eT = std::max(eT, e);
            meanT += e;
            // band limit: alpha'^2 = alpha^2 + 2 S (S = the footprint's slope variance along the axis)
            const float2 al = scene::model::anisoAlphas(mat.roughness, mat.anisotropy);
            const D3 td = d3(t), bd = d3(b);
            const double stt = (dot(r.dndx, td) * dot(r.dndx, td) + dot(r.dndy, td) * dot(r.dndy, td)) / 12;
            const double sbb = (dot(r.dndx, bd) * dot(r.dndx, bd) + dot(r.dndy, bd) * dot(r.dndy, bd)) / 12;
            const double at = std::sqrt((double)al.x * al.x + 2 * stt), ab = std::sqrt((double)al.y * al.y + 2 * sbb);
            eAlpha = std::max({ eAlpha, std::abs(g[1].x - at) / at, std::abs(g[1].y - ab) / ab });
        }
    logf("V: %u surface pixels, mean tangent angle error %.2e rad\n", surfaces, surfaces ? meanT / surfaces : 0.0);
    report(surfaces > 20000, "V: brushed sphere pixels", surfaces, 20000);
    report(eT < 2e-4, "V: vis-buffer frame t vs double replica (radians)", eT, 2e-4);
    report(eAlpha < 2e-3, "V: band-limited alpha_t', alpha_b' vs double replica (rel.)", eAlpha, 2e-3);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, warp = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--warp") warp = true;
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
        Report report;
        TestFrame tf(debugLayer, false, warpDevice.Get());
        run(tf, report);
        logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
        return report.failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
