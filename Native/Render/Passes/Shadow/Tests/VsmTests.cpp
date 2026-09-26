// S virtual shadow maps, correctness (no GPU lock). Test stand-ins for V's raster and M's G-buffer (TestRaster.h).
//  1. sun visibility against an exact reference: the fraction of the sun disk (uniform, angular radius theta_s) visible
//     from each pixel, ray-cast on the CPU against the same boxes;
//  2. page cache and dirty rules: steady camera renders nothing, a small camera move renders only new pages, a moved
//     caster re-renders the pages under its old and new bounds (and the result matches the reference again), a sun
//     change re-renders everything, wind re-renders only levels whose texel is smaller than the sway;
//  3. shadowSunVisibilityAt (ray hits, R) at the static frame's receivers with footprints 1, 4 and 16 pixels: against the
//     same reference, mean error and signed bias (a lookup that leaks light shows as a positive bias);
//  4. D3D12 debug layer clean.
//   unx_test_shadow_vsmtests [--no-debug-layer] [--width W --height H] [--dump DIR] [--verbose N] [--set key=value]
#include "TestRaster.h"

#include "VsmBlockCheck.h"
#include "VsmSystem.h"

#include <algorithm>
#include <cstdio>
#include <fstream>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

namespace
{
struct Box
{
    float3 centre, half;
};

bool hitBox(const Box& b, float3 o, float3 d)
{
    float t0 = 0, t1 = 1e30f;
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

// Exact visible fraction of the uniform sun disk from p (stratified directions; adaptive: 64 then 4096 in penumbrae).
float referenceVisibility(const std::vector<Box>& boxes, float3 p, float3 sun, float tanTheta)
{
    float3 x = normalize(cross(std::abs(sun.y) < 0.999f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 }, sun));
    float3 y = cross(sun, x);
    auto fraction = [&](int n) {
        int lit = 0, total = 0;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
            {
                const float a = (i + 0.5f) / n * 2 - 1, b = (j + 0.5f) / n * 2 - 1;
                if (a * a + b * b > 1) continue;
                const float3 d = normalize(sun + (x * a + y * b) * tanTheta);
                bool blocked = false;
                for (const Box& bx : boxes)
                    if (hitBox(bx, p, d))
                    {
                        blocked = true;
                        break;
                    }
                lit += blocked ? 0 : 1;
                ++total;
            }
        return (float)lit / total;
    };
    const float coarse = fraction(9);
    if (coarse == 0 || coarse == 1)
    {
        const float check = fraction(24);
        if (check == coarse) return coarse;
    }
    return fraction(72);
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

struct Frame
{
    std::vector<uint8_t> vis, depth, gbuffer, table;
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        uint32_t W = 1920, H = 1080;
        std::string dumpDir;
        int verbose = 0;
        std::vector<std::string> overrides;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--no-debug-layer") debugLayer = false;
            else if (a == "--width") W = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--height") H = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--dump") dumpDir = argv[++i];        // R = GPU, G = reference, B = 4 |error| per compared pixel
            else if (a == "--verbose") verbose = std::stoi(argv[++i]);  // print that many pixels with |error| > 0.1
            else if (a == "--set") overrides.push_back(argv[++i]);          // quality override "section.key=value"
        }
        TestFrame tf(debugLayer);
        for (const std::string& o : overrides) tf.quality.applyOverride(o);
        TestRaster raster(tf);
        raster.install();

        scene::Scene sc;
        sc.name = "vsm test";
        sc.materials.push_back({});
        sc.meshes.push_back(boxMesh("ground", { 100, 0.05f, 100 }));
        sc.meshes.push_back(boxMesh("plate", { 1, 0.05f, 1 }));
        sc.meshes.push_back(boxMesh("pole", { 0.1f, 5, 0.1f }));
        sc.instances.push_back(instanceAt(0, { 0, -0.05f, 0 }));
        sc.instances.push_back(instanceAt(1, { 0, 3, 0 }));
        sc.instances.push_back(instanceAt(2, { 3, 5, 1.5f }, scene::InstanceCastShadow | scene::InstanceDynamic));
        sc.sun.direction = normalize(float3{ -0.45f, 0.8f, -0.35f });
        scene::Camera cam;
        cam.name = "main";
        cam.position = { -4, 3.2f, -5 };
        cam.forward = normalize(float3{ 1.2f, -0.55f, 1.4f });
        sc.cameras.push_back(cam);
        tf.setScene(sc);
        std::vector<Box> boxes = { { { 0, -0.05f, 0 }, { 100, 0.05f, 100 } }, { { 0, 3, 0 }, { 1, 0.05f, 1 } }, { { 3, 5, 1.5f }, { 0.1f, 5, 0.1f } } };

        auto setCamera = [&](float3 position) {
            scene::Camera c = cam;
            c.position = position;
            tf.frame.mainView = ViewDesc::fromCamera(c, W, H, float4x4{});
            tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;
        };
        setCamera(cam.position);
        tf.frame.deltaTime = 1.0f / 60;

        uint64_t recorded = 0;  // frames recorded so far = the VSM's frame number of the last one
        auto runFrame = [&](bool read) {
            ++recorded;
            Frame f;
            std::shared_ptr<std::vector<uint8_t>> vis, depth, gb, table;
            tf.run([&](FramePassContext& fc) {
                ViewResources main;
                main.view = fc.frame.mainView;
                main.frameConstants = fc.frameConstantsFor(main.view);
                raster.mainView(fc, main);
                tracks::shadowPages(fc, main);
                tracks::shadowVisibility(fc, main);
                if (read)
                {
                    vis = tf.readback(fc, main.shadowVisibility);
                    depth = tf.readback(fc, main.depth);
                    gb = tf.readback(fc, main.gbuffer);
                    table = tf.readbackBuffer(fc, fc.resources.vsmPageTable, (uint64_t)shadow::kSlots * 8);
                }
            });
            tf.frame.time += tf.frame.deltaTime;
            if (read)
            {
                f.vis = *vis;
                f.depth = *depth;
                f.gbuffer = *gb;
                f.table = *table;
            }
            return f;
        };

        int failures = 0;
        auto report = [&](bool ok, const char* what, double value, double limit) {
            logf("%-62s %.4g (limit %.4g) %s\n", what, value, limit, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };
        // Counters of the last recorded frame: they are copied at the start of the next frame and read back once the
        // GPU has passed it, so a few steady frames follow (the camera and scene do not change in between).
        auto statsAfter = [&]() {
            const uint64_t target = recorded;
            for (int i = 0; i < 4 && shadow::stats(tf.trackState).frame < target; ++i) runFrame(false);
            const shadow::VsmStats st = shadow::stats(tf.trackState);
            if (st.frame != target) fail("VSM statistics of frame %llu not available (latest %llu)", (unsigned long long)target, (unsigned long long)st.frame);
            return st;
        };

        // Compares one frame's visibility against the reference on a pixel grid.
        auto compare = [&](const Frame& f, const char* label, int stride) {
            const uint32_t pv = TestFrame::rowPitch(W, 4), pg = TestFrame::rowPitch(W, 8);
            const ViewDesc& v = tf.frame.mainView;
            const float3 sun = normalize(tf.sceneData.sun.direction);
            const float tanTheta = std::tan(tf.sceneData.sun.angularRadius);
            std::vector<double> errors;
            double penSum = 0;
            int penCount = 0, gross = 0, printed = 0;
            const uint32_t iw = (W + stride - 1) / stride, ih = (H + stride - 1) / stride;
            std::vector<uint8_t> image(dumpDir.empty() ? 0 : (size_t)iw * ih * 3, 0);
            for (uint32_t y = 0; y < H; y += stride)
                for (uint32_t x = 0; x < W; x += stride)
                {
                    float d;
                    std::memcpy(&d, f.depth.data() + y * pv + x * 4, 4);
                    if (d <= 0) continue;
                    uint32_t packed, g[2];
                    std::memcpy(&packed, f.vis.data() + y * pv + x * 4, 4);
                    std::memcpy(g, f.gbuffer.data() + y * pg + x * 8, 8);
                    const float ndc[4] = { (x + 0.5f) / W * 2 - 1, 1 - (y + 0.5f) / H * 2, d, 1 };
                    float wp[4] = {};
                    for (int r = 0; r < 4; ++r)
                        for (int c = 0; c < 4; ++c) wp[r] += v.invViewProj.m[r][c] * ndc[c];
                    const float3 p{ wp[0] / wp[3], wp[1] / wp[3], wp[2] / wp[3] };
                    const float3 n = octDecodeCpu(g[0]);
                    const float ref = referenceVisibility(boxes, p + n * 2e-4f, sun, tanTheta);
                    const float gpu = (packed & 0xFF) / 255.0f;
                    const double e = std::abs(gpu - ref);
                    errors.push_back(e);
                    if (!image.empty())
                    {
                        uint8_t* px = &image[((size_t)(y / stride) * iw + x / stride) * 3];
                        px[0] = (uint8_t)(gpu * 255);
                        px[1] = (uint8_t)(ref * 255);
                        px[2] = (uint8_t)std::min(255.0, e * 4 * 255);
                    }
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
            if (!image.empty())
            {
                const std::string path = dumpDir + "/" + label + ".ppm";
                std::ofstream file(path, std::ios::binary);
                file << "P6 " << iw << " " << ih << " 255" << (char)10;
                file.write(reinterpret_cast<const char*>(image.data()), (std::streamsize)image.size());
            }
            std::sort(errors.begin(), errors.end());
            double mean = 0;
            for (double e : errors) mean += e;
            mean /= std::max<size_t>(errors.size(), 1);
            const double p99 = errors.empty() ? 0 : errors[(size_t)(errors.size() * 0.99)];
            const double grossFraction = (double)gross / std::max<size_t>(errors.size(), 1);
            logf("%s: %zu pixels, penumbra %d (mean |e| %.4f), mean |e| %.5f, P99 %.4f, max %.4f, |e| > 0.25: %.4f %%\n", label, errors.size(), penCount,
                 penCount ? penSum / penCount : 0.0, mean, p99, errors.empty() ? 0.0 : errors.back(), 100 * grossFraction);
            report(mean < 0.005, format("%s: mean |V - V_ref|", label).c_str(), mean, 0.005);
            report(grossFraction < 1e-3, format("%s: fraction |V - V_ref| > 0.25", label).c_str(), grossFraction, 1e-3);
            report(penCount == 0 || penSum / penCount < 0.05, format("%s: penumbra mean |V - V_ref|", label).c_str(), penCount ? penSum / penCount : 0, 0.05);
        };

        auto dirtyByLevel = [&](const Frame& f) {
            std::vector<uint32_t> n(shadow::kLevels, 0);
            for (uint32_t s = 0; s < shadow::kSlots; ++s)
            {
                uint32_t e;
                std::memcpy(&e, f.table.data() + s * 8ull, 4);
                if ((e & (1u << 31)) && (e & (1u << 29))) ++n[s / (shadow::kTable * shadow::kTable)];
            }
            return n;
        };

        // 1. One path (S request 20260926_S_vsm_one_path): every frame assigns and draws every requested page, and the
        //    assignment is a deterministic scan (the same requests give the same page table).
        Frame f1 = runFrame(true);
        const shadow::VsmStats s1 = statsAfter();
        logf("frame %llu: requested %u, allocated %u, dirty %u, exhausted %u, free %u\n", (unsigned long long)s1.frame, s1.requested, s1.allocated, s1.dirty, s1.exhausted, s1.freePages);
        report(s1.requested > 0 && s1.allocated == s1.requested && s1.dirty == s1.requested && s1.exhausted == 0, "first frame: every requested page allocated and rendered",
               s1.dirty, s1.requested);
        const shadow::VsmStats s2 = statsAfter();
        report(s2.requested == s1.requested && s2.dirty == s2.requested && s2.exhausted == 0, "steady camera: every requested page drawn again", s2.dirty, s2.requested);
        Frame f2 = runFrame(true);
        report(f1.table == f2.table, "steady camera: the same page table (deterministic assignment)", f1.table == f2.table ? 0 : 1, 0);
        compare(f2, "static", 2);

        // shadowSunVisibilityAt at the receivers of f2, footprints of 1, 4 and 16 pixels (ray cones wider than a pixel).
        {
            const uint32_t pv = TestFrame::rowPitch(W, 4), pg = TestFrame::rowPitch(W, 8);
            const ViewDesc& v = tf.frame.mainView;
            const float3 sun = normalize(tf.sceneData.sun.direction);
            const float tanTheta = std::tan(tf.sceneData.sun.angularRadius);
            std::vector<float4> points, normals;
            std::vector<float> refs, direct;
            // Groups 0-2: surface points (footprints x1, x4, x16); group 3: air points halfway along the pixel's view ray
            // (shadowSunVisibilityInAir, the particle lookup; pixel footprint at that depth).
            const float factors[3] = { 1, 4, 16 };
            for (uint32_t group = 0; group < 4; ++group)
            {
                const float factor = group < 3 ? factors[group] : 1;
                for (uint32_t y = 0; y < H; y += 6)
                    for (uint32_t x = 0; x < W; x += 6)
                    {
                        float d;
                        std::memcpy(&d, f2.depth.data() + y * pv + x * 4, 4);
                        if (d <= 0) continue;
                        uint32_t packed, g[2];
                        std::memcpy(&packed, f2.vis.data() + y * pv + x * 4, 4);
                        std::memcpy(g, f2.gbuffer.data() + y * pg + x * 8, 8);
                        const float ndc[4] = { (x + 0.5f) / W * 2 - 1, 1 - (y + 0.5f) / H * 2, d, 1 };
                        float wp[4] = {};
                        for (int r = 0; r < 4; ++r)
                            for (int c = 0; c < 4; ++c) wp[r] += v.invViewProj.m[r][c] * ndc[c];
                        const float3 p{ wp[0] / wp[3], wp[1] / wp[3], wp[2] / wp[3] };
                        const float3 n = octDecodeCpu(g[0]);
                        const float z = v.nearPlane / d;
                        if (group == 3)
                        {
                            const float3 q = v.position + (p - v.position) * 0.5f;
                            points.push_back({ q.x, q.y, q.z, 2 * 0.5f * z * std::tan(0.5f * v.verticalFov) / H });
                            normals.push_back({ 0, 0, 0, 1 });
                            refs.push_back(referenceVisibility(boxes, q, sun, tanTheta));
                            direct.push_back(refs.back());
                            continue;
                        }
                        const float footprint = 2 * z * std::tan(0.5f * v.verticalFov) / H * factor;
                        points.push_back({ p.x, p.y, p.z, footprint });
                        normals.push_back({ n.x, n.y, n.z, 0 });
                        refs.push_back(referenceVisibility(boxes, p + n * 2e-4f, sun, tanTheta));
                        direct.push_back((packed & 0xFF) / 255.0f);
                    }
            }
            // Air walk hard cap (INTERFACES 3.6): level-0 segments (page 0.125 m) of 200 m (1600 pages > the cap of 512)
            // and 10 m (80 pages), horizontal across the scene; after the four groups.
            const uint32_t perFactor = (uint32_t)points.size() / 4;
            for (float length : { 200.0f, 10.0f })
            {
                points.push_back({ -length * 0.5f, 1.0f, 0.3f, 0.0f });
                normals.push_back({ length, 0, 0, 2 });
                refs.push_back(0);
                direct.push_back(0);
            }
            const uint32_t count = (uint32_t)points.size();
            std::shared_ptr<std::vector<uint8_t>> out;
            ++recorded;  // a VSM frame like runFrame's (the statistics checks count them)
            tf.run([&](FramePassContext& fc) {
                ViewResources main;
                main.view = fc.frame.mainView;
                main.frameConstants = fc.frameConstantsFor(main.view);
                raster.mainView(fc, main);
                tracks::shadowPages(fc, main);
                const BufferRef pts = tf.uploadBuffer(fc, points.data(), points.size() * 16, 16, "probe points");
                const BufferRef nrm = tf.uploadBuffer(fc, normals.data(), normals.size() * 16, 16, "probe normals");
                const BufferRef o = fc.graph.createBuffer(BufferDesc{ "probe out", (uint64_t)count * 8, 8 });
                const FrameResources r = fc.resources;
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shadow/Tests/ShadowAtProbe");
                const D3D12_GPU_VIRTUAL_ADDRESS cb = main.frameConstants;
                fc.graph.addPass("s.test.atprobe", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     b.use(pts, Use::SrvCompute);
                                     b.use(nrm, Use::SrvCompute);
                                     b.use(o, Use::UavCompute);
                                     b.use(r.vsmPageTable, Use::SrvCompute);
                                     b.use(r.vsmPool, Use::SrvCompute);
                                     b.use(r.vsmBlocks, Use::SrvCompute);
                                     b.use(r.vsmSearchBound, Use::SrvCompute);
                                     b.use(r.vsmLayers, Use::SrvCompute);
                                 },
                                 [=](PassContext& ctx) {
                                     const uint32_t k[12] = { ctx.srv(pts), ctx.srv(nrm), ctx.uav(o), count, ctx.srv(r.vsmPageTable), ctx.srv(r.vsmPool),
                                                              ctx.srv(r.vsmBlocks), ctx.srv(r.vsmSearchBound), r.vsmConstants, r.vsmLocalLights, r.vsmSlotOfLight,
                                                              ctx.srv(r.vsmLayers) };
                                     ctx.cmd->SetPipelineState(pso);
                                     ctx.bindFrameConstants(cb);
                                     ctx.computeConstants(k, 12);
                                     ctx.cmd->Dispatch((count + 63) / 64, 1, 1);
                                 });
                out = tf.readbackBuffer(fc, o, (uint64_t)count * 8);
            });
            tf.frame.time += tf.frame.deltaTime;
            {
                float g[4];
                std::memcpy(g, out->data() + (size_t)(count - 2) * 8, 16);
                logf("air walk cap: 200 m level-0 segment capped %.0f, 10 m capped %.0f" "\n", g[1], g[3]);
                report(g[1] == 1 && g[3] == 0, "air walk hard cap flags the 1600-page segment only (INTERFACES 3.6)", g[1] - g[3], 1);
            }
            for (uint32_t fi = 0; fi < 4; ++fi)
            {
                double sumAbs = 0, sumSigned = 0, sumDirect = 0;
                uint32_t resident = 0;
                for (uint32_t i = fi * perFactor; i < (fi + 1) * perFactor; ++i)
                {
                    float g[2];
                    std::memcpy(g, out->data() + i * 8ull, 8);
                    if (g[1] == 0) continue;
                    ++resident;
                    sumAbs += std::abs(g[0] - refs[i]);
                    sumSigned += g[0] - refs[i];
                    sumDirect += std::abs(g[0] - direct[i]);
                }
                const double n = std::max(resident, 1u);
                if (fi == 3)
                {
                    logf("shadowSunVisibilityInAir (air points): %u of %u resident, mean |V - V_ref| %.5f, bias %+.5f\n", resident, perFactor,
                         sumAbs / n, sumSigned / n);
                    report(resident > perFactor * 0.9, "shadowSunVisibilityInAir: resident fraction", (double)resident / std::max(perFactor, 1u), 0.9);
                    report(sumAbs / n < 0.01, "shadowSunVisibilityInAir: mean |V - V_ref|", sumAbs / n, 0.01);
                    report(std::abs(sumSigned / n) < 0.004, "shadowSunVisibilityInAir: |bias|", std::abs(sumSigned / n), 0.004);
                    continue;
                }
                logf("shadowSunVisibilityAt, footprint x%.0f: %u of %u resident, mean |V - V_ref| %.5f, bias %+.5f, mean |V - direct view| %.5f" "\n", factors[fi],
                     resident, perFactor, sumAbs / n, sumSigned / n, sumDirect / n);
                const double absLimit = fi == 0 ? 0.005 : fi == 1 ? 0.01 : 0.02;
                report(resident > perFactor * 0.9, format("shadowSunVisibilityAt x%.0f: resident fraction", factors[fi]).c_str(), (double)resident / std::max(perFactor, 1u), 0.9);
                report(sumAbs / n < absLimit, format("shadowSunVisibilityAt x%.0f: mean |V - V_ref|", factors[fi]).c_str(), sumAbs / n, absLimit);
                report(std::abs(sumSigned / n) < 0.002, format("shadowSunVisibilityAt x%.0f: |bias|", factors[fi]).c_str(), std::abs(sumSigned / n), 0.002);
            }
        }

        // Segment classification (vsmSegmentClassify, COVERAGE_REDESIGN 4.3 fragment depth ranges): segments from 2 cm off
        // the receivers of f2 towards the camera, 0.05 / 0.5 / 2 m long. Lit must mean visibility 1 at every one of 16
        // points along it, umbra 0 (vsmSunVisibility with flat receivers, the classifier's points).
        {
            const uint32_t pv = TestFrame::rowPitch(W, 4), pg = TestFrame::rowPitch(W, 8);
            const ViewDesc& v = tf.frame.mainView;
            std::vector<float4> segments;
            // Groups: 0 near a surface (2 cm off, towards the camera), 1 free air (0.5 m off), 2 grass (from 5 mm off, along the
            // surface normal: the fragment range of a blade standing on the surface).
            std::vector<uint32_t> groupOf;
            for (int group = 0; group < 3; ++group)
            for (float len : { 0.05f, 0.5f, 2.0f })
                for (uint32_t y = 3; y < H; y += 8)
                    for (uint32_t x = 3; x < W; x += 8)
                    {
                        float d;
                        std::memcpy(&d, f2.depth.data() + y * pv + x * 4, 4);
                        if (d <= 0) continue;
                        uint32_t g[2];
                        std::memcpy(g, f2.gbuffer.data() + y * pg + x * 8, 8);
                        const float ndc[4] = { (x + 0.5f) / W * 2 - 1, 1 - (y + 0.5f) / H * 2, d, 1 };
                        float wp[4] = {};
                        for (int r = 0; r < 4; ++r)
                            for (int c = 0; c < 4; ++c) wp[r] += v.invViewProj.m[r][c] * ndc[c];
                        const float3 p{ wp[0] / wp[3], wp[1] / wp[3], wp[2] / wp[3] };
                        if (group == 2 && len > 1) continue;
                        const float3 nrm = octDecodeCpu(g[0]);
                        const float lift = group == 0 ? 0.02f : group == 1 ? 0.5f : 0.005f;
                        const float3 p0 = p + nrm * lift;
                        const float3 p1 = p0 + (group == 2 ? nrm * (len * 0.6f) : normalize(v.position - p0) * len);
                        groupOf.push_back((uint32_t)group);
                        const float z = v.nearPlane / d;
                        segments.push_back({ p0.x, p0.y, p0.z, 2 * z * std::tan(0.5f * v.verticalFov) / H });
                        segments.push_back({ p1.x, p1.y, p1.z, 0 });
                        segments.push_back({ nrm.x, nrm.y, nrm.z, 0 });
                        segments.push_back({ p.x, p.y, p.z, 0 });
                    }
            const uint32_t count = (uint32_t)segments.size() / 4;
            std::shared_ptr<std::vector<uint8_t>> out;
            ++recorded;
            tf.run([&](FramePassContext& fc) {
                ViewResources main;
                main.view = fc.frame.mainView;
                main.frameConstants = fc.frameConstantsFor(main.view);
                raster.mainView(fc, main);
                tracks::shadowPages(fc, main);
                const BufferRef seg = tf.uploadBuffer(fc, segments.data(), segments.size() * 16, 16, "probe segments");
                const BufferRef o = fc.graph.createBuffer(BufferDesc{ "segment out", (uint64_t)count * 17 * 4, 4 });
                const FrameResources r = fc.resources;
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shadow/Tests/ShadowSegmentProbe");
                const D3D12_GPU_VIRTUAL_ADDRESS cb = main.frameConstants;
                fc.graph.addPass("s.test.segmentprobe", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     b.use(seg, Use::SrvCompute);
                                     b.use(o, Use::UavCompute);
                                     b.use(r.vsmPageTable, Use::SrvCompute);
                                     b.use(r.vsmPool, Use::SrvCompute);
                                     b.use(r.vsmBlocks, Use::SrvCompute);
                                     b.use(r.vsmSearchBound, Use::SrvCompute);
                                 },
                                 [=](PassContext& ctx) {
                                     const uint32_t k[12] = { ctx.srv(seg), ctx.uav(o), count, 0, ctx.srv(r.vsmPageTable), ctx.srv(r.vsmPool),
                                                              ctx.srv(r.vsmBlocks), ctx.srv(r.vsmSearchBound), r.vsmConstants, 0, 0, 0 };
                                     ctx.cmd->SetPipelineState(pso);
                                     ctx.bindFrameConstants(cb);
                                     ctx.computeConstants(k, 12);
                                     ctx.cmd->Dispatch((count + 63) / 64, 1, 1);
                                 });
                out = tf.readbackBuffer(fc, o, (uint64_t)count * 17 * 4);
            });
            tf.frame.time += tf.frame.deltaTime;
            uint32_t classes[3] = {}, violations = 0, mixedUniform = 0, settled[3] = {}, total[3] = {};
            for (uint32_t i = 0; i < count; ++i)
            {
                float vals[17];
                std::memcpy(vals, out->data() + i * 17ull * 4, 17 * 4);
                const uint32_t cls = (uint32_t)vals[0];
                ++classes[std::min(cls, 2u)];
                bool allLit = true, allDark = true;
                for (int j = 1; j < 17; ++j)
                {
                    allLit = allLit && vals[j] >= 1 - 1e-6f;
                    allDark = allDark && vals[j] <= 1e-6f;
                }
                if (cls == 0 && (allLit || allDark)) ++mixedUniform;
                ++total[groupOf[i]];
                settled[groupOf[i]] += cls != 0 ? 1u : 0u;
                for (int j = 1; j < 17; ++j)
                    if ((cls == 1 && vals[j] < 1 - 1e-6f) || (cls == 2 && vals[j] > 1e-6f))
                    {
                        if (violations < 5) logf("  segment %u class %u: point %d visibility %.4f\n", i, cls, j - 1, vals[j]);
                        ++violations;
                    }
            }
            logf("segment classification: %u segments, lit %u, umbra %u, mixed %u (of those uniform at 16 points: %u)\n", count, classes[1], classes[2], classes[0], mixedUniform);
            report(violations == 0, "segment classification: lit / umbra contradicted by a point", violations, 0);
            logf("  settled: near surface %u / %u, free air %u / %u, grass %u / %u" "\n", settled[0], total[0], settled[1], total[1], settled[2], total[2]);
            report(settled[1] > total[1] / 2, "segment classification: settles most free-air segments", (double)settled[1] / std::max(total[1], 1u), 0.5);
        }

        // 2. Small camera move: the moved window's pages, all drawn.
        setCamera(cam.position + float3{ 0.3f, 0, 0.1f });
        runFrame(false);
        const shadow::VsmStats s3 = statsAfter();
        logf("camera move 0.32 m: requested %u, allocated %u, dirty %u\n", s3.requested, s3.allocated, s3.dirty);
        report(s3.dirty == s3.requested && s3.exhausted == 0, "camera move: every requested page drawn", s3.dirty, s3.requested);
        setCamera(cam.position);
        runFrame(false);
        runFrame(false);

        // 3. Moved caster: the next frame's pages hold it at its new place (no invalidation rule to get wrong).
        {
            std::vector<gpu::Instance> inst = tf.gpuScene.instances();
            gpu::Instance& pole = inst[2];
            for (int r = 0; r < 3; ++r) pole.prevObjectToWorld[r] = pole.objectToWorld[r];
            pole.objectToWorld[0].w += 0.6f;
            pole.objectToWorld[2].w -= 0.4f;
            pole.transformRevision += 1;
            ComPtr<ID3D12Resource> staging = tf.makeBuffer(sizeof(gpu::Instance) * inst.size(), D3D12_HEAP_TYPE_UPLOAD);
            void* p = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(staging->Map(0, &none, &p), "map instances");
            std::memcpy(p, inst.data(), sizeof(gpu::Instance) * inst.size());
            staging->Unmap(0, nullptr);
            CommandList cl = tf.device.acquireCommandList(QueueType::Graphics);
            cl.list->CopyBufferRegion(tf.gpuScene.buffer("instances"), 0, staging.Get(), 0, sizeof(gpu::Instance) * inst.size());
            tf.device.queue(QueueType::Graphics).waitCpu(tf.device.submit(cl));
            boxes[2].centre = boxes[2].centre + float3{ 0.6f, 0, -0.4f };
            Frame moved = runFrame(true);
            const shadow::VsmStats sm = statsAfter();
            logf("moved caster: requested %u, dirty %u\n", sm.requested, sm.dirty);
            report(sm.dirty == sm.requested, "moved caster: every requested page drawn", sm.dirty, sm.requested);
            compare(moved, "after caster move", 2);
            const shadow::VsmStats sn = statsAfter();
            report(sn.dirty == sn.requested, "caster at rest again: every requested page drawn", sn.dirty, sn.requested);
        }

        // 3b. Slow drift (2 mm per frame for 30 frames, 6 cm in all): every level is drawn every frame, and the shadow
        //     matches the reference at the end (no accumulated-motion rule).
        {
            std::vector<uint32_t> dirty(shadow::kLevels, 0);
            Frame last;
            for (int step = 0; step < 30; ++step)
            {
                std::vector<gpu::Instance> inst = tf.gpuScene.instances();
                gpu::Instance& pole = inst[2];
                pole.objectToWorld[0].w += 0.6f + 0.002f * (step + 1);  // instances() still holds the uploaded original
                pole.objectToWorld[2].w -= 0.4f;
                for (int r = 0; r < 3; ++r) pole.prevObjectToWorld[r] = pole.objectToWorld[r];
                pole.prevObjectToWorld[0].w -= 0.002f;
                pole.transformRevision += 2 + step;
                ComPtr<ID3D12Resource> staging = tf.makeBuffer(sizeof(gpu::Instance) * inst.size(), D3D12_HEAP_TYPE_UPLOAD);
                void* p = nullptr;
                D3D12_RANGE none{ 0, 0 };
                check(staging->Map(0, &none, &p), "map instances");
                std::memcpy(p, inst.data(), sizeof(gpu::Instance) * inst.size());
                staging->Unmap(0, nullptr);
                CommandList cl = tf.device.acquireCommandList(QueueType::Graphics);
                cl.list->CopyBufferRegion(tf.gpuScene.buffer("instances"), 0, staging.Get(), 0, sizeof(gpu::Instance) * inst.size());
                tf.device.queue(QueueType::Graphics).waitCpu(tf.device.submit(cl));
                last = runFrame(true);
                const std::vector<uint32_t> n = dirtyByLevel(last);
                for (uint32_t k = 0; k < shadow::kLevels; ++k) dirty[k] += n[k];
            }
            std::string line;
            for (uint32_t k = 0; k < 10; ++k) line += format(" %u:%u", k, dirty[k]);
            logf("slow drift 6 cm over 30 frames, dirty pages by level:%s\n", line.c_str());
            const shadow::VsmStats sd = shadow::stats(tf.trackState);
            report(sd.dirty == sd.requested, "slow drift: every requested page drawn each frame", sd.dirty, sd.requested);
            boxes[2].centre = boxes[2].centre + float3{ 0.06f, 0, 0 };
            compare(last, "after slow drift", 2);
        }

        // 4. Sun direction change: every requested page re-renders.
        {
            // The GpuScene points at tf.sceneData: frame constants and the VSM light basis both see the new sun.
            tf.sceneData.sun.direction = normalize(float3{ -0.3f, 0.85f, -0.45f });
            Frame f = runFrame(true);
            const shadow::VsmStats ss = statsAfter();
            report(ss.dirty == ss.requested && ss.requested > 0, "sun change: every requested page re-rendered", ss.dirty, ss.requested);
            compare(f, "after sun change", 2);
        }

        // 4b. Moving sun (time of day, any speed): every level is drawn on the current sun every frame, so there is no
        //     basis age; the shadow matches the reference at slow and fast turns.
        {
            const float tanSun = std::tan(tf.sceneData.sun.angularRadius);
            const float dthetaMax = 1.5707963f * tanSun * (float)tf.quality.number("shadow.vsm.sun_refresh_error");
            const float3 axis = normalize(cross(normalize(tf.sceneData.sun.direction), float3{ 0, 1, 0 }));
            auto turn = [&](float angle) {
                const float3 d = normalize(tf.sceneData.sun.direction);
                tf.sceneData.sun.direction = normalize(d * std::cos(angle) + cross(axis, d) * std::sin(angle));
            };
            for (const float step : { 0.3f, 3.0f })
            {
                uint32_t maxRefreshed = 0, minRefreshed = 1000, maxPages = 0, largestLevel = 0, totalPages = 0;
                float maxAge = 0;
                Frame last;
                for (int i = 0; i < 40; ++i)
                {
                    turn(step * dthetaMax);
                    last = runFrame(i == 39);
                    const shadow::VsmStats& st = shadow::stats(tf.trackState);
                    maxRefreshed = std::max(maxRefreshed, st.levelsRefreshed);
                    if (i >= 8) maxPages = std::max(maxPages, st.pagesRefreshed);  // after the page counts arrive
                    totalPages = 0;
                    largestLevel = 0;
                    for (uint32_t k = 0; k < shadow::kLevels; ++k)
                    {
                        totalPages += st.levelPages[k];
                        largestLevel = std::max(largestLevel, st.levelPages[k]);
                    }
                    minRefreshed = std::min(minRefreshed, st.levelsRefreshed);
                    maxAge = std::max(maxAge, st.largestBasisAge);
                }
                logf("moving sun %.1f x dthetaMax per frame (dthetaMax %.3g rad): levels refreshed per frame %u .. %u, largest basis age %.3g rad"
                     "\n", step, dthetaMax, minRefreshed, maxRefreshed, maxAge);
                const shadow::VsmStats& sf = shadow::stats(tf.trackState);
                report(maxAge == 0 && sf.dirty == sf.requested, format("moving sun x%.1f: every page drawn on the current sun", step).c_str(), maxAge, 0);
                compare(last, step < 1 ? "moving sun (slow)" : "moving sun (fast)", 2);
            }
        }

        // 5. Wind: every requested page is drawn each frame with the current sway.
        {
            scene::Scene windy = tf.sceneData;
            windy.windSpeed = 6;
            windy.windDirection = normalize(float3{ 1, 0, 0.3f });
            windy.instances[2].flags |= scene::InstanceWind;
            windy.instances[2].wind.stiffness = 40;
            windy.instances[2].wind.anchorHeight = -5;
            tf.setScene(windy);
            runFrame(false);  // scene reload: full re-render
            runFrame(false);
            std::vector<uint32_t> total(shadow::kLevels, 0);
            for (int i = 0; i < 20; ++i)
            {
                Frame f = runFrame(true);
                const std::vector<uint32_t> n = dirtyByLevel(f);
                for (uint32_t k = 0; k < shadow::kLevels; ++k) total[k] += n[k];
            }
            // Largest sway change of the pole's top (Deformation.hlsli v1): amplitude A = v^2 0.002 / stiffness h^2,
            // varying by 0.4 A over a period.
            const float h = 10.0f, A = windy.windSpeed * windy.windSpeed * 0.002f / 40 * h * h, sway = 0.8f * A;
            uint32_t coarseDirty = 0, fineDirty = 0;
            std::string line;
            for (uint32_t k = 0; k < shadow::kLevels; ++k)
            {
                const float texel = std::ldexp(1.0f, (int)k - 10);
                (texel >= sway ? coarseDirty : fineDirty) += total[k];
                line += format(" L%u:%u", k, total[k]);
            }
            logf("wind (sway range %.4f m) dirty pages over 20 frames by level:%s\n", sway, line.c_str());
            const shadow::VsmStats sw = shadow::stats(tf.trackState);
            report(sw.dirty == sw.requested && fineDirty + coarseDirty > 0, "wind: every requested page drawn each frame", sw.dirty, sw.requested);

            // 6. Wind change after commit (v1.23): the source scene's wind is edited before a frame (the host's path, no
            //    scene reload). (a) The endpoint bound holds for the v1 model over random transitions (C++ twin of
            //    windOffset / windChangeBound, Deformation.hlsli). (b) A gust (6 -> 6.3 m/s, direction +3 degrees)
            //    re-renders no level whose texel exceeds the bound; the pages were drawn with the old wind.
            {
                // Double precision: the twin checks the bound's algebra, not float rounding of sin at large times.
                struct D3v
                {
                    double x, y, z;
                };
                auto offset = [](D3v d, double K, double speed, double t, double phase) {
                    const double m = K * speed * speed * (0.6 + 0.4 * std::sin(1.7 * t + phase));
                    return D3v{ d.x * m, d.y * m, d.z * m };
                };
                auto dist = [](D3v a, D3v b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); };
                auto bound = [&](double K, double t0, double ws0, D3v d0, double t1, double ws1, D3v d1) {
                    return K * (ws1 * ws1 * 0.4 * std::min(2.0, 1.7 * std::abs(t1 - t0)) + std::abs(ws1 * ws1 - ws0 * ws0) + dist(d1, d0) * ws0 * ws0);
                };
                uint32_t violations = 0;
                double worstRatio = 0;
                uint32_t seed = 12345;
                auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0f; };
                for (int i = 0; i < 200000; ++i)
                {
                    const float K = 0.001f + rnd() * 0.01f, t0 = rnd() * 100, t1 = t0 + rnd() * rnd() * 5, phase = rnd() * 6.2832f;
                    const float ws0 = rnd() * 20, ws1 = rnd() < 0.3f ? ws0 : rnd() * 20;
                    const float a0 = rnd() * 6.2832f, a1 = rnd() < 0.3f ? a0 : a0 + (rnd() - 0.5f) * 3;
                    const D3v d0{ std::cos((double)a0), 0, std::sin((double)a0) }, d1{ std::cos((double)a1), 0, std::sin((double)a1) };
                    const double moved = dist(offset(d1, K, ws1, t1, phase), offset(d0, K, ws0, t0, phase));
                    const double b = bound(K, t0, ws0, d0, t1, ws1, d1);
                    if (moved > b * (1 + 1e-9) + 1e-15) ++violations;
                    if (b > 0) worstRatio = std::max(worstRatio, (double)moved / b);
                }
                logf("wind change bound: 200000 random transitions, %u violations, largest |displacement| / bound %.4f" "\n", violations, worstRatio);
                report(violations == 0, "wind change: endpoint bound holds (v1 model, random transitions)", violations, 0);

                // Steady pages with the old wind, then the gust (no reload: the source scene is edited in place).
                for (int i = 0; i < 4; ++i) runFrame(false);
                const float ws0 = tf.sceneData.windSpeed, ws1 = 6.3f;
                const float3 d0 = normalize(tf.sceneData.windDirection);
                const float a = 3.0f * 0.0174533f;
                const float3 d1 = normalize(float3{ d0.x * std::cos(a) - d0.z * std::sin(a), 0, d0.x * std::sin(a) + d0.z * std::cos(a) });
                tf.sceneData.windSpeed = ws1;
                tf.sceneData.windDirection = d1;
                const Frame fg = runFrame(true);
                const std::vector<uint32_t> n = dirtyByLevel(fg);
                // Largest bound any page can reach at this frame: time term over a full period, speed and direction terms.
                const float K = 0.002f / 40 * 10 * 10;
                const float worst = K * (ws1 * ws1 * 0.8f + std::abs(ws1 * ws1 - ws0 * ws0) + length(d1 - d0) * ws0 * ws0 + ws0 * ws0 * 2e-4f);
                const float change = K * (std::abs(ws1 * ws1 - ws0 * ws0) + length(d1 - d0) * ws0 * ws0);  // the gust alone
                uint32_t coarse = 0, fine = 0;
                std::string gustLine;
                for (uint32_t k = 0; k < shadow::kLevels; ++k)
                {
                    const float texel = std::ldexp(1.0f, (int)k - 10);
                    if (texel > worst) coarse += n[k];
                    if (texel < change) fine += n[k];
                    gustLine += format(" L%u:%u", k, n[k]);
                }
                logf("wind gust %.1f -> %.1f m/s, +3 deg (bound <= %.4f m, gust term %.4f m): dirty pages by level:%s" "\n", ws0, ws1, worst, change, gustLine.c_str());
                const shadow::VsmStats sg = shadow::stats(tf.trackState);
                report(sg.dirty == sg.requested && coarse + fine > 0, "wind change: every requested page drawn with the gust", sg.dirty, sg.requested);
            }
        }

        {
            // The block hierarchy (VsmPageMax) of every resident page against its atlas texels (VsmBlockCheck.hlsl).
            std::shared_ptr<std::vector<uint8_t>> check;
            tf.run([&](FramePassContext& fc) {
                ViewResources main;
                main.view = fc.frame.mainView;
                main.frameConstants = fc.frameConstantsFor(main.view);
                raster.mainView(fc, main);
                tracks::shadowPages(fc, main);
                check = vsmBlockCheck(tf, fc);
            });
            tf.frame.time += tf.frame.deltaTime;
            vsmBlockCheckReport(*check, "sun pages", report);
        }
        report(shadow::stats(tf.trackState).errorBitsSeen == 0, "S error bits (INTERFACES 3.6: a shader loop at its hard cap)",
               shadow::stats(tf.trackState).errorBitsSeen, 0);
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
