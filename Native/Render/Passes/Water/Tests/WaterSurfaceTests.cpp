// Track W water surface shading, stage 1 (FEATURES_GAME 1.9; WaterSurface.hlsli, WaterInterior.hlsl). M's test frame
// (stand-in V, M resolve and shading) renders a pool twice: without water, then with a flat water layer (y = 0.4 over
// |x|, |z| <= 1) given to the shading track as V's water layer (waterVis / waterDepth from a CPU ray cast) and a layer-1
// triangle stream. Needs the M and V tracks (build folder with -Tracks "V;M;W"); without them it only reports a skip.
//   1. every interior water pixel whose refracted ray (exact Snell, double) meets the floor or the sphere under the
//      water inside the screen, away from the base image's edges, equals (1 - F) exp(-sigma_a |PH|) L(H) / n^2 + F x
//      (the sun's specular: none here, the sun's mirror lobe is off screen), with L(H) the no-water image at H's
//      projection, F the exact unpolarised Fresnel (relative 1e-2);
//   2. those pixels are shaded samples, not fallbacks (status image), except within 2 px of the paths the CPU marks as
//      leaving the water's screen region; pixels whose ray leaves the water region are fallbacks (exit or off-screen);
//   3. every pixel that is not an interior water pixel is bit-identical to the no-water image;
//   4. the statistics equal the status image's counts.
//   unx_test_water_watersurfacetests [--no-debug-layer] [--warp]
#if __has_include("unx/shading/ShadingSystem.h") && __has_include("unx/material/MaterialSystem.h")
#include "../../Material/Tests/MTestFrame.h"
#include "unx/water/WaterSurface.h"

#include <dxgi1_6.h>

#include <algorithm>
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
        const uint32_t W = 480, H = 270;
        const double level = 0.4, half = 1.0, ior = 1.333;
        const double sigma[3] = { 0.340, 0.0565, 0.00922 };

        // A pool: a checker floor of 0.25 m tiles, a sphere under the water, a water material (Water class).
        scene::Scene s;
        s.name = "water surface";
        scene::Material light, dark, ball, waterMaterial;
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
        s.materials = { light, dark, ball, waterMaterial };
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
        s.sun.direction = normalize(float3{ 0.3f, 0.8f, 0.5f });  // behind the camera: its mirror lobe is off screen
        scene::Camera cam;
        cam.name = "pool";
        cam.position = { 0.2f, 1.3f, 2.4f };
        cam.forward = normalize(float3{ -0.2f, -1.1f, -2.4f });
        cam.ev100 = 13;
        s.cameras.push_back(cam);
        tf.setScene(s);
        const uint32_t waterIndex = 3;
        const ViewDesc desc = ViewDesc::fromCamera(cam, W, H, float4x4{});
        const V3 camPos{ cam.position.x, cam.position.y, cam.position.z };
        const V3 fwd{ -desc.view.m[2][0], -desc.view.m[2][1], -desc.view.m[2][2] };

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

        std::shared_ptr<std::vector<uint8_t>> base, with, status;
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
                if (pass == 1) status = tf.readback(fc, fc.state<water::WaterSurfaceDebug>("W.surface.debug").image);
                fc.resources.triangleStreams.clear();
            });
        tf.frame.outputLinearHdr = false;
        const water::WaterSurfaceStats st = water::latestWaterSurfaceStats(tf.trackState);

        // CPU reference.
        uint32_t interior = 0, checked = 0, smoothSkipped = 0, insideButFallback = 0, insideTotal = 0, outsideButShaded = 0;
        uint32_t counts[8] = {};
        double worst = 0, worstOther = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                const uint8_t sv = (*status)[(size_t)y * TestFrame::rowPitch(W, 1) + x];
                const float4 g = texelOf<float4>(*with, W, x, y), o = texelOf<float4>(*base, W, x, y);
                if (sv == 0)
                {
                    worstOther = std::max({ worstOther, (double)std::fabs(g.x - o.x), (double)std::fabs(g.y - o.y), (double)std::fabs(g.z - o.z) });
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
                // exact hit: the sphere or the floor
                double best = (0 - P.y) / t.y;
                const V3 oc = P - sphereCentre;
                const double bq = dot(oc, t), cq = dot(oc, oc) - 0.15 * 0.15, disc = bq * bq - cq;
                if (disc >= 0 && -bq - std::sqrt(disc) > 0) best = std::min(best, -bq - std::sqrt(disc));
                const V3 Hp = P + t * best;
                const V3 h = project(desc, Hp, camPos);
                const bool inWater = std::fabs(Hp.x) <= half && std::fabs(Hp.z) <= half;
                const bool onScreen = h.x >= 0 && h.y >= 0 && h.x < W && h.y < H;
                // Is the whole screen path from P to H over this stream's water? (the GPU's inside test)
                bool pathInside = inWater && onScreen;
                double margin = 1e9;
                if (pathInside)
                {
                    const V3 p0 = project(desc, P, camPos);
                    const int n = (int)std::ceil(std::max(std::fabs(h.x - p0.x), std::fabs(h.y - p0.y))) * 4 + 1;
                    for (int k = 0; k <= n && pathInside; ++k)
                    {
                        const double f = double(k) / n;
                        const double qx = p0.x + (h.x - p0.x) * f, qy = p0.y + (h.y - p0.y) * f;
                        V3 Dq;
                        pixelRay(desc, qx, qy, Dq);
                        const V3 wp = camPos + Dq * ((level - camPos.y) / Dq.y);
                        margin = std::min(margin, std::min(half - std::fabs(wp.x), half - std::fabs(wp.z)) / std::max(1e-9, std::sqrt(dot(wp - camPos, wp - camPos))) *
                                                      (W / (2 * std::tan(0.5 * cam.verticalFov) * W / H)));
                        if (std::fabs(wp.x) > half || std::fabs(wp.z) > half) pathInside = false;
                    }
                }
                const bool nearBoundary = margin < 2.0;  // within 2 px of the water region's outline along the path
                if (pathInside && !nearBoundary)
                {
                    ++insideTotal;
                    if (sv != 1) ++insideButFallback;
                }
                if (!pathInside && !nearBoundary && sv == 1) ++outsideButShaded;
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
                const double len = best;
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
        report(insideButFallback == 0, "2. rays inside the water's screen region are shaded, not fallbacks (pixels)", insideButFallback, 0);
        report(outsideButShaded == 0, "2. rays leaving the water region are fallbacks (pixels)", outsideButShaded, 0);
        report(worstOther == 0, "3. pixels outside the interior are unchanged", worstOther, 0);
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
