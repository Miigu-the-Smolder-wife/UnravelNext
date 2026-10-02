// The six test scenes of INTERFACES_KO.md 10.1 and the generator entry points. Each scene documents what it tests.
#include "Common.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>

namespace unx::scenegen
{
using namespace detail;

namespace
{
void commonSky(Scene& s, float elevation, float azimuth)
{
    s.sun.direction = sunDirection(elevation, azimuth);
    s.windDirection = normalize(f3(0.8f, 0, 0.6f));
}

// CityBlock (P0b/P1): 5 x 5 blocks (380 m square), 1-4 buildings per block with window panes, sidewalks, street
// lamps (unlit), street trees (alpha cards, wind), overhead wires (6 mm), 2 km terrain. Sun 35 deg.
Scene cityBlock(const Request& rq)
{
    Scene s;
    s.name = "city_block";
    commonSky(s, 35.0f, 140.0f);
    s.windSpeed = 3.0f;
    const Palette p = buildPalette(s, rq.seed, false);
    const CityLayout city = buildCity(s, p, rq.seed, rq.scale, false);
    const float ev = 15.0f;  // sunny 16 rule
    const float street = -city.extent + 0.5f * 10.0f + 2 * 76.0f;  // centre line of the third street along Z
    s.cameras.push_back(camera("street", { street + 1.5f, 1.7f, 120.0f }, { street + 1.5f, 6.0f, -200.0f }, ev));
    s.cameras.push_back(camera("aerial", { 230.0f, 90.0f, 230.0f }, { 0, 0, 0 }, ev));
    const float3 sun = s.sun.direction;
    s.cameras.push_back(camera("sun_facing", { street - 2.0f, 1.7f, -20.0f }, f3(street - 2.0f, 1.7f, -20.0f) + normalize(f3(sun.x, 0.15f, sun.z)) * 50.0f, ev));
    s.cameras.push_back(camera("wires", { street + 3.0f, 1.7f, 40.0f }, { street + 3.0f, 9.0f, -40.0f }, ev));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    s.paths.push_back(linearPath("walk", { street + 1.5f, 1.7f, 120 }, { street + 1.5f, 1.7f, 60 }, 1.4f, { 0, 0.05f, 0 }));
    s.paths.push_back(linearPath("drive", { street, 1.5f, 170 }, { street, 1.5f, -170 }, 20.0f));
    s.paths.push_back(linearPath("fast", { street, 1.5f, 170 }, { street, 1.5f, -170 }, 60.0f));
    s.paths.push_back(orbitPath("aerial_orbit", { 0, 0, 0 }, 320.0f, 90.0f, 0.1f, 20.0f));
    return s;
}

// CityNight (P2): the same city at night with wet roads (roughness 0.15-0.35) and 512 local lights, 128 shadowed:
// 128 street-light spots (shadowed), 256 shop-front rect panels, 64 neon tubes, 64 bulb spheres (unshadowed).
Scene cityNight(const Request& rq)
{
    Scene s;
    s.name = "city_night";
    commonSky(s, -12.0f, 140.0f);  // nautical twilight: faint sky, no direct sun
    s.windSpeed = 3.0f;
    const Palette p = buildPalette(s, rq.seed, true);
    const CityLayout city = buildCity(s, p, rq.seed, rq.scale, true);
    const float3 sodium = luminanceNormalised(f3(1.0f, 0.78f, 0.55f));
    for (const float3& head : city.lampHeads)
    {
        scene::Light l;
        l.type = scene::LightType::Spot;
        l.position = head - f3(0, 0.02f, 0);
        l.forward = f3(0, -1, 0);
        l.color = sodium;
        l.intensity = 4000.0f;  // cd
        l.range = 35.0f;
        l.spotInner = 0.70f;
        l.spotOuter = 1.20f;
        l.castShadow = true;
        s.lights.push_back(l);
    }
    // Facade anchors: street-level windows, spread deterministically.
    const size_t anchors = city.windowCentres.size();
    const float scale = std::max(rq.scale, 0.04f);
    const size_t rects = (size_t)std::lround(256 * scale), tubes = (size_t)std::lround(64 * scale), bulbs = (size_t)std::lround(64 * scale);
    const size_t need = rects + tubes + bulbs;
    if (anchors < need) fail("city_night: %zu facade anchors for %zu lights", anchors, need);
    const float3 shopColours[4] = { luminanceNormalised(f3(1.0f, 0.85f, 0.7f)), luminanceNormalised(f3(0.7f, 0.85f, 1.0f)),
                                    luminanceNormalised(f3(1.0f, 0.4f, 0.6f)), luminanceNormalised(f3(0.5f, 1.0f, 0.8f)) };
    Rng r(rq.seed, 40);
    for (size_t i = 0; i < need; ++i)
    {
        const size_t a = (i * anchors) / need;
        const float3 c = city.windowCentres[a], n = city.windowNormals[a];
        const float3 along = normalize(cross(f3(0, 1, 0), n));
        scene::Light l;
        l.castShadow = false;
        if (i < rects)
        {
            l.type = scene::LightType::Rect;
            l.position = c + n * 0.1f;
            l.forward = n;
            l.right = along;
            l.size = { 1.6f, 1.8f };
            l.intensity = 150.0f;  // nits
            l.range = 20.0f;
            l.color = shopColours[r.below(4)];
        }
        else if (i < rects + tubes)
        {
            l.type = scene::LightType::Tube;
            l.position = c + f3(0, 1.4f, 0) + n * 0.15f;
            l.forward = n;
            l.right = along;
            l.size = { 1.6f, 0.02f };
            l.intensity = 3000.0f;
            l.range = 15.0f;
            l.color = shopColours[2 + r.below(2)];
        }
        else
        {
            l.type = scene::LightType::Sphere;
            l.position = c + f3(0, 1.6f, 0) + n * 0.6f;
            l.forward = f3(0, -1, 0);
            l.size = { 0.06f, 0 };
            l.intensity = 50000.0f;
            l.range = 15.0f;
            l.color = luminanceNormalised(f3(1.0f, 0.8f, 0.6f));
        }
        s.lights.push_back(l);
    }
    const float ev = 2.0f;
    const float street = -city.extent + 0.5f * 10.0f + 2 * 76.0f;
    s.cameras.push_back(camera("street", { street + 1.5f, 1.7f, 120.0f }, { street + 1.5f, 3.0f, -200.0f }, ev));
    s.cameras.push_back(camera("wet_road", { street - 1.0f, 1.2f, 60.0f }, { street - 1.0f, 0.0f, 20.0f }, ev));
    s.cameras.push_back(camera("aerial", { 230.0f, 90.0f, 230.0f }, { 0, 0, 0 }, ev));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    s.paths.push_back(linearPath("walk", { street + 1.5f, 1.7f, 120 }, { street + 1.5f, 1.7f, 60 }, 1.4f, { 0, 0.05f, 0 }));
    s.paths.push_back(linearPath("drive", { street, 1.5f, 170 }, { street, 1.5f, -170 }, 20.0f));
    return s;
}

// Forest (P1/P3): 2 x 2 km of the microbench terrain, 100k trees and 1M grass clumps at scale 1 (the census scene of
// ARCHITECTURE 1.2), wind 3 m/s. thin = 6 cm geometric leaves / 4 mm blades; card = 35 cm leaf cards / 30 cm cards.
Scene forest(const Request& rq, bool thin)
{
    Scene s;
    s.name = thin ? "forest_thin" : "forest_card";
    commonSky(s, 40.0f, 200.0f);
    s.windSpeed = 3.0f;
    const Palette p = buildPalette(s, rq.seed, false);
    {
        MeshBuilder b("terrain");
        b.material(p.grass);
        b.heightfield(-1000, -1000, 1000, 1000, 1024, rollingTerrain, 1.0f / 8.0f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    const FoliageAssets f = buildFoliage(s, p, rq.seed, FoliageStyle{ thin });
    const float scale = std::max(rq.scale, 1e-4f);
    const uint32_t trees = (uint32_t)std::lround(100000 * scale), clumps = (uint32_t)std::lround(1000000 * scale);
    Rng rt(rq.seed, 50), rg(rq.seed, 51);
    for (uint32_t i = 0; i < trees; ++i)
    {
        const float x = rt.range(-1000, 1000), z = rt.range(-1000, 1000), yaw = rt.range(0, 2 * kPi), sc = rt.range(0.8f, 1.3f);
        Instance& in = addInstance(s, f.treeMeshes[i & 3], placement({ x, rollingTerrain(x, z) - 0.2f, z }, yaw, sc), scene::InstanceCastShadow | scene::InstanceWind);
        in.wind = { 20.0f, rt.range(0, 2 * kPi), 1.0f };
    }
    for (uint32_t i = 0; i < clumps; ++i)
    {
        const float x = rg.range(-1000, 1000), z = rg.range(-1000, 1000), yaw = rg.range(0, 2 * kPi), sc = rg.range(0.7f, 1.2f);
        Instance& in = addInstance(s, f.grassMeshes[i & 7], placement({ x, rollingTerrain(x, z), z }, yaw, sc), scene::InstanceCastShadow | scene::InstanceWind);
        in.wind = { 5.0f, rg.range(0, 2 * kPi), 0.0f };
    }
    const float ev = 14.0f;
    const float3 eye{ 600.0f, rollingTerrain(600, 600) + 1.7f, 600.0f };
    s.cameras.push_back(camera("forest", eye, eye + f3(100, 0, 0), ev));  // the census view (+X)
    const float3 meadow{ -300.0f, rollingTerrain(-300, 150) + 0.6f, 150.0f };
    s.cameras.push_back(camera("meadow", meadow, meadow + f3(6, -0.6f, 4), ev));
    const float3 under{ 10.0f, rollingTerrain(10, 10) + 1.7f, 10.0f };
    s.cameras.push_back(camera("canopy", under, under + f3(10, 12, 3), ev));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    s.paths.push_back(linearPath("walk", eye, eye + f3(84, 0, 0), 1.4f));
    s.paths.push_back(linearPath("run", eye, eye + f3(240, 0, 40), 6.0f));
    s.paths.push_back(linearPath("fast", eye + f3(0, 20, 0), eye + f3(600, 20, 0), 60.0f));
    return s;
}

// Waterside (P2/P4): a lake basin with a calm water plane (planar-mirror case, slope 0), a wind-wave bay (0.08 m
// sinusoid sum), wet shore rocks, card trees on the banks and 3 mm reeds (thin geometry). Low sun (12 deg).
// Water is a Standard dielectric (f0 0.02, roughness 0.02 / 0.05, dark body colour) until the Water class exists.

float waveHeight(float x, float z)
{
    const float taper = smoothstepf(130.0f, 160.0f, x);
    static const float waves[6][4] = { { 6.0f, 0.030f, 0.9f, 0.2f }, { 3.7f, 0.022f, 0.6f, 1.1f }, { 2.3f, 0.014f, 1.3f, 2.3f },
                                       { 1.6f, 0.010f, 0.3f, 0.7f }, { 1.1f, 0.006f, 1.7f, 4.1f }, { 0.8f, 0.004f, 1.0f, 5.3f } };
    float h = 0;
    for (const auto& w : waves)  // wavelength, amplitude, direction angle, phase
    {
        const float k = 2 * kPi / w[0];
        h += w[1] * std::sin(k * (x * std::cos(w[2]) + z * std::sin(w[2])) + w[3]);
    }
    return taper * h;
}

Scene waterside(const Request& rq)
{
    Scene s;
    s.name = "waterside";
    commonSky(s, 12.0f, 160.0f);  // low sun behind the lake camera: the far shore is front-lit in the calm mirror
    s.windSpeed = 4.0f;
    const Palette p = buildPalette(s, rq.seed, false);
    {
        MeshBuilder b("terrain");
        b.material(p.grass);
        b.heightfield(-600, -600, 600, 600, 600, lakeFloor, 1.0f / 8.0f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    Material calm;
    calm.name = "water_calm";
    calm.baseColor = f3(0.012f, 0.02f, 0.02f);
    calm.specular = 0.25f;  // f0 = 0.02 (water, n = 1.33)
    calm.roughness = 0.02f;
    const uint32_t calmMat = addMaterial(s, calm);
    Material wavy = calm;
    wavy.name = "water_waves";
    wavy.roughness = 0.05f;
    const uint32_t waveMat = addMaterial(s, wavy);
    {
        MeshBuilder b("water_calm");
        b.material(calmMat);
        for (int j = 0; j < 12; ++j)
            for (int i = 0; i < 10; ++i)
            {
                const float x0 = -170.0f + i * 30.0f, z0 = -180.0f + j * 30.0f;
                b.quadXZ(x0, z0, x0 + 30.0f, z0 + 30.0f, 0.0f, 1.0f / 30.0f);
            }
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    {
        MeshBuilder b("water_waves");
        b.material(waveMat);
        b.heightfield(130.0f, -120.0f, 430.0f, 180.0f, 1000, waveHeight, 1.0f / 30.0f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    // Shore rocks: displaced spheres, wet below 0.3 m above the water.
    std::vector<uint32_t> rockMeshes;
    for (int v = 0; v < 6; ++v)
    {
        MeshBuilder b("rock_" + std::to_string(v));
        b.material(p.rock);
        b.sphere({ 0, 0, 0 }, 1.0f, 24, 16, 0.5f);
        Mesh m = b.finish(false);
        for (size_t k = 0; k < m.positions.size(); ++k)
        {
            const float3 q = m.positions[k];
            const float d = 0.75f + 0.5f * fbm2(q.x * 1.3f + v * 7.1f, q.z * 1.3f + q.y * 0.7f, (uint32_t)rq.seed + 90 + v, 4);
            m.positions[k] = f3(q.x * d * 1.3f, q.y * d * 0.8f, q.z * d);
        }
        // Recompute smooth normals from the displaced surface.
        std::vector<float3> acc(m.positions.size());
        for (size_t t = 0; t < m.indices.size(); t += 3)
        {
            const uint32_t a = m.indices[t], bb = m.indices[t + 1], c = m.indices[t + 2];
            const float3 n = cross(m.positions[bb] - m.positions[a], m.positions[c] - m.positions[a]);
            acc[a] = acc[a] + n;
            acc[bb] = acc[bb] + n;
            acc[c] = acc[c] + n;
        }
        for (size_t k = 0; k < acc.size(); ++k) m.normals[k] = length(acc[k]) > 0 ? normalize(acc[k]) : f3(0, 1, 0);
        computeTangents(m);
        rockMeshes.push_back(addMesh(s, std::move(m)));
    }
    Rng rr(rq.seed, 60);
    for (int i = 0; i < (int)std::lround(120 * std::max(rq.scale, 0.05f)); ++i)
    {
        const float a = rr.range(0, 2 * kPi), rad = rr.range(150.0f, 185.0f);
        const float x = rad * std::cos(a), z = rad * std::sin(a);
        if (x > 60 && std::fabs(z) < 140) continue;  // bay mouth
        const float y = lakeFloor(x, z);
        Instance& in = addInstance(s, rockMeshes[i % 6], placement({ x, y - 0.3f, z }, rr.range(0, 2 * kPi), rr.range(0.6f, 2.2f)));
        if (y < 0.3f) in.materialOverrides = { p.rockWet };
    }
    // Trees on the banks (card foliage for the reflection tests' far shore) and reeds at the waterline.
    const FoliageAssets f = buildFoliage(s, p, rq.seed ^ 0x7A7Eull, FoliageStyle{ false });
    Rng rt(rq.seed, 61);
    const int treeCount = (int)std::lround(3000 * std::max(rq.scale, 0.05f));
    for (int i = 0, placed = 0; placed < treeCount && i < treeCount * 20; ++i)
    {
        const float x = rt.range(-600, 600), z = rt.range(-600, 600), y = lakeFloor(x, z);
        if (y < 1.0f) continue;
        Instance& in = addInstance(s, f.treeMeshes[placed & 3], placement({ x, y - 0.2f, z }, rt.range(0, 2 * kPi), rt.range(0.8f, 1.3f)), scene::InstanceCastShadow | scene::InstanceWind);
        in.wind = { 20.0f, rt.range(0, 2 * kPi), 1.0f };
        ++placed;
    }
    Material reedMat;
    reedMat.name = "reed";
    reedMat.cls = scene::MaterialClass::Foliage;
    reedMat.baseColor = f3(0.12f, 0.13f, 0.05f);
    reedMat.roughness = 0.5f;
    reedMat.transmission = 0.2f;
    reedMat.twoSided = true;
    const uint32_t reedM = addMaterial(s, reedMat);
    uint32_t reedMesh;
    {
        MeshBuilder b("reeds");
        b.material(reedM);
        Rng r(rq.seed, 62);
        for (int k = 0; k < 30; ++k)
        {
            const float x = r.range(-0.6f, 0.6f), z = r.range(-0.6f, 0.6f), h = r.range(1.0f, 1.8f);
            b.cylinder({ x, 0, z }, { r.range(-0.1f, 0.1f), h, r.range(-0.1f, 0.1f) }, 0.003f, 0.0015f, 5, 3, false, 1.0f);
        }
        reedMesh = addMesh(s, b.finish(false));
    }
    for (int i = 0; i < (int)std::lround(400 * std::max(rq.scale, 0.05f)); ++i)
    {
        const float a = rr.range(0, 2 * kPi), rad = rr.range(125.0f, 150.0f);
        const float x = rad * std::cos(a), z = rad * std::sin(a);
        if (x > 60 && std::fabs(z) < 140) continue;
        Instance& in = addInstance(s, reedMesh, placement({ x, -0.3f, z }, rr.range(0, 2 * kPi)), scene::InstanceCastShadow | scene::InstanceWind);
        in.wind = { 3.0f, rr.range(0, 2 * kPi), 0.3f };
    }
    const float ev = 13.0f;
    s.cameras.push_back(camera("lake", { -175.0f, lakeFloor(-175, 0) + 1.7f, 0.0f }, { 150.0f, 3.0f, 30.0f }, ev));
    s.cameras.push_back(camera("waves", { 300.0f, 2.5f, 175.0f }, { 280.0f, 0.0f, 80.0f }, ev));
    s.cameras.push_back(camera("grazing", { -100.0f, 0.5f, 30.0f }, { 100.0f, 0.2f, 40.0f }, ev));
    // Into the sun over the wave bay: sun glints on the waves (specular aliasing test) and the Mie aureole.
    const float3 toSun = normalize(f3(s.sun.direction.x, 0, s.sun.direction.z));
    s.cameras.push_back(camera("glint", { 400.0f, 3.0f, -100.0f }, f3(400.0f, 3.0f, -100.0f) + toSun * 100.0f + f3(0, -3.0f, 0), ev));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    s.paths.push_back(orbitPath("shore_walk", { 0, 0, 0 }, 190.0f, 2.0f, 1.4f / 190.0f, 30.0f));
    return s;
}

// Interior (P2): 8 x 6 x 3.5 m room. Floor strips of roughness 0.15 / 0.25 / 0.35 / 0.50 (the K/G/M reflection grid),
// a planar wall mirror (roughness 0), a chrome sphere (curved mirror, 0.05), area lights (2 rect panels, disk,
// 2 spheres shadowed; 1 tube unshadowed), a sunlit window, furniture. Cameras at 0/60/75 deg floor incidence.
Scene interior(const Request& rq)
{
    Scene s;
    s.name = "interior";
    commonSky(s, 30.0f, 70.0f);  // sun towards +Z: sunlight enters the +Z window and travels towards -Z
    s.windSpeed = 0.0f;
    const Palette p = buildPalette(s, rq.seed, false);
    const float W = 4.0f, D = 3.0f, H = 3.5f, T = 0.2f;
    auto solid = [&](std::string name, uint32_t mat, float3 lo, float3 hi) {
        MeshBuilder b(std::move(name));
        b.material(mat);
        b.box(lo, hi, 0.5f);
        addInstance(s, addMesh(s, b.finish(s.materials[mat].normalTexture != scene::kNone)), float3x4{});
    };
    // Outside ground.
    {
        MeshBuilder b("ground");
        b.material(p.grass);
        b.heightfield(-100, -100, 100, 100, 50, [](float, float) { return -0.2f; }, 1.0f / 8.0f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    // Floor strips (along Z), each 2 m wide.
    const float roughness[4] = { 0.15f, 0.25f, 0.35f, 0.50f };
    for (int i = 0; i < 4; ++i)
    {
        Material m;
        m.name = "floor_r" + std::to_string((int)std::lround(roughness[i] * 100));
        m.baseColor = f3(0.10f, 0.10f, 0.10f);
        m.roughness = roughness[i];
        solid(m.name, addMaterial(s, m), { -W + 2.0f * i, -T, -D }, { -W + 2.0f * (i + 1), 0.0f, D });
    }
    // Walls with a window opening in +Z (x in [-1.5, 1.5], y in [0.9, 2.6]).
    solid("wall_nz", p.plaster, { -W - T, -T, -D - T }, { W + T, H, -D });
    solid("wall_px", p.plaster, { W, -T, -D }, { W + T, H, D });
    solid("wall_nx", p.plaster, { -W - T, -T, -D }, { -W, H, D });
    solid("wall_pz_left", p.plaster, { -W - T, -T, D }, { -1.5f, H, D + T });
    solid("wall_pz_right", p.plaster, { 1.5f, -T, D }, { W + T, H, D + T });
    solid("wall_pz_below", p.plaster, { -1.5f, -T, D }, { 1.5f, 0.9f, D + T });
    solid("wall_pz_above", p.plaster, { -1.5f, 2.6f, D }, { 1.5f, H, D + T });
    solid("ceiling", p.plaster, { -W - T, H, -D - T }, { W + T, H + T, D + T });
    // Mirror on the -Z wall: frame + silver surface (metal, roughness 0 -> GGX alpha 1e-4).
    Material mirror;
    mirror.name = "mirror";
    mirror.baseColor = f3(0.95f, 0.95f, 0.95f);
    mirror.metallic = 1.0f;
    mirror.roughness = 0.0f;
    const uint32_t mirrorMat = addMaterial(s, mirror);
    solid("mirror_frame", p.metal, { -1.3f, 0.5f, -D }, { 1.3f, 2.3f, -D + 0.03f });
    {
        MeshBuilder b("mirror");
        b.material(mirrorMat);
        const float z = -D + 0.035f;
        b.quad4({ -1.2f, 0.6f, z }, { 1.2f, 0.6f, z }, { 1.2f, 2.2f, z }, { -1.2f, 2.2f, z }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    // Chrome sphere on a plinth.
    Material chrome = mirror;
    chrome.name = "chrome";
    chrome.baseColor = f3(0.55f, 0.56f, 0.55f);
    chrome.roughness = 0.05f;
    const uint32_t chromeMat = addMaterial(s, chrome);
    solid("plinth", p.concrete, { 2.1f, 0.0f, -1.9f }, { 2.9f, 0.4f, -1.1f });
    {
        MeshBuilder b("chrome_sphere");
        b.material(chromeMat);
        b.sphere({ 2.5f, 0.85f, -1.5f }, 0.45f, 64, 32, 1.0f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    // Furniture: table, sofa block, bookshelf with 2 cm shelves, floor lamp pole.
    Material wood;
    wood.name = "wood";
    wood.baseColor = f3(0.25f, 0.14f, 0.07f);
    wood.roughness = 0.45f;
    const uint32_t woodMat = addMaterial(s, wood);
    solid("table_top", woodMat, { -1.0f, 0.72f, -0.5f }, { 0.6f, 0.76f, 0.4f });
    for (float lx : { -0.95f, 0.5f })
        for (float lz : { -0.45f, 0.3f }) solid("table_leg", woodMat, { lx, 0.0f, lz }, { lx + 0.05f, 0.72f, lz + 0.05f });
    Material fabric;
    fabric.name = "fabric";
    fabric.baseColor = f3(0.30f, 0.08f, 0.06f);
    fabric.roughness = 0.95f;
    const uint32_t fabricMat = addMaterial(s, fabric);
    solid("sofa_seat", fabricMat, { -3.9f, 0.0f, -2.2f }, { -3.0f, 0.45f, 0.4f });
    solid("sofa_back", fabricMat, { -4.0f, 0.0f, -2.2f }, { -3.75f, 0.95f, 0.4f });
    solid("shelf_side_a", woodMat, { 3.4f, 0.0f, 0.5f }, { 3.95f, 2.2f, 0.52f });
    solid("shelf_side_b", woodMat, { 3.4f, 0.0f, 2.0f }, { 3.95f, 2.2f, 2.02f });
    for (int k = 0; k < 5; ++k) solid("shelf", woodMat, { 3.4f, 0.1f + 0.5f * k, 0.52f }, { 3.95f, 0.12f + 0.5f * k, 2.0f });
    solid("lamp_pole", p.metal, { -3.2f, 0.0f, 1.6f }, { -3.17f, 1.7f, 1.63f });
    // Ceiling panels (non-emissive housings just above the rect lights' emitting planes).
    Material panel;
    panel.name = "panel";
    panel.baseColor = f3(0.8f, 0.8f, 0.8f);
    panel.roughness = 0.6f;
    const uint32_t panelMat = addMaterial(s, panel);
    const float3 panels[2] = { { -1.5f, H, 0.0f }, { 1.5f, H, 0.0f } };
    for (const float3& c : panels) solid("panel", panelMat, { c.x - 0.62f, H - 0.03f, c.z - 0.32f }, { c.x + 0.62f, H, c.z + 0.32f });
    const float3 white = luminanceNormalised(f3(1.0f, 0.95f, 0.88f));
    for (const float3& c : panels)
    {
        scene::Light l;
        l.type = scene::LightType::Rect;
        l.position = c - f3(0, 0.035f, 0);
        l.forward = f3(0, -1, 0);
        l.right = f3(1, 0, 0);
        l.size = { 1.2f, 0.6f };
        l.intensity = 4000.0f;
        l.range = 12.0f;
        l.color = white;
        l.castShadow = true;
        s.lights.push_back(l);
    }
    {
        scene::Light l;
        l.type = scene::LightType::Disk;
        l.position = { -3.0f, H - 0.01f, 2.0f };
        l.forward = f3(0, -1, 0);
        l.size = { 0.1f, 0 };
        l.intensity = 20000.0f;
        l.range = 10.0f;
        l.color = white;
        l.castShadow = true;
        s.lights.push_back(l);
    }
    for (float dz : { -0.12f, 0.12f })
    {
        scene::Light l;
        l.type = scene::LightType::Sphere;
        l.position = { -3.1f, 1.78f, 1.615f + dz };
        l.size = { 0.05f, 0 };
        l.intensity = 30000.0f;
        l.range = 10.0f;
        l.color = luminanceNormalised(f3(1.0f, 0.8f, 0.6f));
        l.castShadow = true;
        s.lights.push_back(l);
    }
    {
        scene::Light l;
        l.type = scene::LightType::Tube;
        l.position = { 3.67f, 2.4f, 1.26f };
        l.forward = f3(-1, 0, 0);
        l.right = f3(0, 0, 1);
        l.size = { 1.2f, 0.015f };
        l.intensity = 8000.0f;
        l.range = 8.0f;
        l.color = white;
        l.castShadow = false;
        s.lights.push_back(l);
    }
    const float ev = 9.0f;  // sunlit room (a sun patch of ~1e5 lux on the floor)
    s.cameras.push_back(camera("overview", { -3.5f, 1.7f, 2.6f }, { 1.0f, 0.8f, -2.0f }, ev));
    s.cameras.push_back(camera("mirror", { 0.5f, 1.6f, 1.5f }, { 0.0f, 1.4f, -D }, ev));
    s.cameras.push_back(camera("floor_0", { 0.0f, 3.3f, 0.0f }, { 0.0f, 0.0f, 0.0f }, ev));
    s.cameras.push_back(camera("floor_60", { 0.0f, 1.6f, 2.8f }, f3(0.0f, 1.6f, 2.8f) + f3(0, -std::sin(kPi / 6), -std::cos(kPi / 6)), ev));
    s.cameras.push_back(camera("floor_75", { 0.0f, 0.8f, 2.8f }, f3(0.0f, 0.8f, 2.8f) + f3(0, -std::sin(kPi / 12), -std::cos(kPi / 12)), ev));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    s.paths.push_back(linearPath("walk", { -3.0f, 1.7f, 2.5f }, { 2.0f, 1.7f, -1.0f }, 0.8f));
    return s;
}
// RidgeSunset (S: shadows in the atmosphere at distance). 20 x 20 km of gently rolling ground; a ridge 2 km wide and
// 800 m high 3 km in front of the camera; the sun 17 deg high behind it, 10 deg left of the view axis, so the ridge's
// shadow runs ~2.6 km towards the camera; 300 m towers at 1, 2, 5 and 8 km (right of the ridge, outside its shadow)
// cast long shadow shafts through the air. The camera stands 2 m above the ground looking towards the sun, 8 deg up.
// No wind (static reference).
float ridgeTerrain(float x, float z)
{
    const float rolling = 3.0f * std::sin(x / 900.0f) * std::cos(z / 1100.0f) + 1.5f * std::sin(x / 310.0f + z / 270.0f);
    const float dz = (z - 3000.0f) / 450.0f;
    const float lateral = 1.0f - smoothstepf(800.0f, 1000.0f, std::fabs(x + 1000.0f));
    const float crest = 1.0f + 0.06f * std::sin(x / 97.0f) + 0.04f * std::sin(x / 41.0f + 1.3f);
    return rolling + 800.0f * crest * lateral * std::exp(-dz * dz);
}

Scene ridgeSunset(const Request& rq)
{
    Scene s;
    s.name = "ridge_sunset";
    commonSky(s, 17.0f, 100.0f);  // azimuth 100 deg: towards +Z, 10 deg to -X
    s.windSpeed = 0.0f;
    const Palette p = buildPalette(s, rq.seed, false);
    {
        MeshBuilder b("terrain");
        b.material(p.grass);
        b.heightfield(-10000, -10000, 10000, 10000, 1000, ridgeTerrain, 1.0f / 8.0f);  // 20 m grid
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    {
        MeshBuilder b("tower");
        b.material(p.concrete);
        b.box({ -10.0f, -5.0f, -10.0f }, { 10.0f, 300.0f, 10.0f }, 0.5f);
        const uint32_t tower = addMesh(s, b.finish(true));
        for (float z : { 1000.0f, 2000.0f, 5000.0f, 8000.0f })
        {
            const float x = 600.0f + 0.1f * (z - 1000.0f);
            addInstance(s, tower, placement({ x, ridgeTerrain(x, z), z }, 0.0f));
        }
    }
    const float3 eye{ 0.0f, ridgeTerrain(0, 0) + 2.0f, 0.0f };
    const float3 flat = normalize(f3(s.sun.direction.x, 0, s.sun.direction.z));
    const float ev = 14.0f;
    s.cameras.push_back(camera("ridge", eye, eye + flat * 100.0f + f3(0, 100.0f * std::tan(8.0f * kPi / 180.0f), 0), ev));
    s.cameras.push_back(camera("side", eye + f3(-2500.0f, 30.0f, 1500.0f), eye + f3(-1000.0f, 400.0f, 3000.0f), ev));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    s.paths.push_back(linearPath("drive", eye, eye + flat * 1200.0f, 30.0f, f3(0, 0.14f, 0)));
    (void)rq;
    return s;
}
// Forest combat (gate scene for the RPP-1 "forest / combat" section, 2026-09-26): the forest_thin base (2 x 2 km,
// 100k trees, 1M grass clumps, 6 cm leaves, 4 mm blades, wind 3 m/s) plus a closed-canopy stand around the combat point
// (jittered grid, kStandSpacing, radius kStandRadius, crowns overlapping so no sky shows overhead), so the eye-level
// view is the section's heavy side: trunks, grass and canopy fill the screen. Grass keeps the base density. Cameras:
// eye (1.7 m, level), up (into the canopy: leaf coverage and transmission at their heaviest), edge (from outside the
// stand, looking in: the band B/C transition distances), vista (40 m up, across the forest: band C). Combat slots (characters, VFX) are empty until those assets
// exist; the scene's measured metadata (surface pixels, band counts, lights) is in Results/C/Scenes/forest_combat.md.
Scene forestCombat(const Request& rq)
{
    Scene s = forest(rq, true);
    s.name = "forest_combat";
    s.cameras.clear();
    s.paths.clear();
    const float kStandSpacing = 3.5f, kStandRadius = 110.0f;
    const float cx = 300.0f, cz = -200.0f;
    // Tree meshes of the base forest: instances 1..4 (after the terrain) use treeMeshes[i & 3] for i = 0..3.
    uint32_t treeMesh[4];
    for (int k = 0; k < 4; ++k) treeMesh[k] = s.instances[1 + k].mesh;
    Rng rs(rq.seed, 70);
    const int n = (int)std::ceil(kStandRadius / kStandSpacing);
    uint32_t count = 0;
    for (int iz = -n; iz <= n; ++iz)
        for (int ix = -n; ix <= n; ++ix)
        {
            const float x = cx + (ix + rs.range(-0.35f, 0.35f)) * kStandSpacing, z = cz + (iz + rs.range(-0.35f, 0.35f)) * kStandSpacing;
            const float yaw = rs.range(0, 2 * kPi), sc = rs.range(1.0f, 1.4f);
            const float d2 = (x - cx) * (x - cx) + (z - cz) * (z - cz);
            if (d2 > kStandRadius * kStandRadius) continue;
            if (d2 < 2.0f * 2.0f) continue;  // a small clearing so the eye is not inside a trunk
            Instance& in = addInstance(s, treeMesh[count++ & 3], placement({ x, rollingTerrain(x, z) - 0.2f, z }, yaw, sc), scene::InstanceCastShadow | scene::InstanceWind);
            in.wind = { 20.0f, rs.range(0, 2 * kPi), 1.0f };
        }
    const float3 eye{ cx, rollingTerrain(cx, cz) + 1.7f, cz };
    s.cameras.push_back(camera("eye", eye, eye + f3(100, 0, 20), 12.0f));
    s.cameras.push_back(camera("up", eye, eye + f3(8, 14, 3), 13.0f));
    const float ex = cx - kStandRadius - 40.0f;
    const float3 edge{ ex, rollingTerrain(ex, cz) + 1.7f, cz };
    s.cameras.push_back(camera("edge", edge, edge + f3(100, 0, 0), 14.0f));
    // Vista: above the canopy, looking across the forest towards the horizon; distant crowns and far grass (band C)
    // fill most of the frame.
    const float3 vista{ cx, rollingTerrain(cx, cz) + 40.0f, cz };
    s.cameras.push_back(camera("vista", vista, vista + f3(100, -18, 30), 14.0f));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    s.paths.push_back(linearPath("patrol", eye, eye + f3(60, 0, 25), 1.4f));
    s.paths.push_back(linearPath("sprint", eye - f3(40, 0, 0), eye + f3(60, 0, 10), 6.0f));
    return s;
}
} // namespace

// FurnaceRoom (diagnostic; SceneGen.h kFurnace*): six wall slabs of one Lambert material (no specular, roughness 1)
// closing a room, one shadow-casting point light at its centre, nothing else inside. Each slab is its own mesh and
// instance (its own mesh cards). The sun cannot enter.
Scene furnaceRoom(const Request& rq, bool day)
{
    Scene s;
    s.name = day ? "furnace_room_day" : "furnace_room";
    commonSky(s, 45.0f, 30.0f);
    // night outside: whatever a stage lets in through the walls adds nothing, so a leak reads as missing light (with the
    // sun up the first run's gather stood at 2.27 x: the radiance cache's probes outside the room saw the sky).
    // FurnaceRoomDay keeps the sun: the leak instrument.
    if (!day) s.sun.illuminance = 1e-3f;
    s.windSpeed = 0.0f;
    Material wall;
    wall.name = "furnace_wall";
    wall.baseColor = f3(kFurnaceAlbedo, kFurnaceAlbedo, kFurnaceAlbedo);
    wall.roughness = 1.0f;
    wall.specular = 0.0f;
    const uint32_t mat = addMaterial(s, wall);
    const float W = 0.5f * kFurnaceWidth, H = kFurnaceHeight, D = 0.5f * kFurnaceDepth, T = 0.3f;
    auto slab = [&](std::string name, float3 lo, float3 hi) {
        MeshBuilder b(std::move(name));
        b.material(mat);
        b.box(lo, hi, 0.5f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    };
    slab("furnace_floor", { -W - T, -T, -D - T }, { W + T, 0.0f, D + T });
    slab("furnace_ceiling", { -W - T, H, -D - T }, { W + T, H + T, D + T });
    slab("furnace_nx", { -W - T, 0.0f, -D - T }, { -W, H, D + T });
    slab("furnace_px", { W, 0.0f, -D - T }, { W + T, H, D + T });
    slab("furnace_nz", { -W, 0.0f, -D - T }, { W, H, -D });
    slab("furnace_pz", { -W, 0.0f, D }, { W, H, D + T });
    scene::Light light;
    light.type = scene::LightType::Point;
    light.position = f3(0.0f, 0.5f * H, 0.0f);
    light.intensity = kFurnaceCandela;
    light.range = 1000.0f;  // (the range window is 1 over the room to 1e-9)
    light.castShadow = true;
    s.lights.push_back(light);
    // from a corner at eye height toward the opposite corner: three walls, the floor and the ceiling in view
    s.cameras.push_back(camera("corner", f3(-W + 0.4f, 1.6f, -D + 0.4f), f3(W, 1.8f, D), 6.0f, 75.0f));
    s.cameras.push_back(camera("wall", f3(0.0f, 2.0f, -D + 0.4f), f3(0.0f, 2.0f, D), 6.0f, 75.0f));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    (void)rq;
    return s;
}

// ShadingBall (diagnostic): the shading models side by side on a neutral ground. A row of five spheres of radius 0.25 m
// at eye height, from -x: Standard; Subsurface with the class's default lobes and transmission 0.5; Subsurface with one
// lobe (mix 1, scales (1, 1)) and no transmission - by the model the first sphere again -; a sheen (cloth); a clearcoat.
// Beside them a 5 mm slab (Subsurface, transmission 0.8) with a point light 0.4 m behind it that casts no shadow and
// reaches only the slab: the light through a thin part. Key: one shadow-casting point light, front-left, 2.1 m from the
// row's centre. The sun is low and dim (4 degrees up, 100 lux above the atmosphere): it shows the sun's terms without
// competing with the key.
// Cameras: "front" (the key's side) and "back".
Scene shadingBall(const Request& rq)
{
    Scene s;
    s.name = "shading_ball";
    commonSky(s, 4.0f, 60.0f);
    s.sun.illuminance = 100.0f;
    s.windSpeed = 0.0f;
    Material ground;
    ground.name = "shading_ground";
    ground.baseColor = f3(0.5f, 0.5f, 0.5f);
    ground.roughness = 0.9f;
    {
        MeshBuilder b("shading_ground");
        b.material(addMaterial(s, ground));
        b.box({ -6.0f, -0.3f, -6.0f }, { 6.0f, 0.0f, 6.0f }, 0.5f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    const float3 skinTone = f3(0.80f, 0.56f, 0.45f);
    Material standard;
    standard.name = "ball_standard";
    standard.baseColor = skinTone;
    standard.roughness = 0.5f;
    Material skin;
    skin.name = "ball_subsurface";
    skin.cls = scene::MaterialClass::Subsurface;
    skin.baseColor = skinTone;
    skin.roughness = 0.45f;
    skin.specular = 0.35f;
    skin.transmission = 0.5f;
    Material oneLobe = standard;  // the Standard sphere's parameters in the Subsurface class
    oneLobe.name = "ball_subsurface_one_lobe";
    oneLobe.cls = scene::MaterialClass::Subsurface;
    oneLobe.subsurfaceLobeMix = 1.0f;
    oneLobe.subsurfaceLobeRoughness = { 1.0f, 1.0f };
    oneLobe.transmission = 0.0f;
    Material cloth;
    cloth.name = "ball_sheen";
    cloth.baseColor = f3(0.30f, 0.06f, 0.10f);
    cloth.roughness = 0.8f;
    cloth.sheenColor = f3(0.9f, 0.7f, 0.7f);
    cloth.sheenRoughness = 0.4f;
    Material coated;
    coated.name = "ball_clearcoat";
    coated.baseColor = f3(0.60f, 0.05f, 0.05f);
    coated.roughness = 0.5f;
    coated.clearcoat = 1.0f;
    coated.clearcoatRoughness = 0.05f;
    const float eye = 1.6f, radius = 0.25f, spacing = 0.7f;
    const Material* balls[5] = { &standard, &skin, &oneLobe, &cloth, &coated };
    for (int i = 0; i < 5; ++i)
    {
        MeshBuilder b(balls[i]->name);
        b.material(addMaterial(s, *balls[i]));
        b.sphere({ (i - 2) * spacing, eye, 0.0f }, radius, 96, 48);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    Material thin = skin;
    thin.name = "slab_subsurface";
    thin.transmission = 0.8f;
    const float3 slab{ 2.5f, eye, 0.0f };
    {
        MeshBuilder b("slab");
        b.material(addMaterial(s, thin));
        b.box(slab - f3(0.3f, 0.3f, 0.0025f), slab + f3(0.3f, 0.3f, 0.0025f));
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    scene::Light key;
    key.type = scene::LightType::Point;
    key.position = f3(-1.2f, 2.4f, 1.5f);
    key.intensity = 600.0f;  // about 135 lux at the row's centre
    key.range = 12.0f;
    key.castShadow = true;
    s.lights.push_back(key);
    scene::Light behind;
    behind.type = scene::LightType::Point;
    behind.position = slab - f3(0.0f, 0.0f, 0.4f);
    behind.intensity = 20.0f;  // 125 lux on the slab's far face
    behind.range = 1.0f;       // (the nearest sphere, 0.9 m away, takes under 2 lux; the ground none)
    behind.castShadow = false;
    s.lights.push_back(behind);
    s.cameras.push_back(camera("front", f3(0.55f, eye, 3.5f), f3(0.55f, eye, 0.0f), 6.0f, 45.0f));
    s.cameras.push_back(camera("back", f3(0.55f, eye, -3.5f), f3(0.55f, eye, 0.0f), 6.0f, 45.0f));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    (void)rq;
    return s;
}

scene::Scene generate(const Request& rq)
{
    Scene s;
    switch (rq.id)
    {
    case SceneId::CityBlock: s = cityBlock(rq); break;
    case SceneId::ForestThin: s = forest(rq, true); break;
    case SceneId::ForestCard: s = forest(rq, false); break;
    case SceneId::Waterside: s = waterside(rq); break;
    case SceneId::Interior: s = interior(rq); break;
    case SceneId::CityNight: s = cityNight(rq); break;
    case SceneId::RidgeSunset: s = ridgeSunset(rq); break;
    case SceneId::ForestCombat: s = forestCombat(rq); break;
    case SceneId::FurnaceRoom: s = furnaceRoom(rq, false); break;
    case SceneId::FurnaceRoomDay: s = furnaceRoom(rq, true); break;
    case SceneId::ShadingBall: s = shadingBall(rq); break;
    case SceneId::HairBall: s = hairBall(rq); break;
    default: fail("scenegen: unknown scene id %u", (uint32_t)rq.id);
    }
    DynamicContent content = dynamicContent(rq);
    addDynamicBodies(s, content);
    s.seed = rq.seed;
    scene::validate(s);
    return s;
}

scene::Scene generateWithContent(const Request& rq, DynamicContent& content)
{
    scene::Scene s = generate(rq);
    content = dynamicContent(rq);
    // generate() added the same bodies (deterministic): the dynamic instances are the last content.bodies.size() ones.
    uint32_t first = (uint32_t)s.instances.size() - (uint32_t)content.bodies.size();
    for (size_t i = 0; i < content.bodies.size(); ++i) content.bodies[i].instance = first + (uint32_t)i;
    return s;
}

float terrainHeight(SceneId id, float x, float z)
{
    auto inside = [&](float half) { return std::fabs(x) <= half && std::fabs(z) <= half; };
    switch (id)
    {
    case SceneId::CityBlock:
    case SceneId::CityNight: return inside(1000.0f) ? cityTerrain(x, z) : NAN;  // 0 over the city square (street plane)
    case SceneId::ForestThin:
    case SceneId::ForestCard:
    case SceneId::ForestCombat: return inside(1000.0f) ? rollingTerrain(x, z) : NAN;
    case SceneId::Waterside: return inside(600.0f) ? lakeFloor(x, z) : NAN;
    case SceneId::Interior: return inside(100.0f) ? -0.2f : NAN;
    case SceneId::RidgeSunset: return inside(10000.0f) ? ridgeTerrain(x, z) : NAN;
    case SceneId::FurnaceRoom:
    case SceneId::FurnaceRoomDay:
    case SceneId::ShadingBall:
    case SceneId::HairBall: return NAN;
    }
    fail("scenegen: unknown scene id %u", (uint32_t)id);
}

std::vector<SceneId> allScenes()
{
    return { SceneId::CityBlock, SceneId::ForestThin, SceneId::ForestCard, SceneId::Waterside, SceneId::Interior, SceneId::CityNight, SceneId::RidgeSunset, SceneId::ForestCombat };
}

std::vector<SceneId> diagnosticScenes() { return { SceneId::FurnaceRoom, SceneId::FurnaceRoomDay, SceneId::ShadingBall, SceneId::HairBall }; }

const char* sceneName(SceneId id)
{
    switch (id)
    {
    case SceneId::CityBlock: return "city_block";
    case SceneId::ForestThin: return "forest_thin";
    case SceneId::ForestCard: return "forest_card";
    case SceneId::Waterside: return "waterside";
    case SceneId::Interior: return "interior";
    case SceneId::CityNight: return "city_night";
    case SceneId::RidgeSunset: return "ridge_sunset";
    case SceneId::ForestCombat: return "forest_combat";
    case SceneId::FurnaceRoom: return "furnace_room";
    case SceneId::FurnaceRoomDay: return "furnace_room_day";
    case SceneId::ShadingBall: return "shading_ball";
    case SceneId::HairBall: return "hair_ball";
    }
    fail("scenegen: unknown scene id %u", (uint32_t)id);
}
} // namespace unx::scenegen
