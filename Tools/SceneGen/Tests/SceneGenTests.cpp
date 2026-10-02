// Scene generator self-checks (INTERFACES_KO.md 10.1):
//   determinism  two generate() calls with the same request give the same contentHash; a different seed differs
//   validity     generate() already runs scene::validate(); every scene has a camera, a static and a moving path
//   gate content CityNight at scale 1 has exactly 512 lights, 128 shadowed; ForestThin at scale 1 has 100k trees and
//                1M grass clumps with 40k leaves / 100 blades each (checked on a reduced scale for the counts' formula)
//   round trip   serialize/deserialize keeps the hash
//   diagnostic   the diagnostic scenes are deterministic and valid; shading_ball holds what SceneGen.h states
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
        // shading_ball's content (SceneGen.h): the five spheres' materials, the 5 mm slab, the two lights and cameras.
        for (scenegen::SceneId id : scenegen::diagnosticScenes())
        {
            const scenegen::Request rq{ id, 7, 1.0f };
            const scene::Scene a = scenegen::generate(rq);
            const std::string ha = scene::contentHash(a);
            CHECK(scene::contentHash(scenegen::generate(rq)) == ha);
            CHECK(scene::contentHash(scene::deserialize(scene::serialize(a))) == ha);
            CHECK(a.name == scenegen::sceneName(id));
            CHECK(!a.cameras.empty() && a.paths.size() == a.cameras.size());
            if (id == scenegen::SceneId::ShadingBall)
            {
                CHECK(a.cameras.size() == 2 && a.cameras[0].name == "front" && a.cameras[1].name == "back");
                CHECK(a.lights.size() == 2 && a.lights[0].castShadow && !a.lights[1].castShadow);
                CHECK(a.instances.size() == 7);
                uint32_t subsurface = 0, oneLobe = 0, sheen = 0, coat = 0;
                for (const scene::Material& m : a.materials)
                {
                    const bool skin = m.cls == scene::MaterialClass::Subsurface;
                    subsurface += skin;
                    oneLobe += skin && m.subsurfaceLobeMix == 1.0f && m.subsurfaceLobeRoughness.x == 1.0f && m.subsurfaceLobeRoughness.y == 1.0f && m.transmission == 0.0f;
                    sheen += m.sheenColor.x > 0;
                    coat += m.clearcoat > 0;
                }
                CHECK(subsurface == 3 && oneLobe == 1 && sheen == 1 && coat == 1);
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
