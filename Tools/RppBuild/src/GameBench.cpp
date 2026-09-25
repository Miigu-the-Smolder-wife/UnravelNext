#include "GameBench.h"

#include "Meshes.h"
#include "Rng.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

namespace unx::rpp
{
namespace
{
constexpr float kPiF = 3.14159265358979323846f;
constexpr uint32_t kTickHz = 60;

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
float3x4 poseMatrix(float3 p, float4 q)
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    float3x4 m;
    m.m[0][0] = 1 - 2 * (y * y + z * z); m.m[0][1] = 2 * (x * y - z * w);     m.m[0][2] = 2 * (x * z + y * w);     m.m[0][3] = p.x;
    m.m[1][0] = 2 * (x * y + z * w);     m.m[1][1] = 1 - 2 * (x * x + z * z); m.m[1][2] = 2 * (y * z - x * w);     m.m[1][3] = p.y;
    m.m[2][0] = 2 * (x * z - y * w);     m.m[2][1] = 2 * (y * z + x * w);     m.m[2][2] = 1 - 2 * (x * x + y * y); m.m[2][3] = p.z;
    return m;
}
std::string v3(float3 v) { return format("[%.9g, %.9g, %.9g]", v.x, v.y, v.z); }
std::string q4(float4 q) { return format("[%.9g, %.9g, %.9g, %.9g]", q.x, q.y, q.z, q.w); }

// Rigid kinds (shape at scale 1; box: half extents, cylinder: radius, half height, radius; axis local +Y).
struct Kind
{
    const char* name;
    const char* shape;
    float3 half;
    int material;          // index into the physical materials below
    const char* category;  // population category
};
struct PhysMaterial
{
    const char* name;
    float3 colour;
    float roughness, metallic;
    float density, friction, restitution;  // kg/m^3, Coulomb, restitution
};
const PhysMaterial kMaterials[] = {
    { "wood", { 0.42f, 0.29f, 0.16f }, 0.70f, 0.0f, 600, 0.55f, 0.20f },
    { "steel", { 0.56f, 0.57f, 0.58f }, 0.35f, 1.0f, 7850, 0.45f, 0.15f },
    { "concrete", { 0.46f, 0.45f, 0.43f }, 0.85f, 0.0f, 2400, 0.70f, 0.05f },
    { "brick", { 0.45f, 0.20f, 0.14f }, 0.80f, 0.0f, 1900, 0.70f, 0.05f },
    { "plastic", { 0.10f, 0.35f, 0.62f }, 0.45f, 0.0f, 950, 0.40f, 0.35f },
    { "rubber", { 0.05f, 0.05f, 0.05f }, 0.90f, 0.0f, 1100, 0.90f, 0.50f },
    { "car_paint", { 0.55f, 0.08f, 0.06f }, 0.30f, 0.0f, 250, 0.50f, 0.10f },  // chassis: body shell (effective density of a 1.2 t car in its box)
};
enum KindId { CrateS, CrateM, CrateL, Barrel, Plank, Brick, SmallBox, Bucket, Bottle, Pillar, WallPanel, FracCrate, Fragment, Chassis, Wheel, GatePanel, KindCount };
const Kind kKinds[KindCount] = {
    { "crate_s", "box", { 0.25f, 0.25f, 0.25f }, 0, "rigid.box" },     { "crate_m", "box", { 0.40f, 0.40f, 0.40f }, 0, "rigid.box" },
    { "crate_l", "box", { 0.60f, 0.50f, 0.60f }, 0, "rigid.box" },     { "barrel", "cylinder", { 0.30f, 0.45f, 0.30f }, 1, "rigid.clutter" },
    { "plank", "box", { 0.10f, 0.025f, 0.90f }, 0, "rigid.clutter" },  { "brick", "box", { 0.10f, 0.05f, 0.20f }, 3, "rigid.clutter" },
    { "smallbox", "box", { 0.08f, 0.06f, 0.10f }, 4, "rigid.clutter" }, { "bucket", "cylinder", { 0.15f, 0.15f, 0.15f }, 4, "rigid.clutter" },
    { "bottle", "cylinder", { 0.04f, 0.12f, 0.04f }, 4, "rigid.clutter" }, { "pillar", "box", { 0.30f, 1.50f, 0.30f }, 2, "fracturable" },
    { "wall_panel", "box", { 1.50f, 1.25f, 0.10f }, 3, "fracturable" }, { "frac_crate", "box", { 0.40f, 0.40f, 0.40f }, 0, "fracturable" },
    { "fragment", "box", { 0.15f, 0.10f, 0.12f }, 2, "debris" },       { "chassis", "box", { 2.10f, 0.50f, 0.90f }, 6, "articulated.vehicle" },
    { "wheel", "cylinder", { 0.35f, 0.12f, 0.35f }, 5, "articulated.vehicle" }, { "gate_panel", "box", { 0.05f, 1.00f, 0.60f }, 0, "articulated.hinge" },
};
float kindVolume(const Kind& k, float s)
{
    const float3 h = k.half * s;
    return k.shape[0] == 'b' ? 8 * h.x * h.y * h.z : kPiF * h.x * h.x * 2 * h.y;
}

struct Body  // a rigid body of the render replay (also listed in the population)
{
    KindId kind;
    float scale = 1;
    float3 position;
    float4 rotation{ 0, 0, 0, 1 };
    std::string motion = "resting";
    float3 velocity{}, angularVelocity{};
    float restHeight = 0, period = 0;
    uint32_t object = 0;  // population object id
};

// Population object (physics engine input). Rigid objects reference their bodies; the others carry parameters only.
struct Object
{
    uint32_t id;
    std::string category, kind, recipe;
    float3 position;
    float yaw = 0;
    std::vector<uint32_t> bodies;  // indices into the body list (render replay), empty for non-rigid objects
    std::string params;            // extra JSON members (without braces)
};

// First-person camera keys: position (eye), yaw (deg, clockwise from north = -Z towards east = +X), pitch (deg, up +).
struct CamKey
{
    float t;
    float x, z;
    float yaw, pitch;
    float y = -1000.0f;  // absolute eye height (water case); -1000 = the default eye (1.7 m, 1.5 m in the vehicle)
};
} // namespace

GameBenchOutput buildGameBench(const GameBenchRequest& rq)
{
    if (rq.movingObjects == 0 || rq.movingObjects > 100000) fail("gamebench: movingObjects %u", rq.movingObjects);
    const GameBenchMix& mx = rq.mix;
    const float mixSum = mx.box + mx.clutter + mx.fracturable + mx.ragdoll + mx.vehicle + mx.hinge + mx.cloth + mx.rope + mx.soft;
    if (std::fabs(mixSum - 1.0f) > 1e-3f) fail("gamebench: mix shares sum to %.4f, expected 1", mixSum);
    GameBenchOutput out;
    scene::Scene& s = out.scene;
    s.name = "gamebench_" + rq.name;
    s.seed = rq.seed;
    Rng rng(rq.seed, "gamebench.place");

    // ---- materials ----
    auto material = [&](const char* name, float3 c, float rough, float metal) {
        scene::Material m;
        m.name = name;
        m.baseColor = c;
        m.roughness = rough;
        m.metallic = metal;
        s.materials.push_back(m);
        return (uint32_t)s.materials.size() - 1;
    };
    const uint32_t mGround = material("ground_dirt", { 0.26f, 0.23f, 0.18f }, 0.9f, 0);
    const uint32_t mAsphalt = material("asphalt", { 0.07f, 0.07f, 0.075f }, 0.75f, 0);
    const uint32_t mWall = material("warehouse_concrete", { 0.50f, 0.49f, 0.47f }, 0.85f, 0);
    const uint32_t mRoof = material("corrugated_steel", { 0.45f, 0.47f, 0.50f }, 0.45f, 1.0f);
    const uint32_t mSteel = material("gantry_steel", { 0.30f, 0.30f, 0.32f }, 0.40f, 1.0f);
    const uint32_t mWater = material("pond_tank", { 0.35f, 0.36f, 0.36f }, 0.80f, 0);
    uint32_t mBody[std::size(kMaterials)];
    for (size_t k = 0; k < std::size(kMaterials); ++k) mBody[k] = material((std::string("body_") + kMaterials[k].name).c_str(), kMaterials[k].colour, kMaterials[k].roughness, kMaterials[k].metallic);
    auto addStatic = [&](scene::Mesh m, float3x4 t = {}) {
        s.meshes.push_back(std::move(m));
        scene::Instance in;
        in.mesh = (uint32_t)s.meshes.size() - 1;
        in.transform = t;
        in.flags = scene::InstanceCastShadow;
        s.instances.push_back(in);
    };

    // ---- environment: 240 x 240 m ground (flat play area |x|,|z| < 60, mounds outside), asphalt cross, warehouses, gantry, pond tank ----
    {
        scene::Mesh g;
        g.name = "yard_ground";
        const int n = 320;
        const float x0 = -160, d = 320.0f / n;
        auto height = [](float x, float z) {  // flat play area and road corridor (|z| < 8, the vehicle case), mounds elsewhere
            const float r = std::max(std::fabs(x), std::fabs(z));
            const float w = std::clamp((r - 60.0f) / 40.0f, 0.0f, 1.0f) * std::clamp((std::fabs(z) - 8.0f) / 6.0f, 0.0f, 1.0f);
            const float mound = w * w * (3 - 2 * w) * (2.5f + 1.5f * std::sin(x / 17.0f) * std::cos(z / 23.0f));
            // Pond basin (the water case): x -15..15, z -70..-52, 3 m deep with a 6 m wide shelf slope.
            const float dx = std::max(0.0f, std::fabs(x) - 9.0f), dz = std::max(0.0f, std::fabs(z + 61.0f) - 3.0f);
            const float edge = std::clamp(1.0f - std::max(dx, dz) / 6.0f, 0.0f, 1.0f);
            return mound * (1.0f - edge) - 3.0f * edge * edge * (3 - 2 * edge);
        };
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
            {
                const float x = x0 + i * d, z = x0 + j * d, e = 0.25f;
                const float hx = (height(x + e, z) - height(x - e, z)) / (2 * e), hz = (height(x, z + e) - height(x, z - e)) / (2 * e);
                g.positions.push_back({ x, height(x, z), z });
                g.normals.push_back(normalize(float3{ -hx, 1, -hz }));
                g.tangents.push_back({ 1, 0, 0, 1 });
                g.uv0.push_back({ x / 4, z / 4 });
            }
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
            {
                const uint32_t a = (uint32_t)(j * (n + 1) + i), b = a + 1, c = a + n + 1, dd = c + 1;
                g.indices.insert(g.indices.end(), { a, c, dd, a, dd, b });
            }
        g.submeshes.push_back({ 0, (uint32_t)g.indices.size(), mGround });
        addStatic(std::move(g));
        addStatic(boxMesh("road_ew", { -155, 0.0f, -8 }, { 155, 0.02f, 4 }, mAsphalt));
        addStatic(boxMesh("road_ns", { -4, 0.0f, -45 }, { 4, 0.02f, 60 }, mAsphalt));
        {
            // Pond water surface: Water class (P4 model pending in the renderer; ior 1.33), 32 x 20 m plane at y 0.
            scene::Material wm;
            wm.name = "pond_water";
            wm.cls = scene::MaterialClass::Water;
            wm.baseColor = { 0.012f, 0.02f, 0.02f };
            wm.roughness = 0.02f;
            wm.specular = 0.25f;
            wm.ior = 1.33f;
            s.materials.push_back(wm);
            scene::Mesh pond = boxMesh("pond_water", { -16, -0.001f, -71 }, { 16, 0.0f, -51 }, (uint32_t)s.materials.size() - 1);
            addStatic(std::move(pond));
        }
        const float3 houses[6] = { { -80, 0, -30 }, { -80, 0, 30 }, { 80, 0, -30 }, { 80, 0, 30 }, { 0, 0, -85 }, { 0, 0, 85 } };
        for (int k = 0; k < 6; ++k)
        {
            const bool alongX = k >= 4;
            const float hx = alongX ? 18.0f : 10.0f, hz = alongX ? 10.0f : 18.0f;
            addStatic(boxMesh("warehouse_" + std::to_string(k), houses[k] + float3{ -hx, 0, -hz }, houses[k] + float3{ hx, 10, hz }, mWall));
            addStatic(boxMesh("warehouse_roof_" + std::to_string(k), houses[k] + float3{ -hx - 0.5f, 10, -hz - 0.5f }, houses[k] + float3{ hx + 0.5f, 10.4f, hz + 0.5f }, mRoof));
        }
        // Gantry over zone D (x 15..25, z -25..-15): four posts, two beams at 6 m (ropes and cloth hang from the beams).
        for (float px : { 15.0f, 25.0f })
            for (float pz : { -25.0f, -15.0f }) addStatic(boxMesh("gantry_post", { px - 0.15f, 0, pz - 0.15f }, { px + 0.15f, 6.0f, pz + 0.15f }, mSteel));
        for (float pz : { -25.0f, -15.0f }) addStatic(boxMesh("gantry_beam", { 14.85f, 6.0f, pz - 0.15f }, { 25.15f, 6.3f, pz + 0.15f }, mSteel));
        // Fluid tank (inner x 28.75..31.25, z -36.25..-33.75, 1.2 m walls of 0.15 m): physics design 9 asks for a tub-size
        // volume (a few m^3) with rigid bodies falling in. 250k particles at dx 5 cm, 8 per cell = 3.9 m^3 -> 0.62 m deep.
        addStatic(boxMesh("tank_wall", { 28.6f, 0, -36.4f }, { 31.4f, 1.2f, -36.25f }, mWater));
        addStatic(boxMesh("tank_wall", { 28.6f, 0, -33.75f }, { 31.4f, 1.2f, -33.6f }, mWater));
        addStatic(boxMesh("tank_wall", { 28.6f, 0, -36.25f }, { 28.75f, 1.2f, -33.75f }, mWater));
        addStatic(boxMesh("tank_wall", { 31.25f, 0, -36.25f }, { 31.4f, 1.2f, -33.75f }, mWater));
    }
    // ---- lights: shadowed floodlights on the warehouses facing the yard, unshadowed lamp posts along the roads ----
    {
        const float3 warm{ 1.0f, 0.86f, 0.70f };
        const float lum = 0.2126f * warm.x + 0.7152f * warm.y + 0.0722f * warm.z;
        for (uint32_t k = 0; k < rq.lights; ++k)
        {
            scene::Light l;
            l.color = warm / lum;
            if (k < rq.shadowedLights)
            {
                const float a = 2 * kPiF * (k + 0.5f) / (float)rq.shadowedLights;
                l.type = scene::LightType::Spot;
                l.position = { 62.0f * std::cos(a), 9.0f, 62.0f * std::sin(a) };
                l.forward = normalize(float3{ -std::cos(a), -0.45f, -std::sin(a) });
                l.intensity = 30000;  // cd (1 kW metal-halide floodlight)
                l.range = 70;
                l.spotInner = 0.35f;
                l.spotOuter = 0.6f;
                l.castShadow = true;
            }
            else
            {
                const uint32_t i = k - rq.shadowedLights, m = std::max(1u, rq.lights - rq.shadowedLights);
                const float u = -52.0f + 104.0f * (float)(i / 2) / (float)std::max(1u, (m + 1) / 2 - 1);
                const bool ew = (i & 1) == 0;
                l.type = scene::LightType::Sphere;
                l.position = ew ? float3{ u, 5.0f, 5.5f } : float3{ 5.5f, 5.0f, u };
                l.size = { 0.08f, 0 };
                l.intensity = 40000;  // nits
                l.range = 25;
            }
            s.lights.push_back(l);
        }
    }
    s.sun.direction = normalize(float3{ std::cos(0.698f) * std::cos(3.49f), std::sin(0.698f), std::cos(0.698f) * std::sin(3.49f) });  // 40 deg, C azimuth 200
    s.windDirection = normalize(float3{ 0.8f, 0, 0.6f });
    s.windSpeed = 2.0f;

    // ---- population counts ----
    const uint32_t debrisCap = (uint32_t)std::lround(rq.movingObjects * rq.debrisShare);
    const uint32_t spawned = rq.movingObjects - debrisCap;
    const float shares[9] = { mx.box, mx.clutter, mx.fracturable, mx.ragdoll, mx.vehicle, mx.hinge, mx.cloth, mx.rope, mx.soft };
    uint32_t count[9], used = 0;
    for (int k = 0; k < 9; ++k) { count[k] = (uint32_t)std::floor(spawned * shares[k]); used += count[k]; }
    count[0] += spawned - used;  // remainder to boxes
    const uint32_t nBox = count[0], nClutter = count[1], nFrac = count[2], nRagdoll = count[3], nVehicle = count[4], nHinge = count[5], nCloth = count[6], nRope = count[7], nSoft = count[8];

    std::vector<Body> bodies;
    std::vector<Object> objects;
    auto newObject = [&](const char* category, const char* kind, const char* recipe, float3 p, float yaw) -> Object& {
        objects.push_back({ (uint32_t)objects.size(), category, kind, recipe, p, yaw, {}, "" });
        return objects.back();
    };
    auto addBody = [&](Object& o, Body b) {
        b.object = o.id;
        o.bodies.push_back((uint32_t)bodies.size());
        bodies.push_back(b);
    };
    const float g = 9.81f;
    auto fallPeriod = [&](float h) { return std::sqrt(2 * h / g) + 3.0f + rng.range(0, 3); };

    // Zone A (x -45..-10, z -45..-10): crate pyramids (3 x 3 and 4 x 4 bases), 20 % dropped from 4..10 m.
    {
        const uint32_t dropped = nBox / 5;
        uint32_t piled = 0, pile = 0;
        while (piled < nBox - dropped)
        {
            const int gx = pile % 7, gz = pile / 7;
            const float cx = -42.0f + 5.0f * gx, cz = -42.0f + 5.0f * gz;
            if (cz > -12) fail("gamebench: zone A is full (%u crates)", piled);
            const KindId k = (pile & 1) ? CrateM : CrateS;
            const float edge = 2 * kKinds[k].half.x;
            const int base = (pile % 3 == 0) ? 4 : 3;
            // Every crate is its own object (a physics body); the pile is a placement group.
            for (int layer = 0; layer < base && piled < nBox - dropped; ++layer)
            {
                const int m = base - layer;
                for (int j = 0; j < m && piled < nBox - dropped; ++j)
                    for (int i = 0; i < m && piled < nBox - dropped; ++i)
                    {
                        Body b{ k };
                        b.position = { cx + (i - 0.5f * (m - 1)) * edge * 1.02f, edge * (layer + 0.5f), cz + (j - 0.5f * (m - 1)) * edge * 1.02f };
                        Object& o = newObject("rigid.box", kKinds[k].name, "BoxRecipe (stacked)", b.position, 0);
                        o.params = format("\"pile\": %u", pile);
                        addBody(o, b);
                        ++piled;
                    }
            }
            ++pile;
        }
        for (uint32_t i = 0; i < dropped; ++i)
        {
            const KindId k = (KindId)(CrateS + rng.below(3));
            Body b{ k };
            const float h = rng.range(4.0f, 10.0f);
            b.position = { rng.range(-44.0f, -11.0f), h, rng.range(-44.0f, -11.0f) };
            b.rotation = quatAxisAngle({ rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1) }, rng.range(0, kPiF));
            b.motion = "falling";
            b.restHeight = kKinds[k].half.y;
            b.period = fallPeriod(h);
            if (i < 4)  // into the fluid tank (inner x 28.75..31.25, z -36.25..-33.75)
            {
                b.kind = CrateS;
                b.position = { 29.4f + 1.2f * (float)(i & 1), 3.0f + (float)i, -35.6f + 1.2f * (float)(i >> 1) };
                b.restHeight = kKinds[CrateS].half.y;
                b.period = 6.0f;
            }
            Object& o = newObject("rigid.box", kKinds[b.kind].name, i < 4 ? "BoxRecipe (dropped into the fluid tank)" : "BoxRecipe (dropped)", b.position, 0);
            addBody(o, b);
        }
    }
    // Zone B (x 10..45, z 10..45): clutter on a jittered 1.1 m grid, 25 % rolling (barrels, buckets, bottles on their side).
    {
        const KindId kinds[6] = { Barrel, Plank, Brick, SmallBox, Bucket, Bottle };
        for (uint32_t i = 0; i < nClutter; ++i)
        {
            const int cells = 31;
            const int gx = (int)(i % cells), gz = (int)((i / cells) % cells);
            if (i >= (uint32_t)(cells * cells)) fail("gamebench: zone B is full");
            const KindId k = kinds[rng.below(6)];
            Body b{ k };
            const float cx = 11.0f + 1.1f * gx + rng.range(-0.2f, 0.2f), cz = 11.0f + 1.1f * gz + rng.range(-0.2f, 0.2f);
            const bool round = kKinds[k].shape[0] == 'c';
            if (round && rng.below(2) == 0)
            {
                // On its side, rolling along +-X inside the zone: axis local +Y -> world Z; v = w r.
                b.rotation = quatAxisAngle({ 1, 0, 0 }, 0.5f * kPiF);
                const float v = rng.range(0.8f, 2.5f) * (rng.below(2) ? 1.0f : -1.0f), r = kKinds[k].half.x;
                b.position = { cx, r, cz };
                b.motion = "rolling";
                b.velocity = { v, 0, 0 };
                b.angularVelocity = { 0, 0, -v / r };
                b.period = std::min(8.0f, (v > 0 ? 45.0f - cx : cx - 10.0f) / std::fabs(v));
                if (b.period < 0.5f) { b.velocity = -b.velocity; b.angularVelocity = -b.angularVelocity; b.period = 4.0f; }
            }
            else
            {
                b.rotation = quatAxisAngle({ 0, 1, 0 }, rng.range(0, 2 * kPiF));
                b.position = { cx, kKinds[k].half.y, cz };
            }
            Object& o = newObject("rigid.clutter", kKinds[k].name, "CookedBoxRecipe / BakedBoxRecipe", b.position, 0);
            addBody(o, b);
        }
    }
    // Zone C (x -45..-10, z 10..45): fracturable pillars, wall panels and crates, resting; fragments pooled for the debris cap.
    {
        const KindId kinds[3] = { Pillar, WallPanel, FracCrate };
        for (uint32_t i = 0; i + 2 < nFrac; ++i)
        {
            const int cells = 12, rows = 16;  // x -44..-12, z 11..54.5 (the flat play area reaches |z| 60)
            const int gx = (int)(i % cells), gz = (int)(i / cells);
            if (gz >= rows) fail("gamebench: zone C is full");
            const KindId k = kinds[i % 3];
            Body b{ k };
            b.rotation = quatAxisAngle({ 0, 1, 0 }, (gx + gz) % 2 ? 0.5f * kPiF : 0.0f);
            b.position = { -44.0f + 2.9f * gx, kKinds[k].half.y, 11.0f + 2.9f * gz };
            Object& o = newObject("fracturable", kKinds[k].name, "DestructionRecipe + SplitTool", b.position, 0);
            o.params = format("\"fragments\": %u, \"fractureThreshold\": \"recipe\"", rq.fragmentsPerBreak);
            addBody(o, b);
        }
        // Physics design 9: one pre-authored bond-graph wall (~200 elements) and one pillar cut at run time (CARVE, 8 planes).
        {
            Object& w = newObject("fracturable.bonded", "bonded_wall", "DestructionRecipe (bond graph, pre-authored)", { -27.0f, 0, 57.0f }, 0);
            w.params = "\"size\": [8, 3, 0.3], \"elements\": 200, \"bonds\": \"recorded by the physics engine\"";
            Body b{ WallPanel };
            b.scale = 2.4f;  // 7.2 x 6 x 0.48 m stand-in for the render replay
            b.position = { -27.0f, kKinds[WallPanel].half.y * 2.4f, 57.0f };
            addBody(w, b);
            Object& c = newObject("fracturable.carve", "carve_pillar", "SplitTool (CARVE at run time)", { -8.5f, 0, 50.0f }, 0);
            c.params = "\"planes\": 8";
            Body pb{ Pillar };
            pb.position = { -9.5f, kKinds[Pillar].half.y, 50.0f };
            addBody(c, pb);
        }
        // Debris pool: render stand-in for the live fragments (the physics engine spawns the real ones at the events). Falling
        // from 1..4 m over the zone, restarting every 4..8 s, so the live count stays at the cap.
        for (uint32_t i = 0; i < debrisCap; ++i)
        {
            Body b{ Fragment };
            b.scale = rng.range(0.7f, 1.6f);
            const float h = rng.range(1.0f, 4.0f);
            b.position = { rng.range(-44.0f, -11.0f), h, rng.range(11.0f, 44.0f) };
            b.rotation = quatAxisAngle({ rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1) }, rng.range(0, kPiF));
            b.motion = "falling";
            b.velocity = { rng.range(-3, 3), rng.range(0, 4), rng.range(-3, 3) };
            b.restHeight = kKinds[Fragment].half.y * b.scale;
            b.period = rng.range(4.0f, 8.0f);
            Object& o = newObject("debris", "fragment", "SplitTool fragment (pooled)", b.position, 0);
            addBody(o, b);
        }
    }
    // Vehicles on the east-west road: chassis + 4 wheels, alternating lanes, 6..12 m/s, restarting at the road's end.
    for (uint32_t i = 0; i < nVehicle; ++i)
    {
        const bool east = (i & 1) == 0;
        const float v = rng.range(6.0f, 12.0f) * (east ? 1.0f : -1.0f), lane = east ? -2.0f : 2.0f;
        const float x0 = (east ? -55.0f : 55.0f) + (east ? 1.0f : -1.0f) * (float)(i / 2) * 9.0f;
        const float period = (110.0f - (float)(i / 2) * 9.0f) / std::fabs(v);
        Object& o = newObject("articulated.vehicle", "car", "VehicleRecipe + WheelMesh", { x0, 0, lane }, east ? 0.0f : kPiF);
        o.params = "\"wheels\": 4, \"joints\": \"wheel hinges + suspension (VehicleRecipe)\"";
        Body c{ Chassis };
        c.position = { x0, 0.35f + 0.5f + 0.1f, lane };
        c.motion = "rolling";
        c.velocity = { v, 0, 0 };
        c.period = period;
        addBody(o, c);
        for (int w = 0; w < 4; ++w)
        {
            Body wb{ Wheel };
            wb.rotation = quatAxisAngle({ 1, 0, 0 }, 0.5f * kPiF);  // axis local +Y -> world Z
            wb.position = { x0 + (w < 2 ? 1.4f : -1.4f), 0.35f, lane + (w & 1 ? 1.0f : -1.0f) };
            wb.motion = "rolling";
            wb.velocity = { v, 0, 0 };
            wb.angularVelocity = { 0, 0, -v / 0.35f };
            wb.period = period;
            addBody(o, wb);
        }
    }
    // Hinged gates along the zone borders (swing when hit; resting in the render replay).
    for (uint32_t i = 0; i < nHinge; ++i)
    {
        const float x = -45.0f + 90.0f * (float)(i + 0.5f) / (float)std::max(1u, nHinge);
        const float z = (i & 1) ? 9.0f : -9.0f;
        Object& o = newObject("articulated.hinge", "gate", "HingeRecipe", { x, 0, z }, 0);
        o.params = "\"joint\": \"hinge, limits +-110 deg\"";
        Body b{ GatePanel };
        b.position = { x, 1.05f, z };
        addBody(o, b);
    }
    // Physics-only objects (no render representation in the renderer yet: see the gap list in the bench JSON).
    for (uint32_t i = 0; i < nRagdoll; ++i)
    {
        const float a = 2 * kPiF * (i + 0.5f) / (float)std::max(1u, nRagdoll), r = rng.range(14.0f, 30.0f);
        Object& o = newObject("articulated.ragdoll", "humanoid", "CharacterRecipeV6 + RagdollMotion (NativeAnimation DataRagdoll)", { r * std::cos(a), 0, r * std::sin(a) }, rng.range(0, 2 * kPiF));
        o.params = "\"bodies\": 11, \"joints\": 10, \"state\": \"animated until an explosion within 6 m, then ragdoll\"";
    }
    for (uint32_t i = 0; i < nCloth; ++i)
    {
        const float x = 15.5f + 9.0f * (float)(i % 8) / 8.0f, z = (i / 8) % 2 ? -15.0f : -25.0f;
        Object& o = newObject("deformable.cloth", "tarp", "ClothRecipe", { x, 6.0f, z }, 0);
        o.params = "\"size\": [1.2, 2.5], \"grid\": [17, 17], \"vertices\": 289, \"compliance\": { \"edge\": 0.001, \"shear\": 0.01, \"bend\": 0.05 }, \"iterations\": 12, \"substeps\": 2, \"pinned\": \"top edge to the gantry beam\", \"onDynamicBody\": false";
    }
    for (uint32_t i = 0; i < nRope; ++i)
    {
        const float x = 15.3f + 9.4f * (float)(i % 12) / 12.0f, z = (i / 12) % 2 ? -15.0f : -25.0f;
        Object& o = newObject("deformable.rope", "rope", "RopeRecipe", { x, 6.0f, z }, 0);
        o.params = "\"length\": 4.5, \"vertices\": 32, \"iterations\": 12, \"substeps\": 2, \"pinned\": \"top end to the gantry beam\", \"load\": \"crate_s at the free end\"";
    }
    for (uint32_t i = 0; i < nSoft; ++i)
    {
        Object& o = newObject("deformable.soft", "soft_ball", "SoftVolumeRecipe", { rng.range(12.0f, 44.0f), 0.4f, rng.range(12.0f, 44.0f) }, 0);
        o.params = "\"radius\": 0.4, \"tetGrid\": [4, 4, 4], \"iterations\": 8, \"substeps\": 2";
    }

    // ---- bodies -> scene instances (one mesh per kind, uniform scale per body) ----
    uint32_t kindMesh[KindCount];
    for (int k = 0; k < KindCount; ++k)
    {
        const Kind& kd = kKinds[k];
        const uint32_t mat = mBody[kd.material];
        s.meshes.push_back(kd.shape[0] == 'b' ? boxMesh(std::string("gb_") + kd.name, -kd.half, kd.half, mat) : cylinderMesh(std::string("gb_") + kd.name, kd.half.x, kd.half.y, 16, mat));
        kindMesh[k] = (uint32_t)s.meshes.size() - 1;
    }
    std::vector<uint32_t> bodyInstance(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        const Body& b = bodies[i];
        scene::Instance in;
        in.mesh = kindMesh[b.kind];
        in.transform = poseMatrix(b.position, b.rotation);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) in.transform.m[r][c] *= b.scale;
        in.flags = scene::InstanceCastShadow | scene::InstanceDynamic;
        bodyInstance[i] = (uint32_t)s.instances.size();
        s.instances.push_back(in);
    }
    out.spawned = spawned;
    out.debrisCap = debrisCap;
    out.rigidReplayed = (uint32_t)bodies.size();

    // ---- first-person camera track, in cases (design 14.12: each >= 300 render frames; eye 1.7 m, vehicle 1.5 m):
    //   static 0..5 s | walk 1.5 m/s 5..13 | gameplay 13..40 (flick 180 deg in 0.25 s = 720 deg/s, sprint 6 m/s across the road,
    //   look up at the gantry ropes and cloth, jog west past the traffic) | spin 360 deg/s 40..45 | aim 45..50 (focal length
    //   18 -> 50 mm, f/2, focus pull 12 -> 5 m) | cut | vehicle 60 m/s along the road 50..53.5, braking to rest by 55 | walk 55..60.
    // Physical camera: 36 x 20.25 mm sensor (16:9), shutter 1/165 s; the vertical FOV follows the focal length. Lanes stay
    // outside the object zones (|x|, |z| <= 8).
    struct Lens { float focalMm, fNumber, focusM; };  // fNumber 0 = pinhole (user 11:20: all cases except aim)
    const std::vector<CamKey> keys = {
        { 0.0f, 0, 45, 0, -5 },          { 5.0f, 0, 45, 0, -5 },           { 13.0f, 0, 33.0f, 0, -3 },       { 13.25f, 0, 32.7f, 180, -3 },
        { 15.0f, 0, 31.5f, 180, 0 },     { 15.3f, 0, 31.2f, 360, 0 },      { 21.0f, 0, -3.0f, 360, 0 },      { 21.2f, 0.6f, -4.2f, 450, 0 },
        { 26.0f, 28.0f, -5.0f, 450, 0 }, { 28.0f, 30.0f, -8.0f, 320, 10 }, { 30.5f, 30.0f, -8.0f, 320, 35 }, { 31.5f, 30.0f, -8.0f, 330, 20 },
        { 31.8f, 29.6f, -8.0f, 150, 0 }, { 32.3f, 27.0f, -8.0f, 270, 0 },  { 36.0f, 9.0f, -8.0f, 270, 0 },   { 37.5f, 2.0f, -8.0f, 210, 0 },
        { 40.0f, -15.0f, -8.0f, 270, 0 }, { 45.0f, -15.0f, -8.0f, 2070, 0 }, { 45.5f, -15.0f, -8.0f, 1996, -2 }, { 50.0f, -15.0f, -8.0f, 1996, -2 },
        { 50.0f, -110.0f, -6.0f, 90, 0 }, { 53.5f, 100.0f, -6.0f, 90, 0 },  { 55.0f, 145.0f, -6.0f, 90, 0 },  { 60.0f, 138.0f, -6.0f, 60, -3 },
        // water: cut to the pond's north shore, wade in, submerge (eye -1.2 m), look up at the Snell window, surface
        { 60.0f, 0.0f, -46.0f, 180, -10, 1.7f }, { 62.0f, 0.0f, -53.0f, 180, -15, 0.9f }, { 63.0f, 0.0f, -56.0f, 180, 0, -1.2f },
        { 64.0f, 0.0f, -57.0f, 180, 70, -1.2f }, { 66.5f, 0.0f, -57.5f, 200, 75, -1.2f }, { 68.0f, 0.0f, -58.0f, 180, 5, 0.3f },
    };
    const float aimFocus = length(float3{ -21.0f - -15.0f, 1.0f - 1.7f, 13.0f - -8.0f });  // aim target: first fracturable row
    auto lensAt = [&](float time) -> Lens {
        if (time < 45.0f || time >= 50.0f) return { 18.0f, 0.0f, 0.0f };
        const float u = std::clamp((time - 45.5f) / 1.0f, 0.0f, 1.0f), su = u * u * (3 - 2 * u);
        return { 18.0f + 6.0f * su, 2.8f, aimFocus };
    };
    struct CaseRange { const char* name; float t0, t1; };
    const CaseRange cases[] = { { "static", 0, 5 }, { "walk", 5, 13 }, { "gameplay", 13, 40 }, { "spin360", 40, 45 }, { "aim", 45, 50 }, { "vehicle60", 50, 53.5f }, { "brake", 53.5f, 60 }, { "water", 60, 68 } };
    const uint32_t ticks = (uint32_t)std::lround(rq.durationSeconds * kTickHz);
    struct Cam { float3 p, fwd; float yaw, pitch; Lens lens; bool cut; };
    std::vector<Cam> cams(ticks + 1);
    float maxYawRate = 0, maxSpeed = 0;
    for (uint32_t t = 0; t <= ticks; ++t)
    {
        const float time = std::min((float)t / kTickHz, keys.back().t);
        size_t k = 0;
        while (k + 2 < keys.size() && keys[k + 1].t <= time) ++k;
        const CamKey &a = keys[k], &b = keys[k + 1];
        const float u = b.t > a.t ? std::clamp((time - a.t) / (b.t - a.t), 0.0f, 1.0f) : 1.0f;
        // Turns ease in and out (flicks) except the constant-rate spin; positions move linearly (constant speed per leg) except
        // the braking leg (constant deceleration).
        const bool spin = a.t == 40.0f, brake = a.t == 53.5f;
        const float su = spin ? u : u * u * (3 - 2 * u);
        const float pu = brake ? u * (2 - u) : u;
        Cam c;
        const float eye = (time >= 50.0f && time <= 55.0f) ? 1.5f : 1.7f;
        c.p = { a.x + (b.x - a.x) * pu, eye, a.z + (b.z - a.z) * pu };
        c.yaw = a.yaw + (b.yaw - a.yaw) * su;
        c.pitch = a.pitch + (b.pitch - a.pitch) * su;
        const float yr = c.yaw * kPiF / 180, pr = c.pitch * kPiF / 180;
        c.fwd = { std::sin(yr) * std::cos(pr), std::sin(pr), -std::cos(yr) * std::cos(pr) };
        c.lens = lensAt(time);
        c.cut = t == (uint32_t)(50 * kTickHz) || t == (uint32_t)(60 * kTickHz);
        if (b.y > -999.0f) c.p.y = (a.y > -999.0f ? a.y : eye) + (b.y - (a.y > -999.0f ? a.y : eye)) * su;
        cams[t] = c;
        if (t > 0 && !c.cut)
        {
            maxYawRate = std::max(maxYawRate, std::fabs(c.yaw - cams[t - 1].yaw) * kTickHz);
            maxSpeed = std::max(maxSpeed, length(c.p - cams[t - 1].p) * kTickHz);
        }
    }
    auto vfovDeg = [](float focalMm) { return 2.0f * std::atan(10.125f / focalMm) * 180.0f / kPiF; };
    {
        scene::Camera c;
        c.name = "fp_start";
        c.position = cams[0].p;
        c.forward = cams[0].fwd;
        c.verticalFov = vfovDeg(cams[0].lens.focalMm) * kPiF / 180;
        c.ev100 = 14.5f;
        s.cameras.push_back(c);
        for (const CaseRange& cr : cases)
        {
            const uint32_t t = (uint32_t)std::lround(0.5f * (cr.t0 + cr.t1) * kTickHz);
            scene::Camera cc = c;
            cc.name = std::string("case_") + cr.name;
            cc.position = cams[t].p;
            cc.forward = cams[t].fwd;
            cc.verticalFov = vfovDeg(cams[t].lens.focalMm) * kPiF / 180;
            s.cameras.push_back(cc);
        }
        scene::CameraPath p;
        p.name = "fp_track";
        for (uint32_t t = 0; t <= ticks; t += 6) p.keys.push_back({ (float)t / kTickHz, cams[t].p, cams[t].fwd, { 0, 1, 0 } });
        s.paths.push_back(p);
    }

    // ---- destruction events: evenly spaced with jitter, each on a distinct fracturable object, an explosion impulse ----
    struct Event { uint32_t tick, target; float3 centre; const char* type = "fracture"; };
    std::vector<Event> events;
    {
        std::vector<uint32_t> frac;
        for (const Object& o : objects)
            if (o.category == "fracturable") frac.push_back(o.id);
        // Physics design 9: the carve cut at 35 s, the bonded wall breaks at 47 s (inside the aim case's view, yaw 200 deg).
        for (const Object& o : objects)
        {
            if (o.category == "fracturable.carve") events.push_back({ 35 * kTickHz, o.id, o.position + float3{ 0, 1.5f, 0 }, "carve" });
            if (o.category == "fracturable.bonded") events.push_back({ 47 * kTickHz, o.id, o.position + float3{ 0, 1.5f, -0.5f }, "fracture" });
        }
        const uint32_t n = (uint32_t)std::floor(rq.destructionPerMinute * rq.durationSeconds / 60.0f);
        for (uint32_t i = 0; i < n && !frac.empty(); ++i)
        {
            const float t = (i + 0.5f) * rq.durationSeconds / n + rng.range(-1.0f, 1.0f);
            const uint32_t pick = frac[rng.below((uint32_t)frac.size())];
            events.push_back({ (uint32_t)std::lround(std::clamp(t, 0.5f, rq.durationSeconds - 0.5f) * kTickHz), pick, objects[pick].position + float3{ rng.range(-1, 1), 0.5f, rng.range(-1, 1) } });
        }
        std::sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
    }

    // ---- bodies file (format 1, rigid subset) ----
    {
        std::ostringstream o;
        uint32_t mix[3] = {};
        for (const Body& b : bodies) ++mix[b.motion[0] == 'r' ? (b.motion[1] == 'e' ? 0 : 2) : 1];
        o << "{\n  \"format\": 1,\n  \"scene\": \"" << s.name << "\",\n  \"seed\": " << rq.seed << ",\n  \"scale\": 1,\n  \"section\": \"game bench " << rq.name
          << " (render replay of the rigid subset)\",\n  \"gravity\": [0, -9.81, 0],\n  \"t0\": 0,\n"
          << format("  \"mix\": { \"resting\": %u, \"falling\": %u, \"rolling\": %u },\n", mix[0], mix[1], mix[2]) << "  \"bodies\": [\n";
        for (size_t i = 0; i < bodies.size(); ++i)
        {
            const Body& b = bodies[i];
            const Kind& k = kKinds[b.kind];
            const PhysMaterial& pm = kMaterials[k.material];
            const float mass = pm.density * kindVolume(k, b.scale);
            o << "    { \"instance\": " << bodyInstance[i] << ", \"kind\": \"" << k.name << "\", \"shape\": \"" << k.shape << "\", \"halfExtents\": " << v3(k.half * b.scale)
              << ", \"position\": " << v3(b.position) << ", \"rotation\": " << q4(b.rotation) << ", \"motion\": \"" << b.motion << "\", \"velocity\": " << v3(b.velocity)
              << ", \"angularVelocity\": " << v3(b.angularVelocity) << format(", \"restHeight\": %.9g, \"period\": %.9g", b.restHeight, b.period)
              << format(", \"object\": %u, \"material\": \"%s\", \"mass\": %.6g, \"friction\": %.3g, \"restitution\": %.3g }", b.object, pm.name, mass, pm.friction, pm.restitution)
              << (i + 1 < bodies.size() ? ",\n" : "\n");
        }
        o << "  ],\n  \"characters\": []\n}\n";
        out.bodiesJson = o.str();
    }

    // ---- bench JSON ----
    {
        std::ostringstream o;
        uint32_t byCat[16] = {};
        std::vector<std::string> cats;
        for (const Object& ob : objects)
        {
            auto it = std::find(cats.begin(), cats.end(), ob.category);
            if (it == cats.end()) { cats.push_back(ob.category); it = cats.end() - 1; }
            ++byCat[it - cats.begin()];
        }
        o << "{\n  \"format\": \"gamebench 1\",\n  \"name\": \"" << rq.name << "\",\n  \"scene\": \"" << s.name << ".unxscene\",\n  \"bodiesFile\": \"" << s.name << "_bodies.json\",\n";
        o << "  \"parameters\": {" << format(" \"seed\": %llu, \"movingObjects\": %u, \"debrisShare\": %.3g, \"debrisCap\": %u, \"spawnedObjects\": %u,", (unsigned long long)rq.seed, rq.movingObjects,
                                            rq.debrisShare, debrisCap, spawned)
          << format(" \"mix\": { \"box\": %.3g, \"clutter\": %.3g, \"fracturable\": %.3g, \"ragdoll\": %.3g, \"vehicle\": %.3g, \"hinge\": %.3g, \"cloth\": %.3g, \"rope\": %.3g, \"soft\": %.3g },",
                    mx.box, mx.clutter, mx.fracturable, mx.ragdoll, mx.vehicle, mx.hinge, mx.cloth, mx.rope, mx.soft)
          << format(" \"destructionPerMinute\": %.3g, \"fragmentsPerBreak\": %u, \"debrisLifetime\": %.3g, \"fluidParticles\": %u, \"durationSeconds\": %.3g, \"lights\": %u, \"shadowedLights\": %u, \"verticalFovDeg\": %.3g },\n",
                    rq.destructionPerMinute, rq.fragmentsPerBreak, rq.debrisLifetime, rq.fluidParticles, rq.durationSeconds, rq.lights, rq.shadowedLights, rq.verticalFovDeg);
        o << "  \"targets\": { \"resolutions\": [\"3840x2160\", \"2560x1440\"], \"fps\": 165, \"frameMs4K\": 6.06, \"tickHz\": 60, \"note\": \"user profile 2026-09-26: 4K 165 fps render, 60 Hz authority tick\" },\n";
        o << "  \"counts\": { \"objects\": " << objects.size() << ", \"rigidBodiesReplayed\": " << bodies.size() << ", \"byCategory\": {";
        for (size_t c = 0; c < cats.size(); ++c) o << (c ? ", " : " ") << "\"" << cats[c] << "\": " << byCat[c];
        o << " } },\n";
        o << format("  \"cameraCheck\": { \"maxYawRateDegPerS\": %.1f, \"maxSpeedMps\": %.2f },\n", maxYawRate, maxSpeed);
        o << "  \"viewModel\": { \"slot\": \"arms + rifle (MotusMan v55 arms, MCO M4_Rifle_01; personal-use licence, recheck before distribution)\", \"distanceM\": [0.2, 1.0], \"maxTriangles\": 100000, \"skinned\": true, \"materials\": \"2..4 incl. metal\", \"castsShadow\": true, \"inTlas\": true, \"offset\": [0.22, -0.24, 0.45], \"verticalFovDeg\": 55, \"status\": \"asset import pending; renderer feature missing (P8 first-person view model)\" },\n";
        o << "  \"cameraEffects\": { \"motionBlur\": \"on (lens/time integral, P8)\", \"depthOfField\": \"on (P8)\", \"status\": \"renderer v1 is a pinhole camera (INTERFACES 8.4): missing\" },\n";
        {
            const double vol = rq.fluidParticles * 0.05 * 0.05 * 0.05 / 8.0;
            o << format("  \"fluid\": { \"particles\": %u, \"recipe\": \"MatterRecipe (MPM)\", \"dx\": 0.05, \"particlesPerCell\": 8, \"volumeM3\": %.4f, \"depthM\": %.4f, \"substeps\": 2, \"sortEverySteps\": 4, "
                        "\"container\": \"tank inner 2.5 x 2.5 m at (30, -35), walls 1.2 m\", \"coupling\": \"4 crate_s dropped into the tank every 6 s\", "
                        "\"record\": [\"active cells\", \"GPU clock locked\", \"GPU ms per tick (P2G, G2P, sort, grid)\", \"VRAM\"], \"designFormula\": \"T_tick = s N (0.15 + 0.36 + 0.97 / k_sort) ns + grid 0.01..0.02 ms (PHYSICS_DESIGN 9)\" },\n",
                        rq.fluidParticles, vol, vol / (2.5 * 2.5));
        }
        o << "  \"objects\": [\n";
        for (size_t i = 0; i < objects.size(); ++i)
        {
            const Object& ob = objects[i];
            o << "    { \"id\": " << ob.id << ", \"category\": \"" << ob.category << "\", \"kind\": \"" << ob.kind << "\", \"recipe\": \"" << ob.recipe << "\", \"position\": " << v3(ob.position)
              << format(", \"yaw\": %.6g", ob.yaw);
            if (!ob.bodies.empty())
            {
                o << ", \"bodies\": [";
                for (size_t k = 0; k < ob.bodies.size(); ++k) o << (k ? ", " : "") << ob.bodies[k];
                o << "]";
            }
            if (!ob.params.empty()) o << ", " << ob.params;
            o << " }" << (i + 1 < objects.size() ? ",\n" : "\n");
        }
        o << "  ],\n  \"events\": [\n";
        for (size_t i = 0; i < events.size(); ++i)
            o << "    { \"tick\": " << events[i].tick << ", \"type\": \"" << events[i].type << "\", \"target\": " << events[i].target << ", \"explosion\": { \"centre\": " << v3(events[i].centre)
              << ", \"radius\": 6, \"impulse\": 2500 } }" << (i + 1 < events.size() ? ",\n" : "\n");
        o << "  ],\n";
        o << "  \"cases\": [";
        for (size_t c = 0; c < std::size(cases); ++c)
            o << (c ? ", " : "") << format("{ \"name\": \"%s\", \"ticks\": [%u, %u] }", cases[c].name, (uint32_t)std::lround(cases[c].t0 * kTickHz), (uint32_t)std::lround(cases[c].t1 * kTickHz));
        o << "],\n";
        o << "  \"camera\": { \"tickHz\": 60, \"sensorMm\": [36, 20.25], \"shutterS\": 0.0030303, \"shutterAngleDeg\": 180, \"note\": \"user 11:20: 180 deg shutter (1/330 s at 165 fps) and a pinhole aperture (fNumber 0) in every case except aim (24 mm f/2.8, focus = aim target distance); vertical FOV = 2 atan(10.125 / focal)\",\n"
             "    \"columns\": [\"position\", \"forward\", \"yawDeg\", \"pitchDeg\", \"focalMm\", \"fNumber\", \"focusM\", \"verticalFovDeg\", \"cut\"], \"samples\": [\n";
        for (uint32_t t = 0; t <= ticks; ++t)
            o << "      [" << v3(cams[t].p) << ", " << v3(cams[t].fwd)
              << format(", %.4f, %.4f, %.3f, %.3f, %.3f, %.4f, %d]", cams[t].yaw, cams[t].pitch, cams[t].lens.focalMm, cams[t].lens.fNumber, cams[t].lens.focusM, vfovDeg(cams[t].lens.focalMm), cams[t].cut ? 1 : 0)
              << (t < ticks ? ",\n" : "\n");
        o << "    ] },\n";
        o << "  \"resultSchema\": \"Content/RPP1/GameBench/GAMEBENCH_KO.md 4\",\n";
        o << "  \"gaps\": [\n"
             "    \"renderer: deformables (cloth, rope, soft bodies) are not drawn (I bridge logs soft bodies as not shown)\",\n"
             "    \"renderer: Matter fluid surface is not drawn\",\n"
             "    \"renderer: destruction fragments of the old engine are not drawn by the bridge (the render replay uses a pooled fragment stand-in at the debris cap)\",\n"
             "    \"renderer: first-person view model (separate FOV and depth, shadows/GI consistent with the world) - P8\",\n"
             "    \"renderer: motion blur and depth of field as lens/time integrals - P8; v1 is a pinhole camera\",\n"
             "    \"content: ragdoll humanoid mesh + skeleton (no character asset in the old project; physics-only until one exists)\",\n"
             "    \"host: a Unity data-World runner that spawns this population from the recipes and replays the camera (Assets/RPP1/GameBench, not built yet)\"\n"
             "  ]\n}\n";
        out.benchJson = o.str();
    }
    logf("gamebench %s: %u objects (%u spawned + debris cap %u), %zu rigid bodies replayed, %zu events, max yaw rate %.0f deg/s, max speed %.1f m/s\n", rq.name.c_str(),
         (uint32_t)objects.size(), spawned, debrisCap, bodies.size(), events.size(), maxYawRate, maxSpeed);
    return out;
}
} // namespace unx::rpp
