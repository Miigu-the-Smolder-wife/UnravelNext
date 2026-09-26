// Track W water surface shading, stage 1 (FEATURES_GAME 1.9; WaterSurface.hlsli, WaterInterior.hlsl). M's test frame
// (stand-in V, M resolve and shading) renders a closed pool at 960 x 540 twice: without water, then with a flat water
// layer (y = 0.4 over |x|, |z| <= 1, inside four 0.6 m walls, a sphere under it, a checker floor) given to the shading
// track as V's water layer (waterVis / waterDepth from a CPU ray cast with V's depth test) and a layer-1 triangle
// stream. Needs the M and V tracks (build folder with -Tracks "V;M;W"); without them it only reports a skip.
//   Expected classification (exact CPU geometry, double): a pixel's refracted ray (exact Snell) is expected shaded when
//   every point of its path P -> H (every 0.25 px on screen) lies under the water layer and in front of band A at its
//   screen position and H is on screen - the rule WaterSurface.hlsli defines; a fallback otherwise. Boundary cases,
//   where the answer changes within 2 px of a path point or the path comes within 1e-3 of band A's depth (below what
//   band A's pixel depths resolve), may go either way.
//   1. expected-shaded pixels away from the base image's edges equal (1 - F) exp(-sigma_a |PH|) L(H) / n^2 + F x (the
//      sun's specular: none here), L(H) the no-water image at H's projection, F the exact unpolarised Fresnel
//      (relative 1e-2, over more than 5,000 pixels);
//   2. expected-shaded pixels are shaded samples and expected fallbacks are fallbacks (status image), boundary cases
//      excepted;
//   3. every pixel of an 8 x 8 tile without water-layer pixels is bit-identical to the no-water image (M's edge composite
//      blends edge pixels from their tile's representatives, which in water tiles now hold water);
//   4. the statistics equal the status image's counts.
//   unx_test_water_watersurfacetests [--no-debug-layer] [--warp]
#if __has_include("unx/shading/ShadingSystem.h") && __has_include("unx/material/MaterialSystem.h")
#include "../../Material/Tests/MTestFrame.h"
#include "unx/water/WaterSurface.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
#include <cstdio>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

namespace
{
constexpr double kPi = 3.14159265358979323846;
struct Report
{
    int failures = 0;
    void operator()(bool ok, const char* what, double value, double limit)
    {
        logf("%-78s %.3e (limit %.1e) %s\n", what, value, limit, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }
};
struct V3
{
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, double k) { return { a.x * k, a.y * k, a.z * k }; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) { return a * (1 / std::sqrt(dot(a, a))); }

void pixelRay(const ViewDesc& v, double px, double py, V3& D)
{
    const auto& P = v.proj.m;
    const double ndcx = px / v.width * 2 - 1, ndcy = 1 - py / v.height * 2;
    const double vx = (ndcx + P[0][2] - P[0][3]) / P[0][0], vy = (ndcy + P[1][2] - P[1][3]) / P[1][1];
    D = { v.view.m[0][0] * vx + v.view.m[1][0] * vy - v.view.m[2][0], v.view.m[0][1] * vx + v.view.m[1][1] * vy - v.view.m[2][1],
          v.view.m[0][2] * vx + v.view.m[1][2] * vy - v.view.m[2][2] };
}
// Screen position (pixels) and linear view depth of a world point.
V3 project(const ViewDesc& v, V3 w, V3 cam)
{
    const auto& M = v.viewProj.m;
    double c[4];
    for (int r = 0; r < 4; ++r) c[r] = M[r][0] * w.x + M[r][1] * w.y + M[r][2] * w.z + M[r][3];
    const V3 f{ -v.view.m[2][0], -v.view.m[2][1], -v.view.m[2][2] };
    return { (c[0] / c[3] * 0.5 + 0.5) * v.width, (0.5 - c[1] / c[3] * 0.5) * v.height, dot(w - cam, f) };
}
double fresnel(double cosI, double eta)
{
    cosI = std::clamp(cosI, 0.0, 1.0);
    const double s2 = eta * eta * (1 - cosI * cosI);
    if (s2 >= 1) return 1;
    const double ct = std::sqrt(1 - s2), rs = (eta * cosI - ct) / (eta * cosI + ct), rp = (eta * ct - cosI) / (eta * ct + cosI);
    return 0.5 * (rs * rs + rp * rp);
}
uint32_t addPlane(scene::Scene& s, float size, uint32_t material)
{
    scene::Mesh m;
    m.name = "tile";
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
// A wall quad in world coordinates (corners in order around it).
uint32_t addQuad(scene::Scene& s, float3 a, float3 b, float3 c, float3 d, float3 normal, uint32_t material)
{
    scene::Mesh m;
    m.name = "wall";
    m.positions = { a, b, c, d };
    m.normals.assign(4, normal);
    m.tangents.assign(4, { 1, 0, 0, 1 });
    m.uv0 = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    m.indices = { 0, 1, 2, 0, 2, 3 };
    m.submeshes.push_back({ 0, 6, material });
    s.meshes.push_back(std::move(m));
    return (uint32_t)s.meshes.size() - 1;
}
float4 bilinear(const std::vector<uint8_t>& img, uint32_t W, uint32_t H, double px, double py)
{
    const double u = px - 0.5, v = py - 0.5;
    const int x0 = (int)std::floor(u), y0 = (int)std::floor(v);
    const double fx = u - x0, fy = v - y0;
    auto at = [&](int x, int y) { return texelOf<float4>(img, W, (uint32_t)std::clamp(x, 0, (int)W - 1), (uint32_t)std::clamp(y, 0, (int)H - 1)); };
    const float4 a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    float4 r;
    for (int k = 0; k < 3; ++k)
    {
        const double top = (&a.x)[k] + ((&b.x)[k] - (&a.x)[k]) * fx, bottom = (&c.x)[k] + ((&d.x)[k] - (&c.x)[k]) * fx;
        (&r.x)[k] = (float)(top + (bottom - top) * fy);
    }
    r.w = 1;
    return r;
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
        Report report;
        ComPtr<ID3D12Device> software;
        if (warp)
        {
            ComPtr<IDXGIFactory4> factory;
            check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
            check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&software)), "WARP device");
        }
        TestFrame tf(debugLayer, false, software.Get());
        const uint32_t W = 960, H = 540;
        const double level = 0.4, half = 1.0, ior = 1.333;
        const double sigma[3] = { 0.340, 0.0565, 0.00922 };

        // A pool: a checker floor of 0.25 m tiles, a sphere under the water, a water material (Water class).
        scene::Scene s;
        s.name = "water surface";
        scene::Material light, dark, ball, waterMaterial, wall;
        light.name = "light";
        light.baseColor = { 0.8f, 0.78f, 0.7f };
        light.roughness = 0.8f;
        dark.name = "dark";
        dark.baseColor = { 0.1f, 0.25f, 0.45f };
        dark.roughness = 0.8f;
        ball.name = "ball";
        ball.baseColor = { 0.8f, 0.3f, 0.1f };
        ball.roughness = 0.5f;
        waterMaterial.name = "water";
        waterMaterial.cls = scene::MaterialClass::Water;
        waterMaterial.roughness = 0.02f;
        waterMaterial.ior = (float)ior;
        wall.name = "wall";
        wall.baseColor = { 0.55f, 0.45f, 0.35f };
        wall.roughness = 0.7f;
        wall.twoSided = true;
        s.materials = { light, dark, ball, waterMaterial, wall };
        const uint32_t tileLight = addPlane(s, 0.25f, 0), tileDark = addPlane(s, 0.25f, 1), sphere = addSphere(s, 0.15f, 32, 64, 2);
        for (int tz = -12; tz < 12; ++tz)
            for (int tx = -12; tx < 12; ++tx)
            {
                scene::Instance in;
                in.mesh = ((tx + tz) & 1) ? tileDark : tileLight;
                in.transform = float3x4::translation({ (tx + 0.5f) * 0.25f, 0, (tz + 0.5f) * 0.25f });
                s.instances.push_back(in);
            }
        const V3 sphereCentre{ 0.25, 0.15, -0.1 };
        {
            scene::Instance in;
            in.mesh = sphere;
            in.transform = float3x4::translation({ (float)sphereCentre.x, (float)sphereCentre.y, (float)sphereCentre.z });
            s.instances.push_back(in);
        }
        // The pool's four walls (0.6 m high, two-sided): a closed basin, so refracted rays end on its floor or walls.
        const float wh = 0.6f;
        for (const auto& q : { std::array<float3, 5>{ float3{ -1, 0, -1 }, float3{ 1, 0, -1 }, float3{ 1, wh, -1 }, float3{ -1, wh, -1 }, float3{ 0, 0, 1 } },
                               std::array<float3, 5>{ float3{ -1, 0, 1 }, float3{ -1, 0, -1 }, float3{ -1, wh, -1 }, float3{ -1, wh, 1 }, float3{ 1, 0, 0 } },
                               std::array<float3, 5>{ float3{ 1, 0, -1 }, float3{ 1, 0, 1 }, float3{ 1, wh, 1 }, float3{ 1, wh, -1 }, float3{ -1, 0, 0 } },
                               std::array<float3, 5>{ float3{ 1, 0, 1 }, float3{ -1, 0, 1 }, float3{ -1, wh, 1 }, float3{ 1, wh, 1 }, float3{ 0, 0, -1 } } })
        {
            scene::Instance in;
            in.mesh = addQuad(s, q[0], q[1], q[2], q[3], q[4], 4);
            s.instances.push_back(in);
        }
        s.sun.direction = normalize(float3{ 0.3f, 0.8f, 0.5f });  // behind the camera: its mirror lobe is off screen
        scene::Camera cam;
        cam.name = "pool";
        cam.position = { 0.3f, 2.4f, 1.8f };  // above the near wall, looking steeply into the pool
        cam.forward = normalize(float3{ -0.3f, -2.4f, -1.9f });
        cam.ev100 = 13;
        s.cameras.push_back(cam);
        tf.setScene(s);
        const uint32_t waterIndex = 3;
        const ViewDesc desc = ViewDesc::fromCamera(cam, W, H, float4x4{});
        const V3 camPos{ cam.position.x, cam.position.y, cam.position.z };
        const V3 fwd{ -desc.view.m[2][0], -desc.view.m[2][1], -desc.view.m[2][2] };

        // Exact scene ray cast (the floor, the sphere, the walls): the nearest hit parameter along dir (unnormalised).
        auto cast = [&](V3 o, V3 dir) {
            double best = std::numeric_limits<double>::infinity();
            auto consider = [&](double t) { if (t > 1e-9 && t < best) best = t; };
            if (dir.y != 0)
            {
                const double t = -o.y / dir.y;
                const V3 p = o + dir * t;
                if (std::fabs(p.x) <= 3 && std::fabs(p.z) <= 3) consider(t);
            }
            {
                const V3 oc = o - sphereCentre;
                const double a2 = dot(dir, dir), bq = dot(oc, dir), cq = dot(oc, oc) - 0.15 * 0.15, disc = bq * bq - a2 * cq;
                if (disc >= 0) consider((-bq - std::sqrt(disc)) / a2);
            }
            for (double side : { -1.0, 1.0 })
                if (dir.z != 0)
                {
                    const double t = (side - o.z) / dir.z;
                    const V3 p = o + dir * t;
                    if (std::fabs(p.x) <= 1 && p.y >= 0 && p.y <= wh) consider(t);
                }
            for (double side : { -1.0, 1.0 })
                if (dir.x != 0)
                {
                    const double t = (side - o.x) / dir.x;
                    const V3 p = o + dir * t;
                    if (std::fabs(p.z) <= 1 && p.y >= 0 && p.y <= wh) consider(t);
                }
            return best;
        };
        // Band A's linear depth at a screen position (the exact surface under that sub-pixel point).
        auto bandADepth = [&](double px, double py) {
            V3 D;
            pixelRay(desc, px, py, D);
            return dot(D * cast(camPos, D), fwd);
        };
        // The water layer at a screen position: its linear depth where the view ray meets the water square in front of
        // band A, else +inf.
        auto waterDepthAt = [&](double px, double py) {
            V3 D;
            pixelRay(desc, px, py, D);
            if (D.y >= 0) return std::numeric_limits<double>::infinity();
            const V3 p = camPos + D * ((level - camPos.y) / D.y);
            if (std::fabs(p.x) > half || std::fabs(p.z) > half) return std::numeric_limits<double>::infinity();
            const double zw = dot(p - camPos, fwd);
            return zw < bandADepth(px, py) ? zw : std::numeric_limits<double>::infinity();
        };

        // The water layer from a CPU ray cast: triangle 0 = (a, c, b), 1 = (b, c, d) over the square (CCW from above).
        std::vector<uint32_t> wvis((size_t)W * H, 0xFFFFFFFFu);
        std::vector<float> wdepth((size_t)W * H, std::numeric_limits<float>::infinity());
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                V3 D;
                pixelRay(desc, x + 0.5, y + 0.5, D);
                if (D.y >= 0) continue;
                const double u = (level - camPos.y) / D.y;
                const V3 p = camPos + D * u;
                if (std::fabs(p.x) > half || std::fabs(p.z) > half) continue;
                if (!(dot(p - camPos, fwd) < bandADepth(x + 0.5, y + 0.5))) continue;  // behind a wall: V's depth test
                const uint32_t tri = (p.x - (-half)) + (p.z - (-half)) <= 2 * half ? 0u : 1u;  // diagonal b-c: x + z = 0
                wvis[(size_t)y * W + x] = 0xC0000000u | (0u << 24) | tri;
                wdepth[(size_t)y * W + x] = (float)dot(p - camPos, fwd);
            }
        const float a[3] = { -1, (float)level, -1 }, b[3] = { 1, (float)level, -1 }, c[3] = { -1, (float)level, 1 }, d[3] = { 1, (float)level, 1 };
        std::vector<float> verts;
        for (const float* p : { a, c, b, b, c, d })
        {
            verts.insert(verts.end(), { p[0], p[1], p[2], 1, 0, 1, 0, 0 });
        }

        auto uploadTexture = [&](FramePassContext& fc, const char* name, DXGI_FORMAT format, uint32_t bytes, const void* data) {
            const TextureRef t = fc.graph.createTexture({ name, W, H, 1, 1, format });
            const uint32_t pitch = TestFrame::rowPitch(W, bytes);
            ComPtr<ID3D12Resource> staging = makeBuffer(tf.device, (uint64_t)pitch * H, D3D12_HEAP_TYPE_UPLOAD);
            uint8_t* p = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(staging->Map(0, &none, reinterpret_cast<void**>(&p)), "map");
            for (uint32_t y = 0; y < H; ++y) std::memcpy(p + (size_t)y * pitch, static_cast<const uint8_t*>(data) + (size_t)y * W * bytes, (size_t)W * bytes);
            staging->Unmap(0, nullptr);
            ID3D12Resource* src = staging.Get();
            tf.keep(staging);
            fc.graph.addPass("w.test.upload", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(t, Use::CopyDst); },
                             [=](PassContext& ctx) {
                                 D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                                 to.pResource = ctx.resource(t);
                                 to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                                 from.pResource = src;
                                 from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                                 from.PlacedFootprint.Footprint = { format, W, H, 1, pitch };
                                 ctx.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                             });
            return t;
        };
        auto uploadBuffer = [&](FramePassContext& fc, const char* name, const void* data, uint64_t bytes) {
            const BufferRef buf = fc.graph.createBuffer({ name, bytes, 0 });
            ComPtr<ID3D12Resource> staging = makeBuffer(tf.device, bytes, D3D12_HEAP_TYPE_UPLOAD);
            uint8_t* p = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(staging->Map(0, &none, reinterpret_cast<void**>(&p)), "map");
            std::memcpy(p, data, bytes);
            staging->Unmap(0, nullptr);
            ID3D12Resource* src = staging.Get();
            tf.keep(staging);
            fc.graph.addPass("w.test.upload buffer", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(buf, Use::CopyDst); },
                             [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(ctx.resource(buf), 0, src, 0, bytes); });
            return buf;
        };

        std::shared_ptr<std::vector<uint8_t>> base, with, status, march;
        tf.frame.outputLinearHdr = true;
        for (int pass = 0; pass < 2; ++pass)
            tf.run([&](FramePassContext& fc) {
                ViewResources v = tf.mainView(fc, W, H, 0);
                v.color = fc.graph.createTexture({ "w.test.color", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
                tf.vis.record(fc, v);
                if (pass == 1)
                {
                    v.waterVis = uploadTexture(fc, "w.test.water vis", DXGI_FORMAT_R32_UINT, 4, wvis.data());
                    v.waterDepth = uploadTexture(fc, "w.test.water depth", DXGI_FORMAT_R32_FLOAT, 4, wdepth.data());
                    TriangleStream stream;
                    stream.vertices = uploadBuffer(fc, "w.test.water vertices", verts.data(), verts.size() * 4);
                    stream.material = waterIndex;
                    stream.maxTriangles = 2;
                    stream.layer = 1;
                    fc.resources.triangleStreams.push_back(stream);
                    fc.state<water::WaterSurfaceDebug>("W.surface.debug").status = true;
                }
                tracks::materialResolve(fc, v);
                tracks::shading(fc, v);
                (pass == 0 ? base : with) = tf.readback(fc, v.color);
                if (pass == 1)
                {
                    status = tf.readback(fc, fc.state<water::WaterSurfaceDebug>("W.surface.debug").image);
                    march = tf.readback(fc, fc.state<water::WaterSurfaceDebug>("W.surface.debug").march);
                }
                fc.resources.triangleStreams.clear();
            });
        tf.frame.outputLinearHdr = false;
        const water::WaterSurfaceStats st = water::latestWaterSurfaceStats(tf.trackState);

        // CPU reference.
        uint32_t interior = 0, checked = 0, smoothSkipped = 0, insideButFallback = 0, insideTotal = 0, outsideButShaded = 0;
        uint32_t counts[8] = {};
        double worst = 0, worstOther = 0;
        uint32_t loggedOther = 0, loggedFallback = 0, loggedHit = 0, loggedOutside = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                const uint8_t sv = (*status)[(size_t)y * TestFrame::rowPitch(W, 1) + x];
                const float4 g = texelOf<float4>(*with, W, x, y), o = texelOf<float4>(*base, W, x, y);
                if (sv == 0)
                {
                    const double e = std::max({ (double)std::fabs(g.x - o.x), (double)std::fabs(g.y - o.y), (double)std::fabs(g.z - o.z) });
                    // M's own edge composite blends an edge pixel from representatives in its 8 x 8 tile: tiles with
                    // water-layer pixels may change legitimately (their edge groups now hold water); others may not.
                    bool waterTile = false;
                    for (uint32_t ty = y & ~7u; ty < std::min(H, (y & ~7u) + 8); ++ty)
                        for (uint32_t tx = x & ~7u; tx < std::min(W, (x & ~7u) + 8); ++tx) waterTile = waterTile || wvis[(size_t)ty * W + tx] != 0xFFFFFFFFu;
                    if (waterTile) continue;
                    if (e > 0 && loggedOther < 6)
                    {
                        ++loggedOther;
                        logf("  changed non-interior px (%u,%u): %.4f -> %.4f, water vis %08x\n", x, y, o.y, g.y, wvis[(size_t)y * W + x]);
                    }
                    worstOther = std::max(worstOther, e);
                    continue;
                }
                ++interior;
                ++counts[sv - 1];
                V3 D;
                pixelRay(desc, x + 0.5, y + 0.5, D);
                const V3 P = camPos + D * ((level - camPos.y) / D.y), view = norm(camPos - P);
                const double cosI = view.y, eta = 1 / ior, F = fresnel(cosI, eta);
                const double s2 = eta * eta * (1 - cosI * cosI);
                const V3 t = norm(view * -eta + V3{ 0, 1, 0 } * (eta * cosI - std::sqrt(1 - s2)));
                // The exact refracted hit and the expected classification. Along the ray's screen path (samples every
                // 0.25 px of the 3D segment P -> H), every point must lie under the water layer and in front of band A at
                // its screen position (the GPU's rule, from exact geometry), and H must be on screen. A condition that
                // changes within 2 px of a path point makes the pixel a boundary case (either answer is right).
                const double best = cast(P, t);
                const V3 Hp = P + t * best;
                const V3 h = project(desc, Hp, camPos);
                const V3 p0 = project(desc, P, camPos);
                auto pointOk = [&](double qx, double qy, double zX, bool last) {
                    if (qx < 0 || qy < 0 || qx >= W || qy >= H) return false;
                    if (!(waterDepthAt(qx, qy) <= zX * (1 + 1e-5))) return false;  // under the water layer
                    return last || bandADepth(qx, qy) > zX * (1 - 1e-6);           // in front of band A (H: on it)
                };
                // Walk the path in order: the first failing point decides a fallback; a point before it whose answer is
                // ambiguous (it changes within 2 px, or the ray is within 1e-3 of band A's depth: below what band A's
                // pixel depths resolve) makes the pixel a boundary case - the GPU may stop there either way.
                bool expectShaded = true, decisive = false, ambiguous = false;
                double clearance = 1e9;
                const int n = std::max(4, (int)std::ceil(std::hypot(h.x - p0.x, h.y - p0.y) * 4));
                const double offsets[4][2] = { { 2, 0 }, { -2, 0 }, { 0, 2 }, { 0, -2 } };
                for (int k = 1; k <= n && expectShaded; ++k)
                {
                    const V3 X = P + (Hp - P) * (double(k) / n);
                    const V3 q = project(desc, X, camPos);
                    const bool last = k == n;
                    const bool centreOk = pointOk(q.x, q.y, q.z, last);
                    int agree = 0;
                    for (const auto& off : offsets) agree += pointOk(q.x + off[0], q.y + off[1], q.z, last) == centreOk;
                    if (!last && q.x >= 0 && q.y >= 0 && q.x < W && q.y < H)
                    {
                        const double gap = std::fabs(bandADepth(q.x, q.y) - q.z) / q.z;
                        clearance = std::min(clearance, gap);
                        if (gap <= 1e-3) ambiguous = true;
                    }
                    if (!centreOk)
                    {
                        expectShaded = false;
                        decisive = agree == 4;
                    }
                    else if (agree != 4) ambiguous = true;
                }
                const bool robust = !ambiguous && (expectShaded || decisive);
                const bool pathInside = expectShaded, nearBoundary = !robust;
                const float4 mg = texelOf<float4>(*march, W, x, y);
                if (expectShaded && robust)
                {
                    ++insideTotal;
                    if (sv != 1)
                    {
                        ++insideButFallback;
                        if (loggedFallback < 12)
                        {
                            ++loggedFallback;
                            logf("  fallback px (%u,%u) status %u: GPU end (%.2f, %.2f) step %.0f value %.5g; CPU H px (%.2f, %.2f) z %.4f\n", x, y, sv - 1, mg.x, mg.y, mg.z, mg.w, h.x, h.y, h.z);
                        }
                    }
                    else if (loggedHit < 8 && std::hypot(mg.x - h.x, mg.y - h.y) > 0.5)
                    {
                        ++loggedHit;
                        logf("  hit px (%u,%u): GPU H (%.2f, %.2f) step %.0f band A z %.4f; CPU H (%.2f, %.2f) z %.4f\n", x, y, mg.x, mg.y, mg.z, mg.w, h.x, h.y, h.z);
                    }
                }
                if (!expectShaded && robust && sv == 1)
                {
                    ++outsideButShaded;
                    if (loggedOutside < 8)
                    {
                        ++loggedOutside;
                        // the CPU's view at the GPU's stopping point: band A there and the ray's depth there
                        double zRay = 0;
                        for (int k = 0; k <= 4000; ++k)
                        {
                            const V3 X = P + (Hp - P) * (k / 4000.0);
                            const V3 q = project(desc, X, camPos);
                            if (std::hypot(q.x - mg.x, q.y - mg.y) < 0.02) { zRay = q.z; break; }
                        }
                        logf("  shaded but expected fallback px (%u,%u): GPU H (%.2f, %.2f) step %.0f band A z (GPU) %.4f (CPU) %.4f, ray z %.4f; CPU H (%.2f, %.2f) z %.4f, clearance %.2e\n",
                             x, y, mg.x, mg.y, mg.z, mg.w, bandADepth(mg.x, mg.y), zRay, h.x, h.y, h.z, clearance);
                    }
                }
                if (sv != 1 || !pathInside || nearBoundary) continue;
                // smooth neighbourhood of H in the no-water image (3 x 3 within 2 %)
                const int hx = (int)std::floor(h.x), hy = (int)std::floor(h.y);
                double lo = 1e30, hi = 0;
                for (int dy = -1; dy <= 2; ++dy)
                    for (int dx = -1; dx <= 2; ++dx)
                    {
                        const float4 q = texelOf<float4>(*base, W, (uint32_t)std::clamp(hx + dx, 0, (int)W - 1), (uint32_t)std::clamp(hy + dy, 0, (int)H - 1));
                        lo = std::min(lo, (double)q.y);
                        hi = std::max(hi, (double)q.y);
                    }
                if (hi - lo > 0.02 * hi) { ++smoothSkipped; continue; }
                const float4 L = bilinear(*base, W, H, h.x, h.y);
                const double len = best * std::sqrt(dot(t, t));
                double want[3];
                for (int k = 0; k < 3; ++k) want[k] = (1 - F) * std::exp(-sigma[k] * len) * (&L.x)[k] / (ior * ior);
                const double scale = std::max({ want[0], want[1], want[2], 1e-4 });
                const double e = std::max({ std::fabs(g.x - want[0]), std::fabs(g.y - want[1]), std::fabs(g.z - want[2]) }) / scale;
                if (e > 1e-2 && e > worst)
                    logf("  water px (%u,%u): got (%.5f %.5f %.5f) want (%.5f %.5f %.5f), F %.4f, |PH| %.4f, H px (%.2f, %.2f)\n", x, y, g.x, g.y, g.z, want[0], want[1], want[2], F, len,
                         h.x, h.y);
                worst = std::max(worst, e);
                ++checked;
            }
        logf("water surface: %u interior pixels (%u shaded, %u off-screen, %u exit, %u occluded, %u inside); %u checked against the CPU, %u skipped at image edges\n",
             interior, counts[0], counts[1], counts[2], counts[3], counts[5], checked, smoothSkipped);
        logf("statistics: shaded %u, off-screen %u, exit %u, occluded %u, steps %u, inside %u, unlit %u\n", st.shaded, st.offscreen, st.exited, st.occluded, st.steps, st.inside,
             st.unlit);
        report(checked > 5000 && worst <= 1e-2, "1. interior water pixels vs (1 - F) exp(-sigma d) L(H) / n^2 (rel.)", worst, 1e-2);
        report(insideButFallback == 0, "2. rays that stay in the water to a visible H are shaded (pixels)", insideButFallback, 0);
        report(outsideButShaded == 0, "2. rays leaving the water or the screen or passing behind a surface: fallbacks", outsideButShaded, 0);
        report(worstOther == 0, "3. pixels of tiles without water are unchanged", worstOther, 0);
        const bool statsMatch = st.shaded == counts[0] && st.offscreen == counts[1] && st.exited == counts[2] && st.occluded == counts[3] && st.inside == counts[5];
        report(statsMatch && insideTotal > 0, "4. statistics equal the status image", statsMatch ? 0 : 1, 0);
        logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
        return report.failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
#else
#include "unx/core/Log.h"
int main()
{
    unx::logf("water surface tests skipped: they need the M and V tracks (build with -Tracks V;M;W)\n");
    return 0;
}
#endif
