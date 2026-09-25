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

        // 1. First frame renders every requested page; the second renders none.
        Frame f1 = runFrame(true);
        const shadow::VsmStats s1 = statsAfter();
        logf("frame %llu: requested %u, allocated %u, dirty %u, exhausted %u, free %u\n", (unsigned long long)s1.frame, s1.requested, s1.allocated, s1.dirty, s1.exhausted, s1.freePages);
        report(s1.requested > 0 && s1.allocated == s1.requested && s1.dirty == s1.requested && s1.exhausted == 0, "first frame: every requested page allocated and rendered",
               s1.dirty, s1.requested);
        const shadow::VsmStats s2 = statsAfter();
        report(s2.requested == s1.requested && s2.dirty == 0 && s2.allocated == 0, "steady camera: nothing rendered (dirty pages)", s2.dirty, 0);
        Frame f2 = runFrame(true);
        compare(f2, "static", 2);

        // shadowSunVisibilityAt at the receivers of f2, footprints of 1, 4 and 16 pixels (ray cones wider than a pixel).
        {
            const uint32_t pv = TestFrame::rowPitch(W, 4), pg = TestFrame::rowPitch(W, 8);
            const ViewDesc& v = tf.frame.mainView;
            const float3 sun = normalize(tf.sceneData.sun.direction);
            const float tanTheta = std::tan(tf.sceneData.sun.angularRadius);
            std::vector<float4> points, normals;
            std::vector<float> refs, direct;
            const float factors[3] = { 1, 4, 16 };
            for (float factor : factors)
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
                        const float footprint = 2 * z * std::tan(0.5f * v.verticalFov) / H * factor;
                        points.push_back({ p.x, p.y, p.z, footprint });
                        normals.push_back({ n.x, n.y, n.z, 0 });
                        refs.push_back(referenceVisibility(boxes, p + n * 2e-4f, sun, tanTheta));
                        direct.push_back((packed & 0xFF) / 255.0f);
                    }
            const uint32_t count = (uint32_t)points.size(), perFactor = count / 3;
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
                                 },
                                 [=](PassContext& ctx) {
                                     const uint32_t k[12] = { ctx.srv(pts), ctx.srv(nrm), ctx.uav(o), count, ctx.srv(r.vsmPageTable), ctx.srv(r.vsmPool),
                                                              ctx.srv(r.vsmBlocks), ctx.srv(r.vsmSearchBound), r.vsmConstants, r.vsmLocalLights, r.vsmSlotOfLight, 0 };
                                     ctx.cmd->SetPipelineState(pso);
                                     ctx.bindFrameConstants(cb);
                                     ctx.computeConstants(k, 12);
                                     ctx.cmd->Dispatch((count + 63) / 64, 1, 1);
                                 });
                out = tf.readbackBuffer(fc, o, (uint64_t)count * 8);
            });
            tf.frame.time += tf.frame.deltaTime;
            for (uint32_t fi = 0; fi < 3; ++fi)
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
                logf("shadowSunVisibilityAt, footprint x%.0f: %u of %u resident, mean |V - V_ref| %.5f, bias %+.5f, mean |V - direct view| %.5f" "\n", factors[fi],
                     resident, perFactor, sumAbs / n, sumSigned / n, sumDirect / n);
                const double absLimit = fi == 0 ? 0.005 : fi == 1 ? 0.01 : 0.02;
                report(resident > perFactor * 0.9, format("shadowSunVisibilityAt x%.0f: resident fraction", factors[fi]).c_str(), (double)resident / std::max(perFactor, 1u), 0.9);
                report(sumAbs / n < absLimit, format("shadowSunVisibilityAt x%.0f: mean |V - V_ref|", factors[fi]).c_str(), sumAbs / n, absLimit);
                report(std::abs(sumSigned / n) < 0.002, format("shadowSunVisibilityAt x%.0f: |bias|", factors[fi]).c_str(), std::abs(sumSigned / n), 0.002);
            }
        }

        // 2. Small camera move: only newly visible pages render.
        setCamera(cam.position + float3{ 0.3f, 0, 0.1f });
        runFrame(false);
        const shadow::VsmStats s3 = statsAfter();
        logf("camera move 0.32 m: requested %u, allocated %u, dirty %u\n", s3.requested, s3.allocated, s3.dirty);
        report(s3.dirty > 0 && s3.dirty < s1.dirty / 3, "camera move: dirty pages well below a full re-render", s3.dirty, s1.dirty / 3.0);
        setCamera(cam.position);
        runFrame(false);
        runFrame(false);

        // 3. Moved caster: pages under the old and new bounds re-render; the result matches the reference again.
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
            report(sm.dirty > 0 && sm.dirty < s1.dirty, "moved caster: some pages re-rendered, not all", sm.dirty, s1.dirty);
            compare(moved, "after caster move", 2);
            // Next frame: the revision is unchanged -> nothing re-renders.
            const shadow::VsmStats sn = statsAfter();
            report(sn.dirty == 0, "caster at rest again: nothing rendered", sn.dirty, 0);
        }

        // 3b. Slow drift (2 mm per frame for 30 frames, 6 cm in all): a level re-renders the caster's pages only once
        //     the accumulated motion reaches one of its texels, so levels with texels coarser than 6 cm never do, the
        //     finest do often, and the shadow still matches the reference at the end.
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
            uint32_t coarse = 0, fine = 0;
            for (uint32_t k = 0; k < shadow::kLevels; ++k)
                (std::ldexp(1.0, (int)k - 10) > 0.06 ? coarse : fine) += dirty[k];
            report(coarse == 0, "slow drift: no re-render on levels with texel > the 6 cm drift", coarse, 0);
            report(fine > 0, "slow drift: levels with texel <= the drift re-render", fine, 1);
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

        // 5. Wind: levels whose texel exceeds the sway never re-render; finer levels do.
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
            report(coarseDirty == 0, "wind: no re-render on levels with texel >= sway", coarseDirty, 0);
            report(fineDirty > 0, "wind: fine levels re-render (pages)", fineDirty, 1);
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
