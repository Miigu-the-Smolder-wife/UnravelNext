// unx_scenegen: writes the procedural test scenes (INTERFACES_KO.md 10.1) as .unxscene files and prints their size.
//   unx_scenegen --list
//   unx_scenegen --scene <name|all> [--seed N] [--scale S] [--out <dir>] [--stats]
#include "unx/core/Log.h"
#include "unx/scenegen/SceneGen.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace unx;

namespace
{
scenegen::SceneId parseScene(const std::string& name)
{
    for (scenegen::SceneId id : scenegen::allScenes())
        if (name == scenegen::sceneName(id)) return id;
    fail("unknown scene '%s' (use --list)", name.c_str());
}

void stats(const scene::Scene& s)
{
    uint64_t uniqueTris = 0, instancedTris = 0, bytes = 0;
    for (const scene::Mesh& m : s.meshes) uniqueTris += m.indices.size() / 3;
    for (const scene::Instance& in : s.instances) instancedTris += s.meshes[in.mesh].indices.size() / 3;
    for (const scene::Texture& t : s.textures) bytes += t.texels.size();
    uint32_t shadowed = 0;
    for (const scene::Light& l : s.lights) shadowed += l.castShadow ? 1 : 0;
    logf("  %-12s meshes %zu (%.2f M unique tris), instances %zu (%.1f M instanced tris), materials %zu, textures %zu (%.1f MB), lights %zu (%u shadowed), cameras %zu, paths %zu\n",
         s.name.c_str(), s.meshes.size(), uniqueTris / 1e6, s.instances.size(), instancedTris / 1e6, s.materials.size(), s.textures.size(), bytes / 1048576.0,
         s.lights.size(), shadowed, s.cameras.size(), s.paths.size());
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string which, out;
        scenegen::Request rq;
        bool list = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--list") list = true;
            else if (a == "--scene") which = next();
            else if (a == "--seed") rq.seed = std::stoull(next());
            else if (a == "--scale") rq.scale = std::stof(next());
            else if (a == "--out") out = next();
            else if (a == "--stats") {}
            else fail("unknown argument %s", a.c_str());
        }
        if (list || which.empty())
        {
            for (scenegen::SceneId id : scenegen::allScenes()) std::printf("%s\n", scenegen::sceneName(id));
            return 0;
        }
        std::vector<scenegen::SceneId> ids;
        if (which == "all") ids = scenegen::allScenes();
        else ids.push_back(parseScene(which));
        for (scenegen::SceneId id : ids)
        {
            rq.id = id;
            const auto t0 = std::chrono::steady_clock::now();
            const scene::Scene s = scenegen::generate(rq);
            const double genSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            stats(s);
            const std::string hash = scene::contentHash(s);
            logf("  generated in %.2f s, contentHash %s (seed %llu, scale %g)\n", genSec, hash.c_str(), (unsigned long long)rq.seed, rq.scale);
            if (!out.empty())
            {
                const std::string path = out + "/" + scenegen::sceneName(id) + ".unxscene";
                scene::save(s, path);
                logf("  wrote %s\n", path.c_str());
            }
        }
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
