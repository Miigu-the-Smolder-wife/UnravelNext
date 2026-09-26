// Track E decal correctness (A7; no GPU lock needed after the first run of new kernels). An analytic floor (y = -1.5)
// under four decals; every floor pixel's material after decalApply is compared with a double-precision reference of
// Decal.hlsli's rules (box test, angle fade 60 -> 80 degrees, soft edge, upper-layer blend in (priority, order) order):
//   D1 red (priority 0), D3 blue over it (priority 0, created later), D2 green at opacity 0.5 (priority 1, over both),
//   D4 tilted 70 degrees from the floor normal (angle fade 0.516). Pixels within 2e-4 of a box face (float rounding of
//   inside/outside) are left out. Any tile-list false negative shows as a pixel without its decal.
// Then 20 decals in one tile: the tile keeps 16 and the read-back statistics report the full tile.
//   unx_test_decal_decaltests [--no-debug-layer]
#include "../../Atmosphere/Tests/TestFrame.h"

#include "unx/decal/Decals.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <array>
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
    V3 x, y, z, c;
    int priority;
    uint32_t order;
    double opacity, cosStart, cosEnd, edge;
    Mat m;
};

decal::Decal toDecal(const RefDecal& r, uint32_t material)
{
    decal::Decal d;
    const V3 cols[4] = { r.x, r.y, r.z, r.c };
    for (int col = 0; col < 4; ++col)
    {
        d.box.m[0][col] = (float)cols[col].x;
        d.box.m[1][col] = (float)cols[col].y;
        d.box.m[2][col] = (float)cols[col].z;
    }
    d.material = material;
    d.priority = r.priority;
    d.opacity = (float)r.opacity;
    d.fadeStartDegrees = 60;
    d.fadeEndDegrees = 80;
    d.edge = (float)r.edge;
    return d;
}
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
        sc.name = "decal test";
        scene::Camera cam;
        cam.name = "main";
        cam.position = { 0, 0, 0 };
        cam.forward = normalize(float3{ 0, -0.5f, -1 });
        sc.cameras.push_back(cam);
        const Mat mats[4] = { { { 0.5, 0.5, 0.5 }, 0.5, 0 }, { { 1, 0, 0 }, 0.2, 1 }, { { 0, 1, 0 }, 0.8, 0 }, { { 0, 0, 1 }, 0.3, 0.5 } };
        for (int k = 0; k < 4; ++k)
        {
            scene::Material m;
            m.name = "decal material " + std::to_string(k);
            m.baseColor = { (float)mats[k].base.x, (float)mats[k].base.y, (float)mats[k].base.z };
            m.roughness = (float)mats[k].roughness;
            m.metallic = (float)mats[k].metallic;
            sc.materials.push_back(m);
        }
        tf.setScene(sc);
        const ViewDesc view = ViewDesc::fromCamera(cam, W, H, float4x4{});
        tf.frame.mainView = view;
        tf.frame.mainView.prevViewProj = view.viewProj;
        decal::DecalSet& set = decal::decals(tf.trackState);

        const double c60 = std::cos(60 * 3.14159265358979 / 180), c80 = std::cos(80 * 3.14159265358979 / 180), c70 = std::cos(70 * 3.14159265358979 / 180),
                     s70 = std::sin(70 * 3.14159265358979 / 180), c30 = std::cos(30 * 3.14159265358979 / 180), s30 = 0.5;
        std::vector<RefDecal> ref = {
            { { 1.2, 0, 0 }, { 0, 0, -0.8 }, { 0, 0.4, 0 }, { 0, kFloor, -5 }, 0, 0, 1.0, c60, c80, 0.25, mats[1] },
            { { c30 * 0.7, 0, -s30 * 0.7 }, { -s30 * 0.5, 0, -c30 * 0.5 }, { 0, 0.4, 0 }, { 0.6, kFloor, -5.4 }, 1, 1, 0.5, c60, c80, 0.25, mats[2] },
            { { 0.6, 0, 0 }, { 0, 0, -0.6 }, { 0, 0.4, 0 }, { -0.6, kFloor, -5.2 }, 0, 2, 1.0, c60, c80, 0.25, mats[3] },
            { { 0.8, 0, 0 }, { 0, s70 * 0.8, -c70 * 0.8 }, { 0, c70 * 0.8, s70 * 0.8 }, { 1.5, kFloor, -7 }, 2, 3, 1.0, c60, c80, 0.25, mats[3] },
        };
        const uint32_t materialOf[4] = { 1, 2, 3, 3 };
        for (size_t k = 0; k < ref.size(); ++k) set.add(toDecal(ref[k], materialOf[k]));

        auto runFrame = [&](bool probe) {
            std::shared_ptr<std::vector<uint8_t>> out;
            tf.run([&](FramePassContext& fc) {
                ViewResources v;
                v.view = tf.frame.mainView;
                v.frameConstants = fc.frameConstantsFor(v.view);
                v.depth = fc.graph.createTexture(TextureDesc{ "decal.test.depth", W, H, 1, 1, DXGI_FORMAT_R32_FLOAT });
                const TextureRef depth = v.depth;
                const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
                const float floorY = kFloor;
                uint32_t floorBits;
                std::memcpy(&floorBits, &floorY, 4);
                ID3D12PipelineState* depthPso = fc.shaders.compute("Passes/Decal/Tests/DecalProbe.MODE0");
                fc.graph.addPass("decal.test.depth", QueueType::Graphics, [&](PassBuilder& b) { b.use(depth, Use::UavCompute); },
                                 [=](PassContext& c) {
                                     const uint32_t k[8] = { 0, c.uav(depth), 0, 0, floorBits, 0, 0, 0 };
                                     c.cmd->SetPipelineState(depthPso);
                                     c.bindFrameConstants(cb);
                                     c.computeConstants(k, 8);
                                     c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                                 });
                tracks::decals(fc, v);
                if (!probe) return;
                S_CHECK(v.decalFrames.valid() && v.decalTiles.valid(), "no decal lists");
                const BufferRef result = fc.graph.createBuffer(BufferDesc{ "decal.test.result", (uint64_t)W * H * 32, 16 });
                const BufferRef frames = v.decalFrames, tiles = v.decalTiles;
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Decal/Tests/DecalProbe.MODE1");
                fc.graph.addPass("decal.test.apply", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     b.use(result, Use::UavCompute);
                                     b.use(frames, Use::SrvCompute);
                                     b.use(tiles, Use::SrvCompute);
                                 },
                                 [=](PassContext& c) {
                                     const uint32_t k[8] = { 1, c.uav(result), c.srv(tiles), c.srv(frames), floorBits, 0, 0, 0 };
                                     c.cmd->SetPipelineState(pso);
                                     c.bindFrameConstants(cb);
                                     c.computeConstants(k, 8);
                                     c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                                 });
                out = tf.readbackBuffer(fc, result, (uint64_t)W * H * 32);
            });
            return out;
        };

        // ---- 1. every floor pixel vs the reference
        {
            const auto data = runFrame(true);
            const float* r = reinterpret_cast<const float*>(data->data());
            // the probe's ray: invViewProj of the view, as in HLSL
            const float4x4& inv = view.invViewProj;
            std::vector<size_t> order(ref.size());
            for (size_t k = 0; k < order.size(); ++k) order[k] = k;
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return std::make_pair(ref[a].priority, ref[a].order) < std::make_pair(ref[b].priority, ref[b].order); });
            uint32_t compared = 0, covered = 0, ambiguous = 0;
            double worst = 0;
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                {
                    const double nx = (x + 0.5) / W * 2 - 1, ny = 1 - (y + 0.5) / H * 2;
                    double p[4];
                    for (int i = 0; i < 4; ++i) p[i] = inv.m[i][0] * nx + inv.m[i][1] * ny + inv.m[i][2] * 1 + inv.m[i][3];
                    const V3 ray = V3{ p[0] / p[3], p[1] / p[3], p[2] / p[3] } * (1.0 / view.nearPlane);
                    const float* o = r + 8 * (y * W + x);
                    if (!(ray.y < 0))
                    {
                        S_CHECK(o[3] == -1, "pixel (%u, %u): sky expected", x, y);
                        continue;
                    }
                    const V3 pos = ray * (kFloor / ray.y);
                    Mat m = mats[0];
                    bool skip = false, any = false;
                    for (size_t k : order)
                    {
                        const RefDecal& d = ref[k];
                        const V3 r0 = cross(d.y, d.z), r1 = cross(d.z, d.x), r2 = cross(d.x, d.y);
                        const double det = dot(d.x, r0);
                        const V3 q = pos - d.c;
                        const double u[3] = { dot(r0, q) / det, dot(r1, q) / det, dot(r2, q) / det };
                        double maxU = 0;
                        for (double v : u) maxU = std::max(maxU, std::abs(v));
                        if (std::abs(maxU - 1) < 2e-4) skip = true;
                        if (maxU > 1) continue;
                        const double cosA = dot(V3{ 0, 1, 0 }, d.z * (1 / len(d.z)));
                        const double fade = std::clamp((cosA - d.cosEnd) / (d.cosStart - d.cosEnd), 0.0, 1.0);
                        const double edge = std::clamp((1 - std::abs(u[2])) / d.edge, 0.0, 1.0);
                        const double a = std::clamp(d.opacity * fade * edge, 0.0, 1.0);
                        if (!(a > 0)) continue;
                        any = true;
                        m.base = m.base + (d.m.base - m.base) * a;
                        m.roughness += (d.m.roughness - m.roughness) * a;
                        m.metallic += (d.m.metallic - m.metallic) * a;
                    }
                    if (skip)
                    {
                        ++ambiguous;
                        continue;
                    }
                    ++compared;
                    covered += any;
                    const double e = std::max({ std::abs(o[0] - m.base.x), std::abs(o[1] - m.base.y), std::abs(o[2] - m.base.z), std::abs(o[3] - m.roughness),
                                                std::abs(o[7] - m.metallic), std::abs(o[5] - 1.0) });
                    worst = std::max(worst, e);
                    S_CHECK(e <= 2e-4, "pixel (%u, %u): (%.4f %.4f %.4f r %.4f m %.4f) expected (%.4f %.4f %.4f r %.4f m %.4f)", x, y, o[0], o[1], o[2], o[3], o[7], m.base.x,
                            m.base.y, m.base.z, m.roughness, m.metallic);
                }
            S_CHECK(covered > 1000, "only %u floor pixels under decals", covered);
            std::printf("decals: %u floor pixels compared (%u under decals, %u at box faces left out), worst %.2e\n", compared, covered, ambiguous, worst);
        }

        // ---- 2. 20 decals in one tile: 16 kept, the full tile reported (statistics read back two frames later)
        {
            set.clear();
            for (int k = 0; k < 20; ++k)
            {
                RefDecal d{ { 0.05, 0, 0 }, { 0, 0, -0.05 }, { 0, 0.1, 0 }, { 0.001 * k, kFloor, -5 }, 0, 0, 1.0, c60, c80, 0.25, mats[1] };
                set.add(toDecal(d, 1));
            }
            runFrame(false);
            const uint64_t frame = tf.frame.frameIndex - 1;
            set.clear();
            for (int k = 0; k < 2; ++k) runFrame(false);
            const decal::Stats st = decal::lastStats(tf.trackState);
            S_CHECK(st.frame == frame && st.decals == 20 && st.fullTiles >= 1, "overflow stats: frame %llu (want %llu), %u decals, %u full tiles", (unsigned long long)st.frame,
                    (unsigned long long)frame, st.decals, st.fullTiles);
            std::printf("overflow: 20 decals in one tile, %u full tile(s) reported\n", st.fullTiles);
        }
        const uint32_t errors = tf.device.drainDebugMessages();
        S_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("decal tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
