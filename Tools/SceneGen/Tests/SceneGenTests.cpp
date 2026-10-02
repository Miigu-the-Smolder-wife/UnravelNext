// Scene generator self-checks (INTERFACES_KO.md 10.1):
//   determinism  two generate() calls with the same request give the same contentHash; a different seed differs
//   validity     generate() already runs scene::validate(); every scene has a camera, a static and a moving path
//   gate content CityNight at scale 1 has exactly 512 lights, 128 shadowed; ForestThin at scale 1 has 100k trees and
//                1M grass clumps with 40k leaves / 100 blades each (checked on a reduced scale for the counts' formula)
//   round trip   serialize/deserialize keeps the hash
//   diagnostic   the diagnostic scenes are deterministic and valid; shading_ball holds what SceneGen.h states
//   showcase     the showcase scenes hold the content their cameras are for (panes in frames, shadow-only and
//                no-self-shadow instances, lighting channels, steam, the cloud layer over the crest) and their extras
//                (light functions, decals, rain) name lights, materials and cameras of their scene
#include "unx/core/Log.h"
#include "unx/scenegen/SceneGen.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>

using namespace unx;

#define CHECK(c) do { if (!(c)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #c); } while (0)

int main(int argc, char** argv)
{
    try
    {
        const bool full = argc > 1 && std::strcmp(argv[1], "--full") == 0;  // scale-1 forests (1.1 M instances)
        for (scenegen::SceneId id : scenegen::allScenes())
        {
            const bool combat = id == scenegen::SceneId::ForestCombat;
            const bool forest = id == scenegen::SceneId::ForestThin || id == scenegen::SceneId::ForestCard || combat;
            scenegen::Request rq{ id, 7, forest && !full ? 0.01f : 1.0f };
            const scene::Scene a = scenegen::generate(rq);
            const scene::Scene b = scenegen::generate(rq);
            const std::string ha = scene::contentHash(a), hb = scene::contentHash(b);
            CHECK(ha == hb);
            CHECK(scene::contentHash(scene::deserialize(scene::serialize(a))) == ha);
            rq.seed = 8;
            CHECK(scene::contentHash(scenegen::generate(rq)) != ha);
            CHECK(!a.cameras.empty());
            bool staticPath = false, movingPath = false;
            for (const scene::CameraPath& p : a.paths)
            {
                CHECK(p.keys.size() >= 2);
                const bool moves = length(p.keys.front().position - p.keys.back().position) > 1e-3f;
                (moves ? movingPath : staticPath) = true;
            }
            CHECK(staticPath && movingPath);
            CHECK(a.name == scenegen::sceneName(id));
            // terrainHeight: every vertex of the terrain (or interior ground) mesh lies on it.
            {
                size_t checked = 0;
                for (const scene::Mesh& m : a.meshes)
                {
                    if (m.name != "terrain" && m.name != "ground") continue;
                    for (const float3& v : m.positions)
                    {
                        const float h = scenegen::terrainHeight(id, v.x, v.z);
                        CHECK(std::fabs(v.y - h) <= 1e-3f);
                        ++checked;
                    }
                }
                CHECK(checked > 0);
            }
            // RPP-1 dynamic content: 1,024 InstanceDynamic bodies in the section scenes, none elsewhere; the exported content
            // maps each body to its instance with the same t0 position.
            {
                size_t dynamicCount = 0;
                for (const scene::Instance& in : a.instances) dynamicCount += (in.flags & scene::InstanceDynamic) != 0;
                const bool section = id == scenegen::SceneId::CityBlock || id == scenegen::SceneId::CityNight || combat || id == scenegen::SceneId::Waterside ||
                                     id == scenegen::SceneId::Interior;
                CHECK(dynamicCount == (section ? 1024u : 0u));
                if (section)
                {
                    scenegen::DynamicContent content;
                    scenegen::Request r0 = rq;
                    r0.seed = 7;  // rq.seed was changed above
                    const scene::Scene c = scenegen::generateWithContent(r0, content);
                    CHECK(scene::contentHash(c) == ha && content.bodies.size() == 1024 && content.characters.size() == 256);
                    size_t heroes = 0;
                    for (const auto& ch : content.characters) heroes += ch.hero;
                    CHECK(heroes == 8);
                    for (const scenegen::DynamicBody& body : content.bodies)
                    {
                        const scene::Instance& in = c.instances[body.instance];
                        CHECK((in.flags & scene::InstanceDynamic) != 0);
                        const float3 t{ in.transform.m[0][3], in.transform.m[1][3], in.transform.m[2][3] };
                        CHECK(length(t - body.position) < 1e-3f);
                    }
                }
            }
            if (id == scenegen::SceneId::CityNight)
            {
                uint32_t shadowed = 0;
                for (const scene::Light& l : a.lights) shadowed += l.castShadow;
                CHECK(a.lights.size() == 512);
                CHECK(shadowed == 128);
            }
            if (forest)
            {
                size_t trees = 0, clumps = 0;
                for (const scene::Instance& in : a.instances)
                {
                    const std::string& n = a.meshes[in.mesh].name;
                    trees += n.rfind("tree_", 0) == 0;
                    clumps += n.rfind("grass_", 0) == 0;
                }
                // forest_combat adds its closed-canopy stand (a fixed ~3000 trees, not scaled) to the forest_thin base.
                CHECK((combat ? trees > (size_t)std::lround(100000 * rq.scale) + 2500 : trees == (size_t)std::lround(100000 * rq.scale)) &&
                      clumps == (size_t)std::lround(1000000 * rq.scale));
                const bool thin = id == scenegen::SceneId::ForestThin || combat;
                for (const scene::Mesh& m : a.meshes)
                {
                    if (m.name.rfind("tree_", 0) == 0) CHECK(m.submeshes.back().indexCount / 3 == (thin ? 80000u : 3000u));
                    if (m.name.rfind("grass_", 0) == 0) CHECK(m.indices.size() / 3 == (thin ? 200u : 16u));
                }
            }
            logf("%-12s ok  %s\n", a.name.c_str(), ha.substr(0, 16).c_str());
        }
        // Diagnostic scenes (diagnosticScenes: not in the sweep above): deterministic, valid, named, a static path per camera;
        // shading_ball's content (SceneGen.h): the five spheres' materials, the 5 mm slab, the cloth sphere and the two eyes
        // above them, the two lights and the cameras.
        for (scenegen::SceneId id : scenegen::diagnosticScenes())
        {
            const scenegen::Request rq{ id, 7, 1.0f };
            const scene::Scene a = scenegen::generate(rq);
            const std::string ha = scene::contentHash(a);
            CHECK(scene::contentHash(scenegen::generate(rq)) == ha);
            CHECK(scene::contentHash(scene::deserialize(scene::serialize(a))) == ha);
            CHECK(a.name == scenegen::sceneName(id));
            CHECK(!a.cameras.empty() && a.paths.size() == a.cameras.size());
            if (id == scenegen::SceneId::HairBall)
            {
                // hair_ball's content (SceneGen.h, HairBall.cpp): two heads, a dark and a blond Hair material, the key and the
                // rim light, two cameras; its two grooms name those materials and stand on the heads
                CHECK(a.cameras.size() == 2 && a.cameras[0].name == "front" && a.cameras[1].name == "back");
                CHECK(a.lights.size() == 2 && a.lights[0].castShadow && a.lights[1].castShadow);
                CHECK(a.instances.size() == 3);
                const std::vector<scenegen::Groom> grooms = scenegen::grooms(rq);
                CHECK(grooms.size() == 2);
                for (const scenegen::Groom& g : grooms)
                {
                    CHECK(g.material < a.materials.size() && a.materials[g.material].cls == scene::MaterialClass::Hair);
                    CHECK(g.nodesPerStrand == 12 && g.restPositions.size() == 2500u * 12 && g.follows.size() == 2500u * 16);
                    for (uint32_t k = 0; k < 2500; ++k) CHECK(std::fabs(length(g.restPositions[k * 12]) - 0.1f) < 1e-5f && length(g.restPositions[k * 12]) > g.headRadius);
                }
                if (grooms.size() == 2)
                {
                    CHECK(a.materials[grooms[0].material].hairEumelanin > a.materials[grooms[1].material].hairEumelanin);
                    CHECK(grooms[0].head.x < grooms[1].head.x);
                    // (the same request, the same strands)
                    const std::vector<scenegen::Groom> again = scenegen::grooms(rq);
                    CHECK(again.size() == 2 && std::memcmp(again[1].restPositions.data(), grooms[1].restPositions.data(), grooms[1].restPositions.size() * sizeof(float3)) == 0);
                }
            }
            else if (id == scenegen::SceneId::ShowcaseBathhouse)
            {
                // showcase_bathhouse's figure: one groom of a Hair material, its roots outside the head
                const std::vector<scenegen::Groom> grooms = scenegen::grooms(rq);
                CHECK(grooms.size() == 1);
                for (const scenegen::Groom& g : grooms)
                {
                    CHECK(g.material < a.materials.size() && a.materials[g.material].cls == scene::MaterialClass::Hair);
                    CHECK(g.nodesPerStrand == 12 && g.restPositions.size() == 2500u * 12 && g.follows.size() == 2500u * 16);
                    for (uint32_t k = 0; k < 2500; ++k) CHECK(length(g.restPositions[k * 12]) > g.headRadius);
                    for (const scenegen::Groom::Follow& f : g.follows) CHECK(f.guide < 2500u);
                }
            }
            else CHECK(scenegen::grooms(rq).empty());
            // The scenes' extras (SceneGen.h extras): the showcase scenes have them, the others none; each names a light, a
            // material or a camera of its scene, and the same request gives the same extras.
            {
                const scenegen::SceneExtras e = scenegen::extras(rq);
                bool showcase = false;
                for (scenegen::SceneId k : scenegen::showcaseScenes()) showcase = showcase || k == id;
                CHECK(showcase == !(e.lightFunctions.empty() && e.decals.empty() && e.weather.empty()));
                for (const scenegen::ExtraLightFunction& f : e.lightFunctions)
                {
                    CHECK(f.light < a.lights.size() && f.profile >= 1 && f.profile <= 3);
                    if (f.profile == 1) CHECK(f.iesVertical.size() >= 2 && f.iesValues.size() == f.iesVertical.size());
                    else CHECK(f.imageWidth > 0 && f.imageRgb.size() == (size_t)f.imageWidth * f.imageHeight * 3);
                    // (a cookie belongs to a spot, a gobo to a point light)
                    if (f.light < a.lights.size() && f.profile == 2) CHECK(a.lights[f.light].type == scene::LightType::Spot);
                    if (f.light < a.lights.size() && f.profile == 3) CHECK(a.lights[f.light].type == scene::LightType::Point);
                }
                for (const scenegen::ExtraDecal& d : e.decals) CHECK(d.material < a.materials.size() && d.channels >= 1 && d.channels <= 7);
                for (const scenegen::ExtraWeather& w : e.weather)
                {
                    bool found = false;
                    for (const scene::Camera& c : a.cameras) found = found || c.name == w.camera;
                    CHECK(found);
                }
                const scenegen::SceneExtras again = scenegen::extras(rq);
                CHECK(again.lightFunctions.size() == e.lightFunctions.size() && again.decals.size() == e.decals.size() && again.weather.size() == e.weather.size());
            }
            if (id == scenegen::SceneId::ShowcaseBathhouse || id == scenegen::SceneId::ShowcaseAtrium || id == scenegen::SceneId::ShowcaseShore)
            {
                // what the showcase scenes are for: a mesh that holds an opaque and a Glass submesh (a pane in its frame),
                // and per scene the content its cameras show
                uint32_t mixed = 0, shadowOnly = 0, noSelfShadow = 0, channelled = 0, noDecals = 0, glassAlone = 0;
                auto materialOf = [&](const scene::Instance& in, size_t sm) {
                    return sm < in.materialOverrides.size() && in.materialOverrides[sm] != scene::kNone ? in.materialOverrides[sm] : a.meshes[in.mesh].submeshes[sm].material;
                };
                for (const scene::Instance& in : a.instances)
                {
                    uint32_t glass = 0, opaque = 0;
                    for (size_t sm = 0; sm < a.meshes[in.mesh].submeshes.size(); ++sm)
                        (a.materials[materialOf(in, sm)].cls == scene::MaterialClass::Glass ? glass : opaque) += 1;
                    mixed += glass > 0 && opaque > 0;
                    glassAlone += glass > 0 && opaque == 0;
                    shadowOnly += (in.flags & scene::InstanceShadowOnly) != 0 && (in.flags & scene::InstanceCastShadow) != 0;
                    noSelfShadow += (in.flags & scene::InstanceNoSelfShadow) != 0;
                    channelled += scene::instanceLightingChannels(in.flags) != 1;
                    noDecals += (in.flags & scene::InstanceNoDecals) != 0;
                }
                CHECK(mixed >= 1);
                const scenegen::SceneExtras e = scenegen::extras(rq);
                if (id == scenegen::SceneId::ShowcaseBathhouse)
                {
                    CHECK(a.cameras.size() == 7 && a.cameras[0].name == "room" && a.cameras[3].name == "figure" && a.cameras[6].name == "bench");
                    // the two windows; the stained panes and the lamps' shades; the shutter; the fern; the figure's four parts
                    CHECK(mixed == 2 && glassAlone == 5 && shadowOnly == 1 && noSelfShadow == 1 && channelled == 4 && noDecals == 4);
                    uint32_t channelLights = 0, imageLights = 0, barnDoors = 0, steam = 0, eyes = 0, cloth = 0, water = 0, heights = 0, coloured = 0;
                    for (const scene::Light& l : a.lights)
                    {
                        channelLights += l.lightingChannels != 1;
                        imageLights += l.type == scene::LightType::Rect && l.sourceTexture != scene::kNone;
                        barnDoors += l.barnDoorLength > 0;
                    }
                    for (const scene::FogVolume& v : a.fogVolumes) steam += v.riseSpeed > 0 && v.turbulence > 0;
                    for (const scene::Material& m : a.materials)
                    {
                        eyes += m.eyeIrisRadius > 0;
                        cloth += m.cloth > 0;
                        water += m.cls == scene::MaterialClass::Water;
                        heights += m.heightTexture != scene::kNone && m.heightScale > 0;
                    }
                    for (const scene::Mesh& m : a.meshes) coloured += !m.colors.empty();
                    CHECK(channelLights == 1 && imageLights == 1 && barnDoors == 1 && steam == 1 && eyes == 1 && cloth == 2 && water == 1 && heights == 3 && coloured == 1);
                    CHECK(e.lightFunctions.size() == 3 && e.decals.size() == 3 && e.weather.empty());
                }
                if (id == scenegen::SceneId::ShowcaseAtrium)
                {
                    CHECK(a.cameras.size() == 6 && a.cameras[0].name == "floor" && a.cameras[1].name == "roof");
                    // the roof: one mesh of the steel grid and the four pane colours
                    uint32_t roofs = 0;
                    for (const scene::Mesh& m : a.meshes) roofs += m.name == "atrium_roof" && m.submeshes.size() == 5;
                    CHECK(roofs == 1 && mixed == 1 && glassAlone == 10);
                    CHECK(e.lightFunctions.empty() && e.decals.size() == 3 && e.weather.empty());
                }
                if (id == scenegen::SceneId::ShowcaseShore)
                {
                    CHECK(a.cameras.size() == 5 && a.cameras[0].name == "shore" && a.cameras[1].name == "rain");
                    CHECK(a.clouds.coverage > 0 && a.clouds.cirrusCoverage > 0 && a.fog.enabled && a.fogVolumes.size() == 1);
                    CHECK(a.instances.size() > 150000);  // (scale 1: 40,000 trees, 150,000 grass clumps)
                    // the crest stands in the cloud layer, the lake's bank at the water's level
                    CHECK(scenegen::terrainHeight(id, 0.0f, -2600.0f) > a.clouds.baseAltitude && scenegen::terrainHeight(id, 0.0f, -2600.0f) < a.clouds.topAltitude);
                    CHECK(scenegen::terrainHeight(id, 0.0f, 0.0f) == -5.0f && std::fabs(scenegen::terrainHeight(id, 118.0f, 0.0f)) < 1e-4f);
                    CHECK(std::isnan(scenegen::terrainHeight(id, 0.0f, 500.0f)));
                    CHECK(e.lightFunctions.size() == 1 && e.decals.empty() && e.weather.size() == 1 && e.weather[0].camera == "rain");
                }
            }
            if (id == scenegen::SceneId::ShadingBall)
            {
                CHECK(a.cameras.size() == 5 && a.cameras[0].name == "front" && a.cameras[1].name == "back" && a.cameras[2].name == "skin_close" &&
                      a.cameras[3].name == "eye_close" && a.cameras[4].name == "inputs");
                CHECK(a.lights.size() == 2 && a.lights[0].castShadow && !a.lights[1].castShadow);
                CHECK(a.instances.size() == 14);
                // the material inputs' plates: one material each for the uv transform, the detail maps, the height and the
                // vertex colour with the emissive mask; the last one's mesh carries colours
                uint32_t tiledPlates = 0, detailPlates = 0, heightPlates = 0, vertexPlates = 0, colouredMeshes = 0;
                for (const scene::Material& m : a.materials)
                {
                    tiledPlates += m.uvScale.x == 3.0f && m.uvRotation != 0.0f && m.baseColorTexture != scene::kNone;
                    detailPlates += m.detailColorTexture != scene::kNone && m.detailNormalTexture != scene::kNone && m.detailScale.x == 6.0f;
                    heightPlates += m.heightTexture != scene::kNone && m.heightScale == 0.03f;
                    vertexPlates += m.vertexColorTint && m.emissiveMaskTexture != scene::kNone && m.emissiveScale == 0.1f;
                }
                for (const scene::Mesh& m : a.meshes) colouredMeshes += m.colors.size() == m.positions.size() && !m.colors.empty();
                CHECK(tiledPlates == 1 && detailPlates == 1 && heightPlates == 1 && vertexPlates == 1 && colouredMeshes == 1);
                uint32_t subsurface = 0, oneLobe = 0, sheen = 0, coat = 0, clothBlend = 0, eyes = 0;
                for (const scene::Material& m : a.materials)
                {
                    const bool skin = m.cls == scene::MaterialClass::Subsurface && m.eyeIrisRadius == 0.0f;
                    subsurface += skin;
                    oneLobe += skin && m.subsurfaceLobeMix == 1.0f && m.subsurfaceLobeRoughness.x == 1.0f && m.subsurfaceLobeRoughness.y == 1.0f && m.transmission == 0.0f;
                    sheen += m.sheenColor.x > 0;
                    coat += m.clearcoat > 0;
                    clothBlend += m.sheenColor.x > 0 && m.cloth == 1.0f;
                    eyes += m.cls == scene::MaterialClass::Subsurface && m.eyeIrisRadius == 0.245f && m.baseColorTexture != scene::kNone && m.transmission == 0.0f;
                }
                CHECK(subsurface == 3 && oneLobe == 1 && sheen == 2 && coat == 1 && clothBlend == 1 && eyes == 1);
                // the two eyes: one mesh whose uv centre is on its +Z axis, each instance's +Z towards the front camera
                uint32_t eyeInstances = 0;
                for (const scene::Instance& in : a.instances)
                {
                    const scene::Mesh& m = a.meshes[in.mesh];
                    if (m.name != "eye") continue;
                    ++eyeInstances;
                    const float3 centre{ in.transform.m[0][3], in.transform.m[1][3], in.transform.m[2][3] };
                    const float3 axis = normalize(in.transform.transformVector({ 0, 0, 1 }));
                    CHECK(length(axis - normalize(a.cameras[0].position - centre)) < 1e-5f);
                    for (size_t v = 0; v < m.positions.size(); ++v)
                        CHECK(std::fabs(m.uv0[v].x - (0.5f + 0.5f * m.positions[v].x)) < 1e-6f && std::fabs(m.uv0[v].y - (0.5f + 0.5f * m.positions[v].y)) < 1e-6f);
                }
                CHECK(eyeInstances == 2);
                bool slab = false;
                for (const scene::Mesh& m : a.meshes)
                {
                    if (m.name != "slab") continue;
                    float lo = 1e9f, hi = -1e9f;
                    for (const float3& v : m.positions) lo = std::fmin(lo, v.z), hi = std::fmax(hi, v.z);
                    slab = std::fabs((hi - lo) - 0.005f) < 1e-6f;
                    CHECK(a.materials[m.submeshes[0].material].cls == scene::MaterialClass::Subsurface && a.materials[m.submeshes[0].material].transmission == 0.8f);
                }
                CHECK(slab);
            }
            logf("%-12s ok  %s\n", a.name.c_str(), ha.substr(0, 16).c_str());
        }
        logf("scenegen tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAILED: %s\n", e.what());
        return 1;
    }
}
