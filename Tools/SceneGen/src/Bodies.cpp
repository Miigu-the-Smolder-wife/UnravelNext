// RPP-1 dynamic content of the gate scenes (2026-09-26, coordination decision): 1,024 rigid bodies per section scene at
// tick t0, with the mix of the section (piles, scattered debris, falling objects, moving vehicles / drifting objects),
// and 256 character slots (feet position and yaw; the 8 nearest the gate camera carry hair). The bodies are added to the
// scene as InstanceDynamic instances and exported with their kinematic motion to Cache/Scenes/<scene>_bodies.json
// (format 1, agreed with I: the host dynamic gate replays the motion). Characters are not in the scene (no assets yet).
// Deterministic from the request seed. Placements are the canonical gate content (not a physics snapshot).
#include "Common.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <sstream>

namespace unx::scenegen
{
namespace
{
using namespace detail;

struct Kind
{
    const char* name;
    const char* shape;  // box or cylinder (axis local +Y)
    float3 half;        // at scale 1: box half extents; cylinder (radius, half height, radius)
    int material;       // 0 wood, 1 metal, 2 concrete
};
// Base meshes, uniformly scaled per body.
constexpr Kind kKinds[] = {
    { "crate", "box", { 0.4f, 0.4f, 0.4f }, 0 },          { "debris", "box", { 0.15f, 0.08f, 0.12f }, 2 },
    { "chunk", "box", { 0.35f, 0.2f, 0.3f }, 2 },         { "car", "box", { 0.9f, 0.75f, 2.2f }, 1 },
    { "log", "cylinder", { 0.2f, 2.0f, 0.2f }, 0 },       { "branch", "cylinder", { 0.05f, 1.0f, 0.05f }, 0 },
    { "barrel", "cylinder", { 0.3f, 0.45f, 0.3f }, 1 },   { "rock", "box", { 0.3f, 0.22f, 0.26f }, 2 },
    { "smallbox", "box", { 0.08f, 0.06f, 0.1f }, 0 },
};
enum KindId { Crate, Debris, Chunk, Car, Log, Branch, Barrel, Rock, SmallBox, KindCount };

float4 quatAxisAngle(float3 axis, float angle)
{
    const float3 a = normalize(axis);
    const float s = std::sin(0.5f * angle);
    return { a.x * s, a.y * s, a.z * s, std::cos(0.5f * angle) };
}
float4 quatMul(float4 a, float4 b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
void quatToRows(float4 q, float r[3][3])
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    r[0][0] = 1 - 2 * (y * y + z * z);
    r[0][1] = 2 * (x * y - z * w);
    r[0][2] = 2 * (x * z + y * w);
    r[1][0] = 2 * (x * y + z * w);
    r[1][1] = 1 - 2 * (x * x + z * z);
    r[1][2] = 2 * (y * z - x * w);
    r[2][0] = 2 * (x * z - y * w);
    r[2][1] = 2 * (y * z + x * w);
    r[2][2] = 1 - 2 * (x * x + y * y);
}
// Vertical half extent of a rotated body (distance from its centre to its lowest point).
float halfHeight(const Kind& k, float scale, float4 q)
{
    float r[3][3];
    quatToRows(q, r);
    const float3 h = k.half * scale;
    if (k.shape[0] == 'b') return std::fabs(r[1][0]) * h.x + std::fabs(r[1][1]) * h.y + std::fabs(r[1][2]) * h.z;
    const float ay = std::fabs(r[1][1]);  // world-Y component of the cylinder axis
    return ay * h.y + h.x * std::sqrt(std::max(0.0f, 1 - ay * ay));
}

struct Builder
{
    std::vector<DynamicBody> out;
    Rng rng;
    Builder(uint64_t seed, uint64_t stream) : rng(seed, stream) {}
    DynamicBody& add(KindId kind, float scale, float3 pos, float4 q, const char* motion)
    {
        const Kind& k = kKinds[kind];
        DynamicBody b;
        b.kind = k.name;
        b.shape = k.shape;
        b.scale = scale;
        b.halfExtents = k.half * scale;
        b.position = pos;
        b.rotation = q;
        b.motion = motion;
        out.push_back(b);
        return out.back();
    }
    // Resting on ground height g (random yaw, tilt up to maxTilt).
    DynamicBody& rest(KindId kind, float scale, float x, float g, float z, float maxTilt)
    {
        const float4 q = quatMul(quatAxisAngle({ 0, 1, 0 }, rng.range(0, 2 * kPi)), quatAxisAngle({ 1, 0, 0 }, rng.range(-maxTilt, maxTilt)));
        return add(kind, scale, { x, g + halfHeight(kKinds[kind], scale, q), z }, q, "resting");
    }
    // Falling from height y above ground g; stops when its centre reaches the rest height.
    DynamicBody& fall(KindId kind, float scale, float x, float y, float z, float g, float period)
    {
        const float4 q = quatMul(quatAxisAngle({ 0, 1, 0 }, rng.range(0, 2 * kPi)), quatAxisAngle({ rng.range(-1, 1), 0, 1 }, rng.range(0, kPi)));
        DynamicBody& b = add(kind, scale, { x, y, z }, q, "falling");
        b.velocity = { rng.range(-1.0f, 1.0f), rng.range(-3.0f, 0.0f), rng.range(-1.0f, 1.0f) };
        b.angularVelocity = { rng.range(-2.0f, 2.0f), rng.range(-2.0f, 2.0f), rng.range(-2.0f, 2.0f) };
        b.restHeight = g + halfHeight(kKinds[kind], scale, q);
        b.period = period;
        return b;
    }
    // Stacked pile: layers of nx x nz boxes on ground g, centred at (x, z), rotated by yaw.
    void pile(KindId kind, float scale, float x, float g, float z, float yaw, const std::vector<std::pair<int, int>>& layers)
    {
        const Kind& k = kKinds[kind];
        const float3 h = k.half * scale;
        const float c = std::cos(yaw), s = std::sin(yaw);
        float y = g;
        for (const auto& L : layers)
        {
            for (int j = 0; j < L.second; ++j)
                for (int i = 0; i < L.first; ++i)
                {
                    const float lx = (i - 0.5f * (L.first - 1)) * 2.05f * h.x, lz = (j - 0.5f * (L.second - 1)) * 2.05f * h.z;
                    const float4 q = quatAxisAngle({ 0, 1, 0 }, yaw + rng.range(-0.06f, 0.06f));
                    add(kind, scale, { x + c * lx + s * lz, y + h.y, z - s * lx + c * lz }, q, "resting");
                }
            y += 2 * h.y;
        }
    }
    // Triangular log stack (rows of n, n - 1, ..., 2), logs horizontal along the stack's axis at yaw.
    void logStack(float scale, float x, float g, float z, float yaw, int bottom)
    {
        const float R = kKinds[Log].half.x * scale;
        const float4 q = quatMul(quatAxisAngle({ 0, 1, 0 }, yaw), quatAxisAngle({ 1, 0, 0 }, 0.5f * kPi));  // axis Y -> Z, then yaw
        const float c = std::cos(yaw), s = std::sin(yaw);
        for (int row = 0; row < bottom - 1; ++row)  // rows of bottom .. 2 logs
        {
            const int n = bottom - row;
            for (int i = 0; i < n; ++i)
            {
                const float lx = (i - 0.5f * (n - 1)) * 2 * R;
                add(Log, scale, { x + c * lx, g + R + row * std::sqrt(3.0f) * R, z - s * lx }, q, "resting");
            }
        }
    }
};

// Character slots.
void characters(std::vector<CharacterSlot>& out, Rng& r, int n, float3 camera, const std::function<bool(float&, float&, float&)>& place)
{
    for (int i = 0; i < n; ++i)
    {
        CharacterSlot c;
        float x, y, z;
        for (int t = 0; t < 1000 && !place(x, y, z); ++t) {}
        c.position = { x, y, z };
        c.yaw = r.range(0, 2 * kPi);
        out.push_back(c);
    }
    // The 8 nearest the gate camera carry hair.
    std::vector<size_t> idx(out.size());
    for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return length(out[a].position - camera) < length(out[b].position - camera); });
    for (size_t i = 0; i < std::min<size_t>(8, idx.size()); ++i) out[idx[i]].hero = true;
}

// City (city_block, city_night): streets every 76 m, 10 m roads (y 0), 3 m sidewalks (y 0.15). The street camera stands at
// z = 120 on the third street (x = -half + 2 * 76) looking towards -Z; the heavy content sits 10-100 m in front of it.
void cityBodies(Builder& b, float scale, std::vector<CharacterSlot>& chars)
{
    const int blocks = std::max(1, (int)std::lround(5.0f * std::sqrt(std::max(scale, 0.04f))));
    const float pitch = 76.0f, half = blocks * pitch * 0.5f, hot = -half + 2 * pitch;
    auto street = [&](int k) { return -half + k * pitch; };
    auto groundAt = [&](float dx) { return std::fabs(dx) > 5.0f ? 0.15f : 0.0f; };
    Rng& r = b.rng;
    // 24 vehicles: 12 moving (10 m/s along Z, period 6 s), 12 parked; at least 8 on the hot street.
    for (int i = 0; i < 24; ++i)
    {
        const int k = i < 8 ? 2 : (int)r.below((uint32_t)blocks + 1);
        const bool moving = (i & 1) == 0;
        const float lane = moving ? (r.below(2) ? 2.5f : -2.5f) : (r.below(2) ? 4.2f : -4.2f);
        float z;
        do z = i < 8 ? r.range(-60.0f, 110.0f) : r.range(-half + 8, half - 8);
        while ([&] { for (int j = 0; j <= blocks; ++j) if (std::fabs(z - street(j)) < 9.0f) return true; return false; }());
        const float dir = lane > 0 ? 1.0f : -1.0f;
        const float4 q = quatAxisAngle({ 0, 1, 0 }, dir > 0 ? 0.0f : kPi);
        DynamicBody& c = b.add(Car, r.range(0.9f, 1.1f), { street(k) + lane, kKinds[Car].half.y * 1.0f, z }, q, moving ? "rolling" : "resting");
        c.position.y = c.halfExtents.y;
        if (moving)
        {
            c.velocity = { 0, 0, 10.0f * dir };
            c.period = 6.0f;
        }
    }
    // 6 piles of 50 crates on the hot street's sidewalks.
    const std::vector<std::pair<int, int>> layers = { { 4, 4 }, { 4, 3 }, { 3, 3 }, { 3, 2 }, { 2, 2 }, { 3, 1 } };
    for (int p = 0; p < 6; ++p)
    {
        const float side = (p & 1) ? 6.5f : -6.5f, z = 30.0f + 14.0f * p;
        b.pile(Crate, r.range(0.85f, 1.15f), hot + side, 0.15f, z, r.range(-0.3f, 0.3f), layers);
    }
    // 600 debris: 400 in a collapse zone on the hot street, 200 along all streets.
    for (int i = 0; i < 600; ++i)
    {
        const KindId k = r.below(3) == 0 ? Chunk : Debris;
        float x, z;
        if (i < 400)
        {
            x = hot + r.range(-8.0f, 8.0f);
            z = r.range(20.0f, 110.0f);
        }
        else
        {
            const int s = (int)r.below((uint32_t)blocks + 1);
            if (r.below(2))
            {
                x = street(s) + r.range(-8.0f, 8.0f);
                z = r.range(-half, half);
            }
            else
            {
                z = street(s) + r.range(-8.0f, 8.0f);
                x = r.range(-half, half);
            }
        }
        const float dx = std::min(std::fabs(x - hot), 100.0f);
        b.rest(k, r.range(0.6f, 1.6f), x, groundAt(i < 400 ? dx : 0.0f), z, 0.4f);
    }
    // 100 falling over the hot street.
    for (int i = 0; i < 100; ++i)
    {
        const float x = hot + r.range(-7.0f, 7.0f), z = r.range(40.0f, 110.0f);
        b.fall(r.below(2) ? Chunk : Crate, r.range(0.6f, 1.2f), x, r.range(4.0f, 25.0f), z, groundAt(x - hot), 4.0f);
    }
    Rng rc(r.next(), 5);
    characters(chars, rc, 256, { hot + 1.5f, 1.7f, 120.0f }, [&](float& x, float& y, float& z) {
        const bool onHot = rc.below(4) != 0;
        const int s = (int)rc.below((uint32_t)blocks + 1);
        x = (onHot ? hot : street(s)) + rc.range(-7.5f, 7.5f);
        z = rc.range(-half, onHot ? 110.0f : half);
        y = groundAt(x - (onHot ? hot : street(s)));
        return true;
    });
}

// Forest combat: the combat point (300, -200) of forestCombat, eye camera looking towards +X (+Z 20 %).
void forestBodies(Builder& b, std::vector<CharacterSlot>& chars)
{
    const float cx = 300.0f, cz = -200.0f;
    Rng& r = b.rng;
    auto ground = [](float x, float z) { return rollingTerrain(x, z); };
    auto inView = [&](float dMin, float dMax, float spread, float& x, float& z) {
        const float a = 0.1974f + r.range(-spread, spread), d = r.range(dMin, dMax);  // 0.1974 = atan(20 / 100)
        x = cx + d * std::cos(a);
        z = cz + d * std::sin(a);
    };
    // 6 log stacks of 20 (rows 6..2) 12-40 m ahead.
    for (int p = 0; p < 6; ++p)
    {
        float x, z;
        inView(12.0f, 40.0f, 0.6f, x, z);
        b.logStack(r.range(0.85f, 1.15f), x, ground(x, z), z, r.range(0, kPi), 6);
    }
    // 650 scattered: 400 rocks, 250 branches; 70 % in the view cone within 60 m, the rest all around within 40 m.
    for (int i = 0; i < 650; ++i)
    {
        float x, z;
        if (r.below(10) < 7) inView(2.5f, 60.0f, 0.9f, x, z);
        else
        {
            const float a = r.range(0, 2 * kPi), d = r.range(2.5f, 40.0f);
            x = cx + d * std::cos(a);
            z = cz + d * std::sin(a);
        }
        const KindId k = i < 400 ? Rock : Branch;
        b.rest(k, r.range(0.6f, 1.6f), x, ground(x, z), z, k == Branch ? 1.5f : 0.4f);
    }
    // Supply cache: 2 crate piles of 32 and 40 standing barrels, 8-25 m ahead.
    const std::vector<std::pair<int, int>> layers = { { 4, 4 }, { 3, 3 }, { 2, 2 }, { 2, 1 }, { 1, 1 } };
    for (int p = 0; p < 2; ++p)
    {
        float x, z;
        inView(8.0f, 20.0f, 0.4f, x, z);
        b.pile(Crate, r.range(0.9f, 1.1f), x, ground(x, z), z, r.range(0, kPi), layers);
    }
    for (int i = 0; i < 40; ++i)
    {
        float x, z;
        inView(8.0f, 25.0f, 0.5f, x, z);
        const float s = r.range(0.9f, 1.1f);
        const float4 q = quatAxisAngle({ 0, 1, 0 }, r.range(0, 2 * kPi));
        b.add(Barrel, s, { x, ground(x, z) + kKinds[Barrel].half.y * s, z }, q, "resting");
    }
    // 150 falling branches and crates, 3-15 m up, 5-40 m ahead.
    for (int i = 0; i < 150; ++i)
    {
        float x, z;
        inView(5.0f, 40.0f, 0.7f, x, z);
        const float g = ground(x, z);
        b.fall(r.below(3) ? Branch : Crate, r.range(0.7f, 1.3f), x, g + r.range(3.0f, 15.0f), z, g, 3.0f);
    }
    Rng rc(r.next(), 6);
    characters(chars, rc, 256, { cx, ground(cx, cz) + 1.7f, cz }, [&](float& x, float& y, float& z) {
        const float a = 0.1974f + rc.range(-1.2f, 1.2f), d = rc.range(4.0f, 90.0f);
        x = cx + d * std::cos(a);
        z = cz + d * std::sin(a);
        y = ground(x, z);
        return true;
    });
}

// Waterside: calm lake (y 0) around the origin, shore beyond r ~ 150 m; the lake camera stands at x = -175 looking +X.
void watersideBodies(Builder& b, std::vector<CharacterSlot>& chars)
{
    Rng& r = b.rng;
    auto water = [](float x, float z) { return lakeFloor(x, z) < -0.5f && x > -170.0f && x < 130.0f && z > -180.0f && z < 180.0f; };
    // 350 floating (drifting), partly submerged.
    for (int i = 0; i < 350; ++i)
    {
        float x, z;
        do
        {
            x = r.range(-150.0f, 110.0f);
            z = r.range(-120.0f, 120.0f);
        } while (!water(x, z));
        const KindId k = (KindId)std::array<KindId, 4>{ Crate, Log, Barrel, Chunk }[r.below(4)];
        const float s = r.range(0.7f, 1.3f);
        const float4 q = quatMul(quatAxisAngle({ 0, 1, 0 }, r.range(0, 2 * kPi)), quatAxisAngle({ 1, 0, 0 }, k == Log ? 0.5f * kPi : r.range(-0.3f, 0.3f)));
        DynamicBody& d = b.add(k, s, { x, 0.3f * halfHeight(kKinds[k], s, q), z }, q, "rolling");
        d.velocity = { r.range(-0.4f, 0.4f), 0, r.range(-0.4f, 0.4f) };
        d.angularVelocity = { 0, r.range(-0.2f, 0.2f), 0 };
        d.period = 20.0f;
    }
    // 300 falling over the lake and the near shore, 5-30 m up.
    for (int i = 0; i < 300; ++i)
    {
        const float x = r.range(-150.0f, 150.0f), z = r.range(-120.0f, 120.0f);
        const float g = std::max(lakeFloor(x, z), 0.0f);
        b.fall((KindId)std::array<KindId, 3>{ Crate, Chunk, Barrel }[r.below(3)], r.range(0.7f, 1.3f), x, g + r.range(5.0f, 30.0f), z, g, 4.0f);
    }
    // 374 resting on the shore (ring 150-210 m where the ground is above water).
    for (int i = 0; i < 374; ++i)
    {
        float x, z, g;
        do
        {
            const float a = r.range(0, 2 * kPi), d = r.range(150.0f, 210.0f);
            x = d * std::cos(a);
            z = d * std::sin(a);
            g = lakeFloor(x, z);
        } while (g < 0.05f);
        const KindId k = (KindId)std::array<KindId, 4>{ Rock, Log, Crate, Branch }[r.below(4)];
        b.rest(k, r.range(0.7f, 1.4f), x, g, z, k == Log || k == Branch ? 1.5f : 0.4f);
    }
    Rng rc(r.next(), 7);
    characters(chars, rc, 256, { -175.0f, lakeFloor(-175, 0) + 1.7f, 0.0f }, [&](float& x, float& y, float& z) {
        const float a = rc.range(0, 2 * kPi), d = rc.range(150.0f, 230.0f);
        x = d * std::cos(a);
        z = d * std::sin(a);
        y = lakeFloor(x, z);
        return y > 0.05f;
    });
}

// Interior: room x [-4, 4], z [-3, 3], y [0, 3.5]; mirror on the -Z wall (|x| < 1.3), window on +Z (|x| < 1.5), table
// (x [-1, 0.6], top 0.76), sofa (x < -3), plinth (x [2.1, 2.9], z [-1.9, -1.1]), shelf (x > 3.4, z [0.5, 2]). Piles and
// scatter keep to the walls so the floor strips and the mirror stay in view.
void interiorBodies(Builder& b, std::vector<CharacterSlot>& chars)
{
    Rng& r = b.rng;
    const std::vector<std::pair<int, int>> layers = { { 6, 5 }, { 6, 5 }, { 6, 5 }, { 6, 5 } };  // 120 per pile
    const float px[4] = { 2.6f, -2.35f, -2.3f, 3.5f }, pz[4] = { 2.45f, 2.45f, -2.55f, -2.5f };
    for (int p = 0; p < 4; ++p) b.pile(SmallBox, 1.0f, px[p], 0.0f, pz[p], 0.0f, layers);
    // 120 on the table top.
    for (int i = 0; i < 120; ++i) b.rest(SmallBox, r.range(0.7f, 1.3f), r.range(-0.9f, 0.5f), 0.76f, r.range(-0.4f, 0.3f), 0.0f);
    // 300 on the floor along the walls.
    for (int i = 0; i < 300; ++i)
    {
        float x, z;
        do
        {
            x = r.range(-3.9f, 3.9f);
            z = r.range(-2.9f, 2.9f);
        } while (!((std::fabs(z) > 2.0f && std::fabs(x) > 1.6f) || (x < -2.9f && z > 0.5f) || (x > 3.0f && z < 0.4f && z > -1.0f)));
        b.rest(r.below(4) ? SmallBox : Crate, r.below(4) ? r.range(0.7f, 1.4f) : r.range(0.3f, 0.45f), x, 0.0f, z, 0.3f);
    }
    // 124 falling from near the ceiling on the +X side.
    for (int i = 0; i < 124; ++i) b.fall(SmallBox, r.range(0.7f, 1.3f), r.range(1.0f, 3.0f), r.range(2.5f, 3.3f), r.range(0.0f, 2.0f), 0.0f, 1.5f);
    Rng rc(r.next(), 8);
    characters(chars, rc, 256, { 0.0f, 1.6f, 2.0f }, [&](float& x, float& y, float& z) {
        const float a = rc.range(0, 2 * kPi), d = rc.range(8.0f, 60.0f);  // around the house, outside (256 do not fit the room)
        x = d * std::cos(a);
        z = d * std::sin(a);
        y = -0.2f;
        return true;
    });
}
} // namespace

DynamicContent dynamicContent(const Request& rq)
{
    DynamicContent c;
    Builder b(rq.seed, 0xB0D1E5ull + (uint64_t)rq.id);
    switch (rq.id)
    {
    case SceneId::CityBlock:
    case SceneId::CityNight: cityBodies(b, rq.scale, c.characters); c.section = "city / crowd"; break;
    case SceneId::ForestCombat: forestBodies(b, c.characters); c.section = "forest / combat"; break;
    case SceneId::Waterside: watersideBodies(b, c.characters); c.section = "waterside / gravity, VFX"; break;
    case SceneId::Interior: interiorBodies(b, c.characters); c.section = "interior / reflection"; break;
    default: return c;
    }
    c.bodies = std::move(b.out);
    if (c.bodies.size() != 1024) fail("scenegen: %s has %zu dynamic bodies, expected 1024", sceneName(rq.id), c.bodies.size());
    return c;
}

namespace detail
{
void addDynamicBodies(Scene& s, DynamicContent& c)
{
    if (c.bodies.empty()) return;
    Material wood;
    wood.name = "body_wood";
    wood.baseColor = f3(0.36f, 0.24f, 0.13f);
    wood.roughness = 0.7f;
    Material metal;
    metal.name = "body_metal";
    metal.baseColor = f3(0.62f, 0.62f, 0.64f);
    metal.metallic = 1.0f;
    metal.roughness = 0.4f;
    Material concrete;
    concrete.name = "body_concrete";
    concrete.baseColor = f3(0.45f, 0.44f, 0.42f);
    concrete.roughness = 0.85f;
    const uint32_t mats[3] = { addMaterial(s, wood), addMaterial(s, metal), addMaterial(s, concrete) };
    uint32_t meshes[KindCount];
    for (int k = 0; k < KindCount; ++k)
    {
        const Kind& kd = kKinds[k];
        MeshBuilder mb(std::string("body_") + kd.name);
        mb.material(mats[kd.material]);
        if (kd.shape[0] == 'b') mb.box(-kd.half, kd.half, 1.0f);
        else mb.cylinder({ 0, -kd.half.y, 0 }, { 0, 2 * kd.half.y, 0 }, kd.half.x, kd.half.x, 12, 1, true, 1.0f);
        meshes[k] = addMesh(s, mb.finish(false));
    }
    for (DynamicBody& b : c.bodies)
    {
        int k = 0;
        while (std::string(kKinds[k].name) != b.kind) ++k;
        float r[3][3];
        quatToRows(b.rotation, r);
        float3x4 t;
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j) t.m[i][j] = r[i][j] * b.scale;
            t.m[i][3] = (&b.position.x)[i];
        }
        b.instance = (uint32_t)s.instances.size();
        addInstance(s, meshes[k], t, scene::InstanceCastShadow | scene::InstanceDynamic);
    }
}
} // namespace detail

std::string dynamicContentJson(const Request& rq, const DynamicContent& c)
{
    auto v3 = [](float3 v) { return format("[%.6g, %.6g, %.6g]", v.x, v.y, v.z); };
    size_t resting = 0, falling = 0, rolling = 0;
    for (const DynamicBody& b : c.bodies) (b.motion[0] == 'r' ? (b.motion[1] == 'e' ? resting : rolling) : falling)++;
    std::ostringstream o;
    o << "{\n  \"format\": 1,\n  \"scene\": \"" << sceneName(rq.id) << "\",\n  \"seed\": " << rq.seed << ",\n  \"scale\": " << format("%.6g", rq.scale)
      << ",\n  \"section\": \"" << c.section << "\",\n  \"gravity\": [0, -9.81, 0],\n  \"t0\": 0,\n"
      << format("  \"mix\": { \"resting\": %zu, \"falling\": %zu, \"rolling\": %zu },\n", resting, falling, rolling) << "  \"bodies\": [\n";
    for (size_t i = 0; i < c.bodies.size(); ++i)
    {
        const DynamicBody& b = c.bodies[i];
        o << "    { \"instance\": " << b.instance << ", \"kind\": \"" << b.kind << "\", \"shape\": \"" << b.shape << "\", \"halfExtents\": " << v3(b.halfExtents)
          << ", \"position\": " << v3(b.position) << format(", \"rotation\": [%.7g, %.7g, %.7g, %.7g]", b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w)
          << ", \"motion\": \"" << b.motion << "\", \"velocity\": " << v3(b.velocity) << ", \"angularVelocity\": " << v3(b.angularVelocity)
          << format(", \"restHeight\": %.6g, \"period\": %.6g }", b.restHeight, b.period) << (i + 1 < c.bodies.size() ? ",\n" : "\n");
    }
    o << "  ],\n  \"characters\": [\n";
    for (size_t i = 0; i < c.characters.size(); ++i)
    {
        const CharacterSlot& ch = c.characters[i];
        o << "    { \"position\": " << v3(ch.position) << format(", \"yaw\": %.6g, \"hair\": %s }", ch.yaw, ch.hero ? "true" : "false")
          << (i + 1 < c.characters.size() ? ",\n" : "\n");
    }
    o << "  ]\n}\n";
    return o.str();
}
} // namespace unx::scenegen
