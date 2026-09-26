// Decals at ray hits (R, FEATURES_GAME 5.2; RayTracing/HitDecals.hlsli, RayScene::recordDecals): the query structure
// (per-frame AABB BLAS/TLAS over the main view's decal frames, candidates by an inline point ray) and E's decalApplyHit,
// against a double-precision reference of Decal.hlsli's rules (box test, angle fade, soft edge, upper-layer blend in
// (priority, order) order), at ~5,000 floor points:
//   D0 red, D1 blue over it (same priority, created later), D2 green at opacity 0.5 turned 45 degrees (priority 1: its
//   AABB is larger than its box, so the exact box test is exercised), D3 blue attached to instance 1 (applies only to
//   points of that instance), D4 red tilted 70 degrees (angle fade). Points at four heights: on the floor, and at 0.5, 0.9
//   and 1.125 of the box half-depth (soft edge, outside). The camera is away from the origin (the frames are
//   camera-relative, the query boxes world space). Points within 1e-3 of a box face are left out (float rounding).
// Then frames without decals (a local light keeps the light-grid ring published): the slot that held the decal words
// is reused and must read "no decals".
//   unx_test_raytracing_decalhits [--no-debug-layer]
#include "../../Passes/Atmosphere/Tests/TestFrame.h"

#if __has_include("unx/decal/Decals.h")
#include "unx/decal/Decals.h"
#include "unx/render/Tracks.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace
{
constexpr uint32_t W = 320, H = 180;
constexpr float kFloor = -1.5f;

struct V3
{
    double x, y, z;
};
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double len(V3 a) { return std::sqrt(dot(a, a)); }

struct Mat
{
    V3 base;
    double roughness, metallic;
};
struct RefDecal
{
    V3 x, y, z, c;  // world space (instance-attached: the instance's translation added below)
    int priority;
    uint32_t order;
    double opacity;
    uint32_t material, instance;
};
struct Point
{
    float x, y, z;
    uint32_t instance;
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--no-debug-layer") debugLayer = false;
            else fail("unknown argument %s", a.c_str());
        }
        TestFrame tf(debugLayer);
        scene::Scene sc;
        sc.name = "decal hit test";
        scene::Camera cam;
        cam.name = "main";
        cam.position = { 7, 2.5f, 3 };
        cam.forward = normalize(float3{ -0.6f, -0.35f, -1 });
        sc.cameras.push_back(cam);
        const Mat mats[4] = { { { 0.5, 0.5, 0.5 }, 0.5, 0 }, { { 1, 0, 0 }, 0.2, 1 }, { { 0, 1, 0 }, 0.8, 0 }, { { 0, 0, 1 }, 0.3, 0.5 } };
        for (int k = 0; k < 4; ++k)
        {
            scene::Material m;
            m.name = "decal hit material " + std::to_string(k);
            m.baseColor = { (float)mats[k].base.x, (float)mats[k].base.y, (float)mats[k].base.z };
            m.roughness = (float)mats[k].roughness;
            m.metallic = (float)mats[k].metallic;
            sc.materials.push_back(m);
        }
        const V3 instance1{ 4, kFloor, -6 };  // instance 1's translation (identity rotation)
        {
            scene::Mesh mesh;
            mesh.name = "decal hit quad";
            for (float3 q : { float3{ -1, 0, -1 }, float3{ -1, 0, 1 }, float3{ 1, 0, 1 }, float3{ 1, 0, -1 } })
            {
                mesh.positions.push_back(q);
                mesh.normals.push_back({ 0, 1, 0 });
                mesh.uv0.push_back({ q.x, q.z });
            }
            mesh.indices = { 0, 1, 2, 0, 2, 3 };
            mesh.submeshes.push_back({ 0, 6, 0 });
            sc.meshes.push_back(mesh);
            scene::Instance a;
            a.transform.m[1][3] = -20;  // out of the way
            sc.instances.push_back(a);
            scene::Instance b;
            b.transform.m[0][3] = (float)instance1.x;
            b.transform.m[1][3] = (float)instance1.y;
            b.transform.m[2][3] = (float)instance1.z;
            sc.instances.push_back(b);
        }
        scene::Light light;  // keeps R's light-grid ring published every frame (section 2)
        light.type = scene::LightType::Point;
        light.position = { 0, 30, 0 };
        light.intensity = 1;
        light.range = 1;
        sc.lights.push_back(light);
        tf.setScene(sc);
        const ViewDesc view = ViewDesc::fromCamera(cam, W, H, float4x4{});
        tf.frame.mainView = view;
        tf.frame.mainView.prevViewProj = view.viewProj;
        decal::DecalSet& set = decal::decals(tf.trackState);

        const double pi = 3.14159265358979, c45 = std::cos(pi / 4), c70 = std::cos(70 * pi / 180), s70 = std::sin(70 * pi / 180);
        const std::vector<RefDecal> ref = {
            { { 1.0, 0, 0 }, { 0, 0, -0.8 }, { 0, 0.4, 0 }, { 0, kFloor, -5 }, 0, 0, 1.0, 1, decal::kNone },
            { { 0.5, 0, 0 }, { 0, 0, -0.5 }, { 0, 0.4, 0 }, { 0.6, kFloor, -5 }, 0, 1, 1.0, 3, decal::kNone },
            { { c45 * 0.7, 0, -c45 * 0.7 }, { -c45 * 0.5, 0, -c45 * 0.5 }, { 0, 0.4, 0 }, { -0.5, kFloor, -5.3 }, 1, 2, 0.5, 2, decal::kNone },
            { { 0.5, 0, 0 }, { 0, 0, -0.5 }, { 0, 0.4, 0 }, { 0, 0, 0 }, 0, 3, 1.0, 3, 1 },
            { { 0.8, 0, 0 }, { 0, s70 * 0.8, -c70 * 0.8 }, { 0, c70 * 0.4, s70 * 0.4 }, { 2.2, kFloor, -6.5 }, 2, 4, 1.0, 1, decal::kNone },
        };
        for (const RefDecal& r : ref)
        {
            decal::Decal d;
            const V3 cols[4] = { r.x, r.y, r.z, r.c };
            for (int col = 0; col < 4; ++col)
            {
                d.box.m[0][col] = (float)cols[col].x;
                d.box.m[1][col] = (float)cols[col].y;
                d.box.m[2][col] = (float)cols[col].z;
            }
            d.material = r.material;
            d.instance = r.instance;
            d.priority = r.priority;
            d.opacity = (float)r.opacity;
            d.fadeStartDegrees = 60;
            d.fadeEndDegrees = 80;
            d.edge = 0.25f;
            set.add(d);
        }

        // Floor points x in [-2, 5.2], z in [-7.4, -3.8], four heights, instance 1 on every other point.
        const double heights[4] = { 0, 0.2, 0.36, 0.45 };
        std::vector<Point> points;
        for (int iz = 0; iz < 60; ++iz)
            for (int ix = 0; ix < 80; ++ix)
            {
                const uint32_t k = (uint32_t)points.size();
                points.push_back({ (float)(-2 + 7.2 * (ix + 0.37) / 80), (float)(kFloor + heights[k % 4]), (float)(-7.4 + 3.6 * (iz + 0.61) / 60), (k / 4) % 2 });
            }
        const uint32_t count = (uint32_t)points.size();
        const float footprint = 0.01f;
        uint32_t footprintBits;
        std::memcpy(&footprintBits, &footprint, 4);

        auto runFrame = [&](bool probe) {
            std::shared_ptr<std::vector<uint8_t>> out;
            tf.run([&](FramePassContext& fc) {
                ViewResources v;
                v.view = tf.frame.mainView;
                v.frameConstants = fc.frameConstantsFor(v.view);
                v.depth = fc.graph.createTexture(TextureDesc{ "decal.hit.depth", W, H, 1, 1, DXGI_FORMAT_R32_FLOAT });
                const TextureRef depth = v.depth;
                const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
                const float floorY = kFloor;
                uint32_t floorBits;
                std::memcpy(&floorBits, &floorY, 4);
                ID3D12PipelineState* depthPso = fc.shaders.compute("Passes/Decal/Tests/DecalProbe.MODE0");  // E's analytic floor depth
                fc.graph.addPass("decal.hit.depth", QueueType::Graphics, [&](PassBuilder& b) { b.use(depth, Use::UavCompute); },
                                 [=](PassContext& c) {
                                     const uint32_t k[8] = { 0, c.uav(depth), 0, 0, floorBits, 0, 0, 0 };
                                     c.cmd->SetPipelineState(depthPso);
                                     c.bindFrameConstants(cb);
                                     c.computeConstants(k, 8);
                                     c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                                 });
                rt::RayScene& rays = rt::RayScene::get(fc);
                rays.record(fc);
                tracks::decals(fc, v);
                rays.recordDecals(fc, v);
                if (!probe) return;
                uint32_t scene[8];
                rays.rootConstants(scene);
                const BufferRef input = tf.uploadBuffer(fc, points.data(), points.size() * sizeof(Point), 0, "decal.hit.points");
                const BufferRef result = fc.graph.createBuffer(BufferDesc{ "decal.hit.result", (uint64_t)count * 32, 0 });
                ID3D12PipelineState* pso = fc.shaders.compute("RayTracing/Tests/DecalHitProbe");
                fc.graph.addPass("decal.hit.probe", QueueType::Compute,
                                 [&](PassBuilder& b) {
                                     b.use(input, Use::SrvCompute);
                                     b.use(result, Use::UavCompute);
                                     rays.declareDecals(b);
                                 },
                                 [=](PassContext& c) {
                                     uint32_t k[32] = {};
                                     k[0] = c.srv(input);
                                     k[1] = c.uav(result);
                                     k[2] = count;
                                     k[3] = footprintBits;
                                     std::memcpy(&k[24], scene, sizeof scene);
                                     c.cmd->SetPipelineState(pso);
                                     c.bindFrameConstants(cb);
                                     c.computeConstants(k, 32);
                                     c.cmd->Dispatch((count + 63) / 64, 1, 1);
                                 });
                out = tf.readbackBuffer(fc, result, (uint64_t)count * 32);
            });
            return out;
        };

        // ---- 1. every point vs the reference
        auto compare = [&](const std::vector<RefDecal>& live, const char* what) {
            const auto data = runFrame(true);
            const float* r = reinterpret_cast<const float*>(data->data());
            std::vector<size_t> order(live.size());
            for (size_t k = 0; k < order.size(); ++k) order[k] = k;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return std::make_pair(live[a].priority, live[a].order) < std::make_pair(live[b].priority, live[b].order); });
            const double cosStart = std::cos(60 * pi / 180), cosEnd = std::cos(80 * pi / 180);
            uint32_t compared = 0, covered = 0, ambiguous = 0, faded = 0;
            double worst = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                const Point& p = points[i];
                const V3 pos{ p.x, p.y, p.z };
                Mat m = mats[0];
                bool skip = false, any = false;
                for (size_t k : order)
                {
                    const RefDecal& d = live[k];
                    if (d.instance != decal::kNone && d.instance != p.instance) continue;
                    const V3 c = d.instance == decal::kNone ? d.c : d.c + instance1;
                    const V3 r0 = cross(d.y, d.z), r1 = cross(d.z, d.x), r2 = cross(d.x, d.y);
                    const double det = dot(d.x, r0);
                    const V3 q = pos - c;
                    const double u[3] = { dot(r0, q) / det, dot(r1, q) / det, dot(r2, q) / det };
                    double maxU = 0;
                    for (double v : u) maxU = std::max(maxU, std::abs(v));
                    if (std::abs(maxU - 1) < 1e-3) skip = true;
                    if (maxU > 1) continue;
                    const double cosA = dot(V3{ 0, 1, 0 }, d.z * (1 / len(d.z)));
                    const double fade = std::clamp((cosA - cosEnd) / (cosStart - cosEnd), 0.0, 1.0);
                    const double edge = std::clamp((1 - std::abs(u[2])) / 0.25, 0.0, 1.0);
                    const double a = std::clamp(d.opacity * fade * edge, 0.0, 1.0);
                    if (!(a > 0)) continue;
                    any = true;
                    faded += a < 1;
                    const Mat& dm = mats[d.material];
                    m.base = m.base + (dm.base - m.base) * a;
                    m.roughness += (dm.roughness - m.roughness) * a;
                    m.metallic += (dm.metallic - m.metallic) * a;
                }
                if (skip)
                {
                    ++ambiguous;
                    continue;
                }
                ++compared;
                covered += any;
                const float* o = r + 8 * i;
                const double e = std::max({ std::abs(o[0] - m.base.x), std::abs(o[1] - m.base.y), std::abs(o[2] - m.base.z), std::abs(o[3] - m.roughness),
                                            std::abs(o[4] - m.metallic) });
                worst = std::max(worst, e);
                S_CHECK(e <= 2e-4, "point %u (%.3f %.3f %.3f, instance %u): (%.4f %.4f %.4f r %.4f m %.4f) expected (%.4f %.4f %.4f r %.4f m %.4f)", i, p.x, p.y, p.z,
                        p.instance, o[0], o[1], o[2], o[3], o[4], m.base.x, m.base.y, m.base.z, m.roughness, m.metallic);
            }
            std::printf("%s: %u points compared (%u under decals, %u partial layers, %u at box faces left out), worst %.2e\n", what, compared, covered, faded,
                        ambiguous, worst);
            return covered;
        };
        S_CHECK(compare(ref, "decals at hits") > 500, "too few points under decals");

        // ---- 2. no decals: frames 1-3 without, frame 4 reuses frame 0's light-grid slot (kDescSlots = 4)
        set.clear();
        for (int f = 0; f < 3; ++f) runFrame(false);
        S_CHECK(compare({}, "no decals (reused slot)") == 0, "decal words left in the reused slot");

        const uint32_t errors = tf.device.drainDebugMessages();
        S_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("PASS\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
#else
#include <cstdio>
int main()
{
    std::printf("SKIP: no decal module in this build\n");
    return 0;
}
#endif
