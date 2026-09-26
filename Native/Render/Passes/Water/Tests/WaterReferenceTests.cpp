// Track W water surface shading, stage 1, against the CPU reference path tracer (render C's, with the smooth dielectric
// interface and Beer-Lambert medium of Reference/PathTracer/src/Dielectric.h). A closed pool whose floor emits (albedo 0)
// under a flat water layer (y = 0.4 over |x|, |z| <= 1, inside four black 0.6 m walls), black sky, no sun: every
// radiance term is then the one stage 1 models, (1 - F) exp(-sigma_a |PH|) L_floor / n^2 through the interface, and the
// mirror reflection sees only black walls and sky (0 in both). The reference render keeps surface order 0..1 (light that
// crossed the interface once; the floor's own Schlick reflection at f0 = 0 is a second-order term of the Standard
// model, not of the interface, and stage 1 does not model it).
//   1. the engine image (M's test frame with the water layer from an exact CPU ray cast, as WaterSurfaceTests) over 8 x 8
//      blocks whose 10 x 10 neighbourhood is all shaded water (status 1) and smooth in the engine image (max/min <= 1.05):
//      each block's mean equals the reference's per channel within 4.5 x the reference's block noise (from its two
//      independent halves, pooled over the blocks) + 5e-3 relative, over at least 200 blocks
//   2. the bias: the mean of (engine - reference) / reference over those blocks within 4 x its standard error + 2e-3
//   3. the emitting floor seen without water (no-water engine frame, pixels whose +-2 px neighbourhood sees only the floor)
//      equals Le x exposure (the shared radiometric scale)
// The reference image comes from the reference tool on the scene this test writes (Cache/WaterReference/water_pool.unxscene):
//   unx_reference render --scene <that file> --camera pool --res 960x540 --spp 64 --surface-order 0:1
// found in Cache/Reference/water_pool/ (or given with --reference <file.pfm>, its .halfA/.halfB beside it). Without it the
// test writes the scene, prints the command and fails (exit 3): a comparison is never skipped silently.
//   unx_test_water_waterreferencetests [--no-debug-layer] [--warp] [--reference <file.pfm>]
#if __has_include("unx/shading/ShadingSystem.h") && __has_include("unx/material/MaterialSystem.h")
#include "../../Material/Tests/MTestFrame.h"
#include "unx/water/WaterSurface.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

namespace
{
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

void pixelRay(const ViewDesc& v, double px, double py, V3& D)
{
    const auto& P = v.proj.m;
    const double ndcx = px / v.width * 2 - 1, ndcy = 1 - py / v.height * 2;
    const double vx = (ndcx + P[0][2] - P[0][3]) / P[0][0], vy = (ndcy + P[1][2] - P[1][3]) / P[1][1];
    D = { v.view.m[0][0] * vx + v.view.m[1][0] * vy - v.view.m[2][0], v.view.m[0][1] * vx + v.view.m[1][1] * vy - v.view.m[2][1],
          v.view.m[0][2] * vx + v.view.m[1][2] * vy - v.view.m[2][2] };
}
uint32_t addQuad(scene::Scene& s, float3 a, float3 b, float3 c, float3 d, float3 normal, uint32_t material)
{
    scene::Mesh m;
    m.name = "quad";
    m.positions = { a, b, c, d };
    m.normals.assign(4, normal);
    m.tangents.assign(4, { 1, 0, 0, 1 });
    m.uv0 = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    m.indices = { 0, 1, 2, 0, 2, 3 };
    m.submeshes.push_back({ 0, 6, material });
    s.meshes.push_back(std::move(m));
    return (uint32_t)s.meshes.size() - 1;
}
struct Pfm
{
    uint32_t width = 0, height = 0;
    std::vector<float> rgb;  // top row first
    const float* at(uint32_t x, uint32_t y) const { return &rgb[3 * ((size_t)y * width + x)]; }
};
Pfm readPfm(const std::filesystem::path& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) fail("W: cannot read %s", path.string().c_str());
    std::string magic;
    double scale = 0;
    Pfm p;
    f >> magic >> p.width >> p.height >> scale;
    f.get();
    if (magic != "PF" || scale >= 0) fail("W: %s is not a little-endian colour PFM", path.string().c_str());
    p.rgb.resize((size_t)p.width * p.height * 3);
    for (uint32_t row = 0; row < p.height; ++row)  // bottom row first on disk
        f.read(reinterpret_cast<char*>(&p.rgb[3 * (size_t)(p.height - 1 - row) * p.width]), (std::streamsize)p.width * 12);
    if (!f) fail("W: %s is truncated", path.string().c_str());
    return p;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, warp = false;
        std::filesystem::path referencePfm;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--warp") warp = true;
            if (std::string(argv[i]) == "--reference" && i + 1 < argc) referencePfm = argv[++i];
        }
        Report report;
        const uint32_t W = 960, H = 540;
        const double level = 0.4, half = 1.0, ior = 1.333, Le = 1000;
        const double sigma[3] = { 0.340, 0.0565, 0.00922 };

        scene::Scene s;
        s.name = "water_pool";
        scene::Material floor, wall, waterMaterial;
        floor.name = "emitting floor";
        floor.baseColor = { 0, 0, 0 };
        floor.specular = 0;
        floor.roughness = 1;
        floor.emissive = { (float)Le, (float)Le, (float)Le };
        wall.name = "black wall";
        wall.baseColor = { 0, 0, 0 };
        wall.specular = 0;
        wall.roughness = 1;
        wall.twoSided = true;
        waterMaterial.name = "water";
        waterMaterial.cls = scene::MaterialClass::Water;
        waterMaterial.roughness = 0.02f;
        waterMaterial.baseColor = { (float)std::exp(-sigma[0]), (float)std::exp(-sigma[1]), (float)std::exp(-sigma[2]) };  // T over 1 m
        waterMaterial.ior = (float)ior;
        s.materials = { floor, wall, waterMaterial };
        auto instance = [&](uint32_t mesh) {
            scene::Instance in;
            in.mesh = mesh;
            s.instances.push_back(in);
        };
        instance(addQuad(s, { -1, 0, 1 }, { 1, 0, 1 }, { 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, 0 }, 0));
        const float wh = 0.6f;
        for (const auto& q : { std::array<float3, 5>{ float3{ -1, 0, -1 }, float3{ 1, 0, -1 }, float3{ 1, wh, -1 }, float3{ -1, wh, -1 }, float3{ 0, 0, 1 } },
                               std::array<float3, 5>{ float3{ -1, 0, 1 }, float3{ -1, 0, -1 }, float3{ -1, wh, -1 }, float3{ -1, wh, 1 }, float3{ 1, 0, 0 } },
                               std::array<float3, 5>{ float3{ 1, 0, -1 }, float3{ 1, 0, 1 }, float3{ 1, wh, 1 }, float3{ 1, wh, -1 }, float3{ -1, 0, 0 } },
                               std::array<float3, 5>{ float3{ 1, 0, 1 }, float3{ -1, 0, 1 }, float3{ -1, wh, 1 }, float3{ 1, wh, 1 }, float3{ 0, 0, -1 } } })
            instance(addQuad(s, q[0], q[1], q[2], q[3], q[4], 1));
        // The water surface (front face up, out of the water): the reference's medium boundary; the engine draws it as
        // the water layer (below) and ignores the instance's raster (not rasterised).
        const float lv = (float)level;
        instance(addQuad(s, { -1, lv, 1 }, { 1, lv, 1 }, { 1, lv, -1 }, { -1, lv, -1 }, { 0, 1, 0 }, 2));
        const uint32_t waterInstance = (uint32_t)s.instances.size() - 1, waterIndex = 2;
        s.atmosphere.rayleighScattering = { 0, 0, 0 };
        s.atmosphere.mieScattering = { 0, 0, 0 };
        s.atmosphere.mieAbsorption = { 0, 0, 0 };
        s.atmosphere.ozoneAbsorption = { 0, 0, 0 };
        s.atmosphere.groundAlbedo = { 0, 0, 0 };
        s.sun.illuminance = 0;
        scene::Camera cam;
        cam.name = "pool";
        cam.position = { 0.3f, 2.4f, 1.8f };
        cam.forward = normalize(float3{ -0.3f, -2.4f, -1.9f });
        cam.ev100 = 10;
        s.cameras.push_back(cam);
        const std::filesystem::path sceneFile = std::filesystem::path(UNX_SOURCE_DIR) / "Cache" / "WaterReference" / "water_pool.unxscene";
        std::filesystem::create_directories(sceneFile.parent_path());
        scene::save(s, sceneFile);
        const double exposure = 1.0 / (1.2 * std::exp2(cam.ev100));

        if (referencePfm.empty())
        {
            const std::filesystem::path dir = std::filesystem::path(UNX_SOURCE_DIR) / "Cache" / "Reference" / "water_pool";
            const std::string hash = scene::contentHash(s).substr(0, 16);
            if (std::filesystem::exists(dir))
                for (const auto& e : std::filesystem::directory_iterator(dir))
                {
                    const std::string n = e.path().filename().string();
                    if (n.rfind("pool_960x540_64_" + hash, 0) == 0 && n.size() > 4 && n.substr(n.size() - 4) == ".pfm" && n.find(".half") == std::string::npos) referencePfm = e.path();
                }
        }
        if (referencePfm.empty())
        {
            logf("no reference image of this scene (content hash %s); render it with\n  unx_reference render --scene %s --camera pool --res 960x540 --spp 64 --surface-order 0:1\n",
                 scene::contentHash(s).substr(0, 16).c_str(), sceneFile.string().c_str());
            logf("FAILED: the reference comparison needs the reference image\n");
            return 3;
        }
        const std::string stem = referencePfm.string().substr(0, referencePfm.string().size() - 4);
        const Pfm ref = readPfm(referencePfm), refA = readPfm(stem + ".halfA.pfm"), refB = readPfm(stem + ".halfB.pfm");
        if (ref.width != W || ref.height != H) fail("W: the reference is %ux%u, the test renders %ux%u", ref.width, ref.height, W, H);
        logf("reference %s\n", referencePfm.string().c_str());

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
        tf.setScene(s, { waterInstance });
        const ViewDesc desc = ViewDesc::fromCamera(cam, W, H, float4x4{});
        const V3 camPos{ cam.position.x, cam.position.y, cam.position.z };
        const V3 fwd{ -desc.view.m[2][0], -desc.view.m[2][1], -desc.view.m[2][2] };
        // Exact cast against the floor and the walls (band A of the engine frame).
        auto cast = [&](V3 o, V3 dir) {
            double best = std::numeric_limits<double>::infinity();
            auto consider = [&](double t) { if (t > 1e-9 && t < best) best = t; };
            if (dir.y != 0)
            {
                const double t = -o.y / dir.y;
                const V3 p = o + dir * t;
                if (std::fabs(p.x) <= 1 && std::fabs(p.z) <= 1) consider(t);
            }
            for (double side : { -1.0, 1.0 })
            {
                if (dir.z != 0)
                {
                    const double t = (side - o.z) / dir.z;
                    const V3 p = o + dir * t;
                    if (std::fabs(p.x) <= 1 && p.y >= 0 && p.y <= wh) consider(t);
                }
                if (dir.x != 0)
                {
                    const double t = (side - o.x) / dir.x;
                    const V3 p = o + dir * t;
                    if (std::fabs(p.z) <= 1 && p.y >= 0 && p.y <= wh) consider(t);
                }
            }
            return best;
        };
        std::vector<uint32_t> wvis((size_t)W * H, 0xFFFFFFFFu);
        std::vector<float> wdepth((size_t)W * H, std::numeric_limits<float>::infinity());
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                V3 D;
                pixelRay(desc, x + 0.5, y + 0.5, D);
                if (D.y >= 0) continue;
                const V3 p = camPos + D * ((level - camPos.y) / D.y);
                if (std::fabs(p.x) > half || std::fabs(p.z) > half) continue;
                if (!(dot(p - camPos, fwd) < dot(D * cast(camPos, D), fwd))) continue;  // behind a wall: V's depth test
                const uint32_t tri = (p.x + half) + (p.z + half) <= 2 * half ? 0u : 1u;
                wvis[(size_t)y * W + x] = 0xC0000000u | tri;
                wdepth[(size_t)y * W + x] = (float)dot(p - camPos, fwd);
            }
        const float a[3] = { -1, lv, -1 }, b[3] = { 1, lv, -1 }, c[3] = { -1, lv, 1 }, d[3] = { 1, lv, 1 };
        std::vector<float> verts;
        for (const float* p : { a, c, b, b, c, d }) verts.insert(verts.end(), { p[0], p[1], p[2], 1, 0, 1, 0, 0 });

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

        // 3. the floor's radiometric scale (no water): the pool floor seen through no interface.
        {
            double worst = 0;
            uint32_t n = 0;
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                {
                    if (wvis[(size_t)y * W + x] == 0xFFFFFFFFu) continue;  // the pool interior
                    bool floorOnly = true;  // the pixel and its +-2 px neighbours see the floor away from the walls (no edge blend)
                    for (int dy = -2; dy <= 2 && floorOnly; ++dy)
                        for (int dx = -2; dx <= 2 && floorOnly; ++dx)
                        {
                            V3 D;
                            pixelRay(desc, x + 0.5 + dx, y + 0.5 + dy, D);
                            const V3 p = camPos + D * cast(camPos, D);
                            floorOnly = p.y <= 1e-4 && std::fabs(p.x) <= 0.98 && std::fabs(p.z) <= 0.98;
                        }
                    if (!floorOnly) continue;
                    const float4 g = texelOf<float4>(*base, W, x, y);
                    const double want = Le * exposure;
                    worst = std::max({ worst, std::fabs(g.x / want - 1), std::fabs(g.y / want - 1), std::fabs(g.z / want - 1) });
                    ++n;
                }
            report(n > 10000 && worst <= 2e-3, "3. emitting floor without water = Le x exposure (rel.)", worst, 2e-3);
        }

        // 1./2. blocks of shaded water, engine vs reference.
        const uint32_t pitch = TestFrame::rowPitch(W, 1);
        struct Block
        {
            double engine[3], refMean[3], relHalves[3];
        };
        std::vector<Block> blocks;
        for (uint32_t by = 1; by + 1 < H / 8; ++by)
            for (uint32_t bx = 1; bx + 1 < W / 8; ++bx)
            {
                bool usable = true;
                double lo = 1e30, hi = 0;
                for (uint32_t y = by * 8 - 1; y <= by * 8 + 8 && usable; ++y)
                    for (uint32_t x = bx * 8 - 1; x <= bx * 8 + 8 && usable; ++x)
                    {
                        usable = (*status)[(size_t)y * pitch + x] == 1;
                        const float g = texelOf<float4>(*with, W, x, y).y;
                        lo = std::min(lo, (double)g);
                        hi = std::max(hi, (double)g);
                    }
                if (!usable || !(lo > 0) || hi > 1.05 * lo) continue;
                Block blk{};
                double A[3] = {}, B[3] = {};
                for (uint32_t y = by * 8; y < by * 8 + 8; ++y)
                    for (uint32_t x = bx * 8; x < bx * 8 + 8; ++x)
                    {
                        const float4 g = texelOf<float4>(*with, W, x, y);
                        for (int k = 0; k < 3; ++k)
                        {
                            blk.engine[k] += (&g.x)[k] / 64.0;
                            blk.refMean[k] += ref.at(x, y)[k] / 64.0;
                            A[k] += refA.at(x, y)[k] / 64.0;
                            B[k] += refB.at(x, y)[k] / 64.0;
                        }
                    }
                for (int k = 0; k < 3; ++k) blk.relHalves[k] = (A[k] - B[k]) / (A[k] + B[k]);  // (half difference / 2) / mean
                blocks.push_back(blk);
            }
        // The reference's block noise per channel: the relative standard error of a block's mean, pooled over the blocks
        // (each block's two halves differ by 2 sigma_half; the mean of both has sigma_half / sqrt 2).
        double noise[3] = {}, bias[3] = {}, biasSe[3] = {}, worst[3] = {};
        for (int k = 0; k < 3; ++k)
        {
            double s2 = 0;
            for (const Block& q : blocks) s2 += q.relHalves[k] * q.relHalves[k];
            noise[k] = blocks.empty() ? 1 : std::sqrt(s2 / blocks.size());  // sigma_mean = sigma_half / sqrt 2 = E|A - B| / (2 mean) in rms
            std::vector<double> rel;
            for (const Block& q : blocks) rel.push_back(q.engine[k] / q.refMean[k] - 1);
            double m = 0, v = 0;
            for (double r : rel) m += r;
            m /= std::max<size_t>(rel.size(), 1);
            for (double r : rel) v += (r - m) * (r - m);
            v /= std::max<size_t>(rel.size() - 1, 1);
            bias[k] = m;
            biasSe[k] = std::sqrt(v / std::max<size_t>(rel.size(), 1));
            for (double r : rel) worst[k] = std::max(worst[k], std::fabs(r) / (4.5 * noise[k] + 5e-3));
        }
        logf("%zu blocks of shaded water; reference block noise (rel.) %.2e %.2e %.2e; bias (engine / reference - 1) %+.2e %+.2e %+.2e (s.e. %.1e %.1e %.1e)\n",
             blocks.size(), noise[0], noise[1], noise[2], bias[0], bias[1], bias[2], biasSe[0], biasSe[1], biasSe[2]);
        if (!blocks.empty())
        {
            const Block& q = blocks[blocks.size() / 2];
            logf("  e.g. block: engine %.5f %.5f %.5f, reference %.5f %.5f %.5f\n", q.engine[0], q.engine[1], q.engine[2], q.refMean[0], q.refMean[1], q.refMean[2]);
        }
        report(blocks.size() >= 200 && std::max({ worst[0], worst[1], worst[2] }) <= 1, "1. block means vs reference (worst |rel.| / (4.5 noise + 5e-3))",
               std::max({ worst[0], worst[1], worst[2] }), 1);
        double biasWorst = 0;
        for (int k = 0; k < 3; ++k) biasWorst = std::max(biasWorst, std::fabs(bias[k]) / (4 * biasSe[k] + 2e-3));
        report(blocks.size() >= 200 && biasWorst <= 1, "2. mean bias vs reference (worst |bias| / (4 s.e. + 2e-3))", biasWorst, 1);
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
    unx::logf("water reference tests skipped: they need the M and V tracks (build with -Tracks V;M;W)\n");
    return 0;
}
#endif
