// S froxels, correctness (no GPU lock). Test stand-ins for V's raster and M's G-buffer (TestRaster.h).
//  1. light lists (scene of scattered lights plus a dense cluster): every light that reaches a point of a froxel is in
//     its list unless the list is full with lights at least as important; lists ordered by importance; no duplicates;
//     truncation statistics match; lists identical over two frames;
//  2. local lights' in-scattering by the air (sun below the horizon, lists not truncated): each node of the volume against
//     a double-precision integral along the tile-centre ray (every light, fine steps, continuous air coefficients);
//  3. the sun's shadowed air (roof over part of the view, no local lights): per slice, the removed in-scattering against
//     the exact integral with the roof and ground boxes ray-cast towards the sun; slices entirely lit or entirely
//     shadowed must match to the storage precision (the block hierarchy classifies them exactly), all slices within the
//     texel-resolution bound;
//  4. D3D12 debug layer clean.
//   unx_test_shadow_froxeltests [--no-debug-layer] [--width W --height H] [--set key=value]
#include "TestRaster.h"

#include "../../Atmosphere/AtmosphereReference.h"
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
            out.push_back((first + i) & 1 ? w >> 16 : w & 0xFFFF);
        }
        return out;
    }
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, debug = false;
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
        auto node = [&](uint32_t tx, uint32_t ty, uint32_t n) -> ref::D3 {  // node n >= 1 = volume slice n - 1
            uint16_t h[4];
            std::memcpy(h, lastVolume.data() + (size_t)(n - 1) * volumePitch * fg.gridY + (size_t)ty * volumePitch + (size_t)tx * 8, 8);
            const double e = 1.0 / (1.2 * std::exp2(grid.view.ev100));  // stored pre-exposed
            return { halfToFloat(h[0]) / e, halfToFloat(h[1]) / e, halfToFloat(h[2]) / e };
        };
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

        // ---- 2. Local lights in the air (sun below the horizon: no sun term).
        {
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
                sc.lights.push_back(l);
            }
            run(sc, 1, -2.0f);  // night exposure: the lights' air glow is displayed
            const Lists lists{ lastLists };
            report(lists.word(48) == 0, "local-light scene: no truncated list", lists.word(48), 0);
            // Reference: fine midpoint integration of every light along the tile-centre ray, continuous air coefficients.
            double worst = 0, sumErr = 0, sumRef = 0;
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
                                acc = acc + T * ph * (intensity(L, w) * window(L, d) / (dd * dd) * dt) * d3(L.color);
                            }
                            tau = tau + c.extinction * dt;
                        }
                        const ref::D3 g = node(tx, ty, n);
                        if (debug && tx == 3 && ty == 2 && n <= 12)
                        {
                            uint16_t hh[4];
                            std::memcpy(hh, lastVolume.data() + (size_t)(n - 1) * volumePitch * fg.gridY + (size_t)ty * volumePitch + (size_t)tx * 8, 8);
                            logf("  raw %04x %04x %04x %04x (volume bytes %zu, expected %zu)\n", hh[0], hh[1], hh[2], hh[3], lastVolume.size(), (size_t)volumePitch * fg.gridY * fg.slices);
                        }
                        if (debug && tx == 3 && ty == 2 && n <= 12)
                            logf("  tile (3,2) node %u z %.3f: gpu %.4g %.4g %.4g ref %.4g %.4g %.4g\n", n, grid.node(n), g.x, g.y, g.z, acc.x, acc.y, acc.z);
                        const double r = acc.x + acc.y + acc.z, e = std::abs(g.x - acc.x) + std::abs(g.y - acc.y) + std::abs(g.z - acc.z);
                        const double exposure = 1.0 / (1.2 * std::exp2(grid.view.ev100));
                        if (r * exposure > 1e-3)  // displayed above 1e-3 of white: fp16 holds it to 1e-3 relative
                        {
                            if (e / r > worst && debug)
                                logf("  worse: tile (%u,%u) node %u z %.2f gpu %.4g %.4g %.4g ref %.4g %.4g %.4g\n", tx, ty, n, grid.node(n), g.x, g.y, g.z, acc.x, acc.y, acc.z);
                            worst = std::max(worst, e / r);
                            sumErr += e;
                            sumRef += r;
                            ++compared;
                        }
                    }
                }
            logf("local lights: %u nodes compared, largest relative error %.3g\n", compared, worst);
            report(compared > 100 && sumErr / sumRef < 0.01, "air in-scattering of local lights vs reference (mean relative)", sumErr / std::max(sumRef, 1e-30), 0.01);
            report(worst < 0.03, "air in-scattering of local lights vs reference (largest node)", worst, 0.03);
        }

        // ---- 3. Sun shadows in the air (roof, no local lights).
        {
            scene::Scene sc = base;
            sc.sun.direction = normalize(float3{ 0.35f, 0.85f, -0.4f });
            run(sc, 1);
            const ref::D3 sun = d3(sc.sun.direction), camPos = d3(grid.view.position);
            const double E = sc.sun.illuminance;
            const shadow::VsmConstantsCpu& vc = shadow::lastConstants(tf.trackState);
            const ref::D3 lx = d3(vc.lightX), ly = d3(vc.lightY);
            double worstExact = 0, sumErr = 0, sumRef = 0;
            uint32_t exactSlices = 0, mixedSlices = 0, mixedOver = 0;
            for (uint32_t ty = 1; ty < fg.gridY; ty += 4)
                for (uint32_t tx = 2; tx < fg.gridX; tx += 5)
                {
                    const ref::D3 ray = grid.tileRay(tx, ty);
                    const double toRay = std::sqrt(ref::dot(ray, ray));
                    const ref::D3 dir = ray * (1 / toRay);
                    const double nu = ref::dot(dir, sun);
                    ref::D3 tau{}, prevNode{};
                    for (uint32_t n = 1; n <= fg.slices; ++n)
                    {
                        const double z0 = grid.node(n - 1), z1 = grid.node(n);
                        const double t0 = z0 * toRay, t1 = z1 * toRay;
                        if (t1 > 400) break;  // the roof's shadow and the view's ground are nearer
                        const int steps = 256;
                        const double dt = (t1 - t0) / steps;
                        ref::D3 slice{};
                        int shadowed = 0, belowGround = 0, nearBoundary = 0;
                        double peak = 0;
                        // Texel of the air level at this slice (vsmAirLevel): a slice is 'exact' when no point of it lies
                        // within 1.5 texels (laterally, seen from the sun) of a shadow boundary.
                        const double width = fg.tilePx * 2 * 0.5 * (z0 + z1) * std::tan(grid.view.verticalFov * 0.5) / H / fg.shadowTexelsPerTile;
                        const int level = std::clamp((int)std::floor(std::log2(std::max(width, 1e-30) * 1024.0) + vc.lodBias), 0, 11);
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
                                for (const Box& b : boxes)
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
                        const ref::D3 gNode = node(tx, ty, n);
                        if (debug && tx == 2 && ty == 13 && n <= 40)
                            logf("  tile (2,13) node %u z %.3f: gpu %.4g ref slice %.4g | shadowed %d/%d near %d\n", n, z1, gNode.x - prevNode.x, slice.x, shadowed, steps, nearBoundary);
                        const ref::D3 gSlice = gNode - prevNode;
                        prevNode = gNode;
                        if (belowGround) continue;  // beyond the ground: no pixel reads it
                        const double r = std::abs(slice.x) + std::abs(slice.y) + std::abs(slice.z);
                        const double e = std::abs(gSlice.x - slice.x) + std::abs(gSlice.y - slice.y) + std::abs(gSlice.z - slice.z);
                        const double nodeMag = std::abs(gNode.x) + std::abs(gNode.y) + std::abs(gNode.z);
                        const double storage = 2e-3 * nodeMag;  // two fp16 nodes
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
                        }
                        sumErr += e;
                        sumRef += r;
                    }
                }
            logf("sun shadows in the air: %u slices entirely lit or shadowed, %u with a boundary\n", exactSlices, mixedSlices);
            report(exactSlices > 100 && worstExact == 0, "entirely lit / shadowed slices beyond storage precision (worst rel.)", worstExact, 0);
            report(mixedSlices > 20 && mixedOver == 0, "slices with a shadow boundary outside the texel bound", mixedOver, 0);
            report(sumErr / std::max(sumRef, 1e-30) < 0.03, "shadowed air in-scattering vs reference (mean relative)", sumErr / std::max(sumRef, 1e-30), 0.03);
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
