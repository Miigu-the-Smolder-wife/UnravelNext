// S froxels, correctness (no GPU lock). Test stand-ins for V's raster and M's G-buffer (TestRaster.h).
//  1. light lists (scene of scattered lights plus a dense cluster): every light that reaches a point of a froxel is in
//     its list unless the list is full with lights at least as important; lists ordered by importance; no duplicates;
//     truncation statistics match; lists identical over two frames; 1b. FX particle lights (A3) at the light buffer's tail
//     join the lists the same way, without the shadow flag;
//  2. local lights' in-scattering by the air (sun below the horizon, lists not truncated): each node of the air volume
//     with the lights minus without them, against a double-precision integral along the tile-centre ray (every light,
//     fine steps, continuous air coefficients); then with shadows, then with light functions on the point and spot lights
//     (A8: IES profiles over both angles, rotation, intensity and colour keys; the reference multiplies its integrand by E's
//     CPU lights::evaluate);
//  3. the sun's shadowed air: per slice, the in-scattering removed by a roof (volume with the roof minus without it)
//     against the exact integral with the roof ray-cast towards the sun; slices entirely lit or entirely shadowed must
//     match to the storage precision (the block hierarchy classifies them exactly), all slices within the
//     texel-resolution bound;
//  4. the air perspective (atmosphereAirView = atmosphereAerial + sun illuminance, no casters or lights) at arbitrary
//     (uv, depth) against the double-precision atmosphere reference along the pixel's own ray (AtmosphereReference.h);
//  5. D3D12 debug layer clean;
//  7. particle media (E's volumeSlices; synthetic, MediaSlices.hlsl): the air volume with media against the air-only
//     volume of the same view composed with them on the CPU (uniform mixture per slice: L = air g(ta + tp) / g(ta) +
//     S_p g(ta + tp) / g(tp), optical depth ta + tp), node by node; the sky correction (source' - air e^-(tp to far after
//     the slice)) and the media's optical depth to far_m (sky pixels).
//   unx_test_shadow_froxeltests [--no-debug-layer] [--width W --height H] [--set key=value]
#include "TestRaster.h"

#include "../../Atmosphere/AtmosphereReference.h"
#include "../../Atmosphere/AtmosphereSystem.h"
#include "FroxelSystem.h"
#include "VsmSystem.h"
#if __has_include("unx/lights/LightFunctions.h")  // E's module (A8); without it section 2 skips its third variant
#include "unx/lights/LightFunctions.h"
#define FROXEL_TEST_LIGHT_FUNCTIONS 1
#endif

#include <algorithm>
#include <cstdio>
#include <functional>
#include <random>
#include <set>

using namespace unx;
using namespace unx::render;
using namespace unx::stest;
namespace ref = unx::render::atmosphere::reference;

namespace
{
constexpr double kPi = 3.14159265358979323846;

ref::D3 d3(float3 v) { return { v.x, v.y, v.z }; }

float halfToFloat(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = std::ldexp((float)m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}

// Nearest fp16 value of v (normal and subnormal range, ties to even): what a float written to an RGBA16F texel stores.
double roundHalf(double v)
{
    if (v == 0) return 0;
    const double a = std::abs(v);
    const int e = std::max(std::ilogb(a), -14);  // subnormals share the smallest exponent's spacing
    const double spacing = std::ldexp(1.0, e - 10);
    const double q = std::nearbyint(a / spacing) * spacing;  // default rounding mode: ties to even
    return v < 0 ? -q : q;
}

struct Box
{
    float3 centre, half;
};

// Segment o -> o + d (t in (0, 1)).
bool hitSegment(const Box& b, ref::D3 o, ref::D3 d)
{
    double t0 = 1e-6, t1 = 1 - 1e-6;
    const double oc[3] = { o.x, o.y, o.z }, dc[3] = { d.x, d.y, d.z };
    const double c[3] = { b.centre.x, b.centre.y, b.centre.z }, h[3] = { b.half.x, b.half.y, b.half.z };
    for (int a = 0; a < 3; ++a)
    {
        const double lo = c[a] - h[a], hi = c[a] + h[a];
        if (std::abs(dc[a]) < 1e-15)
        {
            if (oc[a] < lo || oc[a] > hi) return false;
            continue;
        }
        double ta = (lo - oc[a]) / dc[a], tb = (hi - oc[a]) / dc[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

bool hitBox(const Box& b, ref::D3 o, ref::D3 d)
{
    double t0 = 0, t1 = 1e30;
    const double oc[3] = { o.x, o.y, o.z }, dc[3] = { d.x, d.y, d.z };
    const double c[3] = { b.centre.x, b.centre.y, b.centre.z }, h[3] = { b.half.x, b.half.y, b.half.z };
    for (int a = 0; a < 3; ++a)
    {
        const double lo = c[a] - h[a], hi = c[a] + h[a];
        if (std::abs(dc[a]) < 1e-15)
        {
            if (oc[a] < lo || oc[a] > hi) return false;
            continue;
        }
        double ta = (lo - oc[a]) / dc[a], tb = (hi - oc[a]) / dc[a];
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

// CPU twin of the grid (FroxelCommon.hlsli).
struct Grid
{
    shadow::FroxelGridCpu g;
    ViewDesc view;
    double logRatio = 0;
    double node(uint32_t n) const { return n == 0 ? 0.0 : g.nearM * std::exp2(logRatio * n / g.slices); }
    // Ray scaled to unit view depth through a pixel position.
    ref::D3 rayAt(double px, double py) const
    {
        const double ndc[4] = { px / view.width * 2 - 1, 1 - py / view.height * 2, 1, 1 };
        double p[4];
        for (int r = 0; r < 4; ++r)
        {
            p[r] = 0;
            for (int c = 0; c < 4; ++c) p[r] += (double)view.invViewProj.m[r][c] * ndc[c];
        }
        const ref::D3 w{ p[0] / p[3], p[1] / p[3], p[2] / p[3] };
        return (w - d3(view.position)) * (1.0 / view.nearPlane);
    }
    ref::D3 tileRay(uint32_t tx, uint32_t ty) const { return rayAt((tx + 0.5) * g.tilePx, (ty + 0.5) * g.tilePx); }
};

// Contribution window and reach of a light (INTERFACES 8.2).
double window(const scene::Light& l, double d)
{
    const double x = d / std::max((double)l.range, 1e-6), x2 = x * x;
    const double w = std::clamp(1 - x2 * x2, 0.0, 1.0);
    return w * w;
}
bool reaches(const scene::Light& l, ref::D3 p)
{
    const ref::D3 v = p - d3(l.position);
    const double d = std::sqrt(ref::dot(v, v));
    if (d >= l.range) return false;
    if (l.type == scene::LightType::Spot)
    {
        const double ci = std::cos(l.spotInner), co = std::cos(l.spotOuter);
        const double scale = 1.0 / std::max(ci - co, 1e-4);
        return d > 0 && ref::dot(v, d3(l.forward)) / d * scale - co * scale > 0;
    }
    if (l.type == scene::LightType::Rect || l.type == scene::LightType::Disk) return ref::dot(v, d3(l.forward)) > 0;
    return true;
}
// Intensity towards w (unit, from the light), twin of froxelIntensity.
double intensity(const scene::Light& l, ref::D3 w)
{
    switch (l.type)
    {
    case scene::LightType::Point: return l.intensity;
    case scene::LightType::Spot:
    {
        const double ci = std::cos(l.spotInner), co = std::cos(l.spotOuter);
        const double scale = 1.0 / std::max(ci - co, 1e-4);
        const double s = std::clamp(ref::dot(w, d3(l.forward)) * scale - co * scale, 0.0, 1.0);
        return l.intensity * s * s;
    }
    case scene::LightType::Rect: return l.intensity * l.size.x * l.size.y * std::max(0.0, ref::dot(w, d3(l.forward)));
    case scene::LightType::Disk: return l.intensity * kPi * l.size.x * l.size.x * std::max(0.0, ref::dot(w, d3(l.forward)));
    case scene::LightType::Sphere: return l.intensity * kPi * l.size.x * l.size.x;
    default: return l.intensity * (2.0 * l.size.y * l.size.x + kPi * l.size.y * l.size.y);
    }
}
double peakIntensity(const scene::Light& l)
{
    switch (l.type)
    {
    case scene::LightType::Point:
    case scene::LightType::Spot: return l.intensity;
    case scene::LightType::Rect: return l.intensity * l.size.x * l.size.y;
    case scene::LightType::Disk:
    case scene::LightType::Sphere: return l.intensity * kPi * l.size.x * l.size.x;
    default: return l.intensity * (2.0 * l.size.y * l.size.x + kPi * l.size.y * l.size.y);
    }
}

struct Lists
{
    std::vector<uint8_t> raw;
    // Header of froxel i: (first entry, count), two words per froxel (FroxelCommon.hlsli).
    uint32_t first(uint32_t i) const { uint32_t h; std::memcpy(&h, raw.data() + 64 + i * 8, 4); return h; }
    uint32_t count(uint32_t i) const { uint32_t h; std::memcpy(&h, raw.data() + 64 + i * 8 + 4, 4); return h; }
    uint32_t word(uint32_t byte) const { uint32_t w; std::memcpy(&w, raw.data() + byte, 4); return w; }
    std::vector<uint32_t> list(uint32_t froxel) const
    {
        const uint32_t first = this->first(froxel), count = this->count(froxel), indexBase = word(36);
        std::vector<uint32_t> out;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t w = word(indexBase + ((first + i) >> 1) * 4);
            out.push_back(((first + i) & 1 ? w >> 16 : w & 0xFFFF) & 0x7FFF);  // bit 15: shadow slot
        }
        return out;
    }
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, debug = false, keepFroxels = false, uploadFirst = false;
        int debugTile[2] = { 3, 2 };
        bool gbv = false, queueAb = false;
        uint32_t W = 1920, H = 1080;
        std::vector<std::string> overrides;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--no-debug-layer") debugLayer = false;
            else if (a == "--width") W = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--height") H = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--set") overrides.push_back(argv[++i]);
            else if (a == "--debug") debug = true;
            else if (a == "--gbv") gbv = true;  // GPU-based validation
            else if (a == "--queue-ab") queueAb = true; // same frame, all stored volume bits (padding excluded)
            else if (a == "--tile") { debugTile[0] = std::stoi(argv[++i]); debugTile[1] = std::stoi(argv[++i]); }  // per-node print of one tile (section 2)
            else if (a == "--keep") keepFroxels = true;
            else if (a == "--upload-first") uploadFirst = true;  // diagnostic: probe input declared before the froxel passes
        }
        TestFrame tf(debugLayer, gbv);
        for (const std::string& o : overrides) tf.quality.applyOverride(o);
        TestRaster raster(tf);
        raster.install();
        int failures = 0;
        auto report = [&](bool ok, const char* what, double value, double limit) {
            logf("%-66s %.4g (limit %.4g) %s\n", what, value, limit, ok ? "ok" : "FAIL");
            if (!ok) ++failures;
        };

        scene::Scene base;
        base.name = "froxel test";
        base.materials.push_back({});
        base.meshes.push_back(boxMesh("ground", { 200, 0.05f, 200 }));
        base.meshes.push_back(boxMesh("roof", { 12, 0.1f, 8 }));
        base.instances.push_back(instanceAt(0, { 0, -0.05f, 0 }));
        base.instances.push_back(instanceAt(1, { 0, 7, 25 }));
        const std::vector<Box> boxes = { { { 0, -0.05f, 0 }, { 200, 0.05f, 200 } }, { { 0, 7, 25 }, { 12, 0.1f, 8 } } };
        scene::Camera cam;
        cam.name = "main";
        cam.position = { -2, 2.5f, 0 };
        cam.forward = normalize(float3{ 0.1f, -0.02f, 1 });
        base.cameras.push_back(cam);

        const shadow::FroxelGridCpu fg = shadow::froxelGridFor(tf.quality, W, H);
        const uint32_t F = fg.gridX * fg.gridY * fg.slices;
        const uint32_t listMax = (uint32_t)tf.quality.integer("atmosphere.froxels.lights_max");  // the ordered head
        // The lists buffer's size follows the frame's capacity (froxelListCapacity: the scene bound + the FX allowance).
        Grid grid{ fg, {}, std::log2((double)fg.farM / fg.nearM) };

        std::vector<uint8_t> lastLists, lastVolume, lastDepth;
        std::vector<gpu::Light> gpuLights;  // the scene's light records of the last read frame (froxelListBound)
        bool wantDepth = false;
        std::function<TextureRef(FramePassContext&)> injectMedia;  // section 7: the view's volumeSlices before froxels()
        std::function<void(FramePassContext&)> beforeFroxels;      // section 1b: writes the FX light tail
        // Node-by-node comparisons need every slice integrated (production integrates only the slices a reader reaches;
        // section 6 checks that those are the same numbers).
        shadow::setFroxelFullDepth(tf.trackState, true);
        auto run = [&](const scene::Scene& sc, int frames, float ev100 = 14.0f, bool setScene = true) {
            if (setScene) tf.setScene(sc);
            tf.frame.mainView = ViewDesc::fromCamera(sc.cameras[0], W, H, float4x4{});
            tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;
            tf.frame.mainView.ev100 = ev100;
            tf.frame.deltaTime = 1.0f / 60;
            grid.view = tf.frame.mainView;
            for (int f = 0; f < frames; ++f)
            {
                std::shared_ptr<std::vector<uint8_t>> lists, volume, depthRb, otherVolume;
                const bool read = f + 1 == frames;  // earlier frames without readback: the plan changes (consumer culled)
                tf.run([&](FramePassContext& fc) {
                    ViewResources main;
                    main.view = fc.frame.mainView;
                    main.frameConstants = fc.frameConstantsFor(main.view);
                    tracks::lightFunctions(fc);  // E (A8): FrameResources::lightFunctions (invalid when none is set)
                    tracks::atmosphere(fc);
                    raster.mainView(fc, main);
                    tracks::shadowPages(fc, main);
                    if (injectMedia) main.volumeSlices = injectMedia(fc);
                    if (beforeFroxels) beforeFroxels(fc);
                    tracks::froxels(fc, main);
                    if (read)
                    {
                        lists = tf.readbackBuffer(fc, fc.resources.froxelLights, shadow::froxelListBytes(fg, shadow::froxelListCapacity(tf.trackState)));
                        gpuLights = fc.scene.lights();
                        volume = tf.readback(fc, fc.resources.froxels);
                        if (wantDepth) depthRb = tf.readback(fc, main.depth);
                        if (queueAb)
                        {
                            const bool saved = tf.quality.boolean("atmosphere.froxels.integration_queue");
                            tf.quality.applyOverride(saved ? "atmosphere.froxels.integration_queue=false" : "atmosphere.froxels.integration_queue=true");
                            tracks::froxels(fc, main);
                            otherVolume = tf.readback(fc, fc.resources.froxels);
                            tf.quality.applyOverride(saved ? "atmosphere.froxels.integration_queue=true" : "atmosphere.froxels.integration_queue=false");
                        }
                    }
                });
                tf.frame.time += tf.frame.deltaTime;
                if (read)
                {
                    lastLists = *lists;
                    lastVolume = *volume;
                    if (depthRb) lastDepth = *depthRb;
                    if (otherVolume)
                    {
                        const uint32_t pitch = TestFrame::rowPitch(fg.gridX, 8), rows = fg.gridY * (3 * (fg.slices + 1) + 2);
                        uint32_t different = 0;
                        for (uint32_t row = 0; row < rows; ++row)
                            for (uint32_t byte = 0; byte < fg.gridX * 8; ++byte)
                                different += (*volume)[(size_t)row * pitch + byte] != (*otherVolume)[(size_t)row * pitch + byte] ? 1u : 0u;
                        report(different == 0, "froxel queued/tile volume bits identical", different, 0);
                    }
                }
            }
        };
        const uint32_t volumePitch = TestFrame::rowPitch(fg.gridX, 8);
        // Air volume: part 0 in-scattering as L / (1 - T) (pre-exposed), 1 optical depth, 2 sun transmittance; node n = 0..S
        // at slice part (S + 1) + n. Part 0 is returned as the in-scattering L itself (times the node's 1 - T).
        auto nodeOf = [&](const std::vector<uint8_t>& vol, uint32_t tx, uint32_t ty, uint32_t n, uint32_t part = 0) -> ref::D3 {
            auto raw = [&](uint32_t pt) {
                uint16_t h[4];
                std::memcpy(h, vol.data() + (size_t)(pt * (fg.slices + 1) + n) * volumePitch * fg.gridY + (size_t)ty * volumePitch + (size_t)tx * 8, 8);
                return ref::D3{ halfToFloat(h[0]), halfToFloat(h[1]), halfToFloat(h[2]) };
            };
            if (part != 0) return raw(part);
            const double e = 1.0 / (1.2 * std::exp2(grid.view.ev100));  // part 0 stored pre-exposed
            const ref::D3 hat = raw(0), tau = raw(1);
            auto omt = [](double t) { return t < 1e-3 ? t * (1 - t * (0.5 - t / 6)) : 1 - std::exp(-t); };  // airOneMinusExp
            return { hat.x / e * omt(tau.x), hat.y / e * omt(tau.y), hat.z / e * omt(tau.z) };
        };
        auto node = [&](uint32_t tx, uint32_t ty, uint32_t n) { return nodeOf(lastVolume, tx, ty, n); };
        const ref::Model model = ref::fromScene(base.atmosphere);
        std::mt19937 rng(7);
        auto uni = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); };

        // ---- 1. Lists.
        {
            scene::Scene sc = base;
            sc.sun.direction = normalize(float3{ 0.3f, 0.8f, 0.2f });
            for (int i = 0; i < 700; ++i)
            {
                scene::Light l;
                l.type = (scene::LightType)(i % 6);
                l.position = { uni(-60, 60), uni(0.3f, 12), uni(-10, 180) };
                l.forward = normalize(float3{ uni(-1, 1), uni(-1, 0.2f), uni(-1, 1) });
                l.right = normalize(cross(l.forward, float3{ 0, 1, 0.01f }));
                l.intensity = uni(50, 5000);
                l.range = uni(2, 25);
                l.spotInner = uni(0.1f, 0.5f);
                l.spotOuter = l.spotInner + uni(0.05f, 0.6f);
                l.size = { uni(0.1f, 1.5f), uni(0.05f, 0.5f) };
                sc.lights.push_back(l);
            }
            for (int i = 0; i < 60; ++i)  // dense cluster: more than lights_max reach the froxels around it
            {
                scene::Light l;
                l.position = { uni(-1, 1), uni(1, 3), uni(14, 16) };
                l.intensity = uni(100, 2000);
                l.range = uni(4, 8);
                sc.lights.push_back(l);
            }
            run(sc, 3);
            const Lists lists{ lastLists };
            run(sc, 1);
            {
                const Lists again{ lastLists };  // runs may sit elsewhere (atomic allocation order); the lists may not differ
                uint32_t differ = 0;
                for (uint32_t f = 0; f < F; ++f) differ += lists.list(f) != again.list(f) ? 1 : 0;
                report(differ == 0, "lists identical over two frames (froxels differing)", differ, 0);
            }
            const uint32_t indexCount = lists.word(44), cut = lists.word(48), dropped = lists.word(52), maxCount = lists.word(56), candOverflow = lists.word(60);
            logf("lists: %u entries (%.2f per froxel), %u lists cut by the capacity (%u entries lost), max %u lights per froxel, %u tiles over the candidate buffer\n",
                 indexCount, (double)indexCount / F, cut, dropped, maxCount, candOverflow);
            report(candOverflow == 0, "candidate buffer not exceeded (tiles)", candOverflow, 0);
            report(maxCount > listMax, "the dense cluster exceeds the ordered head (max lights per froxel)", maxCount, listMax + 1);
            report(cut == 0 && dropped == 0, "no list cut by the capacity (froxels cut + entries lost)", cut + dropped, 0);
            {
                // The headers' runs: even starts, disjoint and in order, every count stored (the sum is the entry count).
                uint32_t badRuns = 0, sum = 0;
                for (uint32_t f = 0; f < F; ++f)
                {
                    const uint32_t first = lists.first(f), n = lists.count(f);
                    if (first & 1) ++badRuns;
                    if (f > 0 && n > 0 && first < lists.first(f - 1) + lists.count(f - 1)) ++badRuns;
                    sum += n;
                }
                report(badRuns == 0, "runs start at even entries and do not overlap", badRuns, 0);
                report(sum == indexCount, "sum of list counts equals the stored entry count", sum, indexCount);
            }
            {
                // Capacity (FroxelSystem.cpp): the CPU bound of the scene lights' entries covers the GPU's exact need, and the
                // buffer held it (no fallback).
                const uint32_t needed = lists.word(28), capacity = lists.word(40);
                const uint64_t bound = shadow::froxelListBound(fg, grid.view, gpuLights);
                logf("capacity: GPU need %u entries, CPU scene bound %llu (%.2f x), buffer capacity %u (reserve %llu)\n", needed, (unsigned long long)bound,
                     (double)bound / std::max(needed, 1u), capacity, (unsigned long long)F * listMax);
                report(bound >= needed, "scene bound covers the GPU need (entries)", (uint32_t)std::min<uint64_t>(bound, 0xFFFFFFFFu), needed);
                report(needed <= capacity, "the lists buffer held the frame's need (no fallback)", needed, capacity);
            }
            {
                // The fallback of a frame over the capacity (FroxelLists.hlsl, forced here): the scene lights' own allocation
                // and lists; without FX lights it is the normal frame, entry for entry, with nothing counted as lost.
                tf.quality.applyOverride("atmosphere.froxels.list_fallback_forced=1");
                run(sc, 1);
                tf.quality.applyOverride("atmosphere.froxels.list_fallback_forced=0");
                const Lists fb{ lastLists };
                uint32_t differing = 0;
                for (uint32_t f = 0; f < F; ++f)
                    if (fb.first(f) != lists.first(f) || fb.count(f) != lists.count(f) || fb.list(f) != lists.list(f)) ++differing;
                logf("forced fallback (no FX lights): %u entries stored, %u lists cut, %u entries lost, need %u\n", fb.word(44), fb.word(48), fb.word(52), fb.word(28));
                report(differing == 0, "fallback without FX lights: the normal lists, entry for entry (froxels differing)", differing, 0);
                report(fb.word(48) == 0 && fb.word(52) == 0, "fallback without FX lights: nothing cut or lost", fb.word(48) + fb.word(52), 0);
                report(fb.word(28) == lists.word(28) && fb.word(44) == lists.word(44), "fallback: the need and the stored count unchanged", fb.word(28), lists.word(28));
            }

            // Every froxel: well-formed, ordered. Sampled froxels: conservative against 8 points inside each.
            uint32_t malformed = 0, disorder = 0, missing = 0, checkedFroxels = 0, samples = 0;
            double worstOrder = 1;
            const ref::D3 camPos = d3(grid.view.position);
            for (uint32_t s = 0; s < fg.slices; ++s)
                for (uint32_t ty = 0; ty < fg.gridY; ++ty)
                    for (uint32_t tx = 0; tx < fg.gridX; ++tx)
                    {
                        const uint32_t f = (s * fg.gridY + ty) * fg.gridX + tx;
                        const std::vector<uint32_t> li = lists.list(f);
                        std::set<uint32_t> unique(li.begin(), li.end());
                        if (unique.size() != li.size() || (!li.empty() && *unique.rbegin() >= sc.lights.size())) ++malformed;
                        if ((tx * 7 + ty * 13 + s * 3) % 5 != 0) continue;  // conservativeness on a fifth of the froxels
                        ++checkedFroxels;
                        const bool last = s + 1 == fg.slices;
                        const double z0 = grid.node(s), z1 = last ? 4.0 * fg.farM : grid.node(s + 1);
                        for (int k = 0; k < 8; ++k)
                        {
                            const double px = std::min((tx + uni(0, 1)) * fg.tilePx, (float)W - 1e-3f), py = std::min((ty + uni(0, 1)) * fg.tilePx, (float)H - 1e-3f);
                            const double z = z0 + (z1 - z0) * uni(0, 1);
                            const ref::D3 p = camPos + grid.rayAt(px, py) * z;
                            ++samples;
                            for (uint32_t l = 0; l < sc.lights.size(); ++l)
                            {
                                if (!reaches(sc.lights[l], p) || unique.count(l)) continue;
                                ++missing;  // no list is ever full: every light reaching the froxel is listed
                            }
                        }
                    }
            // Order: importance at the froxel centre, recomputed as the kernel does, must not increase along the ordered
            // head (the first lights_max entries; the tail past it is complete but unordered).
            for (uint32_t f = 0; f < F; f += 97)
            {
                const uint32_t s = f / (fg.gridX * fg.gridY), ty = (f / fg.gridX) % fg.gridY, tx = f % fg.gridX;
                std::vector<uint32_t> li = lists.list(f);
                if (li.size() > listMax) li.resize(listMax);
                if (li.size() < 2) continue;
                const bool last = s + 1 == fg.slices;
                const double z0 = grid.node(s), zb = last ? std::max((double)fg.farM, 2 * z0) : grid.node(s + 1);
                ref::D3 lo{ 1e30, 1e30, 1e30 }, hi{ -1e30, -1e30, -1e30 };
                const double cx[2] = { (double)tx * fg.tilePx, (double)(tx + 1) * fg.tilePx }, cy[2] = { (double)ty * fg.tilePx, (double)(ty + 1) * fg.tilePx };
                for (int q = 0; q < 8; ++q)
                {
                    const ref::D3 p = camPos + grid.rayAt(cx[q & 1], cy[(q >> 1) & 1]) * ((q & 4) ? zb : z0);
                    lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
                    hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
                }
                const ref::D3 centre = (lo + hi) * 0.5;
                const double radius = 0.5 * std::sqrt(ref::dot(hi - lo, hi - lo));
                double prev = 1e300;
                for (uint32_t l : li)
                {
                    const scene::Light& L = sc.lights[l];
                    const ref::D3 v = centre - d3(L.position);
                    const double dn = std::max(std::sqrt(ref::dot(v, v)) - radius, 0.0), extent = std::max(L.size.x, L.size.y);
                    const double imp = peakIntensity(L) * window(L, dn) / (dn * dn + 0.25 * radius * radius + extent * extent);
                    if (imp > prev * 1.01)
                    {
                        ++disorder;
                        worstOrder = std::min(worstOrder, prev / imp);
                    }
                    prev = imp;
                }
            }
            report(malformed == 0, "froxel lists well formed (unique, valid)", malformed, 0);
            logf("conservativeness: %u froxels, %u points\n", checkedFroxels, samples);
            report(missing == 0, "lights reaching a froxel point missing from its list", missing, 0);
            report(disorder == 0, "ordered heads out of importance order (entries)", disorder, 0);
        }

        // ---- 2. Local lights in the air (sun below the horizon: no sun term); then the same lights casting shadows (their
        //         VSM shadows their air: the roof above them cuts their glow above it).
#if defined(FROXEL_TEST_LIGHT_FUNCTIONS)
        for (const int variant : { 0, 1, 2 })
#else
        for (const int variant : { 0, 1 })
#endif
        {
            const bool shadowed = variant == 1, functions = variant == 2;
            const std::string what = shadowed ? "shadowed air of local lights" : (functions ? "air of local lights with light functions" : "air in-scattering of local lights");
            scene::Scene sc = base;
            sc.sun.direction = normalize(float3{ 0.3f, -0.5f, 0.2f });
            for (int i = 0; i < 24; ++i)
            {
                scene::Light l;
                l.type = (scene::LightType)(i % 6);
                l.position = { uni(-8, 8), uni(0.5f, 6), uni(2, 60) };
                l.forward = normalize(float3{ uni(-1, 1), uni(-1, 0.2f), uni(-1, 1) });
                l.right = normalize(cross(l.forward, float3{ 0, 1, 0.01f }));
                l.intensity = uni(200, 20000);
                l.range = uni(5, 20);
                l.spotInner = 0.3f;
                l.spotOuter = 0.7f;
                l.size = { uni(0.01f, 0.05f), uni(0.01f, 0.03f) };  // small: the line-distance floor rarely applies
                l.castShadow = shadowed;
                sc.lights.push_back(l);
            }
            // Light functions (variant 2) on the point and spot lights: IES profiles varying over both angles (the light's
            // forward and right axes), rotation about forward, intensity and colour keys in time.
#if defined(FROXEL_TEST_LIGHT_FUNCTIONS)
            lights::LightFunctions& lfSet = lights::lightFunctions(tf.trackState);
            std::vector<lights::LightFunction> lf(sc.lights.size());
            if (functions)
                for (uint32_t i = 0; i < sc.lights.size(); ++i)
                {
                    if (sc.lights[i].type != scene::LightType::Point && sc.lights[i].type != scene::LightType::Spot) continue;
                    lights::LightFunction& f = lf[i];
                    f.profile = lights::Profile::Ies;
                    f.ies.vertical = { 0, 30, 60, 90, 120, 150, 180 };
                    f.ies.horizontal = { 0, 90, 180, 270, 360 };
                    for (int v = 0; v < 7; ++v)
                        for (int h = 0; h < 5; ++h) f.ies.values.push_back(0.15f + 0.1f * ((v * 3 + (h % 4) * 5 + (int)i) % 9));
                    f.ies.peak = 1;
                    f.rotationSpeed = 0.2f + 0.1f * (i % 3);
                    f.intensityKeys = { { 0, 0.7f }, { 5, 1.2f } };
                    f.colorKeys = { { 0, 1, 0.7f, 0.5f }, { 4, 0.6f, 0.9f, 1 } };
                    lfSet.set(i, f);
                }
#endif
            scene::Scene dark = sc;
            dark.lights.clear();
            run(dark, 1, -2.0f);  // night exposure: the lights' air glow is displayed
            const std::vector<uint8_t> without = lastVolume;
            [[maybe_unused]] const float frameTime = (float)tf.frame.time;  // the frame the comparison reads (run advances the time after it)
            run(sc, shadowed ? 6 : 1, -2.0f);  // shadowed: steady state (a pool the local pages exhaust grows once)
            if (shadowed)
            {
                const std::vector<uint8_t> keep = lastVolume;
                const uint64_t frame = shadow::lastConstants(tf.trackState).frame;
                for (int i = 0; i < 6 && shadow::stats(tf.trackState).frame < frame; ++i) run(sc, 1, -2.0f);
                lastVolume = keep;
                const shadow::VsmStats st = shadow::stats(tf.trackState);
                logf("%s: VSM pages requested %u, allocated %u, exhausted %u; local slots %u, active %u\n", what.c_str(), st.requested, st.allocated, st.exhausted,
                     st.localAssigned, st.localActive);
            }
            const Lists lists{ lastLists };
            report(lists.word(48) == 0, "local-light scene: no truncated list", lists.word(48), 0);
            // Reference: fine midpoint integration of every light along the tile-centre ray, continuous air coefficients.
            double worst = 0, sumErr = 0, sumRef = 0, sumGpu = 0;
            uint32_t compared = 0;
            const ref::D3 camPos = d3(grid.view.position);
            for (uint32_t ty = 2; ty < fg.gridY; ty += 7)
                for (uint32_t tx = 3; tx < fg.gridX; tx += 9)
                {
                    const ref::D3 ray = grid.tileRay(tx, ty);
                    const double toRay = std::sqrt(ref::dot(ray, ray));
                    const ref::D3 dir = ray * (1 / toRay);
                    ref::D3 acc{}, tau{};
                    for (uint32_t n = 1; n <= fg.slices; ++n)
                    {
                        const double t0 = grid.node(n - 1) * toRay, t1 = grid.node(n) * toRay;
                        const int steps = 400;
                        const double dt = (t1 - t0) / steps;
                        for (int k = 0; k < steps; ++k)
                        {
                            const double t = t0 + (k + 0.5) * dt;
                            const ref::D3 p = camPos + dir * t;
                            const ref::Coefficients c = ref::coefficients(model, std::max(0.0, ref::altitudeOf(model, p)));
                            const ref::D3 T = ref::expNeg(tau + c.extinction * (0.5 * dt));
                            for (uint32_t li = 0; li < sc.lights.size(); ++li)
                            {
                                const scene::Light& L = sc.lights[li];
                                const ref::D3 v = p - d3(L.position);
                                const double d = std::sqrt(ref::dot(v, v));
                                if (d >= L.range || d < 1e-9) continue;
                                const ref::D3 w = v * (1 / d);
                                const double h2 = std::max(ref::dot(v, v) - ref::dot(v, dir) * ref::dot(v, dir), 0.0);
                                const double hmin = std::max({ (double)L.size.x, (double)L.size.y, 0.01 });
                                // The kernel's line-distance floor: the same regularisation of the point-source singularity.
                                const double dd = std::sqrt(std::max(d * d, d * d - h2 + hmin * hmin));
                                const double nu = ref::dot(w, dir * -1.0);
                                const ref::D3 ph = c.rayleigh * ref::rayleighPhase(nu) + c.mie * ref::miePhase(nu, model.g);
                                if (L.castShadow)
                                {
                                    bool blocked = false;
                                    for (const Box& bx : boxes)
                                        if (hitSegment(bx, p, d3(L.position) - p)) blocked = true;
                                    if (blocked) continue;
                                }
                                ref::D3 fl{ 1, 1, 1 };
#if defined(FROXEL_TEST_LIGHT_FUNCTIONS)
                                if (lfSet.get(li))
                                {
                                    const float3 f = lights::evaluate(*lfSet.get(li), L.forward, L.right, float3{ (float)w.x, (float)w.y, (float)w.z }, frameTime);
                                    fl = { f.x, f.y, f.z };
                                }
#endif
                                acc = acc + T * ph * (intensity(L, w) * window(L, d) / (dd * dd) * dt) * d3(L.color) * fl;
                            }
                            tau = tau + c.extinction * dt;
                        }
                        const ref::D3 g = node(tx, ty, n) - nodeOf(without, tx, ty, n);
                        if (debug && (int)tx == debugTile[0] && (int)ty == debugTile[1] && n <= 40)
                            logf("  tile (%u,%u) node %u z %.3f: gpu %.4g %.4g %.4g ref %.4g %.4g %.4g\n", tx, ty, n, grid.node(n), g.x, g.y, g.z, acc.x, acc.y, acc.z);
                        const double r = acc.x + acc.y + acc.z, e = std::abs(g.x - acc.x) + std::abs(g.y - acc.y) + std::abs(g.z - acc.z);
                        const double exposure = 1.0 / (1.2 * std::exp2(grid.view.ev100));
                        if (r * exposure > 1e-3)  // displayed above 1e-3 of white: fp16 holds it to 1e-3 relative
                        {
                            if (e / r > worst && debug)
                                logf("  worse: tile (%u,%u) node %u z %.2f gpu %.4g %.4g %.4g ref %.4g %.4g %.4g\n", tx, ty, n, grid.node(n), g.x, g.y, g.z, acc.x, acc.y, acc.z);
                            worst = std::max(worst, e / r);
                            sumGpu += g.x + g.y + g.z;
                            sumErr += e;
                            sumRef += r;
                            ++compared;
                        }
                    }
                }
            logf("%s: %u nodes compared, largest relative error %.3g; sum of compared GPU nodes %.6g, reference %.6g\n", what.c_str(), compared, worst, sumGpu, sumRef);
            // Shadowed: the air's shadow boundaries are resolved to the froxel (slice length, tile width).
            const double meanLimit = shadowed ? 0.03 : 0.01, worstLimit = shadowed ? 0.25 : 0.03;
            report(compared > 100 && sumErr / sumRef < meanLimit, (what + " vs reference (mean relative)").c_str(), sumErr / std::max(sumRef, 1e-30), meanLimit);
            report(worst < worstLimit, (what + " vs reference (largest node)").c_str(), worst, worstLimit);
            if (shadowed)
            {
                // The walk in K pieces per shadowed item (K lanes, FroxelIntegrate.hlsl) against one walk per item (experiment
                // bit 128): the pieces tile each segment and their lit sets' moments add, so the nodes agree to rounding. Two of
                // the lights: fewer items per tile, so items get K >= 2 lanes.
                scene::Scene few = sc;
                few.lights.resize(2);
                auto volumeWith = [&](int bits) {
                    tf.quality.applyOverride("atmosphere.froxels.experiment_disable=" + std::to_string(bits));
                    // The same scene, six frames after the quality change.
                    run(few, 6, -2.0f, false);
                    return lastVolume;
                };
                run(few, 6, -2.0f);
                {
                    // Regression (S_STATUS 9e): a static scene gives the same air every frame through a whole cycle of
                    // the local-light upload ring (16 frames), with scene sets in between, after more than 256 scene
                    // lights have grown the ring's slot map. The ring's slice stride was a multiple of 256 only, so two
                    // slices in three put the 48-byte light records' view 16 or 32 bytes off: those frames' local shadow
                    // pages were neither requested nor found and the shadowed air was unshadowed.
                    scene::Scene many = few;
                    for (uint32_t i = (uint32_t)many.lights.size(); i < 301; ++i)
                    {
                        scene::Light d = few.lights[0];
                        d.position = { 1000.0f + (float)i, 500, 1000 };  // far outside the view and every froxel
                        d.range = 1;
                        d.castShadow = false;
                        many.lights.push_back(d);
                    }
                    run(many, 1, -2.0f);
                    std::shared_ptr<std::vector<uint8_t>> tableRb;
                    beforeFroxels = [&](FramePassContext& fc) { tableRb = tf.readbackBuffer(fc, fc.resources.vsmPageTable, (uint64_t)shadow::kTotalSlots * 8); };
                    run(few, 2, -2.0f);
                    std::vector<uint8_t> first;
                    uint32_t differing = 0, withoutLocal = 0;
                    for (int f = 0; f < 16; ++f)
                    {
                        run(few, 1, -2.0f, f % 2 == 0);
                        uint32_t local = 0;
                        for (uint32_t sl = shadow::kSlots; sl < shadow::kTotalSlots; ++sl)
                        {
                            uint32_t e0;
                            std::memcpy(&e0, tableRb->data() + (size_t)sl * 8, 4);
                            local += (e0 >> 31) & 1u;
                        }
                        withoutLocal += local == 0 ? 1u : 0u;
                        if (f == 0) first = lastVolume;
                        else differing += lastVolume != first ? 1u : 0u;
                    }
                    beforeFroxels = nullptr;
                    logf("static scene over the local-light ring (16 frames, scene set every other frame): %u frames differ from the first, %u without local shadow pages\n",
                         differing, withoutLocal);
                    report(differing == 0, "static scene: the same air every frame (bits), 16 frames after 301 scene lights", differing, 0);
                    report(withoutLocal == 0, "static scene: local shadow pages resident every frame", withoutLocal, 0);
                }                const std::vector<uint8_t> whole = volumeWith(128), pieces = volumeWith(0);
                tf.quality.applyOverride("atmosphere.froxels.experiment_disable=0");
                const double exposure = 1.0 / (1.2 * std::exp2(grid.view.ev100));
                double worstP = 0, sumD = 0, sumW = 0;
                uint32_t nodes = 0;
                for (uint32_t ty = 0; ty < fg.gridY; ++ty)
                    for (uint32_t tx = 0; tx < fg.gridX; ++tx)
                        for (uint32_t n = 1; n <= fg.slices; ++n)
                        {
                            const ref::D3 p = nodeOf(pieces, tx, ty, n) - nodeOf(without, tx, ty, n);
                            const ref::D3 w = nodeOf(whole, tx, ty, n) - nodeOf(without, tx, ty, n);
                            const double r = w.x + w.y + w.z, e = std::abs(p.x - w.x) + std::abs(p.y - w.y) + std::abs(p.z - w.z);
                            if (r * exposure <= 1e-3) continue;
                            worstP = std::max(worstP, e / r);
                            sumD += e;
                            sumW += r;
                            ++nodes;
                        }
                logf("shadowed air, K pieces vs one walk per item: %u nodes, mean relative difference %.3g, largest %.3g\n", nodes, sumD / std::max(sumW, 1e-30), worstP);
                report(nodes > 1000 && sumD / std::max(sumW, 1e-30) < 1e-3, "shadowed air: walk in K pieces = one walk (mean relative)", sumD / std::max(sumW, 1e-30), 1e-3);
                report(worstP < 1e-2, "shadowed air: walk in K pieces = one walk (largest node)", worstP, 1e-2);
                {
                    // L4 (RENDERER_REDESIGN_V2 14.4, bounded walk omission; experiment bit 1024): shadowed lights under 1e-3 of
                    // their slice's local in-scatter (cumulative, list order) are added lit without their walk. The air with
                    // the omission against the air without it, relative to the local lights' share (the volume minus the dark
                    // scene's): the omitted shares bound the difference by 1e-3 per slice, so by 1e-3 of the share per node.
                    tf.quality.applyOverride("atmosphere.froxels.experiment_disable=1024");
                    run(sc, 6, -2.0f, false);
                    const std::vector<uint8_t> omitted = lastVolume;
                    tf.quality.applyOverride("atmosphere.froxels.experiment_disable=0");
                    run(sc, 6, -2.0f, false);
                    const std::vector<uint8_t> walked = lastVolume;
                    double worstO = 0, sumDo = 0, sumWo = 0;
                    uint32_t nodesO = 0, changed = 0;
                    for (uint32_t ty = 0; ty < fg.gridY; ++ty)
                        for (uint32_t tx = 0; tx < fg.gridX; ++tx)
                            for (uint32_t n = 1; n <= fg.slices; ++n)
                            {
                                const ref::D3 o = nodeOf(omitted, tx, ty, n) - nodeOf(walked, tx, ty, n);
                                const ref::D3 w = nodeOf(walked, tx, ty, n) - nodeOf(without, tx, ty, n);
                                const double r = w.x + w.y + w.z, e = std::abs(o.x) + std::abs(o.y) + std::abs(o.z);
                                if (e > 0) ++changed;
                                if (r * exposure <= 1e-3) continue;
                                worstO = std::max(worstO, e / r);
                                sumDo += e;
                                sumWo += r;
                                ++nodesO;
                            }
                    logf("shadowed air, bounded walk omission (bit 1024) vs every walk: %u nodes, %u changed, mean relative difference %.3g, largest %.3g\n", nodesO, changed,
                         sumDo / std::max(sumWo, 1e-30), worstO);
                    report(nodesO > 1000 && sumDo / std::max(sumWo, 1e-30) <= 1e-3, "shadowed air: bounded walk omission vs every walk (mean relative to the local share)", sumDo / std::max(sumWo, 1e-30), 1e-3);
                    report(worstO <= 2e-3, "shadowed air: bounded walk omission vs every walk (largest node, relative to the local share)", worstO, 2e-3);
                }            }
#if defined(FROXEL_TEST_LIGHT_FUNCTIONS)
            for (uint32_t i = 0; i < sc.lights.size(); ++i) lfSet.clear(i);
#endif
        }

        // ---- 3. Sun shadows in the air: the in-scattering a caster removes (volume with it minus without it) against the
        //         exact integral with the casters ray-cast towards the sun, slice by slice up to maxT along the ray.
        auto airShadows = [&](const scene::Scene& sc, const scene::Scene& open, const std::vector<Box>& casters, double maxT, int steps, uint32_t stride,
                              const char* label) {
            // Steady state: a pool that the view exhausts grows once (its statistics arrive two frames later).
            run(open, 6);
            const std::vector<uint8_t> without = lastVolume;
            run(sc, 6);
            {
                // Page statistics of that frame (read back a few frames later; the scene and camera stay).
                const std::vector<uint8_t> keep = lastVolume;
                const uint64_t frame = shadow::lastConstants(tf.trackState).frame;
                for (int i = 0; i < 6 && shadow::stats(tf.trackState).frame < frame; ++i) run(sc, 1);
                lastVolume = keep;
                const shadow::VsmStats st = shadow::stats(tf.trackState);
                logf("%s: VSM pages requested %u (by pixels %u), allocated %u, pool exhausted %u, free %u\n", label, st.requested, st.pixelRequested, st.allocated,
                     st.exhausted, st.freePages);
                report(st.exhausted == 0, (std::string(label) + ": VSM pool not exhausted (requests dropped)").c_str(), st.exhausted, 0);
            }
            const ref::D3 sun = d3(sc.sun.direction), camPos = d3(grid.view.position);
            const double E = sc.sun.illuminance;
            const shadow::VsmConstantsCpu& vc = shadow::lastConstants(tf.trackState);
            const ref::D3 lx = d3(vc.lightX), ly = d3(vc.lightY);
            double worstExact = 0, sumErr = 0, sumRef = 0;
            uint32_t exactSlices = 0, mixedSlices = 0, mixedOver = 0;
            for (uint32_t ty = 1; ty < fg.gridY; ty += stride)
                for (uint32_t tx = 2; tx < fg.gridX; tx += stride + 1)
                {
                    const ref::D3 ray = grid.tileRay(tx, ty);
                    const double toRay = std::sqrt(ref::dot(ray, ray));
                    const ref::D3 dir = ray * (1 / toRay);
                    const double nu = ref::dot(dir, sun);
                    ref::D3 tau{}, prevNode{}, prevMag{};
                    for (uint32_t n = 1; n <= fg.slices; ++n)
                    {
                        const double z0 = grid.node(n - 1), z1 = grid.node(n);
                        const double t0 = z0 * toRay, t1 = z1 * toRay;
                        if (t1 > maxT) break;  // the casters' shadows and the view's ground are nearer
                        const double dt = (t1 - t0) / steps;
                        ref::D3 slice{};
                        int shadowed = 0, belowGround = 0, nearBoundary = 0;
                        double peak = 0;
                        // Texel of the air level at this slice (vsmAirLevel): a slice is 'exact' when no point of it lies
                        // within 1.5 texels (laterally, seen from the sun) of a shadow boundary.
                        const double width = fg.tilePx * 2 * 0.5 * (z0 + z1) * std::tan(grid.view.verticalFov * 0.5) / H / fg.shadowTexelsPerTile;
                        const int level = std::clamp((int)std::floor(std::log2(std::max(width, 1e-30) * 1024.0) + vc.lodBias), 0, (int)shadow::kLevels - 1);
                        const double texel = std::ldexp(1.0, level - 10);
                        for (int k = 0; k < steps; ++k)
                        {
                            const double t = t0 + (k + 0.5) * dt;
                            const ref::D3 p = camPos + dir * t;
                            const ref::Coefficients c = ref::coefficients(model, std::max(0.0, ref::altitudeOf(model, p)));
                            const ref::D3 T = ref::expNeg(tau + c.extinction * (0.5 * dt));
                            tau = tau + c.extinction * dt;
                            if (p.y < 0) ++belowGround;
                            auto blockedAt = [&](ref::D3 q) {
                                for (const Box& b : casters)
                                    if (hitBox(b, q, sun)) return true;
                                return false;
                            };
                            const bool blocked = blockedAt(p);
                            const double o = 1.5 * texel;
                            if (blockedAt(p + lx * o) != blocked || blockedAt(p - lx * o) != blocked || blockedAt(p + ly * o) != blocked || blockedAt(p - ly * o) != blocked)
                                ++nearBoundary;
                            const ref::D3 source = T * (c.rayleigh * ref::rayleighPhase(nu) + c.mie * ref::miePhase(nu, model.g)) * ref::sunTransmittance(model, p, sun, 256) * (E * dt);
                            peak = std::max(peak, (source.x + source.y + source.z) / dt);
                            if (blocked)
                            {
                                ++shadowed;
                                slice = slice - source;
                            }
                        }
                        const ref::D3 withRoof = node(tx, ty, n), noRoof = nodeOf(without, tx, ty, n);
                        const ref::D3 gNode = withRoof - noRoof;
                        if (debug && tx == 2 && ty == 13 && n <= 40)
                            logf("  tile (2,13) node %u z %.3f: gpu %.4g ref slice %.4g | shadowed %d/%d near %d\n", n, z1, gNode.x - prevNode.x, slice.x, shadowed, steps, nearBoundary);
                        const ref::D3 gSlice = gNode - prevNode;
                        prevNode = gNode;
                        const double mag = std::abs(withRoof.x) + std::abs(withRoof.y) + std::abs(withRoof.z) + std::abs(noRoof.x) + std::abs(noRoof.y) + std::abs(noRoof.z);
                        const double magBefore = prevMag.x;
                        prevMag.x = mag;
                        if (belowGround) continue;  // beyond the ground: no pixel reads it
                        const double r = std::abs(slice.x) + std::abs(slice.y) + std::abs(slice.z);
                        const double e = std::abs(gSlice.x - slice.x) + std::abs(gSlice.y - slice.y) + std::abs(gSlice.z - slice.z);
                        const double storage = 1e-3 * (mag + magBefore);  // four fp16 nodes
                        if ((shadowed == 0 || shadowed == steps) && nearBoundary == 0)
                        {
                            ++exactSlices;
                            const double err = e / std::max(r + 1e-9, 1e-9);
                            if (e > storage + 0.01 * r) worstExact = std::max(worstExact, err);
                        }
                        else
                        {
                            ++mixedSlices;
                            // Texel-resolution bound: each shadow boundary in the slice moves by at most a texel or two
                            // of the air level laterally, i.e. texel / sin(ray, sun) along the ray.
                            const double sinA = std::sqrt(std::max(1 - nu * nu, 1e-4));
                            const double bound = 2 * 2 * texel / sinA * peak + storage + 0.01 * r;
                            if (e > bound) ++mixedOver;
                            if (e > bound && debug)
                                logf("  over bound: tile (%u,%u) node %u z %.2f gpu %.4g ref %.4g bound %.3g texel %.4f shadowed %d/%d\n", tx, ty, n, z1, gSlice.x + gSlice.y + gSlice.z,
                                     slice.x + slice.y + slice.z, bound, texel, shadowed, steps);
                        }
                        sumErr += std::max(0.0, e - storage);  // the test's difference of two volumes adds their fp16 rounding
                        sumRef += r;
                    }
                }
            logf("%s: sun shadows in the air: %u slices entirely lit or shadowed, %u with a boundary\n", label, exactSlices, mixedSlices);
            report(exactSlices > 100 && worstExact == 0, (std::string(label) + ": uniform slices beyond storage precision (worst rel.)").c_str(), worstExact, 0);
            report(mixedSlices > 20 && mixedOver == 0, (std::string(label) + ": slices with a shadow boundary outside the texel bound").c_str(), mixedOver, 0);
            report(sumRef > 0 && sumErr / std::max(sumRef, 1e-30) < 0.03, (std::string(label) + ": shadowed air in-scattering vs reference (mean rel.)").c_str(),
                   sumErr / std::max(sumRef, 1e-30), 0.03);
        };
        {
            scene::Scene sc = base;  // roof 24 x 16 m at 7 m, sun high
            sc.sun.direction = normalize(float3{ 0.35f, 0.85f, -0.4f });
            scene::Scene open = sc;
            open.instances.resize(1);  // ground only
            airShadows(sc, open, boxes, 400, 256, 4, "roof");
        }
        {
            // A ridge 2 km wide and 800 m high, 3 km ahead; low sun behind it: its shadow fills the air 0.3 - 3 km away
            // (god rays against the sun, where only the coarse clipmap levels reach).
            scene::Scene sc;
            sc.name = "ridge";
            sc.materials.push_back({});
            sc.meshes.push_back(boxMesh("ground", { 10000, 0.05f, 10000 }));
            sc.meshes.push_back(boxMesh("ridge", { 1000, 400, 100 }));
            sc.instances.push_back(instanceAt(0, { 0, -0.05f, 0 }));
            sc.instances.push_back(instanceAt(1, { 0, 400, 3000 }));
            sc.sun.direction = normalize(float3{ 0.1f, 0.3f, 1 });
            scene::Camera c;
            c.name = "main";
            c.position = { 0, 2, 0 };
            c.forward = normalize(float3{ 0, 0.05f, 1 });
            sc.cameras.push_back(c);
            scene::Scene open = sc;
            open.instances.resize(1);
            const std::vector<Box> ridge = { { { 0, -0.05f, 0 }, { 10000, 0.05f, 10000 } }, { { 0, 400, 3000 }, { 1000, 400, 100 } } };
            airShadows(sc, open, ridge, 6000, 128, 6, "ridge 3 km");

            // ---- 6. Reader bound: production integrates only the slices a reader reaches (FroxelIntegrate.hlsl); every
            //         node a surface pixel of the 3 x 3 tile neighbourhood reads, and the sky correction of tiles with
            //         sky around them, must be the same bits as with every slice integrated.
            wantDepth = true;
            run(sc, 6);
            const std::vector<uint8_t> full = lastVolume;
            shadow::setFroxelFullDepth(tf.trackState, false);
            run(sc, 6);
            shadow::setFroxelFullDepth(tf.trackState, true);
            wantDepth = false;
            const std::vector<uint8_t> bounded = lastVolume;
            const uint32_t pd = TestFrame::rowPitch(W, 4);
            std::vector<float> tileZ(fg.gridX * fg.gridY, 0.0f);
            std::vector<uint8_t> tileSky(fg.gridX * fg.gridY, 0);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                {
                    float d;
                    std::memcpy(&d, lastDepth.data() + y * pd + x * 4, 4);
                    const uint32_t t = (y / fg.tilePx) * fg.gridX + x / fg.tilePx;
                    if (d <= 0) tileSky[t] = 1;
                    else tileZ[t] = std::max(tileZ[t], tf.frame.mainView.nearPlane / d);
                }
            uint32_t compared = 0, differing = 0, skyCompared = 0, skyDiffering = 0, skipped = 0;
            const uint32_t N = fg.slices + 1;
            auto rawAt = [&](const std::vector<uint8_t>& vol, uint32_t tx, uint32_t ty, uint32_t slice) {
                uint64_t v;
                std::memcpy(&v, vol.data() + (size_t)slice * volumePitch * fg.gridY + (size_t)ty * volumePitch + (size_t)tx * 8, 8);
                return v;
            };
            for (uint32_t ty = 0; ty < fg.gridY; ++ty)
                for (uint32_t tx = 0; tx < fg.gridX; ++tx)
                {
                    float zs = 0;
                    bool sky = false;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int qx = std::clamp((int)tx + dx, 0, (int)fg.gridX - 1), qy = std::clamp((int)ty + dy, 0, (int)fg.gridY - 1);
                            zs = std::max(zs, tileZ[qy * fg.gridX + qx]);
                            sky = sky || tileSky[qy * fg.gridX + qx];
                        }
                    // Slices with z0 < zs are read; nodes 0 .. last read slice + 1.
                    uint32_t lastNode = 0;
                    for (uint32_t sl = 0; sl < fg.slices; ++sl)
                        if (grid.node(sl) < zs) lastNode = sl + 1;
                    skipped += fg.slices - lastNode;
                    for (uint32_t part = 0; part < 3; ++part)
                        for (uint32_t n = 0; n <= lastNode && zs > 0; ++n)
                        {
                            ++compared;
                            differing += rawAt(full, tx, ty, part * N + n) != rawAt(bounded, tx, ty, part * N + n) ? 1 : 0;
                        }
                    if (sky)
                    {
                        ++skyCompared;
                        skyDiffering += rawAt(full, tx, ty, 3 * N) != rawAt(bounded, tx, ty, 3 * N) ? 1 : 0;
                    }
                }
            logf("reader bound: %u nodes read by surfaces compared (%u differ), %u sky corrections compared (%u differ), %u slices beyond the surfaces"
                 "\n", compared, differing, skyCompared, skyDiffering, skipped);
            report(compared > 1000 && differing == 0, "reader bound: nodes a surface reads equal every-slice integration (bits)", differing, 0);
            report(skyCompared > 10 && skyDiffering == 0, "reader bound: sky corrections equal every-slice integration (bits)", skyDiffering, 0);
        }

        // ---- 7. Particle media in the air volume (FroxelIntegrate.hlsl, E's volumeSlices): the volume with synthetic
        //         media against the air-only volume composed with them on the CPU.
        {
            scene::Scene sc;
            sc.name = "media";
            sc.materials.push_back({});
            sc.meshes.push_back(boxMesh("far box", { 1, 1, 1 }));
            sc.instances.push_back(instanceAt(0, { -3000, 1, -3000 }));  // behind the camera: no shadow in the view's air
            sc.sun.direction = normalize(float3{ 0.55f, 0.35f, 0.2f });
            scene::Camera c;
            c.name = "main";
            c.position = { 3, 1.7f, -2 };
            c.forward = normalize(float3{ 0.8f, 0.05f, 0.3f });
            sc.cameras.push_back(c);
            run(sc, 1);
            const std::vector<uint8_t> air = lastVolume;
            injectMedia = [&](FramePassContext& fc) {
                const TextureRef t = fc.graph.createTexture(TextureDesc{ "test media slices", fg.gridX, fg.gridY, (uint16_t)(2 * fg.slices), 1,
                                                                        DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D });
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Shadow/Tests/MediaSlices");
                const uint32_t gx = fg.gridX, gy = fg.gridY, sl = fg.slices;
                fc.graph.addPass("s.test.media", QueueType::Compute, [&](PassBuilder& b) { b.use(t, Use::UavCompute); },
                                 [=](PassContext& ctx) {
                                     const uint32_t k[4] = { ctx.uav(t), gx, gy, sl };
                                     ctx.cmd->SetPipelineState(pso);
                                     ctx.computeConstants(k, 4);
                                     ctx.cmd->Dispatch((gx * gy + 7) / 8, (sl + 7) / 8, 1);
                                 });
                return t;
            };
            run(sc, 1);
            injectMedia = nullptr;
            const std::vector<uint8_t> mixed = lastVolume;
            // MediaSlices.hlsl's formula (fp16 as stored).
            auto mediaAt = [&](uint32_t tx, uint32_t ty, uint32_t s, ref::D3& tau, ref::D3& source) {
                tau = { 0, 0, 0 };
                source = { 0, 0, 0 };
                if ((tx + ty) % 3 == 0 && s >= 6 && s < 18)
                {
                    const double k = 0.08 * (1 + 0.1 * (s - 6.0)), q = 1 + 0.03 * s;
                    tau = { k, k * 0.85, k * 0.7 };
                    source = { 900 * q, 1200 * q, 1500 * q };
                }
                if (ty % 4 == 1 && s >= 30 && s < 34)
                {
                    tau = { tau.x + 0.5, tau.y + 0.5, tau.z + 0.5 };
                    source = { source.x + 300, source.y + 250, source.z + 200 };
                }
                auto h = [](double v) { return roundHalf((double)(float)v); };  // as stored (fp16 nearest)
                tau = { h(tau.x), h(tau.y), h(tau.z) };
                source = { h(source.x), h(source.y), h(source.z) };
            };
            auto g = [](double x) { return x > 1e-6 ? (1 - std::exp(-x)) / x : 1 - 0.5 * x; };
            const uint32_t N = fg.slices + 1;
            const double exposure = 1.0 / (1.2 * std::exp2(grid.view.ev100));
            auto rawSlice = [&](const std::vector<uint8_t>& vol, uint32_t tx, uint32_t ty, uint32_t slice) {
                uint16_t hv[4];
                std::memcpy(hv, vol.data() + (size_t)slice * volumePitch * fg.gridY + (size_t)ty * volumePitch + (size_t)tx * 8, 8);
                return ref::D3{ halfToFloat(hv[0]), halfToFloat(hv[1]), halfToFloat(hv[2]) };
            };
            double worstL = 0, worstTau = 0, worstSky = 0, worstMedia = 0, airSky = 0, maxL = 0;
            uint32_t mediaTiles = 0;
            for (uint32_t ty = 0; ty < fg.gridY; ++ty)
                for (uint32_t tx = 0; tx < fg.gridX; ++tx)
                    for (uint32_t n = 0; n <= fg.slices; ++n)
                    {
                        const ref::D3 l = nodeOf(air, tx, ty, n);
                        maxL = std::max({ maxL, l.x, l.y, l.z });
                    }
            for (uint32_t ty = 0; ty < fg.gridY; ++ty)
                for (uint32_t tx = 0; tx < fg.gridX; ++tx)
                {
                    double L[3] = {}, T[3] = {}, sky[3] = {}, mTotal[3] = {}, mBefore[3] = {};
                    for (uint32_t s = 0; s < fg.slices; ++s)
                    {
                        ref::D3 tp, sp;
                        mediaAt(tx, ty, s, tp, sp);
                        mTotal[0] += tp.x, mTotal[1] += tp.y, mTotal[2] += tp.z;
                    }
                    mediaTiles += mTotal[0] > 0;
                    for (uint32_t s = 0; s < fg.slices; ++s)
                    {
                        const ref::D3 l0 = nodeOf(air, tx, ty, s), l1 = nodeOf(air, tx, ty, s + 1);
                        const ref::D3 t0 = nodeOf(air, tx, ty, s, 1), t1 = nodeOf(air, tx, ty, s + 1, 1);
                        ref::D3 tp, sp;
                        mediaAt(tx, ty, s, tp, sp);
                        const double la[3] = { l0.x, l0.y, l0.z }, lb[3] = { l1.x, l1.y, l1.z }, ta0[3] = { t0.x, t0.y, t0.z }, ta1[3] = { t1.x, t1.y, t1.z };
                        const double tps[3] = { tp.x, tp.y, tp.z }, sps[3] = { sp.x, sp.y, sp.z };
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            const double ta = std::max(0.0, ta1[ch] - ta0[ch]), src = (lb[ch] - la[ch]) * std::exp(ta0[ch]);  // the slice's air alone
                            const double mix = g(ta + tps[ch]);
                            const double src2 = tps[ch] > 0 ? src * mix / g(ta) + sps[ch] * mix / g(tps[ch]) : src;
                            const double skyTerm = src2 - src * std::exp(-(mTotal[ch] - mBefore[ch]));  // mBefore: media before the slice
                            L[ch] += std::exp(-T[ch]) * src2;
                            sky[ch] += std::exp(-T[ch]) * skyTerm;
                            T[ch] += ta + tps[ch];
                            mBefore[ch] += tps[ch];
                        }
                        const ref::D3 gl = nodeOf(mixed, tx, ty, s + 1), gt = nodeOf(mixed, tx, ty, s + 1, 1);
                        const double gls[3] = { gl.x, gl.y, gl.z }, gts[3] = { gt.x, gt.y, gt.z };
                        for (int ch = 0; ch < 3; ++ch)
                        {
                            worstL = std::max(worstL, std::abs(gls[ch] - L[ch]) / std::max(std::abs(L[ch]), 1e-3 * maxL));
                            worstTau = std::max(worstTau, std::abs(gts[ch] - T[ch]) / std::max(T[ch], 1e-3));
                        }
                    }
                    const ref::D3 gs = rawSlice(mixed, tx, ty, 3 * N), gm = rawSlice(mixed, tx, ty, 3 * N + 1), as = rawSlice(air, tx, ty, 3 * N);
                    const double gss[3] = { gs.x / exposure, gs.y / exposure, gs.z / exposure }, gms[3] = { gm.x, gm.y, gm.z };
                    airSky = std::max({ airSky, std::abs(as.x), std::abs(as.y), std::abs(as.z) });
                    for (int ch = 0; ch < 3; ++ch)
                    {
                        worstSky = std::max(worstSky, std::abs(gss[ch] - sky[ch]) / std::max(std::abs(sky[ch]), 1e-3 * maxL));
                        worstMedia = std::max(worstMedia, std::abs(gms[ch] - mTotal[ch]) / std::max(mTotal[ch], 1e-3));
                    }
                }
            logf("media: %u of %u tiles with media; nodes worst %.2e (in-scattering), %.2e (optical depth); sky correction %.2e, media to far %.2e; "
                 "air-only sky correction %.2e (pre-exposed; 0: no shadowed air or local lights here)\n",
                 mediaTiles, fg.gridX * fg.gridY, worstL, worstTau, worstSky, worstMedia, airSky);
            report(mediaTiles > 10 && airSky / exposure < 1e-5 * maxL, "media: scene without shadowed air or lights (air-only sky correction / largest node)", airSky / exposure / maxL, 1e-5);
            report(worstL < 3e-3, "media: in-scattering nodes vs air-only volume composed with the media (rel.)", worstL, 3e-3);
            report(worstTau < 2e-3, "media: optical depth nodes (rel.)", worstTau, 2e-3);
            report(worstSky < 3e-3, "media: sky correction (rel.)", worstSky, 3e-3);
            report(worstMedia < 2e-3, "media: optical depth to far_m for sky pixels (rel.)", worstMedia, 2e-3);
        }

        // ---- 4. Air perspective at arbitrary (uv, depth) vs the atmosphere reference (no casters, no lights).
        {
            scene::Scene sc;
            sc.name = "air perspective";
            sc.materials.push_back({});
            sc.meshes.push_back(boxMesh("far box", { 1, 1, 1 }));
            sc.instances.push_back(instanceAt(0, { -3000, 1, -3000 }));  // behind the camera: no shadow in the view's air
            sc.sun.direction = normalize(float3{ 0.55f, 0.25f, 0.2f });  // low sun: long paths, strong horizon gradients
            scene::Camera c;
            c.name = "main";
            c.position = { 3, 1.7f, -2 };
            c.forward = normalize(float3{ 0.8f, 0.05f, 0.3f });
            sc.cameras.push_back(c);
            std::vector<float4> queries;
            for (float u : { 0.1f, 0.5f, 0.93f })
                for (float v : { 0.2f, 0.55f, 0.9f })
                    for (float z : { 3.f, 40.f, 700.f, 5000.f, 30000.f }) queries.push_back({ u, v, z, 0 });
            std::shared_ptr<std::vector<uint8_t>> out, ms, vol4, hdr4;
            tf.setScene(sc);
            tf.frame.mainView = ViewDesc::fromCamera(c, W, H, float4x4{});
            tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;
            grid.view = tf.frame.mainView;
            const uint32_t n = (uint32_t)queries.size();
            if (keepFroxels) shadow::setKeepFroxels(tf.trackState, true);
            tf.run([&](FramePassContext& fc) {
                ViewResources main;
                main.view = fc.frame.mainView;
                main.frameConstants = fc.frameConstantsFor(main.view);
                BufferRef early;
                if (uploadFirst) early = tf.uploadBuffer(fc, queries.data(), queries.size() * 16, 16, "probe queries");  // diagnostic
                tracks::atmosphere(fc);
                raster.mainView(fc, main);
                tracks::shadowPages(fc, main);
                tracks::froxels(fc, main);
                BufferRef in = uploadFirst ? early : tf.uploadBuffer(fc, queries.data(), queries.size() * 16, 16, "probe queries");
                BufferRef o = fc.graph.createBuffer(BufferDesc{ "probe out", 3ull * n * 16, 16 });
                ID3D12PipelineState* pso = fc.shaders.compute("Passes/Atmosphere/Tests/AtmosphereProbe.MODE2");
                const FrameResources r = fc.resources;
                const D3D12_GPU_VIRTUAL_ADDRESS cb = main.frameConstants;
                fc.graph.addPass("s.test.probe", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     for (TextureRef t : { r.transmittanceLut, r.multiScatterLut, r.skyViewLut, r.aerialPerspective }) b.use(t, Use::SrvCompute);
                                     b.use(in, Use::SrvCompute);
                                     b.use(o, Use::UavCompute);
                                 },
                                 [=](PassContext& ctx) {
                                     const uint32_t k[8] = { ctx.srv(r.transmittanceLut), ctx.srv(r.multiScatterLut), ctx.srv(r.skyViewLut), ctx.srv(r.aerialPerspective),
                                                             ctx.srv(in), ctx.uav(o), n, 0 };
                                     ctx.cmd->SetPipelineState(pso);
                                     ctx.bindFrameConstants(cb);
                                     ctx.computeConstants(k, 8);
                                     ctx.cmd->Dispatch((n + 63) / 64, 1, 1);
                                 });
                out = tf.readbackBuffer(fc, o, 3ull * n * 16);
                if (debug) vol4 = tf.readback(fc, r.aerialPerspective);
                ms = tf.readback(fc, r.multiScatterLut);
            });
            if (debug && hdr4)
            {
                const uint32_t* hw = reinterpret_cast<const uint32_t*>(hdr4->data());
                logf("  header %u %u %u %u | %u %u %u %u\n", hw[0], hw[1], hw[2], hw[3], hw[8], hw[9], hw[10], hw[11]);
            }
            if (debug && vol4)
            {
                const ref::D3 t30 = nodeOf(*vol4, 5, 5, 30, 1), s30 = nodeOf(*vol4, 5, 5, 30, 0);
                logf("  readback: tau node 30 tile (5,5) %g %g %g, inscatter %g\n", t30.x, t30.y, t30.z, s30.x);
            }
            const atmosphere::AtmosphereParams p = atmosphere::makeParams(sc.atmosphere, tf.quality);
            const ref::D3 sun = ref::normalize(d3(sc.sun.direction)), camPos = d3(c.position);
            // C++ twin of airMultipleScattering (the GPU J_ms table read back, AtmosphereReference msTableLookup): the
            // reference uses the same multiple-scattering source.
            ref::MsTable msTable;
            msTable.texels = ms.get();
            for (int i = 0; i < 4; ++i) msTable.n[i] = p.multiScatterSize[i];
            auto psi = [&](ref::D3 pos, ref::D3 dd, ref::D3 sd) {
                const ref::D3 up = ref::upOf(model, pos);
                const double alt = std::clamp(ref::altitudeOf(model, pos), 0.0, model.top - model.bottom);
                return ref::msTableLookup(model, msTable, alt, ref::dot(up, dd), ref::dot(up, sd), ref::dot(dd, sd));
            };
            auto relErr = [](ref::D3 a, ref::D3 b, double floor) {
                return std::max({ std::abs(a.x - b.x) / std::max(std::abs(b.x), floor), std::abs(a.y - b.y) / std::max(std::abs(b.y), floor),
                                  std::abs(a.z - b.z) / std::max(std::abs(b.z), floor) });
            };
            double worstL = 0, worstT = 0, nearL = 0, nearT = 0, worstE = 0;
            uint32_t mismatch = 0;
            const ViewDesc& v = tf.frame.mainView;
            const float3 fwd = normalize(c.forward);
            for (uint32_t i = 0; i < n; ++i)
            {
                float4 gi, gt, ge;
                std::memcpy(&gi, out->data() + (3 * i) * 16, 16);
                std::memcpy(&gt, out->data() + (3 * i + 1) * 16, 16);
                std::memcpy(&ge, out->data() + (3 * i + 2) * 16, 16);
                mismatch += gi.w != 0 ? 1 : 0;
                const float4 q = queries[i];
                const double ndc[4] = { q.x * 2.0 - 1, 1 - q.y * 2.0, 1, 1 };
                double np[4] = {};
                for (int r = 0; r < 4; ++r)
                    for (int cc = 0; cc < 4; ++cc) np[r] += v.invViewProj.m[r][cc] * ndc[cc];
                const ref::D3 dir = ref::normalize(ref::D3{ np[0] / np[3], np[1] / np[3], np[2] / np[3] } - camPos);
                const double cosA = ref::dot(dir, d3(fwd));
                ref::D3 L, Tr;
                ref::aerial(model, camPos, dir, q.z / cosA, sun, psi, L, Tr, 4096);
                // In-scattering floor 1e-6 per unit illuminance (0.13 nit at 128 klx, < 1e-4 of a sunlit surface).
                const double eL = relErr(ref::D3{ gi.x, gi.y, gi.z } * (1.0 / sc.sun.illuminance), L, 1e-6), eT = relErr(ref::D3{ gt.x, gt.y, gt.z }, Tr, 1e-6);
                // Unshadowed sun illuminance at the surface point, per unit illuminance.
                ref::D3 surface = camPos + dir * (q.z / cosA);
                if (ref::altitudeOf(model, surface) < 0) surface = surface + ref::upOf(model, surface) * -ref::altitudeOf(model, surface);
                const ref::D3 Es = ref::sunTransmittance(model, surface, sun, 4096);
                const double eE = relErr(ref::D3{ ge.x, ge.y, ge.z } * (1.0 / sc.sun.illuminance), Es, 1e-3);
                worstL = std::max(worstL, eL);
                worstT = std::max(worstT, eT);
                worstE = std::max(worstE, eE);
                if (q.z <= 700)
                {
                    nearL = std::max(nearL, eL);
                    nearT = std::max(nearT, eT);
                }
                if (debug) logf("  raw t %g %g %g %g e %g %g %g %g\n", gt.x, gt.y, gt.z, gt.w, ge.x, ge.y, ge.z, ge.w);
                if (eL > 5e-3 || eT > 1e-3 || eE > 1e-3 || debug)
                    logf("  air uv (%.2f %.2f) z %.0f: L gpu %.4e %.4e %.4e ref %.4e %.4e %.4e  T gpu %.5f ref %.5f  Esun gpu %.5f ref %.5f\n", q.x, q.y, q.z,
                         gi.x / sc.sun.illuminance, gi.y / sc.sun.illuminance, gi.z / sc.sun.illuminance, L.x, L.y, L.z, gt.y, Tr.y, ge.y / sc.sun.illuminance, Es.y);
                if (eL > 5e-3)
                {
                    // Diagnosis: the exact air of the four surrounding tile-centre rays, blended as the lookup blends the
                    // volume (bilinear across tiles), against the pixel's own; the rows' blend alone (pixel column).
                    auto exactAt = [&](double px, double py, ref::D3& Lx, ref::D3& Tx) {
                        const double nd[4] = { px / W * 2.0 - 1, 1 - py / H * 2.0, 1, 1 };
                        double wp[4] = {};
                        for (int r = 0; r < 4; ++r)
                            for (int cc = 0; cc < 4; ++cc) wp[r] += v.invViewProj.m[r][cc] * nd[cc];
                        const ref::D3 d = ref::normalize(ref::D3{ wp[0] / wp[3], wp[1] / wp[3], wp[2] / wp[3] } - camPos);
                        ref::aerial(model, camPos, d, q.z / ref::dot(d, d3(fwd)), sun, psi, Lx, Tx, 4096);
                    };
                    const double px = q.x * W, py = q.y * H, tp = fg.tilePx;
                    const double cx = px / tp - 0.5, cy = py / tp - 0.5, x0 = std::floor(cx), y0 = std::floor(cy), fx = cx - x0, fy = cy - y0;
                    ref::D3 l00, l10, l01, l11, lr0, lr1, t;
                    exactAt((x0 + 0.5) * tp, (y0 + 0.5) * tp, l00, t);
                    exactAt((x0 + 1.5) * tp, (y0 + 0.5) * tp, l10, t);
                    exactAt((x0 + 0.5) * tp, (y0 + 1.5) * tp, l01, t);
                    exactAt((x0 + 1.5) * tp, (y0 + 1.5) * tp, l11, t);
                    ref::D3 tr0, tr1, lp, tpx;
                    exactAt(px, (y0 + 0.5) * tp, lr0, tr0);
                    exactAt(px, (y0 + 1.5) * tp, lr1, tr1);
                    exactAt(px, py, lp, tpx);
                    // Candidates: blend of L / (1 - T) times the pixel's own 1 - T; blend in log L.
                    auto hat = [](ref::D3 l, ref::D3 tt) { return ref::D3{ l.x / (1 - tt.x), l.y / (1 - tt.y), l.z / (1 - tt.z) }; };
                    const ref::D3 hb = hat(lr0, tr0) * (1 - fy) + hat(lr1, tr1) * fy;
                    const ref::D3 hatL{ hb.x * (1 - tpx.x), hb.y * (1 - tpx.y), hb.z * (1 - tpx.z) };
                    const ref::D3 logL{ std::exp(std::log(lr0.x) * (1 - fy) + std::log(lr1.x) * fy), std::exp(std::log(lr0.y) * (1 - fy) + std::log(lr1.y) * fy),
                                        std::exp(std::log(lr0.z) * (1 - fy) + std::log(lr1.z) * fy) };
                    // The GPU's pixel transmittance: optical depth blended across the rows.
                    auto odBlend = [&](double t0, double t1) { return std::exp(-(-std::log(t0) * (1 - fy) - std::log(t1) * fy)); };
                    const ref::D3 tb{ odBlend(tr0.x, tr1.x), odBlend(tr0.y, tr1.y), odBlend(tr0.z, tr1.z) };
                    const ref::D3 hatB{ hb.x * (1 - tb.x), hb.y * (1 - tb.y), hb.z * (1 - tb.z) };
                    logf("    candidate with the blended optical depth: rel %.4f (T %.4f)%c", relErr(hatB, L, 1e-6), tb.y, (char)10);
                    const char nl[2] = { (char)10, 0 };
                    logf("    candidates: L/(1-T) blend x (1 - T pixel) rel %.4f; log blend rel %.4f; T rows %.4f / %.4f pixel %.4f%s", relErr(hatL, L, 1e-6),
                         relErr(logL, L, 1e-6), tr0.y, tr1.y, tpx.y, nl);
                    const ref::D3 bl = (l00 * (1 - fx) + l10 * fx) * (1 - fy) + (l01 * (1 - fx) + l11 * fx) * fy;
                    const ref::D3 rows = lr0 * (1 - fy) + lr1 * fy;
                    logf("    exact tile blend: bilinear %.4e (rel %.4f), rows at the pixel column %.4e (rel %.4f); rows %.4e / %.4e, fy %.3f\n", bl.y,
                         relErr(bl, L, 1e-6), rows.y, relErr(rows, L, 1e-6), lr0.y, lr1.y, fy);
                }
            }
            report(mismatch == 0, "atmosphereAerial and atmosphereAirView agree to 1e-5 (queries differing)", mismatch, 0);
            report(nearL < 1e-2, "air in-scattering, depth <= 700 m (rel.)", nearL, 1e-2);
            report(nearT < 1e-4, "air transmittance, depth <= 700 m (rel.)", nearT, 1e-4);
            report(worstL < 2e-2, "air in-scattering, all depths to 30 km (rel.)", worstL, 2e-2);
            report(worstT < 1e-2, "air transmittance, all depths to 30 km (rel.)", worstT, 1e-2);
            report(worstE < 2e-3, "sun illuminance at the surface (rel.)", worstE, 2e-3);
        }

        report(shadow::stats(tf.trackState).errorBitsSeen == 0, "S error bits (INTERFACES 3.6: a shader loop at its hard cap)",
               shadow::stats(tf.trackState).errorBitsSeen, 0);
        if (debugLayer) logf("D3D12 debug layer: enabled (errors abort the run)\n");
        // ---- 1b (run last: section 2's largest-node check depends on the frames run before it, S_STATUS 10). FX particle
        // lights (A3, INTERFACES v1.79): gpu::Light point records after the scene lights, written by a
        // copy as the FX module's pass would (FrameResources::fxLights / fxLightCount); the lists take them like scene lights
        // (froxelLightTotal) and never flag them as shadowed.
        {
            // Its own random numbers: the later sections keep the scenes they had before this one existed.
            std::mt19937 rngFx(77);
            auto uniFx = [&](float a, float b) { return std::uniform_real_distribution<float>(a, b)(rngFx); };
            scene::Scene sc = base;
            sc.sun.direction = normalize(float3{ 0.3f, 0.8f, 0.2f });
            for (int i = 0; i < 3; ++i)
            {
                scene::Light l;
                l.position = { uniFx(-4, 4), uniFx(1, 4), uniFx(6, 30) };
                l.intensity = uniFx(100, 1000);
                l.range = uniFx(4, 10);
                sc.lights.push_back(l);
            }
            std::vector<scene::Light> fxLights;
            std::vector<gpu::Light> records;
            for (int i = 0; i < 12; ++i)
            {
                scene::Light l;
                l.position = { uniFx(-5, 5), uniFx(0.5f, 4), uniFx(4, 40) };
                l.intensity = uniFx(200, 5000);
                l.range = uniFx(3, 7);
                fxLights.push_back(l);
                gpu::Light g{};
                g.position = l.position;
                g.typeFlags = (uint32_t)scene::LightType::Point | (0xFFFFu << 16);
                g.forward = { 0, 0, 1 };
                g.right = { 1, 0, 0 };
                g.range = l.range;
                g.intensity = l.intensity;
                g.color = { 1, 0.6f, 0.3f };
                records.push_back(g);
            }
            tf.setScene(sc);
            if (!tf.gpuScene.setFxLightCapacity(16)) fail("FX light capacity refused");
            const uint32_t fxCount = (uint32_t)records.size();
            beforeFroxels = [&](FramePassContext& fc) {
                const GpuScene::FxLightRange r = tf.gpuScene.fxLightRange();
                fc.resources.fxLights = fc.graph.importBuffer(r.lightBuffer, BufferDesc{ "scene lights (FX tail)", (uint64_t)(r.first + r.capacity) * sizeof(gpu::Light), (uint32_t)sizeof(gpu::Light) });
                fc.resources.fxLightCount = fc.graph.importBuffer(r.countBuffer, BufferDesc{ "FX light count", 16, 4 });
                const uint32_t countWords[4] = { fxCount, 0, 0, 0 };
                const BufferRef src = tf.uploadBuffer(fc, records.data(), records.size() * sizeof(gpu::Light), 0, "fx.lights");
                const BufferRef srcCount = tf.uploadBuffer(fc, countWords, 16, 0, "fx.count");
                const BufferRef lights = fc.resources.fxLights, count = fc.resources.fxLightCount;
                fc.graph.addPass("test.fx.lights", QueueType::Graphics,
                                 [&](PassBuilder& b) {
                                     b.use(src, Use::CopySrc);
                                     b.use(srcCount, Use::CopySrc);
                                     b.use(lights, Use::CopyDst);
                                     b.use(count, Use::CopyDst);
                                 },
                                 [=](PassContext& c) {
                                     c.cmd->CopyBufferRegion(c.resource(lights), (uint64_t)r.first * sizeof(gpu::Light), c.resource(src), 0, records.size() * sizeof(gpu::Light));
                                     c.cmd->CopyBufferRegion(c.resource(count), 0, c.resource(srcCount), 0, 16);
                                 });
            };
            run(sc, 2);
            tf.quality.applyOverride("atmosphere.froxels.list_fallback_forced=1");  // a frame over the capacity: FX lights left out
            run(sc, 1);
            tf.quality.applyOverride("atmosphere.froxels.list_fallback_forced=0");
            const Lists fbLists{ lastLists };
            run(sc, 1);
            beforeFroxels = nullptr;
            const Lists lists{ lastLists };
            const uint32_t N = (uint32_t)sc.lights.size(), indexBase = lists.word(36);
            uint32_t flagged = 0, outOfRange = 0, fxEntries = 0, missing = 0, points = 0, fxReaching = 0;
            for (uint32_t f = 0; f < F; ++f)
            {
                const uint32_t first = lists.first(f), n = lists.count(f);
                for (uint32_t i = 0; i < n; ++i)
                {
                    const uint32_t w = lists.word(indexBase + ((first + i) >> 1) * 4), e = (first + i) & 1 ? w >> 16 : w & 0xFFFF, li = e & 0x7FFF;
                    if (li >= N + fxCount) ++outOfRange;
                    if (li >= N)
                    {
                        ++fxEntries;
                        if (e & 0x8000) ++flagged;
                    }
                }
            }
            {
                // The fallback frame: every froxel's list is the normal one without its FX entries; the FX entries are the
                // lost entries, the froxels that had any are the cut lists.
                uint32_t differing = 0, cutLists = 0;
                for (uint32_t f = 0; f < F; ++f)
                {
                    std::vector<uint32_t> expect;
                    for (uint32_t li : lists.list(f)) if (li < N) expect.push_back(li);
                    if (expect.size() != lists.list(f).size()) ++cutLists;
                    if (fbLists.list(f) != expect) ++differing;
                }
                logf("forced fallback with FX lights: %u entries stored, %u lists cut, %u entries lost (FX entries of the normal frame %u in %u lists)\n",
                     fbLists.word(44), fbLists.word(48), fbLists.word(52), fxEntries, cutLists);
                report(differing == 0, "fallback with FX lights: scene lights listed as usual, FX lights left out (froxels differing)", differing, 0);
                report(fbLists.word(52) == fxEntries && fbLists.word(48) == cutLists, "fallback with FX lights: lost entries and cut lists counted", fbLists.word(52), fxEntries);
            }
            const ref::D3 camPos = d3(grid.view.position);
            for (uint32_t s = 0; s + 1 < fg.slices; ++s)
                for (uint32_t ty = 0; ty < fg.gridY; ++ty)
                    for (uint32_t tx = 0; tx < fg.gridX; ++tx)
                    {
                        if ((tx * 7 + ty * 13 + s * 3) % 3 != 0) continue;
                        const uint32_t f = (s * fg.gridY + ty) * fg.gridX + tx;
                        const std::vector<uint32_t> li = lists.list(f);
                        const std::set<uint32_t> in(li.begin(), li.end());
                        for (int k = 0; k < 4; ++k)
                        {
                            const double px = std::min((tx + uniFx(0, 1)) * fg.tilePx, (float)W - 1e-3f), py = std::min((ty + uniFx(0, 1)) * fg.tilePx, (float)H - 1e-3f);
                            const double z = grid.node(s) + (grid.node(s + 1) - grid.node(s)) * uniFx(0, 1);
                            const ref::D3 p = camPos + grid.rayAt(px, py) * z;
                            ++points;
                            for (uint32_t j = 0; j < fxCount; ++j)
                            {
                                if (!reaches(fxLights[j], p)) continue;
                                ++fxReaching;
                                if (!in.count(N + j) && li.size() < listMax) ++missing;
                            }
                        }
                    }
            logf("FX lights: %u list entries, %u (point, FX light) pairs that reach over %u points\n", fxEntries, fxReaching, points);
            report(fxEntries > 0 && fxReaching > 50, "FX lights reach the probed froxels (list entries)", fxEntries, 1);
            report(missing == 0, "FX lights reaching a froxel point missing from a non-full list", missing, 0);
            report(flagged == 0, "FX light entries with the shadow flag", flagged, 0);
            report(outOfRange == 0, "list entries past the scene and FX lights", outOfRange, 0);
            if (!tf.gpuScene.setFxLightCapacity(0)) fail("FX light capacity 0 refused");
        }

        logf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
