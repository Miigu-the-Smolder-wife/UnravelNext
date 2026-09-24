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
float lakeFloor(float x, float z)
{
    const float r = std::sqrt(x * x + z * z);
    const float lake = 1.0f - smoothstepf(120.0f, 190.0f, r);
    const float bay = (1.0f - smoothstepf(90.0f, 140.0f, std::fabs(z))) * smoothstepf(60.0f, 120.0f, x) * (1.0f - smoothstepf(430.0f, 470.0f, x));
    const float basin = std::max(lake, bay);
    return lerpf(0.5f * rollingTerrain(x, z) + 3.0f, -4.0f, basin);
}

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
} // namespace

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
    default: fail("scenegen: unknown scene id %u", (uint32_t)rq.id);
    }
    s.seed = rq.seed;
    scene::validate(s);
    return s;
}

std::vector<SceneId> allScenes()
{
    return { SceneId::CityBlock, SceneId::ForestThin, SceneId::ForestCard, SceneId::Waterside, SceneId::Interior, SceneId::CityNight };
}

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
    }
    fail("scenegen: unknown scene id %u", (uint32_t)id);
}
} // namespace unx::scenegen
