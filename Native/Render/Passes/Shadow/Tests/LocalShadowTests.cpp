// S local-light shadows, correctness (no GPU lock needed; run small). Test stand-ins for V's raster and M's G-buffer
// (TestRaster.h). A box over a ground plane lit by shadow-casting local lights (a sphere light of radius 0.1 m and a
// point light): visibility slot 1 of each pixel against the exact reference, the visible fraction of the light's disk
// seen from the receiver (stratified directions, ray-cast against the same boxes); hard and soft shadows; debug layer.
// Then the box moves 0.3 m: the local pages under its old and new bounds re-render (VsmLocalInvalidate) and the result
// matches the reference at the new position.
// Overflow list (INTERFACES 7.3, v1.20): six shadow-casting sphere lights around the box; every pixel's lights in froxel
// list order, the first three from slots 1-3 and the rest from shadowOverflowTiles / shadowOverflow, against the same
// reference; counts match the lists, no listed pixel is missing, and the steady state has no tile over the capacity.
//   unx_test_shadow_localshadowtests [--no-debug-layer] [--width W --height H] [--verbose N]
#include "TestRaster.h"

#include "FroxelSystem.h"
#include "VsmBlockCheck.h"
#include "VsmSystem.h"

#include <algorithm>
#include <cstdio>
#include <set>

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
            std::vector<Box> boxes = { { { 0, -0.05f, 0 }, { 6, 0.05f, 6 } }, { { 0, 0.9f, 0 }, { 0.4f, 0.4f, 0.4f } } };
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
            auto compareFrame = [&](const std::string& label, int frames) {
            std::vector<uint8_t> vis, depth, gbuffer;
            for (int frame = 0; frame < frames; ++frame)
            {
                std::shared_ptr<std::vector<uint8_t>> rv, rd, rg;
                const bool read = frame + 1 == frames;
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
            logf("%s: local shadow slots %u, raster-active %u, casting lights without a slot %u\n", label.c_str(), st.localAssigned, st.localActive, st.localWithoutSlot);
            report(st.localAssigned == 1 && st.localActive == 1, (label + ": one shadow slot, raster-active").c_str(), st.localAssigned, 1);

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
            logf("%s: %zu pixels (%d in shadow), penumbra %d (mean |e| %.4f), mean |e| %.5f, |e| > 0.25: %.4f %%\n", label.c_str(), errors.size(), shadowed, penCount,
                 penCount ? penSum / penCount : 0.0, mean, 100 * grossFraction);
            report(shadowed > 100, (label + ": pixels in the box's shadow").c_str(), shadowed, 100);
            report(mean < 0.01, (label + ": mean |V - V_ref|").c_str(), mean, 0.01);
            report(grossFraction < 2e-3, (label + ": fraction |V - V_ref| > 0.25").c_str(), grossFraction, 2e-3);
            report(penCount == 0 || penSum / penCount < 0.06, (label + ": penumbra mean |V - V_ref|").c_str(), penCount ? penSum / penCount : 0, 0.06);
            };
            compareFrame(cs.label, 2);
            // The box moves: its old and new bounds' local pages re-render.
            {
                std::vector<gpu::Instance> inst = tf.gpuScene.instances();
                gpu::Instance& box = inst[1];
                for (int r = 0; r < 3; ++r) box.prevObjectToWorld[r] = box.objectToWorld[r];
                box.objectToWorld[0].w += 0.3f;
                box.transformRevision += 1;
                ComPtr<ID3D12Resource> staging = tf.makeBuffer(sizeof(gpu::Instance) * inst.size(), D3D12_HEAP_TYPE_UPLOAD);
                void* p = nullptr;
                D3D12_RANGE nothing{ 0, 0 };
                check(staging->Map(0, &nothing, &p), "map instances");
                std::memcpy(p, inst.data(), sizeof(gpu::Instance) * inst.size());
                staging->Unmap(0, nullptr);
                CommandList cl = tf.device.acquireCommandList(QueueType::Graphics);
                cl.list->CopyBufferRegion(tf.gpuScene.buffer("instances"), 0, staging.Get(), 0, sizeof(gpu::Instance) * inst.size());
                tf.device.queue(QueueType::Graphics).waitCpu(tf.device.submit(cl));
                boxes[1].centre.x += 0.3f;
            }
            compareFrame(std::string(cs.label) + ", box moved", 1);
        }

        // Overflow list: six shadow-casting lights reach the lit froxels, three past the visibility slots.
        {
            scene::Scene sc;
            sc.name = "local shadow overflow test";
            sc.materials.push_back({});
            sc.meshes.push_back(boxMesh("ground", { 6, 0.05f, 6 }));
            sc.meshes.push_back(boxMesh("box", { 0.4f, 0.4f, 0.4f }));
            sc.instances.push_back(instanceAt(0, { 0, -0.05f, 0 }));
            sc.instances.push_back(instanceAt(1, { 0, 0.9f, 0 }));
            const std::vector<Box> boxes = { { { 0, -0.05f, 0 }, { 6, 0.05f, 6 } }, { { 0, 0.9f, 0 }, { 0.4f, 0.4f, 0.4f } } };
            sc.sun.direction = normalize(float3{ 0.3f, -0.6f, 0.2f });  // below the horizon
            const float radius = 0.1f;
            for (int i = 0; i < 6; ++i)
            {
                const float a = i * 1.0471976f;
                scene::Light l;
                l.type = scene::LightType::Sphere;
                l.position = { 1.3f * std::cos(a), 2.4f + 0.12f * i, 1.3f * std::sin(a) };
                l.intensity = 2000;
                l.range = 9;
                l.size = { radius, 0 };
                l.castShadow = true;
                sc.lights.push_back(l);
            }
            scene::Camera cam;
            cam.name = "main";
            cam.position = { -3.2f, 2.4f, -2.9f };
            cam.forward = normalize(float3{ 3.2f, -2.3f, 2.9f });
            sc.cameras.push_back(cam);
            tf.setScene(sc);
            tf.frame.mainView = ViewDesc::fromCamera(cam, W, H, float4x4{});
            tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;

            const shadow::FroxelGridCpu fg = shadow::froxelGridFor(tf.quality, W, H);
            const uint32_t tilesX = (W + 7) / 8;
            std::vector<uint8_t> vis, depth, gbuffer, heads, overflow, lists;
            uint32_t capacity = 0;
            // Frames enough for the pool and the overflow capacity to settle (the stats reach the CPU a few frames late).
            const int frames = 10;
            for (int frame = 0; frame < frames + 3; ++frame)
            {
                std::shared_ptr<std::vector<uint8_t>> rv, rd, rg, rh, ro, rl;
                const bool read = frame + 1 == frames;
                tf.run([&](FramePassContext& fc) {
                    ViewResources main;
                    main.view = fc.frame.mainView;
                    main.frameConstants = fc.frameConstantsFor(main.view);
                    raster.mainView(fc, main);
                    tracks::shadowPages(fc, main);
                    tracks::shadowVisibility(fc, main);
                    if (read)
                    {
                        capacity = shadow::stats(tf.trackState).overflowCapacity;
                        rv = tf.readback(fc, main.shadowVisibility);
                        rd = tf.readback(fc, main.depth);
                        rg = tf.readback(fc, main.gbuffer);
                        rh = tf.readback(fc, main.shadowOverflowTiles);
                        ro = tf.readbackBuffer(fc, main.shadowOverflow, (uint64_t)capacity * 4);
                        rl = tf.readbackBuffer(fc, fc.resources.froxelLights, shadow::froxelListBytes(fg, shadow::froxelListCapacity(tf.trackState)));
                    }
                });
                tf.frame.time += tf.frame.deltaTime;
                if (read)
                {
                    vis = *rv;
                    depth = *rd;
                    gbuffer = *rg;
                    heads = *rh;
                    overflow = *ro;
                    lists = *rl;
                }
            }
            auto word = [](const std::vector<uint8_t>& b, uint64_t byte) {
                uint32_t w = 0;
                if (byte + 4 <= b.size()) std::memcpy(&w, b.data() + byte, 4);
                return w;
            };
            const shadow::VsmStats& st = shadow::stats(tf.trackState);
            logf("overflow: frame %llu needed %u words (capacity %u), tiles over capacity %u (%u pixels), lights past the third %u; local slots %u\n",
                 (unsigned long long)st.frame, st.overflowWords, capacity, st.overflowOverTiles, st.overflowOverPixels, st.overflowLights, st.localAssigned);
            report(st.localAssigned == 6, "overflow: six shadow slots", st.localAssigned, 6);
            report(st.overflowOverTiles == 0, "overflow: steady state, tiles over the capacity", st.overflowOverTiles, 0);

            const uint32_t pv = TestFrame::rowPitch(W, 4), pg = TestFrame::rowPitch(W, 8), ph = TestFrame::rowPitch(tilesX, 4);
            const ViewDesc& v = tf.frame.mainView;
            const double logRatio = std::log2((double)fg.farM / fg.nearM);
            const uint32_t headerBase = word(lists, 32), indexBase = word(lists, 36);
            double sumSlots = 0, sumOverflow = 0;
            int nSlots = 0, nOverflow = 0, grossSlots = 0, grossOverflow = 0, badCount = 0, missing = 0, overCapacity = 0, printed = 0;
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
                    // The pixel's froxel list (skip pixels at a slice boundary, where float rounding may pick either).
                    const double z = dot(p - cam.position, cam.forward);
                    const double slc = std::log2(std::max(z, 1e-6) / fg.nearM) / logRatio * fg.slices;
                    if (std::abs(slc - std::round(slc)) < 1e-3) continue;
                    const uint32_t slice = (uint32_t)std::clamp(std::floor(slc), 0.0, (double)fg.slices - 1);
                    const uint32_t tx = std::min(x / fg.tilePx, fg.gridX - 1), ty = std::min(y / fg.tilePx, fg.gridY - 1);
                    const uint32_t froxel = (slice * fg.gridY + ty) * fg.gridX + tx;
                    const uint32_t first = word(lists, headerBase + froxel * 8ull), count = word(lists, headerBase + froxel * 8ull + 4);
                    const uint32_t past = count > 3 ? count - 3 : 0;  // every light of this scene casts shadows
                    const uint32_t head = word(heads, (y / 8) * ph + (x / 8) * 4ull);
                    uint32_t block = 0, pw = 0;
                    bool unlisted = false;  // head 0 with lights past the third: none of them may reach the pixel
                    if (past)
                    {
                        if (head == 0)
                        {
                            // A tile is listed only when a light past the third can shade one of its pixels
                            // (shadowLocalCanShadow: within its reach, farM >= range): here every such light is out of
                            // reach (M takes 1 for them), or the tile is missing.
                            for (uint32_t k = 3; k < count && !unlisted; ++k)
                            {
                                const uint32_t iw = word(lists, indexBase + ((first + k) >> 1) * 4ull);
                                const scene::Light& l = sc.lights.at(((first + k) & 1 ? iw >> 16 : iw & 0xFFFF) & 0x7FFF);
                                if (length(l.position - p) < l.range) unlisted = true;
                            }
                            if (unlisted)
                            {
                                ++missing;
                                continue;
                            }
                            unlisted = true;
                        }
                        if (head == 0xFFFFFFFFu)
                        {
                            ++overCapacity;
                            continue;
                        }
                        if (!unlisted)
                        {
                            block = head - 1;
                            pw = word(overflow, (block + (y % 8) * 8 + x % 8) * 4ull);
                        }
                    }
                    else if (head != 0 && head != 0xFFFFFFFFu)
                        pw = word(overflow, (head - 1 + (y % 8) * 8 + x % 8) * 4ull);
                    if (!unlisted && (pw >> 24) != past)
                    {
                        if (badCount++ < verbose) logf("  px (%u,%u): list has %u lights, overflow word count %u\n", x, y, count, pw >> 24);
                        continue;
                    }
                    for (uint32_t k = 0; k < count; ++k)
                    {
                        const uint32_t iw = word(lists, indexBase + ((first + k) >> 1) * 4ull);
                        const uint32_t li = ((first + k) & 1 ? iw >> 16 : iw & 0xFFFF) & 0x7FFF;
                        const scene::Light& l = sc.lights.at(li);
                        const float3 toLight = l.position - p;
                        if (length(toLight) > l.range * 0.95f || dot(n, normalize(toLight)) < 0.05f) continue;
                        if (unlisted && k >= 3) continue;  // (out of reach: not reached here)
                        float gpu;
                        if (k < 3)
                            gpu = ((packed >> (8 * (k + 1))) & 0xFF) / 255.0f;
                        else
                        {
                            const uint32_t j = k - 3;
                            gpu = ((word(overflow, (block + (pw & 0xFFFFFF) + j / 4) * 4ull) >> (8 * (j % 4))) & 0xFF) / 255.0f;
                        }
                        const float ref = referenceVisibility(boxes, p + n * 2e-4f, l.position, radius);
                        const double e = std::abs(gpu - ref);
                        if (e > 0.1 && printed < verbose)
                        {
                            ++printed;
                            logf("  px (%u,%u) light %u ordinal %u: gpu %.3f ref %.3f\n", x, y, li, k + 1, gpu, ref);
                        }
                        (k < 3 ? sumSlots : sumOverflow) += e;
                        ++(k < 3 ? nSlots : nOverflow);
                        if (e > 0.25) ++(k < 3 ? grossSlots : grossOverflow);
                    }
                }
            logf("overflow: %d slot samples (mean |e| %.5f), %d overflow samples (mean |e| %.5f), count mismatches %d, missing tiles %d, over capacity %d\n",
                 nSlots, nSlots ? sumSlots / nSlots : 0.0, nOverflow, nOverflow ? sumOverflow / nOverflow : 0.0, badCount, missing, overCapacity);
            report(nOverflow > 2000, "overflow: light samples past the third slot", nOverflow, 2000);
            report(badCount == 0, "overflow: pixel counts equal the lists' lights past the third", badCount, 0);
            report(missing == 0 && overCapacity == 0, "overflow: listed pixels without a block", missing + overCapacity, 0);
            report(nSlots && sumSlots / nSlots < 0.01, "overflow scene: slots 1-3 mean |V - V_ref|", nSlots ? sumSlots / nSlots : 1, 0.01);
            report(nOverflow && sumOverflow / nOverflow < 0.01, "overflow scene: lights past the third mean |V - V_ref|", nOverflow ? sumOverflow / nOverflow : 1, 0.01);
            report((double)grossOverflow / std::max(nOverflow, 1) < 2e-3, "overflow scene: fraction |V - V_ref| > 0.25 past the third",
                   (double)grossOverflow / std::max(nOverflow, 1), 2e-3);

            // L3 stage 1 (RENDERER_REDESIGN_V2 14.3-1/2): the (tile, light) lit classification over the classification
            // pages (shadow.vsm.classification_pages) against the reference ray cast: every surface pixel of a tile whose
            // (slice, entry) is marked lit sees that light unoccluded (visibility 1), the exactness the conservative
            // raster's nearest-over-texel depth promises. Lit pairs must exist in this scene (six casters over boxes).
            {
                tf.quality.applyOverride("shadow.vsm.classification_pages=true");
                std::shared_ptr<std::vector<uint8_t>> rt, rl2, rd2, rg2;
                const uint32_t tilesY = (H + 7) / 8;
                for (int frame = 0; frame < 3; ++frame)
                    tf.run([&](FramePassContext& fc) {
                        ViewResources main;
                        main.view = fc.frame.mainView;
                        main.frameConstants = fc.frameConstantsFor(main.view);
                        raster.mainView(fc, main);
                        tracks::shadowPages(fc, main);
                        tracks::shadowVisibility(fc, main);
                        if (frame == 2)
                        {
                            if (!fc.resources.vsmTileLit.valid()) fail("classification pages on, but no tile lit records");
                            rt = tf.readbackBuffer(fc, fc.resources.vsmTileLit, (uint64_t)tilesX * tilesY * 48);
                            rl2 = tf.readbackBuffer(fc, fc.resources.froxelLights, shadow::froxelListBytes(fg, shadow::froxelListCapacity(tf.trackState)));
                            rd2 = tf.readback(fc, main.depth);
                            rg2 = tf.readback(fc, main.gbuffer);
                        }
                    });
                tf.quality.applyOverride("shadow.vsm.classification_pages=false");
                const std::vector<uint8_t>& tl = *rt;
                const std::vector<uint8_t>& lists2 = *rl2;
                const std::vector<uint8_t>& depth2 = *rd2;
                const std::vector<uint8_t>& gbuffer2 = *rg2;
                const uint32_t headerBase2 = word(lists2, 32), indexBase2 = word(lists2, 36);
                uint32_t litPairs = 0, validTiles = 0, violations = 0, checkedPixels = 0, printedV = 0;
                for (uint32_t ty = 0; ty < tilesY; ++ty)
                    for (uint32_t tx = 0; tx < tilesX; ++tx)
                    {
                        const uint64_t o = (uint64_t)(ty * tilesX + tx) * 48;
                        if ((word(tl, o) & 1u) == 0) continue;
                        ++validTiles;
                        const uint32_t firstSlice = word(tl, o + 4), sliceCount = word(tl, o + 8);
                        const uint32_t ftx = std::min(tx * 8 / fg.tilePx, fg.gridX - 1), fty = std::min(ty * 8 / fg.tilePx, fg.gridY - 1);
                        for (uint32_t s = 0; s < sliceCount && s < 4; ++s)
                        {
                            const uint32_t lo = word(tl, o + 16 + s * 8), hi = word(tl, o + 20 + s * 8);
                            if (lo == 0 && hi == 0) continue;
                            const uint32_t slice = firstSlice + s;
                            const uint32_t froxel = (slice * fg.gridY + fty) * fg.gridX + ftx;
                            const uint32_t first = word(lists2, headerBase2 + froxel * 8ull), count = word(lists2, headerBase2 + froxel * 8ull + 4);
                            for (uint32_t i = 0; i < count && i < 64; ++i)
                            {
                                if ((((i < 32 ? lo : hi) >> (i & 31)) & 1u) == 0) continue;
                                ++litPairs;
                                const uint32_t iw = word(lists2, indexBase2 + ((first + i) >> 1) * 4ull);
                                const scene::Light& l = sc.lights.at(((first + i) & 1 ? iw >> 16 : iw & 0xFFFF) & 0x7FFF);
                                for (uint32_t py = ty * 8; py < std::min(ty * 8 + 8, H); ++py)
                                    for (uint32_t px = tx * 8; px < std::min(tx * 8 + 8, W); ++px)
                                    {
                                        float d;
                                        std::memcpy(&d, depth2.data() + py * pv + px * 4, 4);
                                        if (d <= 0) continue;
                                        uint32_t g2[2];
                                        std::memcpy(g2, gbuffer2.data() + py * pg + px * 8, 8);
                                        const float ndc[4] = { (px + 0.5f) / W * 2 - 1, 1 - (py + 0.5f) / H * 2, d, 1 };
                                        float wp[4] = {};
                                        for (int r = 0; r < 4; ++r)
                                            for (int c = 0; c < 4; ++c) wp[r] += v.invViewProj.m[r][c] * ndc[c];
                                        const float3 p{ wp[0] / wp[3], wp[1] / wp[3], wp[2] / wp[3] };
                                        const double z = dot(p - cam.position, cam.forward);
                                        const double slc = std::log2(std::max(z, 1e-6) / fg.nearM) / logRatio * fg.slices;
                                        if ((uint32_t)std::clamp(std::floor(slc), 0.0, (double)fg.slices - 1) != slice) continue;  // another slice's list
                                        const float3 n = octDecodeCpu(g2[0]);
                                        const float3 toLight = l.position - p;
                                        if (length(toLight) > l.range || dot(n, normalize(toLight)) <= 0.05f) continue;
                                        ++checkedPixels;
                                        const float ref = referenceVisibility(boxes, p + n * 2e-4f, l.position, radius);
                                        if (ref < 0.999f)
                                        {
                                            ++violations;
                                            if (printedV++ < 8) logf("  classification: tile (%u,%u) slice %u entry %u light %u marked lit, pixel (%u,%u) reference visibility %.3f\n", tx, ty, slice, i, (unsigned)(&l - sc.lights.data()), px, py, ref);
                                        }
                                    }
                            }
                        }
                    }
                logf("classification pages: %u valid tiles, %u lit (tile, light) pairs, %u pixels checked, %u occluded in the reference\n", validTiles, litPairs, checkedPixels, violations);
                report(validTiles > 100 && litPairs > 100, "classification pages: lit (tile, light) pairs present", litPairs, 100);
                report(violations == 0, "classification pages: every pixel of a lit pair is unoccluded in the reference (violations)", violations, 0);
            }

            // Two schedules of the same frame/VSM: compare logical overflow
            // bytes, not atomic allocation addresses or unused buffer padding.
            {
                std::shared_ptr<std::vector<uint8_t>> abHeads[2], abValues[2];
                const bool savedQueue = tf.quality.boolean("shadow.vsm.overflow_filter_queue");
                tf.run([&](FramePassContext& fc) {
                    ViewResources main;
                    main.view = fc.frame.mainView;
                    main.frameConstants = fc.frameConstantsFor(main.view);
                    raster.mainView(fc, main);
                    tracks::shadowPages(fc, main);
                    for (uint32_t mode = 0; mode < 2; ++mode)
                    {
                        tf.quality.applyOverride(mode ? "shadow.vsm.overflow_filter_queue=true" : "shadow.vsm.overflow_filter_queue=false");
                        ViewResources candidate = main;
                        tracks::shadowVisibility(fc, candidate);
                        abHeads[mode] = tf.readback(fc, candidate.shadowOverflowTiles);
                        abValues[mode] = tf.readbackBuffer(fc, candidate.shadowOverflow, (uint64_t)capacity * 4);
                    }
                });
                tf.quality.applyOverride(savedQueue ? "shadow.vsm.overflow_filter_queue=true" : "shadow.vsm.overflow_filter_queue=false");
                uint32_t different = 0, compared = 0;
                for (uint32_t y = 0; y < H; ++y)
                    for (uint32_t x = 0; x < W; ++x)
                    {
                        const uint32_t a = word(*abHeads[0], (y / 8) * ph + (x / 8) * 4ull);
                        const uint32_t b = word(*abHeads[1], (y / 8) * ph + (x / 8) * 4ull);
                        if (a == 0 || b == 0 || a == UINT32_MAX || b == UINT32_MAX)
                        {
                            different += a != b ? 1u : 0u;
                            continue;
                        }
                        const uint32_t pixelOffset = (y % 8) * 8 + x % 8;
                        const uint32_t ra = word(*abValues[0], (uint64_t)(a - 1 + pixelOffset) * 4);
                        const uint32_t rb = word(*abValues[1], (uint64_t)(b - 1 + pixelOffset) * 4);
                        different += (ra >> 24) != (rb >> 24) ? 1u : 0u;
                        for (uint32_t j = 0; j < std::min(ra >> 24, rb >> 24); ++j)
                        {
                            const uint32_t va = word(*abValues[0], (uint64_t)(a - 1 + (ra & 0xFFFFFFu) + j / 4) * 4);
                            const uint32_t vb = word(*abValues[1], (uint64_t)(b - 1 + (rb & 0xFFFFFFu) + j / 4) * 4);
                            different += ((va >> (8 * (j % 4))) & 255u) != ((vb >> (8 * (j % 4))) & 255u) ? 1u : 0u;
                            ++compared;
                        }
                    }
                report(compared > 2000 && different == 0, "overflow queued/inline visibility bytes identical", different, 0);
            }

            // Forced capacity 1 word (M's fallback test): every overflow tile is over it, listed once in the fallback tiles.
            {
                tf.quality.applyOverride("shadow.vsm.overflow_capacity_words=1");
                std::shared_ptr<std::vector<uint8_t>> rh, rf;
                const uint32_t tiles = tilesX * ((H + 7) / 8);
                tf.run([&](FramePassContext& fc) {
                    ViewResources main;
                    main.view = fc.frame.mainView;
                    main.frameConstants = fc.frameConstantsFor(main.view);
                    raster.mainView(fc, main);
                    tracks::shadowPages(fc, main);
                    tracks::shadowVisibility(fc, main);
                    rh = tf.readback(fc, main.shadowOverflowTiles);
                    rf = tf.readbackBuffer(fc, main.shadowOverflowFallbackTiles, 16 + (uint64_t)tiles * 4);
                });
                tf.frame.time += tf.frame.deltaTime;
                tf.quality.applyOverride("shadow.vsm.overflow_capacity_words=0");
                uint32_t over = 0, blocks = 0, listedWrong = 0;
                std::set<uint32_t> listed;
                const uint32_t count = word(*rf, 0);
                for (uint32_t i = 0; i < count; ++i) listed.insert(word(*rf, 16 + i * 4ull));
                for (uint32_t ty = 0; ty < (H + 7) / 8; ++ty)
                    for (uint32_t tx = 0; tx < tilesX; ++tx)
                    {
                        const uint32_t hd = word(*rh, ty * ph + tx * 4ull);
                        if (hd == 0xFFFFFFFFu)
                        {
                            ++over;
                            listedWrong += listed.count(ty << 16 | tx) ? 0 : 1;
                        }
                        else if (hd != 0) ++blocks;
                    }
                logf("overflow, capacity 1 word: %u tiles over capacity, %u with a block, fallback list %u (args %u %u %u)\n", over, blocks, count, word(*rf, 4),
                     word(*rf, 8), word(*rf, 12));
                report(over > 100 && blocks == 0, "overflow, capacity 1: every overflow tile over capacity", blocks, 0);
                report(count == over && listedWrong == 0 && word(*rf, 4) == count && word(*rf, 8) == 1 && word(*rf, 12) == 1,
                       "overflow, capacity 1: fallback list = the tiles over capacity, args (n, 1, 1)", (double)count - over + listedWrong, 0);
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
            vsmBlockCheckReport(*check, "local and sun pages", report);
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
