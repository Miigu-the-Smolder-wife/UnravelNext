// Track E volume correctness (no GPU lock needed after the first run of new kernels): VolumeCommon.hlsli's math and the
// Volume pass's lists, froxel slices and haze field against double-precision references.
//   1. probe: the tent-mass line integral (exact, piecewise cubic) and the haze gradient (blob closed form, shell Gauss-
//      Legendre 16) for random queries vs fine composite quadrature (and a central difference of the integrated index);
//   2. media: given records (VolumePass::recordRecords) -> froxel tile lists -> volumeSlices, every tile and slice vs the
//      reference sum of the records' exact segment integrals (half-float storage: |d| <= 2e-3 |ref| + 1e-4); one record
//      crosses the near plane (whole-view binning);
//   3. haze: blob and shell records -> 1/4-resolution deflection field and front depth, every texel vs the reference bent
//      ray (|dD| <= 2e-3 |D| + 2e-3 px); a strong record sets VOLUME_STATUS_HAZE_LARGE (|D| >= 8 px reported, not clamped).
//   unx_test_volume_volumetests [--no-debug-layer]
#include "../../Atmosphere/Tests/TestFrame.h"

#include "../../FX/Tests/RppStream.h"
#include "unx/volume/VolumePass.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <random>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace
{
struct D3
{
    double x, y, z;
};
D3 operator+(D3 a, D3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
D3 operator-(D3 a, D3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
D3 operator*(D3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
double len(D3 a) { return std::sqrt(dot(a, a)); }
D3 norm(D3 a) { return a * (1.0 / len(a)); }
D3 d3(float3 v) { return { v.x, v.y, v.z }; }

struct Record  // VolumeCommon.hlsli VolumeRecord
{
    float centre[3], radius, a[3], mass, b[3];
    uint32_t kind;
};
static_assert(sizeof(Record) == 48);

double tentFactor(D3 d, double t, D3 c, double r)
{
    const D3 x = d * t - c;
    const double f = std::max(0.0, 1 - std::abs(x.x) / r) * std::max(0.0, 1 - std::abs(x.y) / r) * std::max(0.0, 1 - std::abs(x.z) / r);
    return f;
}
double tentLineRef(D3 d, double a, double b, D3 c, double r)
{
    if (!(b > a)) return 0;
    const int n = 20000;
    const double h = (b - a) / n;
    double s = 0;
    for (int i = 0; i < n; ++i) s += tentFactor(d, a + (i + 0.5) * h, c, r);
    return s * h;
}
// Integrated index along a line at impact parameter b.
double hazeN(double b, double radius, double A, double width, int profile)
{
    if (profile == 0) return A * radius * std::sqrt(3.14159265358979323846) * std::exp(-b * b / (radius * radius));
    const double R = radius, w = width * R, L = std::sqrt(std::max((R + 8 * w) * (R + 8 * w) - b * b, 0.0));
    if (L <= 0) return 0;
    const int n = 40000;
    const double h = 2 * L / n;
    double s = 0;
    for (int i = 0; i <= n; ++i)
    {
        const double l = -L + i * h, r = std::sqrt(b * b + l * l), u = (r - R) / w;
        const double f = A * std::exp(-u * u);
        s += (i == 0 || i == n ? 1 : (i % 2 ? 4 : 2)) * f;
    }
    return s * h / 3;
}
double hazeGradientRef(double b, double radius, double A, double width, int profile)
{
    if (profile == 0) return -2 * A * std::sqrt(3.14159265358979323846) * (b / radius) * std::exp(-b * b / (radius * radius));
    const double h = 1e-4 * radius;
    return (hazeN(b + h, radius, A, width, profile) - hazeN(std::max(b - h, 0.0), radius, A, width, profile)) / (b + h - std::max(b - h, 0.0));
}
double hazeSupport(double radius, double width, int profile) { return profile == 0 ? 3 * radius : radius * (1 + 4 * width); }

// Row-major matrix x column vector (the shaders' mul(M, v)).
std::array<double, 4> mulV(const float4x4& m, std::array<double, 4> v)
{
    std::array<double, 4> r{};
    for (int i = 0; i < 4; ++i) r[i] = m.m[i][0] * v[0] + m.m[i][1] * v[1] + m.m[i][2] * v[2] + m.m[i][3] * v[3];
    return r;
}
// Camera-relative ray through a full-resolution pixel at unit view depth (VolumeCommon.hlsli volumeRayAt).
D3 rayAt(const ViewDesc& v, double px, double py)
{
    const double nx = px / v.width * 2 - 1, ny = 1 - py / v.height * 2;
    const auto p = mulV(v.invViewProj, { nx, ny, 1, 1 });
    const D3 world{ p[0] / p[3], p[1] / p[3], p[2] / p[3] };
    return (world - d3(v.position)) * (1.0 / v.nearPlane);
}
// Pixel of a camera-relative direction (volumePixelOf).
std::array<double, 2> pixelOf(const ViewDesc& v, D3 dir)
{
    const D3 p = d3(v.position) + dir;
    const auto c = mulV(v.viewProj, { p.x, p.y, p.z, 1 });
    return { (c[0] / c[3] + 1) * 0.5 * v.width, (1 - c[1] / c[3]) * 0.5 * v.height };
}
double viewDepth(const ViewDesc& v, D3 rel)
{
    const auto p = mulV(v.view, { rel.x, rel.y, rel.z, 0 });
    return -p[2];
}
float half(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float f;
    if (e == 0) f = std::ldexp((float)m, -24);
    else if (e == 31) f = m ? NAN : INFINITY;
    else f = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -f : f;
}

// FroxelGrid header + empty lists (FroxelCommon.hlsli): what S's lists hold for a view without local lights.
std::vector<uint8_t> froxelHeader(uint32_t gridX, uint32_t gridY, uint32_t slices, uint32_t tilePx, float nearM, float farM)
{
    const uint32_t froxels = gridX * gridY * slices;
    std::vector<uint8_t> b(64 + 4 * (size_t)froxels + 64, 0);
    uint32_t u[16] = {};
    u[0] = gridX; u[1] = gridY; u[2] = slices; u[3] = tilePx;
    const float f[4] = { nearM, farM, std::log2(farM / nearM), 0 };
    std::memcpy(&u[4], f, 16);
    u[8] = 64; u[9] = 64 + 4 * froxels; u[10] = 0; u[11] = 0;
    std::memcpy(b.data(), u, 64);
    return b;
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
        std::printf("stage device\n");
        std::fflush(stdout);
        TestFrame tf(debugLayer);
        scene::Scene sc;
        sc.name = "volume test";
        scene::Camera cam;
        cam.name = "main";
        cam.position = { 0.25f, 1.7f, -0.5f };
        cam.forward = normalize(float3{ 0.05f, -0.02f, 1.0f });
        sc.cameras.push_back(cam);
        tf.setScene(sc);
        const uint32_t W = 1280, H = 720;
        const ViewDesc view = ViewDesc::fromCamera(cam, W, H, float4x4{});
        tf.frame.mainView = view;
        tf.frame.mainView.prevViewProj = view.viewProj;
        std::mt19937 rng(20260926);
        std::uniform_real_distribution<double> U(0, 1);

        // ---- 1. probe
        std::printf("stage probe\n");
        std::fflush(stdout);
        {
            const uint32_t n = 256;
            std::vector<float> q(16 * n);
            std::vector<std::array<double, 2>> ref(n);
            for (uint32_t i = 0; i < n; ++i)
            {
                const D3 d = norm({ U(rng) - 0.5, U(rng) - 0.5, 0.6 + U(rng) });
                const double r = 0.2 + 2.0 * U(rng), t = 3 + 20 * U(rng);
                const D3 c = d * t + D3{ (U(rng) - 0.5) * 1.5 * r, (U(rng) - 0.5) * 1.5 * r, (U(rng) - 0.5) * 1.5 * r };
                const double a = t - 2 * r * U(rng), b = t + 2 * r * U(rng);
                const int profile = i % 2;
                const double radius = 0.3 + 3 * U(rng), width = 0.05 + 0.3 * U(rng), A = (profile ? 5e-4 : -1.4e-4) * (0.5 + U(rng));
                const double hb = hazeSupport(radius, width, profile) * U(rng);
                const float row[16] = { (float)d.x, (float)d.y, (float)d.z, (float)a, (float)c.x, (float)c.y, (float)c.z, (float)b,
                                        (float)r, (float)hb, (float)radius, 0, (float)A, (float)width, (float)profile, 0 };
                std::memcpy(&q[16 * i], row, sizeof row);
                // references from the float inputs the kernel sees
                ref[i][0] = tentLineRef({ row[0], row[1], row[2] }, row[3], row[7], { row[4], row[5], row[6] }, row[8]);
                ref[i][1] = hazeGradientRef(row[9], row[10], row[12], row[13], profile);
            }
            std::shared_ptr<std::vector<uint8_t>> out;
            tf.run([&](FramePassContext& fc) {
                const BufferRef qb = tf.uploadBuffer(fc, q.data(), q.size() * 4, 16, "volume.test.queries");
                const BufferRef ob = fc.graph.createBuffer(BufferDesc{ "volume.test.results", (uint64_t)n * 8, 8 });
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Volume/Tests/VolumeProbe");
                fc.graph.addPass("volume.test.probe", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     b.use(qb, Use::SrvCompute);
                                     b.use(ob, Use::UavCompute);
                                 },
                                 [=](PassContext& c) {
                                     const std::array<uint32_t, 8> p = { c.srv(qb), c.uav(ob), n, 0, 0, 0, 0, 0 };
                                     c.cmd->SetPipelineState(pso);
                                     c.computeConstants(p.data(), 8);
                                     c.cmd->Dispatch((n + 63) / 64, 1, 1);
                                 });
                out = tf.readbackBuffer(fc, ob, (uint64_t)n * 8);
            });
            const float* r = reinterpret_cast<const float*>(out->data());
            double worstLine = 0, worstGrad = 0;
            for (uint32_t i = 0; i < n; ++i)
            {
                const double e0 = std::abs(r[2 * i] - ref[i][0]) / (std::abs(ref[i][0]) + 1e-5);
                const double amplitude = std::abs(q[16 * i + 12]);  // gradient scale: |A| (the profile's peak index)
                const double e1 = std::abs(r[2 * i + 1] - ref[i][1]) / (std::abs(ref[i][1]) + 5e-3 * amplitude);
                worstLine = std::max(worstLine, e0);
                worstGrad = std::max(worstGrad, e1);
                S_CHECK(e0 <= 1e-4, "probe %u: tent line %.8g vs reference %.8g", i, r[2 * i], ref[i][0]);
                S_CHECK(e1 <= 2e-3, "probe %u (profile %u): haze gradient %.8g vs reference %.8g", i, i % 2, r[2 * i + 1], ref[i][1]);
            }
            std::printf("probe: %u queries, tent line worst rel %.2e, haze gradient worst rel %.2e\n", n, worstLine, worstGrad);
        }

        // ---- 2. media slices
        std::printf("stage media\n");
        std::fflush(stdout);
        {
            uint32_t gridX, gridY, slices, tilePx;
            volume::froxelGridSize(tf.quality, W, H, gridX, gridY, slices, tilePx);
            const float nearM = (float)tf.quality.number("atmosphere.froxels.near_m"), farM = (float)tf.quality.number("atmosphere.froxels.far_m");
            std::vector<Record> recs;
            const D3 camPos = d3(view.position), fwd = norm(d3(cam.forward));
            auto media = [&](D3 rel, float r, float m, std::array<float, 3> a, std::array<float, 3> b) {
                Record x{};
                x.centre[0] = (float)rel.x; x.centre[1] = (float)rel.y; x.centre[2] = (float)rel.z;
                x.radius = r; x.mass = m;
                for (int k = 0; k < 3; ++k) { x.a[k] = a[k]; x.b[k] = b[k]; }
                x.kind = 1;
                recs.push_back(x);
            };
            media(fwd * 6 + D3{ 0.4, 0.1, 0 }, 1.2f, 2.0f, { 0.8f, 0.9f, 1.0f }, { 120, 100, 80 });
            media(fwd * 7 + D3{ -0.3, -0.2, 0.5 }, 0.9f, 1.0f, { 0.2f, 0.2f, 0.2f }, { 40, 60, 90 });
            media(fwd * 18 + D3{ 2.0, 1.0, 0 }, 3.0f, 6.0f, { 0.5f, 0.5f, 0.5f }, { 10, 10, 10 });
            media(fwd * 35 + D3{ -4.0, 0.5, 0 }, 0.4f, 0.3f, { 1.0f, 0.5f, 0.2f }, { 500, 200, 50 });
            media(fwd * 0.6 + D3{ 0.0, 0.0, 0 }, 1.0f, 0.5f, { 0.3f, 0.3f, 0.3f }, { 5, 5, 5 });  // reaches the near plane
            Record none{};
            recs.push_back(none);  // an empty slot (kind 0)
            std::shared_ptr<std::vector<uint8_t>> slicesTex, counters;
            uint32_t pitch = 0;
            tf.run([&](FramePassContext& fc) {
                const std::vector<uint8_t> header = froxelHeader(gridX, gridY, slices, tilePx, nearM, farM);
                const BufferRef lists = tf.uploadBuffer(fc, header.data(), header.size(), 0, "volume.test.froxels");
                const BufferRef rb = tf.uploadBuffer(fc, recs.data(), recs.size() * sizeof(Record), sizeof(Record), "volume.test.records");
                auto& pass = fc.trackState->get<std::unique_ptr<volume::VolumePass>>("volume.test.pass");
                if (!pass) pass = std::make_unique<volume::VolumePass>(fc.device);
                volume::VolumeFrame f;
                f.view = &tf.frame.mainView;
                f.frameConstants = fc.frameConstantsFor(tf.frame.mainView);
                f.froxelLights = lists;
                f.media = true;
                const volume::VolumeOutput o = pass->recordRecords(rb, (uint32_t)recs.size(), fc.graph, fc.shaders, fc.quality, f);
                S_CHECK(o.valid && o.volumeSlices.valid(), "media pass declared no slices");
                slicesTex = tf.readback(fc, o.volumeSlices);
                counters = tf.readbackBuffer(fc, o.counters, 32);
            });
            const uint32_t* cnt = reinterpret_cast<const uint32_t*>(counters->data());
            S_CHECK(cnt[2] == 0, "media status 0x%x", cnt[2]);
            S_CHECK(cnt[3] == (uint32_t)recs.size() - 1, "records binned %u", cnt[3]);
            pitch = TestFrame::rowPitch(gridX, 8);
            const size_t slicePitch = (size_t)pitch * gridY;
            auto texel = [&](uint32_t x, uint32_t y, uint32_t z, int k) {
                const uint16_t* p = reinterpret_cast<const uint16_t*>(slicesTex->data() + z * slicePitch + (size_t)y * pitch + (size_t)x * 8);
                return half(p[k]);
            };
            auto node = [&](uint32_t n) { return n == 0 ? 0.0 : nearM * std::pow((double)farM / nearM, (double)n / slices); };
            double worst = 0, maxTau = 0;
            uint32_t nonzero = 0, mismatches = 0;
            for (uint32_t ty = 0; ty < gridY; ++ty)
                for (uint32_t tx = 0; tx < gridX; ++tx)
                {
                    const D3 ray = rayAt(view, (tx + 0.5) * tilePx, (ty + 0.5) * tilePx);
                    const double speed = len(ray);
                    for (uint32_t s = 0; s < slices; ++s)
                    {
                        double tau[3] = {}, src[3] = {};
                        for (const Record& r : recs)
                        {
                            if (r.kind != 1) continue;
                            const D3 c{ r.centre[0], r.centre[1], r.centre[2] };
                            const double zc = viewDepth(view, c), R = r.radius * std::sqrt(3.0);
                            const double z0 = std::max({ node(s), zc - R, (double)view.nearPlane }), z1 = std::min(node(s + 1), zc + R);
                            if (!(z1 > z0)) continue;
                            const double A = speed * r.mass / ((double)r.radius * r.radius * r.radius) * tentLineRef(ray, z0, z1, c, r.radius);
                            for (int k = 0; k < 3; ++k) { tau[k] += A * r.a[k]; src[k] += A * r.b[k]; }
                        }
                        for (int k = 0; k < 3; ++k)
                        {
                            const double g = tau[k] > 1e-6 ? (1 - std::exp(-tau[k])) / tau[k] : 1 - 0.5 * tau[k];
                            const double sref = src[k] * g;
                            const double gt = texel(tx, ty, s, k), gs = texel(tx, ty, slices + s, k);
                            const double et = std::abs(gt - tau[k]), es = std::abs(gs - sref);
                            if (!(et <= 2e-3 * tau[k] + 1e-4) || !(es <= 2e-3 * sref + 1e-3))
                            {
                                if (++mismatches <= 12)
                                    std::printf("  mismatch tile (%u, %u) slice %u [%d]: tau %.6g vs %.6g, source %.6g vs %.6g\n", tx, ty, s, k, gt, tau[k], gs, sref);
                            }
                            worst = std::max(worst, et / (tau[k] + 1e-4));
                            maxTau = std::max(maxTau, tau[k]);
                            if (tau[k] > 1e-4 && k == 0) ++nonzero;
                        }
                    }
                }
            S_CHECK(mismatches == 0, "%u slice values differ from the reference", mismatches);
            S_CHECK(nonzero > 100, "only %u froxels hold media", nonzero);
            std::printf("media: %u x %u x %u froxels, %u with media, max slice tau %.3f, worst tau rel %.2e\n", gridX, gridY, slices, nonzero, maxTau, worst);
        }

        // ---- 3. haze
        for (int strong = 0; strong < 2; ++strong)
        {
            std::vector<Record> recs;
            const D3 fwd = norm(d3(cam.forward));
            auto haze = [&](D3 rel, float radius, float A, float width, int profile) {
                Record x{};
                x.centre[0] = (float)rel.x; x.centre[1] = (float)rel.y; x.centre[2] = (float)rel.z;
                x.radius = radius; x.a[0] = A; x.a[1] = width; x.a[2] = (float)profile;
                x.mass = (float)hazeSupport(radius, width, profile);
                x.kind = 2;
                recs.push_back(x);
            };
            haze(fwd * 6 + D3{ 0.3, 0.2, 0 }, 0.8f, -1.4e-4f, 0, 0);
            haze(fwd * 4 + D3{ -0.6, -0.3, 0 }, 0.5f, 0.004f, 0, 0);
            haze(fwd * 14 + D3{ -1.0, 0.5, 0 }, 3.0f, 5e-4f, 0.15f, 1);
            if (strong) haze(fwd * 3 + D3{ 0.8, 0.0, 0 }, 0.4f, 0.03f, 0, 0);
            std::shared_ptr<std::vector<uint8_t>> offsetTex, depthTex, counters;
            uint32_t hw = 0, hh = 0;
            tf.run([&](FramePassContext& fc) {
                const BufferRef rb = tf.uploadBuffer(fc, recs.data(), recs.size() * sizeof(Record), sizeof(Record), "volume.test.records");
                auto& pass = fc.trackState->get<std::unique_ptr<volume::VolumePass>>("volume.test.pass");
                if (!pass) pass = std::make_unique<volume::VolumePass>(fc.device);
                volume::VolumeFrame f;
                f.view = &tf.frame.mainView;
                f.frameConstants = fc.frameConstantsFor(tf.frame.mainView);
                f.haze = true;
                const volume::VolumeOutput o = pass->recordRecords(rb, (uint32_t)recs.size(), fc.graph, fc.shaders, fc.quality, f);
                S_CHECK(o.valid && o.distortionOffset.valid(), "haze pass declared no field");
                hw = o.hazeWidth;
                hh = o.hazeHeight;
                offsetTex = tf.readback(fc, o.distortionOffset);
                depthTex = tf.readback(fc, o.distortionDepth);
                counters = tf.readbackBuffer(fc, o.counters, 32);
            });
            const uint32_t status = reinterpret_cast<const uint32_t*>(counters->data())[2];
            if (strong)
            {
                S_CHECK(status == 8u, "strong haze: status 0x%x (expected VOLUME_STATUS_HAZE_LARGE only)", status);
                std::printf("haze: a record bending rays past 8 px is reported (status 0x%x)\n", status);
                continue;
            }
            S_CHECK(status == 0, "haze status 0x%x", status);
            const uint32_t po = TestFrame::rowPitch(hw, 4), pd = TestFrame::rowPitch(hw, 2);
            double worst = 0, maxD = 0;
            for (uint32_t y = 0; y < hh; ++y)
                for (uint32_t x = 0; x < hw; ++x)
                {
                    const double px = x * 4.0 + 2, py = y * 4.0 + 2;
                    const D3 dir = norm(rayAt(view, px, py));
                    D3 theta{ 0, 0, 0 };
                    double front = 0;
                    for (const Record& r : recs)
                    {
                        const D3 c{ r.centre[0], r.centre[1], r.centre[2] };
                        const double t = dot(c, dir);
                        const D3 q = dir * t - c;
                        const double b = len(q);
                        if (!(t > 0) || b >= r.mass) continue;
                        if (b > 0) theta = theta + q * (hazeGradientRef(b, r.radius, r.a[0], r.a[1], (int)r.a[2]) / b);
                        front = std::max(front, view.nearPlane / std::max(viewDepth(view, c) - r.mass, (double)view.nearPlane));
                    }
                    double Dx = 0, Dy = 0;
                    if (len(theta) > 0)
                    {
                        const auto p1 = pixelOf(view, norm(dir + theta)), p0 = pixelOf(view, dir);
                        Dx = p1[0] - p0[0];
                        Dy = p1[1] - p0[1];
                    }
                    const uint16_t* o = reinterpret_cast<const uint16_t*>(offsetTex->data() + (size_t)y * po + (size_t)x * 4);
                    const float gx = half(o[0]), gy = half(o[1]);
                    const float gd = half(*reinterpret_cast<const uint16_t*>(depthTex->data() + (size_t)y * pd + (size_t)x * 2));
                    const double mag = std::sqrt(Dx * Dx + Dy * Dy), err = std::sqrt((gx - Dx) * (gx - Dx) + (gy - Dy) * (gy - Dy));
                    S_CHECK(err <= 2e-3 * mag + 2e-3, "texel (%u, %u): D (%.5f, %.5f) vs reference (%.5f, %.5f)", x, y, gx, gy, Dx, Dy);
                    S_CHECK(std::abs(gd - front) <= 1e-3 * front + 1e-6, "texel (%u, %u): front depth %.6g vs reference %.6g", x, y, gd, front);
                    worst = std::max(worst, err / (mag + 1e-3));
                    maxD = std::max(maxD, mag);
                }
            S_CHECK(maxD > 1.0, "the test haze bends by only %.3f px", maxD);
            std::printf("haze: %u x %u texels, max |D| %.3f px, worst rel %.2e\n", hw, hh, maxD, worst);
        }
        // ---- 4. media from the FX particle module: the RPP stream's volume program (23: size 0.05, alpha 1, flat curves,
        // absorption 0.02, scattering 0.3, emission (0.1, 0, 0), g 0.4) after some ticks, the frame at the latest tick's end
        // (w = 1: each particle at its latest state). Every media record is one live volume particle at its checkpoint
        // position, with r = 0.025 m, m = 1, extinction 0.32 and source 0.3 E HG(0.4, sun . D) + (0.1, 0, 0) (sun only: no
        // transmittance LUT, shadows, GI or local lights in this frame).
        {
            fx::ParticleSystem& ps = fx::particles(tf.trackState, tf.device, tf.quality);
            fx::test::RppConfig rc;
            rc.emitters = 16;
            rc.particles = 4096;
            rc.features = false;
            rc.ribbons = 4;
            rc.ribbonPoints = 16;
            rc.volumes = 4;
            rc.volumeParticles = 256;
            fx::test::RppStream stream(rc);
            std::vector<NV_StreamEvent> previous;
            std::vector<NV_StreamEmitter> table;
            NV_StreamHeader last{};
            for (uint32_t t = 1; t <= 40; ++t)
            {
                const std::vector<uint8_t> packet = stream.next(t == 1 ? nullptr : &previous);
                const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet.data());
                nv_stream::apply_emitter_table(table, h, packet.data(), packet.size());
                ps.submit(packet.data(), packet.size());
                tf.run([&](FramePassContext& fc) { tracks::simulation(fc); });
                previous = ps.readback(h.stream, h.generation, h.tick).events;
                last = h;
            }
            const std::vector<NV_StreamParticle> all = ps.checkpoint(tf.shaders);
            std::vector<D3> expected;  // world positions of the live volume particles
            for (const NV_StreamParticle& p : all)
            {
                const NV_StreamEmitter& e = table[p.emitter];
                if (e.program != fx::test::RppStream::kVolumeProgram) continue;
                expected.push_back({ last.anchor[0] + e.origin_anchor[0] + p.position[0], last.anchor[1] + e.origin_anchor[1] + p.position[1],
                                     last.anchor[2] + e.origin_anchor[2] + p.position[2] });
            }
            S_CHECK(!expected.empty(), "no live volume particles after 40 ticks");
            D3 centre{ 0, 0, 0 };
            for (const D3& x : expected) centre = centre + x * (1.0 / expected.size());
            const D3 eye = centre + D3{ -6, 3, -18 };
            scene::Camera vc;
            vc.name = "main";
            vc.position = { (float)eye.x, (float)eye.y, (float)eye.z };
            vc.forward = normalize(float3{ (float)(centre.x - eye.x), (float)(centre.y - eye.y), (float)(centre.z - eye.z) });
            scene::Scene s2 = sc;
            s2.cameras = { vc };
            s2.sun.direction = normalize(float3{ 0.3f, 0.8f, -0.5f });
            s2.sun.illuminance = 1000;
            s2.sun.color = { 1.0f, 0.9f, 0.8f };
            tf.setScene(s2);
            const ViewDesc v2 = ViewDesc::fromCamera(vc, W, H, float4x4{});
            tf.frame.mainView = v2;
            tf.frame.mainView.prevViewProj = v2.viewProj;
            uint32_t gridX, gridY, slices, tilePx;
            volume::froxelGridSize(tf.quality, W, H, gridX, gridY, slices, tilePx);
            const float nearM = (float)tf.quality.number("atmosphere.froxels.near_m"), farM = (float)tf.quality.number("atmosphere.froxels.far_m");
            std::shared_ptr<std::vector<uint8_t>> records, counters;
            uint32_t threads = 0;
            tf.run([&](FramePassContext& fc) {
                const std::vector<uint8_t> header = froxelHeader(gridX, gridY, slices, tilePx, nearM, farM);
                const BufferRef lists = tf.uploadBuffer(fc, header.data(), header.size(), 0, "volume.test.froxels");
                auto& pass = fc.trackState->get<std::unique_ptr<volume::VolumePass>>("volume.test.pass");
                if (!pass) pass = std::make_unique<volume::VolumePass>(fc.device);
                volume::VolumeFrame f;
                f.view = &tf.frame.mainView;
                f.frameConstants = fc.frameConstantsFor(tf.frame.mainView);
                f.camera[0] = eye.x;
                f.camera[1] = eye.y;
                f.camera[2] = eye.z;
                f.time = last.time;
                f.froxelLights = lists;
                f.media = true;
                const volume::VolumeOutput o = pass->record(ps, fc.graph, fc.shaders, fc.quality, fc.frame.frameIndex, f);
                S_CHECK(o.valid && o.volumeSlices.valid(), "no media output from the particle module");
                threads = o.threads;
                records = tf.readbackBuffer(fc, o.records, (uint64_t)o.threads * sizeof(Record));
                counters = tf.readbackBuffer(fc, o.counters, 32);
            });
            const uint32_t* cnt = reinterpret_cast<const uint32_t*>(counters->data());
            S_CHECK(cnt[2] == 0, "stream media status 0x%x", cnt[2]);
            const Record* r = reinterpret_cast<const Record*>(records->data());
            const D3 sun = norm(d3(s2.sun.direction));
            const double g = 0.4, E[3] = { 1000.0 * 1.0, 1000.0 * 0.9, 1000.0 * 0.8 };
            uint32_t media = 0;
            double worstPos = 0, worstSource = 0;
            std::vector<bool> used(expected.size(), false);
            for (uint32_t i = 0; i < threads; ++i)
            {
                if (r[i].kind != 1) continue;
                ++media;
                const D3 rel{ r[i].centre[0], r[i].centre[1], r[i].centre[2] };
                const D3 world = eye + rel;
                size_t best = 0;
                double bestD = 1e30;
                for (size_t k = 0; k < expected.size(); ++k)
                {
                    const double d = len(expected[k] - world);
                    if (d < bestD) { bestD = d; best = k; }
                }
                worstPos = std::max(worstPos, bestD);
                S_CHECK(bestD <= 2e-3, "record %u at (%.4f, %.4f, %.4f) is %.4g m from every live volume particle", i, world.x, world.y, world.z, bestD);
                S_CHECK(!used[best], "two records for one particle");
                used[best] = true;
                S_CHECK(std::abs(r[i].radius - 0.025f) < 1e-6f && std::abs(r[i].mass - 1.0f) < 1e-6f, "record %u: r %.6g m %.6g", i, r[i].radius, r[i].mass);
                const double cosTheta = dot(sun, norm(rel));
                const double hg = (1 - g * g) / (4 * 3.14159265358979323846 * std::pow(1 + g * g - 2 * g * cosTheta, 1.5));
                for (int k = 0; k < 3; ++k)
                {
                    S_CHECK(std::abs(r[i].a[k] - 0.32f) < 1e-6f, "record %u: extinction %.6g", i, r[i].a[k]);
                    const double b = 0.3 * E[k] * hg + (k == 0 ? 0.1 : 0.0);
                    const double e = std::abs(r[i].b[k] - b) / b;
                    worstSource = std::max(worstSource, e);
                    S_CHECK(e <= 1e-4, "record %u: source[%d] %.6g vs %.6g", i, k, r[i].b[k], b);
                }
            }
            S_CHECK(media == expected.size(), "%u media records for %zu live volume particles", media, expected.size());
            std::printf("stream media: %u records = live volume particles at the frame time (worst position %.2e m, source rel %.2e)\n", media, worstPos, worstSource);
        }
        std::printf("PASS volume: probe, media slices, haze field, stream media\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
