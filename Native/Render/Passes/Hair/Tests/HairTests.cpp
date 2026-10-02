// Track E hair correctness (B10; no GPU lock needed after the first run of new kernels).
//   1. fibre BSDF (HairBsdf.hlsli), 36 configurations (outgoing elevation, h, roughness) x 1M sample pairs:
//      - a non-absorbing fibre scatters all incident energy: the uniform-sphere estimate of the directional albedo is 1
//        (white furnace) within 3 standard errors + 0.3 %;
//      - the sampling pdf integrates to 1 (same tolerance), and hairSample draws from it: the sampled mean of wi.x equals
//        the pdf-weighted uniform estimate (within 4 standard errors + 2e-3);
//      - an absorbing fibre (sigma_a = 0.5, 1, 2) keeps its albedo below 1 and ordered by absorption.
//   2. guide simulation and follow strands (Hair.h, HairSimulate.hlsl), 64 guides x 12 nodes (2 cm segments), 60 Hz:
//      - under gravity for 2 s: every segment keeps its rest length (DFTL: |l - l0| <= 1e-4 l0), roots stay on their joint
//        (1e-5 m), tips droop; two identical bodies stay bit-identical (same GPU);
//      - no gravity, the joint turned 90 degrees at once: the strands return to the turned rest shape (max node error
//        after 4 s <= 10 % of the first tick's and <= 1 cm);
//      - a capsule under the strands: after 2 s no node is inside radius + margin (1e-5 m);
//      - wind along +z deflects the tips to +z;
//      - follow strands at rest: segment ends = guide node + offset (camera-relative, 1e-5 m), consecutive segments
//        share their end points, radii taper from root to tip; LOD far away keeps a subset with widths x 1 / fraction.
//   3. density volume (HairDensity.hlsl / .hlsli), two slabs of parallel strands (2 mm apart both ways, diameter 0.1 mm:
//      rho = 25 / m; 6.4 cm thick: (pi / 4) rho L = 1.2566 fibres across one), rays across them, means over 64 rays
//      (8 x 8 places over 12 x 6 cm: the strands' lattice and the segments' deposits beat with the cells):
//      - hairFibreCount across a slab within 5 %, to its middle (hairFibreCountWithin) half of it within 10 %;
//      - hairTransmittance across both slabs = exp(-the two bodies' counts) (the exponent within 5 %), and across the
//        first alone when the ray ends between them;
//      - hairFirstFibre: the probability of meeting a fibre in a slab = 1 - exp(-count) (within 5 %), the median of the
//        first fibre's place at the uniform slab's ln(1 - met / 2) / (-(pi / 4) rho) past its face (within 7 mm), the
//        quantiles in order and inside the slab.
//   unx_test_hair_hairtests [--no-debug-layer]
#include "../../Atmosphere/Tests/TestFrame.h"

#include "unx/hair/Hair.h"
#include "unx/render/Tracks.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;

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
        sc.name = "hair test";
        scene::Camera cam;
        cam.name = "main";
        sc.cameras.push_back(cam);
        tf.setScene(sc);

        // ---- 1. fibre BSDF
        {
            struct Config
            {
                float c[12];
                bool absorbing;
            };
            std::vector<Config> configs;
            const float elevations[3] = { -0.5f, 0.0f, 0.7f }, hs[3] = { -0.6f, 0.0f, 0.8f };
            const float roughness[4][2] = { { 0.3f, 0.3f }, { 0.3f, 0.6f }, { 0.6f, 0.3f }, { 0.6f, 0.6f } };
            for (float e : elevations)
                for (float h : hs)
                    for (const auto& r : roughness)
                    {
                        const float c = std::sqrt(1 - e * e);
                        configs.push_back({ { e, c * 0.6f, c * 0.8f, h, 0, 0, 0, 1.55f, r[0], r[1], 0.0349f, 0 }, false });
                    }
            configs.push_back({ { 0.2f, 0.9f, 0.3f, 0.3f, 0.5f, 1.0f, 2.0f, 1.55f, 0.3f, 0.3f, 0.0349f, 0 }, true });
            const uint32_t count = (uint32_t)configs.size(), perThread = 4096, threads = 256;
            std::vector<float> packed;
            for (const Config& c : configs) packed.insert(packed.end(), c.c, c.c + 12);
            std::shared_ptr<std::vector<uint8_t>> results;
            tf.run([&](FramePassContext& fc) {
                const BufferRef cfg = tf.uploadBuffer(fc, packed.data(), packed.size() * 4, 16, "hair.test.configs");
                const BufferRef out = fc.graph.createBuffer(BufferDesc{ "hair.test.results", (uint64_t)count * threads * 64, 16 });
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Hair/Tests/HairBsdfProbe");
                fc.graph.addPass("hair.test.bsdf", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     b.use(cfg, Use::SrvCompute);
                                     b.use(out, Use::UavCompute);
                                 },
                                 [=](PassContext& c) {
                                     const uint32_t k[4] = { c.srv(cfg), c.uav(out), perThread, count };
                                     c.cmd->SetPipelineState(pso);
                                     c.computeConstants(k, 4);
                                     c.cmd->Dispatch(count, 1, 1);
                                 });
                results = tf.readbackBuffer(fc, out, (uint64_t)count * threads * 64);
            });
            const float* r = reinterpret_cast<const float*>(results->data());
            const double N = (double)threads * perThread;
            double worstFurnace = 0, worstPdf = 0, worstMoment = 0;
            for (uint32_t k = 0; k < count; ++k)
            {
                // per-thread partial sums -> mean and standard error of the mean (threads as batches)
                double sum[9] = {}, sq[9] = {};
                for (uint32_t t = 0; t < threads; ++t)
                {
                    const float* v = r + 16 * ((size_t)k * threads + t);
                    const double x[9] = { v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8] };  // albedoU rgb, pdfU, albedoI rgb, momentU, momentI
                    for (int i = 0; i < 9; ++i)
                    {
                        const double m = x[i] / perThread;
                        sum[i] += m;
                        sq[i] += m * m;
                    }
                }
                double mean[9], se[9];
                for (int i = 0; i < 9; ++i)
                {
                    mean[i] = sum[i] / threads;
                    se[i] = std::sqrt(std::max(0.0, sq[i] / threads - mean[i] * mean[i]) / threads);
                }
                if (!configs[k].absorbing)
                {
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        const double e = std::abs(mean[ch] - 1);
                        worstFurnace = std::max(worstFurnace, e);
                        S_CHECK(e <= 3 * se[ch] + 3e-3, "config %u: white furnace albedo[%d] = %.5f (se %.1e)", k, ch, mean[ch], se[ch]);
                    }
                    const double pe = std::abs(mean[3] - 1);
                    worstPdf = std::max(worstPdf, pe);
                    S_CHECK(pe <= 3 * se[3] + 3e-3, "config %u: pdf integrates to %.5f (se %.1e)", k, mean[3], se[3]);
                }
                const double me = std::abs(mean[8] - mean[7]);
                worstMoment = std::max(worstMoment, me);
                S_CHECK(me <= 4 * std::sqrt(se[7] * se[7] + se[8] * se[8]) + 2e-3, "config %u: sampled E[wi.x] %.5f vs pdf %.5f", k, mean[8], mean[7]);
                if (configs[k].absorbing)
                {
                    S_CHECK(mean[0] < 1 && mean[1] < mean[0] && mean[2] < mean[1], "absorbing fibre albedo (%.4f, %.4f, %.4f) not below 1 and ordered", mean[0], mean[1], mean[2]);
                    S_CHECK(std::abs(mean[4] - mean[0]) <= 4 * std::sqrt(se[0] * se[0] + se[4] * se[4]) + 3e-3, "absorbing: importance %.5f vs uniform %.5f", mean[4], mean[0]);
                    std::printf("absorbing fibre (sigma_a 0.5 / 1 / 2): albedo %.4f / %.4f / %.4f\n", mean[0], mean[1], mean[2]);
                }
            }
            std::printf("hair bsdf: %u configurations x %.0f samples - white furnace worst |1 - albedo| %.2e, pdf normalization %.2e, sampler moment %.2e\n", count, N,
                        worstFurnace, worstPdf, worstMoment);
        }

        // ---- 2. guide simulation and follow strands
        {
            hair::HairSystem& hs = hair::hairSystem(tf.trackState);
            tf.frame.mainView = ViewDesc::fromCamera(cam, 1280, 720, float4x4{});
            const uint32_t N = 12, G = 64;
            auto makeBody = [&](bool gravity, uint32_t followsPerGuide) {
                hair::BodyDesc d;
                d.nodesPerStrand = N;
                d.joints = 1;
                for (uint32_t g = 0; g < G; ++g)
                {
                    for (uint32_t i = 0; i < N; ++i) d.restPositions.push_back({ 0.02f * i, 0, 0.004f * g - 0.128f });
                    d.guideJoint.push_back(0);
                    for (uint32_t k = 0; k < followsPerGuide; ++k) d.follows.push_back({ g, float3{ 0, 0.0015f * (k + 1), 0 }, 1.0f });
                }
                if (!gravity) d.params.gravity = { 0, 0, 0 };
                return hs.addBody(d);
            };
            float3x4 joint;  // at (0, 1, 0)
            joint.m[1][3] = 1;
            const float dt = 1.0f / 60;
            auto readState = [&](uint32_t body) {
                std::shared_ptr<std::vector<uint8_t>> data;
                tf.run([&](FramePassContext& fc) {
                    ViewResources v;
                    v.view = tf.frame.mainView;
                    tracks::hair(fc, v);
                    data = tf.readbackBuffer(fc, hs.tickState(body), (uint64_t)G * N * 16);
                });
                std::vector<float3> out(G * N);
                for (uint32_t n = 0; n < G * N; ++n) std::memcpy(&out[n], data->data() + 16 * n, 12);
                return out;
            };
            auto run = [&](const std::vector<uint32_t>& bodies, int ticks, const float3x4& j, const std::vector<hair::Capsule>& caps, float3 wind) {
                for (int t = 0; t < ticks; ++t)
                {
                    for (uint32_t b : bodies) hs.tick(b, &j, 1, caps.data(), (uint32_t)caps.size(), wind, dt);
                    tf.run([&](FramePassContext& fc) {
                        ViewResources v;
                        v.view = tf.frame.mainView;
                        tracks::hair(fc, v);
                    });
                }
            };
            // a: gravity, twins
            const uint32_t a0 = makeBody(true, 0), a1 = makeBody(true, 0);
            for (uint32_t b : { a0, a1 }) hs.tick(b, &joint, 1, nullptr, 0, { 0, 0, 0 }, dt);  // first tick: start at rest
            run({ a0, a1 }, 120, joint, {}, { 0, 0, 0 });
            const std::vector<float3> s0 = readState(a0), s1 = readState(a1);
            double worstLength = 0, worstRoot = 0, droop = 0;
            for (uint32_t g = 0; g < G; ++g)
            {
                const float3 root = s0[g * N];
                worstRoot = std::max(worstRoot, (double)length(root - float3{ 0, 1, 0.004f * g - 0.128f }));
                for (uint32_t i = 0; i + 1 < N; ++i) worstLength = std::max(worstLength, std::abs(length(s0[g * N + i + 1] - s0[g * N + i]) - 0.02) / 0.02);
                droop += (root.y - s0[g * N + N - 1].y) / G;
            }
            S_CHECK(worstLength <= 1e-4, "segment length off by %.2e relative", worstLength);
            S_CHECK(worstRoot <= 1e-5, "root %.2e m off its joint", worstRoot);
            S_CHECK(droop > 0.05, "tips drooped only %.4f m", droop);
            S_CHECK(std::memcmp(s0.data(), s1.data(), s0.size() * sizeof(float3)) == 0, "twin bodies differ");
            std::printf("hair gravity: 2 s, lengths within %.1e, roots within %.1e m, mean droop %.3f m, twins bit-identical\n", worstLength, worstRoot, droop);
            hs.removeBody(a0);
            hs.removeBody(a1);

            // b: shape recovery after a sudden 90-degree turn of the joint (no gravity)
            const uint32_t b0 = makeBody(false, 0);
            hs.tick(b0, &joint, 1, nullptr, 0, { 0, 0, 0 }, dt);
            float3x4 turned = joint;  // 90 degrees about z: x -> y
            turned.m[0][0] = 0; turned.m[0][1] = -1;
            turned.m[1][0] = 1; turned.m[1][1] = 0;
            auto shapeError = [&](const std::vector<float3>& st) {
                double e = 0;
                for (uint32_t g = 0; g < G; ++g)
                    for (uint32_t i = 0; i < N; ++i)
                    {
                        const float3 rest{ 0.02f * i, 0, 0.004f * g - 0.128f };
                        const float3 target = turned.transformPoint(rest);
                        e = std::max(e, (double)length(st[g * N + i] - target));
                    }
                return e;
            };
            run({ b0 }, 1, turned, {}, { 0, 0, 0 });
            const double e0 = shapeError(readState(b0));
            run({ b0 }, 240, turned, {}, { 0, 0, 0 });
            const double e1 = shapeError(readState(b0));
            S_CHECK(e1 <= 0.1 * e0 && e1 <= 0.01, "shape error %.4f m after 4 s (first tick %.4f m)", e1, e0);
            std::printf("hair shape: after a 90-degree joint turn the max node error falls from %.4f m to %.5f m in 4 s\n", e0, e1);
            hs.removeBody(b0);

            // c: capsule under the strands, d: wind
            const uint32_t c0 = makeBody(true, 0), w0 = makeBody(false, 0);
            for (uint32_t b : { c0, w0 }) hs.tick(b, &joint, 1, nullptr, 0, { 0, 0, 0 }, dt);
            const hair::Capsule shoulder{ float3{ 0.12f, 0.93f, -0.3f }, 0.05f, float3{ 0.12f, 0.93f, 0.3f } };
            for (int t = 0; t < 120; ++t)
            {
                hs.tick(c0, &joint, 1, &shoulder, 1, { 0, 0, 0 }, dt);
                hs.tick(w0, &joint, 1, nullptr, 0, { 0, 0, 5 }, dt);
                tf.run([&](FramePassContext& fc) {
                    ViewResources v;
                    v.view = tf.frame.mainView;
                    tracks::hair(fc, v);
                });
            }
            const std::vector<float3> sc0 = readState(c0), sw0 = readState(w0);
            double deepest = 0, tipZ = 0;
            for (uint32_t n = 0; n < G * N; ++n)
            {
                const float3 p = sc0[n];
                const float3 a = shoulder.a, ab = shoulder.b - shoulder.a;
                const float t = std::clamp(dot(p - a, ab) / dot(ab, ab), 0.0f, 1.0f);
                const float d = length(p - (a + ab * t));
                deepest = std::max(deepest, (double)(shoulder.radius + 0.002f - d));
            }
            for (uint32_t g = 0; g < G; ++g) tipZ += (sw0[g * N + N - 1].z - (0.004f * g - 0.128f)) / G;
            S_CHECK(deepest <= 1e-5, "a node is %.2e m inside the capsule", deepest);
            S_CHECK(tipZ > 0.02, "wind moved the tips only %.4f m along +z", tipZ);
            std::printf("hair collision: deepest %.1e m inside radius + margin; wind: tips +%.3f m along +z\n", deepest, tipZ);
            hs.removeBody(c0);
            hs.removeBody(w0);

            // e: follow strands at rest, segments, LOD
            const uint32_t f0 = makeBody(false, 2);
            hs.tick(f0, &joint, 1, nullptr, 0, { 0, 0, 0 }, dt);
            auto segmentsOf = [&](float3 cameraAt) {
                scene::Camera cam2 = cam;
                cam2.position = cameraAt;
                cam2.forward = normalize(float3{ 0, 1, 0 } - cameraAt);
                tf.frame.mainView = ViewDesc::fromCamera(cam2, 1280, 720, float4x4{});
                std::shared_ptr<std::vector<uint8_t>> seg, head;
                tf.run([&](FramePassContext& fc) {
                    ViewResources v;
                    v.view = tf.frame.mainView;
                    tracks::hair(fc, v);
                    seg = tf.readbackBuffer(fc, fc.resources.hairSegments, (uint64_t)G * 2 * (N - 1) * 32);
                    head = tf.readbackBuffer(fc, fc.resources.hairBodies, 16 + hair::kBodyHeaderWords * 4);
                });
                return std::make_pair(seg, head);
            };
            const float3 nearCam{ 0.1f, 1.2f, 0.6f };
            auto [seg, head] = segmentsOf(nearCam);
            const float* sg = reinterpret_cast<const float*>(seg->data());
            double worstPlace = 0;
            for (uint32_t f = 0; f < G * 2; ++f)
            {
                const uint32_t g = f / 2, k = f % 2;
                for (uint32_t i = 0; i + 1 < N; ++i)
                {
                    const float* r = sg + 8 * (f * (N - 1) + i);
                    const float3 p0{ r[0], r[1], r[2] }, p1{ r[4], r[5], r[6] };
                    const float3 q0 = float3{ 0.02f * i, 1 + 0.0015f * (k + 1), 0.004f * g - 0.128f } - nearCam;
                    const float3 q1 = float3{ 0.02f * (i + 1), 1 + 0.0015f * (k + 1), 0.004f * g - 0.128f } - nearCam;
                    worstPlace = std::max({ worstPlace, (double)length(p0 - q0), (double)length(p1 - q1) });
                    if (i + 2 < N) S_CHECK(std::memcmp(r + 4, r + 8, 12) == 0, "follow %u: segment %u does not end where %u starts", f, i, i + 1);
                    S_CHECK(r[3] >= r[7] && r[7] > 0, "follow %u segment %u: radii %.3g -> %.3g do not taper", f, i, r[3], r[7]);
                }
            }
            S_CHECK(worstPlace <= 1e-5, "follow strand node %.2e m off guide + offset", worstPlace);
            const uint32_t* h0 = reinterpret_cast<const uint32_t*>(head->data());
            float frac0;
            std::memcpy(&frac0, h0 + 1 + 5, 4);
            S_CHECK(h0[0] == 1 && h0[1 + 1] == G * 2 * (N - 1) && frac0 == 1.0f, "near header: bodies %u, segments %u, fraction %.3f", h0[0], h0[2], frac0);
            // far: a subset with widths x 1 / fraction
            auto [segFar, headFar] = segmentsOf(float3{ 0, 1.5f, 400 });
            const uint32_t* h1 = reinterpret_cast<const uint32_t*>(headFar->data());
            float frac1, scale1;
            std::memcpy(&frac1, h1 + 1 + 5, 4);
            std::memcpy(&scale1, h1 + 1 + 6, 4);
            const float* sf = reinterpret_cast<const float*>(segFar->data());
            uint32_t kept = 0;
            for (uint32_t f = 0; f < G * 2; ++f)
            {
                const float r0 = sf[8 * (f * (N - 1)) + 3];
                if (r0 > 0)
                {
                    ++kept;
                    S_CHECK(std::abs(r0 - 40e-6f * scale1) <= 1e-9f, "kept strand root radius %.4g, expected %.4g", r0, 40e-6f * scale1);
                }
            }
            S_CHECK(frac1 < 1 && std::abs(scale1 * frac1 - 1) < 1e-5 && kept > 0 && kept < G * 2, "far LOD: fraction %.4f, scale %.2f, kept %u of %u", frac1, scale1, kept, G * 2);
            std::printf("hair follows: nodes within %.1e m of guide + offset, continuous tapered segments; far LOD keeps %u of %u (fraction %.3f), widths x %.1f\n",
                        worstPlace, kept, G * 2, frac1, scale1);
            hs.removeBody(f0);
        }
        // ---- 3. density volume
        {
            hair::HairSystem& hs = hair::hairSystem(tf.trackState);
            scene::Camera cam3 = cam;
            cam3.position = { 0.11f, 1.1f, 0.4f };  // (near: the LOD keeps every strand)
            cam3.forward = normalize(float3{ 0.11f, 1.1f, 0 } - cam3.position);
            tf.frame.mainView = ViewDesc::fromCamera(cam3, 1280, 720, float4x4{});
            const uint32_t N = 12, GY = 32, GZ = 64;
            const float spacing = 0.002f, radius = 0.05e-3f;
            auto makeSlab = [&](float y0) {
                hair::BodyDesc d;
                d.nodesPerStrand = N;
                d.joints = 1;
                d.rootRadius = d.tipRadius = radius;
                d.params.gravity = { 0, 0, 0 };
                for (uint32_t gy = 0; gy < GY; ++gy)
                    for (uint32_t gz = 0; gz < GZ; ++gz)
                    {
                        for (uint32_t i = 0; i < N; ++i) d.restPositions.push_back({ 0.02f * i, y0 + spacing * (gy + 0.5f), spacing * (gz + 0.5f) - 0.064f });
                        d.guideJoint.push_back(0);
                        d.follows.push_back({ gy * GZ + gz, float3{ 0, 0, 0 }, 1.0f });
                    }
                return hs.addBody(d);
            };
            float3x4 joint;  // at (0, 1, 0): slab A fills y in [1, 1.064], slab B y in [1.2, 1.264]
            joint.m[1][3] = 1;
            const uint32_t slabA = makeSlab(0.0f), slabB = makeSlab(0.2f);
            for (uint32_t b : { slabA, slabB }) hs.tick(b, &joint, 1, nullptr, 0, { 0, 0, 0 }, 1.0f / 60);
            // probes: rays along +y from y = 0.85, 8 x 8 places in x and z (steps that share no period with the cells); per place
            //   0 body A to the box's end, reach = far; 1 body A, reach to the slab's middle (y = 1.032); 2 reach between the slabs
            //   (y = 1.13); 3..5 body A's first fibre at u = 0.001, 0.5, 0.999
            const uint32_t side = 8, places = side * side, kinds = 6;
            std::vector<float> probes;
            const float3 camera = cam3.position;
            for (uint32_t z = 0; z < places; ++z)
                for (uint32_t kind = 0; kind < kinds; ++kind)
                {
                    const float3 p = float3{ 0.05f + 0.017f * (z % side), 0.85f, -0.03f + 0.0085f * (z / side) } - camera;
                    const float reach = kind == 1 ? 1.032f - 0.85f : (kind == 2 ? 1.13f - 0.85f : 10.0f);
                    const float u = kind == 3 ? 0.001f : (kind == 5 ? 0.999f : 0.5f);
                    const float c[12] = { p.x, p.y, p.z, reach, 0, 1, 0, u, slabA < slabB ? 0.0f : 1.0f, 0, 0, 0 };  // (slab A's place in the frame's bodies)
                    probes.insert(probes.end(), c, c + 12);
                }
            const uint32_t count = places * kinds;
            std::shared_ptr<std::vector<uint8_t>> results, header;
            bool volume = false;
            tf.run([&](FramePassContext& fc) {
                ViewResources v;
                v.view = tf.frame.mainView;
                tracks::hair(fc, v);
                const FrameResources r = fc.resources;
                volume = r.hairDensityParams.valid() && r.hairDensity.valid() && r.hairDensityCoarse.valid();
                if (!volume) return;
                const BufferRef cfg = tf.uploadBuffer(fc, probes.data(), probes.size() * 4, 16, "hair.test.probes");
                const BufferRef out = fc.graph.createBuffer(BufferDesc{ "hair.test.density", (uint64_t)count * 32, 16 });
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Hair/Tests/HairDensityProbe");
                const uint32_t steps = (uint32_t)fc.quality.integer("shading.hair_shadow_steps");
                fc.graph.addPass("hair.test.density", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     b.use(cfg, Use::SrvCompute);
                                     b.use(r.hairDensityParams, Use::SrvCompute);
                                     b.use(r.hairDensity, Use::SrvCompute);
                                     b.use(r.hairDensityCoarse, Use::SrvCompute);
                                     b.use(out, Use::UavCompute);
                                 },
                                 [=](PassContext& c) {
                                     const uint32_t k[8] = { c.srv(cfg), c.uav(out), c.srv(r.hairDensityParams), count, steps, 0, 0, 0 };
                                     c.cmd->SetPipelineState(pso);
                                     c.computeConstants(k, 8);
                                     c.cmd->Dispatch((count + 63) / 64, 1, 1);
                                 });
                results = tf.readbackBuffer(fc, out, (uint64_t)count * 32);
                header = tf.readbackBuffer(fc, r.hairBodies, 4 + 2 * hair::kBodyHeaderWords * 4);
            });
            S_CHECK(volume, "the frame made no density volume (visibility.coverage_hair, shading.hair_density_resolution)");
            const uint32_t* hw = reinterpret_cast<const uint32_t*>(header->data());
            float kept[2];
            std::memcpy(&kept[0], hw + 1 + 5, 4);
            std::memcpy(&kept[1], hw + 1 + hair::kBodyHeaderWords + 5, 4);
            S_CHECK(hw[0] == 2 && kept[0] == 1.0f && kept[1] == 1.0f, "bodies %u, LOD fractions %.3f, %.3f (every strand expected)", hw[0], kept[0], kept[1]);
            const float* rs = reinterpret_cast<const float*>(results->data());
            const double kPi = 3.14159265358979, rho = 2 * radius / (spacing * spacing), mu = 0.25 * kPi * rho, across = mu * GY * spacing;
            double mean[kinds][5] = {};
            for (uint32_t z = 0; z < places; ++z)
                for (uint32_t kind = 0; kind < kinds; ++kind)
                    for (int i = 0; i < 5; ++i) mean[kind][i] += rs[8 * (z * kinds + kind) + i] / places;
            const double full = mean[0][0], half = mean[1][1], both = mean[0][2], firstOnly = mean[2][2], met = mean[4][4];
            const double median = (1.0 - 0.85) - std::log(1 - 0.5 * (1 - std::exp(-across))) / mu;
            std::printf("hair density: fibres across a slab %.4f (expected %.4f), to its middle %.4f; transmittance of two slabs %.5f, of one %.5f; first fibre met with "
                        "probability %.4f, at %.4f / %.4f / %.4f m (quantiles 0.001, 0.5, 0.999; median expected %.4f)\n",
                        full, across, half, both, firstOnly, met, mean[3][3], mean[4][3], mean[5][3], median);
            S_CHECK(std::abs(full - across) <= 0.05 * across, "fibres across a slab: %.4f, expected %.4f", full, across);
            S_CHECK(std::abs(half - 0.5 * across) <= 0.1 * across, "fibres to the slab's middle: %.4f, expected %.4f", half, 0.5 * across);
            S_CHECK(both > 0 && std::abs(-std::log(both) - 2 * full) <= 0.05 * 2 * full, "transmittance across both slabs: %.5f = exp(-%.4f), the two counts %.4f", both,
                    -std::log(both), 2 * full);
            S_CHECK(firstOnly > 0 && std::abs(-std::log(firstOnly) - full) <= 0.05 * full, "transmittance of a ray that ends between the slabs: %.5f = exp(-%.4f), the count %.4f",
                    firstOnly, -std::log(firstOnly), full);
            S_CHECK(std::abs(met - (1 - std::exp(-full))) <= 0.05 * (1 - std::exp(-full)), "a ray meets a fibre with probability %.4f, expected %.4f", met, 1 - std::exp(-full));
            S_CHECK(std::abs(mean[4][3] - median) <= 0.007, "median place of the first fibre: %.4f m along the ray, expected %.4f", mean[4][3], median);
            S_CHECK(mean[3][3] < mean[4][3] && mean[4][3] < mean[5][3] && mean[3][3] >= 0.15 - 0.011 && mean[5][3] <= 0.214 + 0.011,
                    "first fibre quantiles %.4f < %.4f < %.4f m, inside the slab [0.150, 0.214] m", mean[3][3], mean[4][3], mean[5][3]);
            hs.removeBody(slabA);
            hs.removeBody(slabB);
        }
        const uint32_t errors = tf.device.drainDebugMessages();
        S_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("hair tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
