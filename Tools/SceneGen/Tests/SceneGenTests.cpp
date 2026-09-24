// Scene generator self-checks (INTERFACES_KO.md 10.1):
//   determinism  two generate() calls with the same request give the same contentHash; a different seed differs
//   validity     generate() already runs scene::validate(); every scene has a camera, a static and a moving path
//   gate content CityNight at scale 1 has exactly 512 lights, 128 shadowed; ForestThin at scale 1 has 100k trees and
//                1M grass clumps with 40k leaves / 100 blades each (checked on a reduced scale for the counts' formula)
//   round trip   serialize/deserialize keeps the hash
#include "unx/core/Log.h"
#include "unx/scenegen/SceneGen.h"

#include <cstdio>
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
            const bool forest = id == scenegen::SceneId::ForestThin || id == scenegen::SceneId::ForestCard;
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
                CHECK(trees == (size_t)std::lround(100000 * rq.scale) && clumps == (size_t)std::lround(1000000 * rq.scale));
                const bool thin = id == scenegen::SceneId::ForestThin;
                for (const scene::Mesh& m : a.meshes)
                {
                    if (m.name.rfind("tree_", 0) == 0) CHECK(m.submeshes.back().indexCount / 3 == (thin ? 80000u : 3000u));
                    if (m.name.rfind("grass_", 0) == 0) CHECK(m.indices.size() / 3 == (thin ? 200u : 16u));
                }
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
