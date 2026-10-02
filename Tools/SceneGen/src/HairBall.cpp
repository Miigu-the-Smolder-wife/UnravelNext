// HairBall (diagnostic): strand hair in two colours side by side on a neutral ground. Two head-sized spheres (radius
// 9.7 cm) at eye height, 56 cm apart, each under a groom of 40,000 strands (the hair gate's body: 2,500 guides with 16
// follow strands each, 12 nodes, 2.5 cm segments, roots on the upper scalp at 10 cm from the centre, falling outwards
// and down, along the scalp where that would lead into the head): from -x a dark groom (eumelanin 2.5) and a blond one (eumelanin 0.15); the fibres are otherwise the same
// (eta 1.55, beta_M 0.3, beta_N 0.3, cuticle tilt 2 degrees). Key: a shadow-casting point light front-left above, about
// 110 to 160 lux on the heads. Rim: a shadow-casting point light behind them, right and above, 310 to 380 lux. The sun is low
// and dim (12 degrees up, 100 lux above the atmosphere). The scene holds the spheres and the materials; the strands are
// its grooms (SceneGen.h grooms: the scene's host makes a hair body of each).
// Cameras: "front" (the key's side, the rim light behind the hair) and "back" (the rim's side).
#include "Common.h"

namespace unx::scenegen
{
using namespace detail;

namespace
{
constexpr float kEye = 1.6f, kHeadRadius = 0.097f, kHeadOffset = 0.28f;

void build(Scene& s, std::vector<Groom>* grooms)
{
    s.name = "hair_ball";
    s.sun.direction = sunDirection(12.0f, 60.0f);
    s.sun.illuminance = 100.0f;
    s.windDirection = normalize(f3(0.8f, 0, 0.6f));
    s.windSpeed = 0.0f;
    Material ground;
    ground.name = "hair_ground";
    ground.baseColor = f3(0.5f, 0.5f, 0.5f);
    ground.roughness = 0.9f;
    {
        MeshBuilder b("hair_ground");
        b.material(addMaterial(s, ground));
        b.box({ -6.0f, -0.3f, -6.0f }, { 6.0f, 0.0f, 6.0f }, 0.5f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    Material scalp;
    scalp.name = "hair_scalp";
    scalp.baseColor = f3(0.80f, 0.56f, 0.45f);
    scalp.roughness = 0.5f;
    const uint32_t scalpMaterial = addMaterial(s, scalp);
    Material dark;
    dark.name = "hair_dark";
    dark.cls = scene::MaterialClass::Hair;
    dark.baseColor = f3(0.05f, 0.03f, 0.02f);  // (the colour a reader without the fibre model takes)
    dark.roughness = 0.3f;
    dark.ior = 1.55f;
    dark.hairEumelanin = 2.5f;
    dark.hairBetaN = 0.3f;
    Material blond = dark;
    blond.name = "hair_blond";
    blond.baseColor = f3(0.65f, 0.50f, 0.30f);
    blond.hairEumelanin = 0.15f;
    const uint32_t hairMaterials[2] = { addMaterial(s, dark), addMaterial(s, blond) };
    for (int i = 0; i < 2; ++i)
    {
        const float3 head{ (i == 0 ? -kHeadOffset : kHeadOffset), kEye, 0.0f };
        MeshBuilder b(i == 0 ? "head_dark" : "head_blond");
        b.material(scalpMaterial);
        b.sphere(head, kHeadRadius, 96, 48);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
        if (!grooms) continue;
        Groom g;
        g.head = head;
        g.headRadius = kHeadRadius;
        g.material = hairMaterials[i];
        g.nodesPerStrand = 12;
        const uint32_t guides = 2500, follows = 16;
        Rng r(11, 400 + (uint64_t)i);
        for (uint32_t k = 0; k < guides; ++k)
        {
            // roots on the upper scalp, strands falling outwards and down; a strand never starts into the head (the part
            // of its direction towards the centre is left out: over the crown it leaves along the scalp)
            const float phi = 2 * kPi * r.uniform(), c = 0.1f + 0.88f * r.uniform(), sn = std::sqrt(1 - c * c);
            const float3 n{ sn * std::cos(phi), c, sn * std::sin(phi) };
            float3 dir = n + f3(0, -1.5f, 0);
            dir = normalize(dir - n * std::fmin(dot(dir, n), 0.0f));
            for (uint32_t node = 0; node < g.nodesPerStrand; ++node) g.restPositions.push_back(n * 0.1f + dir * (0.025f * node));
            for (uint32_t f = 0; f < follows; ++f) g.follows.push_back({ k, f3(0, 0.003f * (r.uniform() - 0.5f), 0.003f * (r.uniform() - 0.5f)), 1.2f });
        }
        grooms->push_back(std::move(g));
    }
    scene::Light key;
    key.type = scene::LightType::Point;
    key.position = f3(-0.9f, 2.3f, 1.3f);
    key.intensity = 400.0f;  // 156 lux on the dark head (1.60 m), 112 on the blond one (1.89 m)
    key.range = 12.0f;
    key.castShadow = true;
    s.lights.push_back(key);
    scene::Light rim;
    rim.type = scene::LightType::Point;
    rim.position = f3(0.35f, 1.95f, -1.2f);
    rim.intensity = 600.0f;  // 383 lux on the blond head (1.25 m), 306 on the dark one (1.40 m)
    rim.range = 12.0f;
    rim.castShadow = true;
    s.lights.push_back(rim);
    s.cameras.push_back(camera("front", f3(0.0f, kEye + 0.02f, 1.3f), f3(0.0f, kEye - 0.03f, 0.0f), 6.0f, 30.0f));
    s.cameras.push_back(camera("back", f3(0.0f, kEye + 0.02f, -1.3f), f3(0.0f, kEye - 0.03f, 0.0f), 6.0f, 30.0f));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
}
} // namespace

namespace detail
{
Scene hairBall(const Request& rq)
{
    Scene s;
    build(s, nullptr);
    (void)rq;
    return s;
}
} // namespace detail

std::vector<Groom> grooms(const Request& rq)
{
    std::vector<Groom> out;
    if (rq.id != SceneId::HairBall) return out;
    Scene s;
    build(s, &out);
    return out;
}
} // namespace unx::scenegen
