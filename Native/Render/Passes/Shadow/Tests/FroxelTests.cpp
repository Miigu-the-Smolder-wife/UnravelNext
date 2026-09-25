// S froxels, correctness (no GPU lock). Test stand-ins for V's raster and M's G-buffer (TestRaster.h).
//  1. light lists (scene of scattered lights plus a dense cluster): every light that reaches a point of a froxel is in
//     its list unless the list is full with lights at least as important; lists ordered by importance; no duplicates;
//     truncation statistics match; lists identical over two frames;
//  2. local lights' in-scattering by the air (sun below the horizon, lists not truncated): each node of the air volume
//     with the lights minus without them, against a double-precision integral along the tile-centre ray (every light,
//     fine steps, continuous air coefficients);
//  3. the sun's shadowed air: per slice, the in-scattering removed by a roof (volume with the roof minus without it)
//     against the exact integral with the roof ray-cast towards the sun; slices entirely lit or entirely shadowed must
//     match to the storage precision (the block hierarchy classifies them exactly), all slices within the
//     texel-resolution bound;
//  4. the air perspective (atmosphereAirView = atmosphereAerial + sun illuminance, no casters or lights) at arbitrary
//     (uv, depth) against the double-precision atmosphere reference along the pixel's own ray (AtmosphereReference.h);
//  5. D3D12 debug layer clean.
//   unx_test_shadow_froxeltests [--no-debug-layer] [--width W --height H] [--set key=value]
#include "TestRaster.h"

#include "../../Atmosphere/AtmosphereReference.h"
#include "../../Atmosphere/AtmosphereSystem.h"
#include "FroxelSystem.h"
#include "VsmSystem.h"

#include <algorithm>
#include <cstdio>
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
    uint32_t header(uint32_t i) const { uint32_t h; std::memcpy(&h, raw.data() + 64 + i * 4, 4); return h; }
    uint32_t word(uint32_t byte) const { uint32_t w; std::memcpy(&w, raw.data() + byte, 4); return w; }
    std::vector<uint32_t> list(uint32_t froxel) const
    {
        const uint32_t h = header(froxel), first = h >> 6, count = h & 63, indexBase = word(36);
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
            else if (a == "--keep") keepFroxels = true;
            else if (a == "--upload-first") uploadFirst = true;  // diagnostic: probe input declared before the froxel passes
        }
        TestFrame tf(debugLayer);
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
        const uint32_t listMax = (uint32_t)tf.quality.integer("atmosphere.froxels.lights_max");
        const uint32_t stride = (listMax + 1) & ~1u;
        const uint64_t listBytes = 64 + (uint64_t)F * 4 + (uint64_t)F * stride * 2;
        Grid grid{ fg, {}, std::log2((double)fg.farM / fg.nearM) };

        std::vector<uint8_t> lastLists, lastVolume;
        auto run = [&](const scene::Scene& sc, int frames, float ev100 = 14.0f) {
            tf.setScene(sc);
            tf.frame.mainView = ViewDesc::fromCamera(sc.cameras[0], W, H, float4x4{});
            tf.frame.mainView.prevViewProj = tf.frame.mainView.viewProj;
            tf.frame.mainView.ev100 = ev100;
            tf.frame.deltaTime = 1.0f / 60;
            grid.view = tf.frame.mainView;
            for (int f = 0; f < frames; ++f)
            {
                std::shared_ptr<std::vector<uint8_t>> lists, volume;
                const bool read = f + 1 == frames;  // earlier frames without readback: the plan changes (consumer culled)
                tf.run([&](FramePassContext& fc) {
                    ViewResources main;
                    main.view = fc.frame.mainView;
                    main.frameConstants = fc.frameConstantsFor(main.view);
                    tracks::atmosphere(fc);
                    raster.mainView(fc, main);
                    tracks::shadowPages(fc, main);
                    tracks::froxels(fc, main);
                    if (read)
                    {
                        lists = tf.readbackBuffer(fc, fc.resources.froxelLights, listBytes);
                        volume = tf.readback(fc, fc.resources.froxels);
                    }
                });
                tf.frame.time += tf.frame.deltaTime;
                if (read)
                {
                    lastLists = *lists;
                    lastVolume = *volume;
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
            const uint32_t indexCount = lists.word(44), overflow = lists.word(48), dropped = lists.word(52), maxCount = lists.word(56), candOverflow = lists.word(60);
            logf("lists: %u entries (%.2f per froxel), %u froxels truncated (%u lights dropped), max %u lights per froxel, %u tiles over the candidate buffer\n",
                 indexCount, (double)indexCount / F, overflow, dropped, maxCount, candOverflow);
            report(candOverflow == 0, "candidate buffer not exceeded (tiles)", candOverflow, 0);
            report(overflow > 0 && maxCount > listMax, "the dense cluster truncates some lists (truncated froxels)", overflow, 1);

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
                        if (li.size() > listMax) ++malformed;
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
                                if (li.size() < listMax) ++missing;  // a full list may leave out lights of lower importance
                            }
                        }
                    }
            // Order: importance at the froxel centre, recomputed as the kernel does, must not increase along a list.
            for (uint32_t f = 0; f < F; f += 97)
            {
                const uint32_t s = f / (fg.gridX * fg.gridY), ty = (f / fg.gridX) % fg.gridY, tx = f % fg.gridX;
                const std::vector<uint32_t> li = lists.list(f);
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
            report(malformed == 0, "froxel lists well formed (count <= lights_max, unique, valid)", malformed, 0);
            logf("conservativeness: %u froxels, %u points\n", checkedFroxels, samples);
            report(missing == 0, "lights reaching a froxel point missing from a non-full list", missing, 0);
            report(disorder == 0, "lists out of importance order (entries)", disorder, 0);
        }

        // ---- 2. Local lights in the air (sun below the horizon: no sun term); then the same lights casting shadows (their
        //         VSM shadows their air: the roof above them cuts their glow above it).
        for (const bool shadowed : { false, true })
        {
            const std::string what = shadowed ? "shadowed air of local lights" : "air in-scattering of local lights";
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
            scene::Scene dark = sc;
            dark.lights.clear();
            run(dark, 1, -2.0f);  // night exposure: the lights' air glow is displayed
            const std::vector<uint8_t> without = lastVolume;
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
                            for (const scene::Light& L : sc.lights)
                            {
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
                                acc = acc + T * ph * (intensity(L, w) * window(L, d) / (dd * dd) * dt) * d3(L.color);
                            }
                            tau = tau + c.extinction * dt;
                        }
                        const ref::D3 g = node(tx, ty, n) - nodeOf(without, tx, ty, n);
                        if (debug && tx == 3 && ty == 2 && n <= 12)
                            logf("  tile (3,2) node %u z %.3f: gpu %.4g %.4g %.4g ref %.4g %.4g %.4g\n", n, grid.node(n), g.x, g.y, g.z, acc.x, acc.y, acc.z);
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
            // C++ twin of airMultipleScattering (bilinear on the GPU LUT): the reference uses the same Psi_ms.
            auto psi = [&](ref::D3 pos, ref::D3 sd) {
                const double alt = std::clamp(ref::altitudeOf(model, pos) / (model.top - model.bottom), 0.0, 1.0);
                const double qx = (ref::dot(ref::upOf(model, pos), sd) * 0.5 + 0.5) * (p.multiScatterSize[0] - 1), qy = alt * (p.multiScatterSize[1] - 1);
                const uint32_t x0 = std::min((uint32_t)qx, p.multiScatterSize[0] - 1), y0 = std::min((uint32_t)qy, p.multiScatterSize[1] - 1);
                const uint32_t x1 = std::min(x0 + 1, p.multiScatterSize[0] - 1), y1 = std::min(y0 + 1, p.multiScatterSize[1] - 1);
                const double fx = qx - x0, fy = qy - y0;
                auto at = [&](uint32_t x, uint32_t y) { const float4 t = texel(*ms, p.multiScatterSize[0], p.multiScatterSize[1] + 1, x, y); return ref::D3{ t.x, t.y, t.z }; };
                return (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
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

        if (debugLayer) logf("D3D12 debug layer: enabled (errors abort the run)\n");
        logf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
