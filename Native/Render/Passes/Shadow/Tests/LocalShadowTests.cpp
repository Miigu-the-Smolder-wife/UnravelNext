// S local-light shadows, correctness (no GPU lock needed; run small). Test stand-ins for V's raster and M's G-buffer
// (TestRaster.h). A box over a ground plane lit by shadow-casting local lights (a sphere light of radius 0.1 m and a
// point light): visibility slot 1 of each pixel against the exact reference, the visible fraction of the light's disk
// seen from the receiver (stratified directions, ray-cast against the same boxes); hard and soft shadows; debug layer.
//   unx_test_shadow_localshadowtests [--no-debug-layer] [--width W --height H] [--verbose N]
#include "TestRaster.h"

#include "VsmSystem.h"

#include <algorithm>
#include <cstdio>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace
{
struct Box
{
    float3 centre, half;
};

// Segment o -> o + d (t in (0, 1)) against a box.
bool hitBox(const Box& b, float3 o, float3 d)
{
    float t0 = 1e-5f, t1 = 1 - 1e-5f;
    for (int a = 0; a < 3; ++a)
    {
        const float oa = (&o.x)[a], da = (&d.x)[a], lo = (&b.centre.x)[a] - (&b.half.x)[a], hi = (&b.centre.x)[a] + (&b.half.x)[a];
        if (std::abs(da) < 1e-12f)
        {
            if (oa < lo || oa > hi) return false;
            continue;
        }
        float ta = (lo - oa) / da, tb = (hi - oa) / da;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

// Visible fraction of the light's disk (radius r, facing p) from p: stratified points, segments to them.
float referenceVisibility(const std::vector<Box>& boxes, float3 p, float3 light, float r)
{
    const float3 w = normalize(light - p);
    const float3 x = normalize(cross(std::abs(w.y) < 0.999f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 }, w)), y = cross(w, x);
    auto fraction = [&](int n) {
        int lit = 0, total = 0;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
            {
                const float a = (i + 0.5f) / n * 2 - 1, b = (j + 0.5f) / n * 2 - 1;
                if (n > 1 && a * a + b * b > 1) continue;
                const float3 q = light + (x * a + y * b) * r;
                bool blocked = false;
                for (const Box& bx : boxes)
                    if (hitBox(bx, p, q - p))
                    {
                        blocked = true;
                        break;
                    }
                lit += blocked ? 0 : 1;
                ++total;
            }
        return (float)lit / total;
    };
    if (r <= 0) return fraction(1);
    const float coarse = fraction(9);
    if (coarse == 0 || coarse == 1)
    {
        const float check = fraction(24);
        if (check == coarse) return coarse;
    }
    return fraction(64);
}

float3 octDecodeCpu(uint32_t packed)
{
    const float x = (int16_t)(packed & 0xFFFF) / 32767.0f, y = (int16_t)(packed >> 16) / 32767.0f;
    float3 n{ x, y, 1 - std::abs(x) - std::abs(y) };
    const float t = std::max(-n.z, 0.0f);
    n.x += n.x >= 0 ? -t : t;
    n.y += n.y >= 0 ? -t : t;
    return normalize(n);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        uint32_t W = 960, H = 540;
        int verbose = 0;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--no-debug-layer") debugLayer = false;
            else if (a == "--width") W = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--height") H = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--verbose") verbose = std::stoi(argv[++i]);
        }
        TestFrame tf(debugLayer);
        TestRaster raster(tf);
        raster.install();
        int failures = 0;
        auto report = [&](bool ok, const char* what, double value, double limit) {
            logf("%-62s %.4g (limit %.4g) %s\n", what, value, limit, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };

        struct Case
        {
            const char* label;
            scene::LightType type;
            float radius;
        };
        for (const Case& cs : { Case{ "sphere light r 0.1 m", scene::LightType::Sphere, 0.1f }, Case{ "point light", scene::LightType::Point, 0.0f } })
        {
            scene::Scene sc;
            sc.name = "local shadow test";
            sc.materials.push_back({});
            sc.meshes.push_back(boxMesh("ground", { 6, 0.05f, 6 }));
            sc.meshes.push_back(boxMesh("box", { 0.4f, 0.4f, 0.4f }));
            sc.instances.push_back(instanceAt(0, { 0, -0.05f, 0 }));
            sc.instances.push_back(instanceAt(1, { 0, 0.9f, 0 }));
            const std::vector<Box> boxes = { { { 0, -0.05f, 0 }, { 6, 0.05f, 6 } }, { { 0, 0.9f, 0 }, { 0.4f, 0.4f, 0.4f } } };
            sc.sun.direction = normalize(float3{ 0.3f, -0.6f, 0.2f });  // below the horizon: only the local light
            scene::Light l;
            l.type = cs.type;
            l.position = { 0.25f, 2.6f, 0.15f };
            l.intensity = 2000;
            l.range = 9;
            l.size = { cs.radius, 0 };
            l.castShadow = true;
            sc.lights.push_back(l);
            scene::Camera cam;
            cam.name = "main";
            cam.position = { -3.2f, 2.4f, -2.9f };
            cam.forward = normalize(float3{ 3.2f, -2.3f, 2.9f });
            sc.cameras.push_back(cam);
            tf.setScene(sc);
            tf.frame.mainView = ViewDesc::fromCamera(cam, W, H, float4x4{});
            tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;
            tf.frame.deltaTime = 1.0f / 60;
            std::vector<uint8_t> vis, depth, gbuffer;
            for (int frame = 0; frame < 2; ++frame)
            {
                std::shared_ptr<std::vector<uint8_t>> rv, rd, rg;
                const bool read = frame == 1;
                tf.run([&](FramePassContext& fc) {
                    ViewResources main;
                    main.view = fc.frame.mainView;
                    main.frameConstants = fc.frameConstantsFor(main.view);
                    raster.mainView(fc, main);
                    tracks::shadowPages(fc, main);
                    tracks::shadowVisibility(fc, main);
                    if (read)
                    {
                        rv = tf.readback(fc, main.shadowVisibility);
                        rd = tf.readback(fc, main.depth);
                        rg = tf.readback(fc, main.gbuffer);
                    }
                });
                tf.frame.time += tf.frame.deltaTime;
                if (read)
                {
                    vis = *rv;
                    depth = *rd;
                    gbuffer = *rg;
                }
            }
            const shadow::VsmStats& st = shadow::stats(tf.trackState);
            logf("%s: local shadow slots %u, raster-active %u, casting lights without a slot %u\n", cs.label, st.localAssigned, st.localActive, st.localWithoutSlot);
            report(st.localAssigned == 1 && st.localActive == 1, (std::string(cs.label) + ": one shadow slot, raster-active").c_str(), st.localAssigned, 1);

            const uint32_t pv = TestFrame::rowPitch(W, 4), pg = TestFrame::rowPitch(W, 8);
            const ViewDesc& v = tf.frame.mainView;
            std::vector<double> errors;
            double penSum = 0;
            int penCount = 0, gross = 0, printed = 0, shadowed = 0;
            for (uint32_t y = 0; y < H; y += 2)
                for (uint32_t x = 0; x < W; x += 2)
                {
                    float d;
                    std::memcpy(&d, depth.data() + y * pv + x * 4, 4);
                    if (d <= 0) continue;
                    uint32_t packed, g[2];
                    std::memcpy(&packed, vis.data() + y * pv + x * 4, 4);
                    std::memcpy(g, gbuffer.data() + y * pg + x * 8, 8);
                    const float ndc[4] = { (x + 0.5f) / W * 2 - 1, 1 - (y + 0.5f) / H * 2, d, 1 };
                    float wp[4] = {};
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c) wp[r] += v.invViewProj.m[r][c] * ndc[c];
                    const float3 p{ wp[0] / wp[3], wp[1] / wp[3], wp[2] / wp[3] };
                    const float3 n = octDecodeCpu(g[0]);
                    const float3 toLight = l.position - p;
                    if (length(toLight) > l.range * 0.95f || dot(n, normalize(toLight)) < 0.05f) continue;  // unlit side / range edge
                    const float ref = referenceVisibility(boxes, p + n * 2e-4f, l.position, cs.radius);
                    const float gpu = ((packed >> 8) & 0xFF) / 255.0f;
                    const double e = std::abs(gpu - ref);
                    errors.push_back(e);
                    shadowed += ref < 0.5f ? 1 : 0;
                    if (e > 0.1 && printed < verbose)
                    {
                        ++printed;
                        logf("  px (%u,%u) world (%.3f %.3f %.3f) n (%.2f %.2f %.2f): gpu %.3f ref %.3f\n", x, y, p.x, p.y, p.z, n.x, n.y, n.z, gpu, ref);
                    }
                    if (e > 0.25) ++gross;
                    if (ref > 0 && ref < 1)
                    {
                        penSum += e;
                        ++penCount;
                    }
                }
            double mean = 0;
            for (double e : errors) mean += e;
            mean /= std::max<size_t>(errors.size(), 1);
            const double grossFraction = (double)gross / std::max<size_t>(errors.size(), 1);
            logf("%s: %zu pixels (%d in shadow), penumbra %d (mean |e| %.4f), mean |e| %.5f, |e| > 0.25: %.4f %%\n", cs.label, errors.size(), shadowed, penCount,
                 penCount ? penSum / penCount : 0.0, mean, 100 * grossFraction);
            report(shadowed > 100, (std::string(cs.label) + ": pixels in the box's shadow").c_str(), shadowed, 100);
            report(mean < 0.01, (std::string(cs.label) + ": mean |V - V_ref|").c_str(), mean, 0.01);
            report(grossFraction < 2e-3, (std::string(cs.label) + ": fraction |V - V_ref| > 0.25").c_str(), grossFraction, 2e-3);
            report(penCount == 0 || penSum / penCount < 0.06, (std::string(cs.label) + ": penumbra mean |V - V_ref|").c_str(), penCount ? penSum / penCount : 0, 0.06);
        }
        const uint32_t debugErrors = tf.device.drainDebugMessages();
        report(debugErrors == 0, "D3D12 debug layer errors", debugErrors, 0);
        logf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
