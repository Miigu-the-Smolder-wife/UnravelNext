#include "Path.h"

#include "Environment.h"
#include "Rng.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>

namespace unx::rpp
{
namespace
{
constexpr float kDt = 1.0f / kTickHz;

// A camera key: position mode 'ground' keeps 'height' above the smoothed ground (water surface over the lake, the lodge
// floor inside the lodge); 'absolute' uses p.y. The look target is interpolated like the position.
struct Key
{
    float t;
    float3 p;
    float3 look;
    bool ground = true;
    float height = 1.7f;
    bool cut = false;  // starts a new shot (no interpolation from the previous key)
};

struct Ground
{
    const World& w;
    Terrain terrain;
    explicit Ground(const World& world) : w(world), terrain(world.layout) {}
    float waterY() const { return w.layout.lake.translation.y; }
    bool inLodge(float3 p) const
    {
        const float3 l = w.layout.lodge.toLocal(p);
        return std::fabs(l.x) < 4.2f && std::fabs(l.z) < 3.2f;
    }
    float at(float x, float z) const  // ground a camera stands on
    {
        const float3 p{ x, 0, z };
        if (inLodge(p)) return w.layout.lodge.translation.y;
        float h = terrain.height(x, z);
        if (terrain.lakeWeight(x, z) > 0.5f) h = std::max(h, waterY());
        return h;
    }
    float smoothed(float x, float z, float radius) const
    {
        if (inLodge({ x, 0, z })) return w.layout.lodge.translation.y;
        float sum = 0;
        for (int j = -2; j <= 2; ++j)
            for (int i = -2; i <= 2; ++i) sum += at(x + radius * 0.5f * i, z + radius * 0.5f * j);
        return std::max(sum / 25.0f, at(x, z) - 0.3f);
    }
};

float3 hermite(float3 p0, float3 m0, float3 p1, float3 m1, float t, float dt)
{
    const float t2 = t * t, t3 = t2 * t;
    return p0 * (2 * t3 - 3 * t2 + 1) + m0 * ((t3 - 2 * t2 + t) * dt) + p1 * (-2 * t3 + 3 * t2) + m1 * ((t3 - t2) * dt);
}

// Keys along a polyline with a speed profile: v0 at t0, accelerate for 'accel' s to the cruise speed, cruise, decelerate
// for 'decel' s to v1 at t1; the cruise speed is solved so the distance is the polyline length. One key every 0.25 s.
void profileKeys(std::vector<Key>& keys, const std::vector<float3>& route, float t0, float t1, float v0, float v1, float accel, float decel, float height, bool ground,
                 float lookAhead)
{
    std::vector<float> cum{ 0 };
    for (size_t k = 1; k < route.size(); ++k) cum.push_back(cum.back() + length(route[k] - route[k - 1]));
    const float L = cum.back(), T = t1 - t0;
    // L = accel (v0 + v)/2 + v (T - accel - decel) + decel (v + v1)/2
    const float v = (L - 0.5f * accel * v0 - 0.5f * decel * v1) / (0.5f * accel + (T - accel - decel) + 0.5f * decel);
    auto dist = [&](float t) {
        const float u = t - t0;
        if (u <= accel) return v0 * u + 0.5f * (v - v0) / accel * u * u;
        const float sa = 0.5f * accel * (v0 + v);
        if (u <= T - decel) return sa + v * (u - accel);
        const float w = u - (T - decel);
        return sa + v * (T - accel - decel) + v * w + 0.5f * (v1 - v) / decel * w * w;
    };
    auto at = [&](float s) {
        s = std::clamp(s, 0.0f, L);
        size_t k = 1;
        while (k + 1 < cum.size() && cum[k] < s) ++k;
        const float f = (s - cum[k - 1]) / std::max(1e-6f, cum[k] - cum[k - 1]);
        return route[k - 1] + (route[k] - route[k - 1]) * f;
    };
    for (float t = t0 + 0.25f; t <= t1 + 1e-4f; t += 0.25f)
    {
        const float s = dist(t);
        Key k{ t, at(s), at(s + lookAhead) };
        k.ground = ground;
        k.height = height;
        k.look.y = ground ? 0 : k.p.y;  // resolved below for ground keys
        keys.push_back(k);
    }
    logf("path profile %.2f..%.2f s: %.0f m, cruise %.1f m/s\n", t0, t1, L, v);
}

struct TreeRef
{
    float x, y, z, s;
};
struct TreeGrid
{
    float cell = 8.0f;
    std::unordered_map<int64_t, std::vector<TreeRef>> cells;
    static int64_t key(int i, int j) { return ((int64_t)i << 32) ^ (uint32_t)j; }
    void add(const TreeRef& t) { cells[key((int)std::floor(t.x / cell), (int)std::floor(t.z / cell))].push_back(t); }
    template <class F>
    void near(float x, float z, float r, F f) const
    {
        const int i0 = (int)std::floor((x - r) / cell), i1 = (int)std::floor((x + r) / cell), j0 = (int)std::floor((z - r) / cell), j1 = (int)std::floor((z + r) / cell);
        for (int i = i0; i <= i1; ++i)
            for (int j = j0; j <= j1; ++j)
            {
                auto it = cells.find(key(i, j));
                if (it != cells.end())
                    for (const TreeRef& t : it->second) f(t);
            }
    }
};
// C's tree meshes (Foliage.cpp): trunk radius 0.35 at the base (x scale), branches from 3.5 m, crown ellipsoid 3.5 / 3 / 3.5
// around 7 m; all x instance scale.
float trunkClearance(const TreeRef& t, float3 p)
{
    if (p.y < t.y - 0.3f || p.y > t.y + 3.5f * t.s) return 1e30f;
    return std::sqrt((p.x - t.x) * (p.x - t.x) + (p.z - t.z) * (p.z - t.z)) - 0.35f * t.s;
}
float crownClearance(const TreeRef& t, float3 p)
{
    // Distance outside the crown ellipsoid (conservative: scaled to a unit sphere, times the smallest radius) plus branches.
    const float3 c{ t.x, t.y + 7.0f * t.s, t.z };
    const float3 d{ (p.x - c.x) / (3.6f * t.s), (p.y - c.y) / (3.4f * t.s), (p.z - c.z) / (3.6f * t.s) };
    const float r = length(d);
    return (r - 1.0f) * 3.4f * t.s;
}

float3 aabbDistanceVec(float3 lo, float3 hi, float3 p)
{
    return { std::max({ lo.x - p.x, 0.0f, p.x - hi.x }), std::max({ lo.y - p.y, 0.0f, p.y - hi.y }), std::max({ lo.z - p.z, 0.0f, p.z - hi.z }) };
}
} // namespace

std::vector<CameraSample> buildCameraTrack(const World& world, PathReport& rep)
{
    const Layout& L = world.layout;
    const Ground ground(world);
    const PathAnchors& pa = world.anchorsForPath;
    auto city = [&](float x, float z) { return L.city.toWorld({ x, 0, z }); };
    auto lake = [&](float x, float z) { return L.lake.toWorld({ x, 0, z }); };
    auto lodge = [&](float x, float y, float z) { return L.lodge.toWorld({ x, y, z }); };
    auto camera = [&](const char* name) -> const scene::Camera& {
        for (const scene::Camera& c : world.scene.cameras)
            if (c.name == name) return c;
        fail("rppbuild: world has no camera %s", name);
    };

    std::vector<Key> keys;
    auto key = [&](float t, float3 p, float3 look, float height = 1.7f, bool groundMode = true, bool cut = false) {
        Key k{ t, p, look, groundMode, height, cut };
        keys.push_back(k);
    };
    // ---- city 0..31 s: walk north on the hot street through the crowd, turn east, drive out along the forest road ----
    key(0, city(-31.5f, 136), city(-31.5f, 40) + float3{ 0, 3, 0 });
    key(5, city(-31.5f, 129), city(-31.5f, 40) + float3{ 0, 3, 0 });
    key(10, city(-31.5f, 122.5f), city(-5, 108) + float3{ 0, 2, 0 });
    key(13, city(-28.5f, 119.5f), city(40, 119.5f) + float3{ 0, 1.5f, 0 });
    key(14, city(-26.5f, 119.5f), city(40, 119.5f) + float3{ 0, 1.5f, 0 }, 1.6f);
    {
        std::vector<float3> route{ city(-26.5f, 119.5f), pa.road[0], pa.road[1], pa.road[2], pa.road[3] };
        profileKeys(keys, route, 14, 31, 1.4f, 6.0f, 6, 3, 1.5f, true, 30.0f);
    }
    // ---- forest 31..50 s: run into the closed stand towards the combat point, watch the fight, look up; cut to the drone ----
    const float2 F = L.standCentre;
    key(35, { F.x - 96, 0, F.y }, { F.x + 20, 0, F.y + 5 });
    key(40, { F.x - 66, 0, F.y + 1 }, { F.x + 30, 0, F.y + 8 });
    key(44, { F.x - 42, 0, F.y + 1 }, { F.x + 40, 0, F.y + 10 });
    key(46, { F.x - 34, 0, F.y + 2 }, { F.x + 40, 0, F.y + 12 });
    key(49, { F.x - 25, 0, F.y + 2 }, { F.x - 17, 14, F.y + 5 });
    {
        const scene::Camera& vista = camera("forest.vista");
        key(50, vista.position, vista.position + vista.forward * 100.0f, 0, false, true);
        const float3 south = lake(0, -175);
        key(52.5f, vista.position, float3{ south.x, vista.position.y - 30, south.z }, 0, false);
        // Drone: over the canopy (25 m above the ground) to the lake's north shore, down to the boat on the water.
        const float3 mid{ 260.0f, ground.terrain.height(260, 0) + 25.0f, 0.0f };
        const float3 overLake = lake(0, -120) + float3{ 0, 15.0f, 0 };
        const float3 boat0 = lake(0, -150) + float3{ 0, 0.8f, 0 };
        std::vector<float3> route{ vista.position, mid, overLake, boat0 };
        profileKeys(keys, route, 52.5f, 63, 0.0f, 15.0f, 3, 3, 0, false, 40.0f);
    }
    // ---- waterside 63..84 s: boat across the lake past the gravity anomaly (lake-local (0, 12, 0), r 40) to the lodge shore ----
    {
        const float w = 0.8f;  // above the water (lake(x, z) is at the water level)
        const float3 anomaly = lake(0, 0) + float3{ 0, 12.0f, 0 };
        std::vector<float3> route{ lake(0, -150) + float3{ 0, w, 0 }, lake(-55, -60) + float3{ 0, w, 0 }, lake(-45, 40) + float3{ 0, w, 0 },
                                   lake(20, 100) + float3{ 0, w, 0 }, lake(86, 123) + float3{ 0, w, 0 } };
        const size_t first = keys.size();
        profileKeys(keys, route, 63, 84, 15.0f, 3.0f, 2, 3, 0, false, 30.0f);
        for (size_t k = first; k < keys.size(); ++k)
            if (keys[k].t >= 66 && keys[k].t <= 81) keys[k].look = anomaly;  // watch the anomaly
    }
    // ---- interior 84..120 s: up the shore to the lodge door, inside: overview, mirror, floor at 60 deg, window, end ----
    {
        // Ground keys: look targets carry their height above the ground at the target (inside the lodge: above the floor).
        auto rel = [](float3 p, float y) { return float3{ p.x, y, p.z }; };
        const float3 door = pa.lodgeDoorOutside, in = pa.lodgeDoorInside;
        const float s = (float)pa.lodgeDoorWall;
        key(87, (lake(86, 123) + door) * 0.5f, rel(door, 1.5f));
        key(90, door, rel(in, 1.5f), 1.65f);
        key(92, in, rel(lodge(0, 0, 0), 1.4f), 1.6f);
        key(96, lodge(-3.3f * s, 0, 2.4f), rel(lodge(1.0f, 0, -2.0f), 0.8f), 1.7f);
        key(102, lodge(0.5f, 0, 1.5f), rel(lodge(0.0f, 0, -3.0f), 1.4f), 1.6f);
        key(108, lodge(0.0f, 0, 2.3f), rel(lodge(0.0f, 0, -0.3f), 0.1f), 1.6f);
        key(114, lodge(1.2f, 0, -0.2f), rel(lodge(0.0f, 0, 3.0f), 1.6f), 1.6f);
        key(120, lodge(2.0f, 0, -0.8f), rel(lodge(-1.0f, 0, -3.0f), 1.4f), 1.6f);
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
    // Ground keys store ground-relative positions: resolve y now (smoothed ground) for positions and look targets.
    for (Key& k : keys)
    {
        if (k.ground) k.p.y = ground.smoothed(k.p.x, k.p.z, 3.0f) + k.height;
        if (k.look.y == 0 && k.ground) k.look.y = k.p.y;
        else if (k.ground && k.look.y < 50) k.look.y += ground.at(k.look.x, k.look.z);
    }

    // ---- sample every tick: Hermite through the keys of the same shot ----
    std::vector<CameraSample> out(kPathTicks + 1);
    std::vector<uint8_t> groundLevel(out.size());
    size_t seg = 0;
    for (uint32_t tick = 0; tick <= kPathTicks; ++tick)
    {
        const float t = tick * kDt;
        while (seg + 2 < keys.size() && keys[seg + 1].t <= t) ++seg;
        const Key& a = keys[seg];
        const Key& b = keys[std::min(seg + 1, keys.size() - 1)];
        CameraSample& c = out[tick];
        const bool sameShot = !b.cut;
        const float span = std::max(1e-4f, b.t - a.t), u = std::clamp((t - a.t) / span, 0.0f, 1.0f);
        auto tangent = [&](size_t i, bool look) {
            const size_t lo = (i > 0 && !keys[i].cut) ? i - 1 : i, hi = (i + 1 < keys.size() && !keys[i + 1].cut) ? i + 1 : i;
            if (lo == hi) return float3{};
            const float3 p0 = look ? keys[lo].look : keys[lo].p, p1 = look ? keys[hi].look : keys[hi].p;
            return (p1 - p0) / std::max(1e-4f, keys[hi].t - keys[lo].t);
        };
        if (sameShot && t <= b.t)
        {
            c.position = hermite(a.p, tangent(seg, false), b.p, tangent(seg + 1, false), u, span);
            const float3 look = hermite(a.look, tangent(seg, true), b.look, tangent(seg + 1, true), u, span);
            c.forward = normalize(look - c.position);
        }
        else
        {
            const Key& k = t >= b.t ? b : a;
            c.position = k.p;
            c.forward = normalize(k.look - k.p);
        }
        c.cut = tick > 0 && a.cut && std::fabs(t - a.t) < 0.5f * kDt;
        groundLevel[tick] = a.ground && b.ground ? 1 : 0;
        c.ev100 = ev100At(t);
    }

    // ---- trees near the path, trunk avoidance for ground-level samples ----
    TreeGrid trees;
    for (const scene::Instance& in : world.scene.instances)
    {
        const std::string& n = world.scene.meshes[in.mesh].name;
        if (n.rfind("tree_", 0) != 0) continue;
        const float s = std::sqrt(in.transform.m[0][0] * in.transform.m[0][0] + in.transform.m[1][0] * in.transform.m[1][0] + in.transform.m[2][0] * in.transform.m[2][0]);
        trees.add({ in.transform.m[0][3], in.transform.m[1][3], in.transform.m[2][3], s });
    }
    auto segmentOf = [&](uint32_t tick) {  // shots are separated by cuts; smoothing never crosses them
        return tick >= 3000 ? 1 : 0;
    };
    for (int iter = 0; iter < 12; ++iter)
    {
        for (uint32_t tick = 0; tick <= kPathTicks; ++tick)
        {
            if (!groundLevel[tick]) continue;
            CameraSample& c = out[tick];
            trees.near(c.position.x, c.position.z, 3.0f, [&](const TreeRef& t) {
                const float want = 0.35f * t.s + 0.9f;
                if (c.position.y < t.y - 0.3f || c.position.y > t.y + 3.5f * t.s) return;
                float dx = c.position.x - t.x, dz = c.position.z - t.z;
                const float d = std::sqrt(dx * dx + dz * dz);
                if (d >= want) return;
                if (d < 1e-3f) { dx = 0; dz = 1; }
                else { dx /= d; dz /= d; }
                c.position.x = t.x + dx * want;
                c.position.z = t.z + dz * want;
            });
        }
        // Smooth x, z over +-12 ticks (0.2 s) within the shot, ground-level samples only.
        std::vector<float3> copy(out.size());
        for (size_t i = 0; i < out.size(); ++i) copy[i] = out[i].position;
        for (uint32_t tick = 0; tick <= kPathTicks; ++tick)
        {
            if (!groundLevel[tick]) continue;
            float3 sum{};
            int n = 0;
            for (int d = -12; d <= 12; ++d)
            {
                const int64_t j = (int64_t)tick + d;
                if (j < 0 || j > (int64_t)kPathTicks || !groundLevel[j] || segmentOf((uint32_t)j) != segmentOf(tick)) continue;
                sum = sum + copy[j];
                ++n;
            }
            out[tick].position.x = sum.x / n;
            out[tick].position.z = sum.z / n;
        }
    }
    // Ground-level samples follow the smoothed ground after the lateral moves.
    for (uint32_t tick = 0; tick <= kPathTicks; ++tick)
        if (groundLevel[tick])
        {
            const float t = tick * kDt;
            size_t k = 0;
            while (k + 1 < keys.size() && keys[k + 1].t <= t) ++k;
            out[tick].position.y = ground.smoothed(out[tick].position.x, out[tick].position.z, 3.0f) + keys[k].height;
        }

    // ---- verification: trunks, crowns, static geometry (AABBs), speed ----
    struct Box { float3 lo, hi; std::string name; };
    std::vector<Box> boxes;
    for (const scene::Instance& in : world.scene.instances)
    {
        if (in.flags & scene::InstanceDynamic) continue;
        const scene::Mesh& m = world.scene.meshes[in.mesh];
        const std::string& n = m.name;
        if (n.rfind("tree_", 0) == 0 || n.rfind("grass_", 0) == 0 || n == "reeds" || n == "rpp1_terrain" || n == "streets" || n.rfind("water_", 0) == 0) continue;
        float3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
        for (const float3& p : m.positions)
        {
            const float3 q = in.transform.transformPoint(p);
            lo = { std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z) };
            hi = { std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z) };
        }
        boxes.push_back({ lo, hi, n });
    }
    for (uint32_t tick = 0; tick <= kPathTicks; ++tick)
    {
        const float3 p = out[tick].position;
        trees.near(p.x, p.z, 8.0f, [&](const TreeRef& t) {
            const float a = trunkClearance(t, p), b = crownClearance(t, p);
            if (a < rep.minTrunkClearance) { rep.minTrunkClearance = a; rep.worstTrunkTick = tick; }
            if (p.y > t.y + 2.5f * t.s && b < rep.minCrownClearance) { rep.minCrownClearance = b; rep.worstCrownTick = tick; }
        });
        for (const Box& b : boxes)
        {
            const float d = length(aabbDistanceVec(b.lo, b.hi, p));
            if (d < rep.minStaticClearance) { rep.minStaticClearance = d; rep.worstStaticTick = tick; rep.worstStatic = b.name; }
        }
        if (tick > 0 && !out[tick].cut)
        {
            const float v = length(out[tick].position - out[tick - 1].position) * kTickHz;
            if (v > rep.maxSpeed) { rep.maxSpeed = v; rep.maxSpeedTick = tick; }
        }
    }
    logf("path: trunk clearance %.2f m (tick %u), crown %.2f m (tick %u), static %.2f m (tick %u, %s), max speed %.1f m/s (tick %u)\n", rep.minTrunkClearance,
         rep.worstTrunkTick, rep.minCrownClearance, rep.worstCrownTick, rep.minStaticClearance, rep.worstStaticTick, rep.worstStatic.c_str(), rep.maxSpeed, rep.maxSpeedTick);
    return out;
}

std::string pathJson(const World& world, const std::vector<CameraSample>& cam, const PathReport& rep)
{
    const PathAnchors& pa = world.anchorsForPath;
    std::ostringstream o;
    auto v3 = [](float3 v) { return format("[%.6f, %.6f, %.6f]", v.x, v.y, v.z); };
    o << "{\n  \"format\": 1,\n  \"profile\": \"RPP-1 1.0-draft.1\",\n  \"scene\": \"" << world.scene.name << "\",\n  \"tickHz\": 60,\n  \"ticks\": " << kPathTicks + 1 << ",\n";
    o << format("  \"check\": { \"minTrunkClearance\": %.3f, \"minCrownClearance\": %.3f, \"minStaticClearance\": %.3f, \"worstStatic\": \"%s\", \"maxSpeed\": %.2f },\n",
                rep.minTrunkClearance, rep.minCrownClearance, rep.minStaticClearance, rep.worstStatic.c_str(), rep.maxSpeed);
    // Sections: active set per tick range; the bodies file of each; the world instances of each set (hidden when inactive).
    o << "  \"sections\": [\n";
    const uint32_t starts[SectionCount + 1] = { 0, kSwapTicks[0], kSwapTicks[1], kSwapTicks[2], kPathTicks + 1 };
    for (uint32_t s = 0; s < SectionCount; ++s)
    {
        const auto& c = world.sections[s].content;
        o << "    { \"id\": \"" << sectionId((Section)s) << "\", \"activeTicks\": [" << starts[s] << ", " << starts[s + 1] << "], \"bodiesFile\": \"rpp1_" << sectionId((Section)s)
          << "_bodies.json\", \"bodyInstances\": [" << c.bodies.front().instance << ", " << c.bodies.back().instance + 1 << "] }" << (s + 1 < SectionCount ? ",\n" : "\n");
    }
    o << "  ],\n";
    // Events (manifest "events"): swaps, combat, gravity, VFX peak; light changes are listed per tick below.
    o << "  \"events\": [\n";
    o << "    { \"tick\": 0, \"type\": \"spawn\", \"section\": \"city\" },\n";
    for (uint32_t k = 0; k < 3; ++k)
        o << "    { \"tick\": " << kSwapTicks[k] << ", \"type\": \"swap\", \"from\": \"" << sectionId((Section)k) << "\", \"to\": \"" << sectionId((Section)(k + 1)) << "\" },\n";
    o << "    { \"tick\": 2520, \"endTick\": 3000, \"type\": \"combat\" },\n";
    o << "    { \"tick\": 2700, \"type\": \"explosion\", \"position\": " << v3({ world.layout.standCentre.x + 25, 0, world.layout.standCentre.y + 18 }) << " },\n";
    o << "    { \"tick\": 2880, \"type\": \"explosion\", \"position\": " << v3({ world.layout.standCentre.x + 40, 0, world.layout.standCentre.y - 12 }) << " },\n";
    o << "    { \"tick\": 4200, \"endTick\": 4800, \"type\": \"gravity\", \"centre\": " << v3(world.layout.lake.toWorld({ 0, 12, 0 }))
      << ", \"radius\": 40, \"acceleration\": 9.81, \"field\": \"attractor replaces gravity inside the sphere\" },\n";
    o << "    { \"tick\": 4320, \"endTick\": 4680, \"type\": \"vfxPeak\", \"effects\": 128, \"particles\": 524288, \"ribbons\": 256, \"localVolumes\": 16 },\n";
    o << "    { \"tick\": 3000, \"type\": \"cameraCut\", \"note\": \"forest ground -> vista drone (no crown-free column to rise through the closed canopy)\" }\n";
    o << "  ],\n";
    // Light changes: muzzle flashes (combat window, 3-tick flashes) and neon flicker (city window), deterministic.
    o << "  \"lightChanges\": [";
    {
        Rng r(world.scene.seed, "lights");
        bool first = true;
        auto change = [&](uint32_t tick, uint32_t light, float intensity) {
            o << (first ? "\n    " : ",\n    ") << format("[%u, %u, %.1f]", tick, light, intensity);
            first = false;
        };
        for (uint32_t k = 0; k < pa.neonCount; ++k)
        {
            const float base = world.scene.lights[pa.neonFirst + k].intensity;
            for (uint32_t tick = 0; tick < 1800;)
            {
                tick += 30 + r.below(240);
                if (tick >= 1800) break;
                const uint32_t off = 2 + r.below(5);
                change(tick, pa.neonFirst + k, 0.0f);
                change(tick + off, pa.neonFirst + k, base);
                tick += off;
            }
        }
        for (uint32_t k = 0; k < pa.muzzleCount; ++k)
            for (uint32_t tick = 2520 + r.below(20); tick < 3000;)
            {
                change(tick, pa.muzzleFirst + k, pa.muzzleIntensity);
                change(tick + 3, pa.muzzleFirst + k, 0.0f);
                tick += 6 + r.below(40);
            }
    }
    o << "\n  ],\n";
    // Per tick: camera, sun, medium, wind.
    o << "  \"columns\": [\"tick\", \"cut\", \"position\", \"forward\", \"up\", \"fovDeg\", \"ev100\", \"sunDirection\", \"mieScattering\", \"mieAbsorption\", \"mieScaleHeight\", "
         "\"mieG\", \"groundAlbedo\", \"windDirection\", \"windSpeed\", \"weather\"],\n  \"samples\": [\n";
    for (uint32_t tick = 0; tick <= kPathTicks; ++tick)
    {
        const float t = tick * kDt;
        const CameraSample& c = cam[tick];
        const char* label = "";
        const scene::Atmosphere a = atmosphereAt(t, &label);
        const WindState w = windAt(t);
        o << format("    [%u, %d, ", tick, c.cut ? 1 : 0) << v3(c.position) << ", " << v3(c.forward) << ", " << v3(c.up) << format(", %.2f, %.4f, ", c.fovDeg, c.ev100)
          << v3(sunAt(t).direction) << ", " << format("[%.6e, %.6e, %.6e], [%.6e, %.6e, %.6e], %.3f, %.5f, ", a.mieScattering.x, a.mieScattering.y, a.mieScattering.z, a.mieAbsorption.x,
                                                       a.mieAbsorption.y, a.mieAbsorption.z, a.mieScaleHeight, a.mieG)
          << v3(a.groundAlbedo) << ", " << v3(w.direction) << format(", %.4f, \"%s\"]", w.speed, label) << (tick < kPathTicks ? ",\n" : "\n");
    }
    o << "  ]\n}\n";
    return o.str();
}
} // namespace unx::rpp
