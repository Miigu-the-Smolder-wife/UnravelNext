// unx_rppbuild: builds the RPP-1 content from the manifest's rules (Content/RPP1/rpp1_manifest.json, RPP1_MANIFEST_KO.md).
//   unx_rppbuild --out <dir> [--seed N]
// Writes <dir>/rpp1_world.unxscene, <dir>/rpp1_<section>_bodies.json (C bodies format 1, world coordinates) and
// <dir>/rpp1_build.json (counts, identities, asset bytes). CPU only; no GPU. Large outputs go to Cache/RPP1 (not committed);
// the report is copied to Results/RPP by the caller.
#include "../src/Path.h"
#include "../src/World.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>

using namespace unx;

namespace
{
std::string sha256Hex(const std::string& text) { return Sha256::hex(text); }

std::string report(const rpp::World& w, const std::string& sceneHash, const std::string (&bodiesHash)[rpp::SectionCount], double seconds)
{
    const scene::Scene& s = w.scene;
    const rpp::WorldStats& st = w.stats;
    // Asset bytes (design 2.13b VRAM table): geometry = unique meshes at the GpuScene v1 layout (vertex 32 B, index 4 B,
    // skin 16 B/vertex); textures = mip 0 bytes x 4/3 (full chain) as stored (C's palette is uncompressed).
    uint64_t geometry = 0, textures = 0, uniqueTris = 0;
    for (const scene::Mesh& m : s.meshes)
    {
        geometry += m.positions.size() * 32 + m.indices.size() * 4 + (m.skin.joints.empty() ? 0 : m.positions.size() * 16);
        uniqueTris += m.indices.size() / 3;
    }
    for (const scene::Texture& t : s.textures) textures += t.texels.size() * 4 / 3;
    std::map<std::string, uint32_t> species;
    for (const scene::Instance& in : s.instances)
    {
        const std::string& n = s.meshes[in.mesh].name;
        if (n.rfind("tree_", 0) == 0 || n.rfind("grass_", 0) == 0 || n == "reeds") ++species[n];
    }
    std::ostringstream o;
    o << "{\n  \"tool\": \"unx_rppbuild\",\n  \"manifest\": \"Content/RPP1/rpp1_manifest.json 1.0-draft.1\",\n";
    o << format("  \"seconds\": %.1f,\n", seconds);
    o << "  \"scene\": { \"name\": \"" << s.name << "\", \"contentHash\": \"" << sceneHash << "\"" << format(", \"instances\": %zu, \"meshes\": %zu, \"materials\": %zu, \"textures\": %zu, \"lights\": %zu, \"cameras\": %zu",
                                                                                                    s.instances.size(), s.meshes.size(), s.materials.size(), s.textures.size(), s.lights.size(), s.cameras.size())
      << format(", \"uniqueTriangles\": %llu, \"instancedTriangles\": %llu },\n", (unsigned long long)uniqueTris, (unsigned long long)st.triangles);
    o << "  \"sources\": {";
    for (uint32_t k = 0; k < rpp::SectionCount; ++k)
        o << (k ? ", " : " ") << "\"" << scenegen::sceneName(w.sections[k].source) << "\": \"" << w.sourceHashes[k] << "\"";
    o << " },\n";
    o << format("  \"vegetation\": { \"trees\": %u, \"stand\": %u, \"cityStreet\": %u, \"lakeBanks\": %u, \"base\": %u, \"fill\": %u, \"grass\": %u, \"reeds\": %u, \"grassBase\": %u, \"grassFill\": %u },\n",
                st.trees, st.treesStand, st.treesCity, st.treesLake, st.treesBase, st.treesFill, st.grass, st.grassReeds, st.grassBase, st.grassFill);
    o << "  \"species\": {";
    bool first = true;
    for (const auto& [n, c] : species) { o << (first ? " " : ", ") << "\"" << n << "\": " << c; first = false; }
    o << " },\n";
    o << format("  \"lights\": { \"total\": %u, \"shadowed\": %u", st.lights, st.lightsShadowed);
    for (uint32_t k = 0; k < rpp::SectionCount; ++k) o << format(", \"%s\": [%u, %u]", rpp::sectionId((rpp::Section)k), st.lightsBySection[k], st.shadowedBySection[k]);
    o << " },\n";
    o << format("  \"dynamicBodies\": %u, \"bodiesPerSection\": 1024, \"charactersPerSection\": 256,\n", st.dynamicBodies);
    o << "  \"bodiesFiles\": {";
    for (uint32_t k = 0; k < rpp::SectionCount; ++k) o << (k ? ", " : " ") << "\"" << rpp::sectionId((rpp::Section)k) << "\": \"" << bodiesHash[k] << "\"";
    o << " },\n";
    o << format("  \"assetBytes\": { \"geometry\": %llu, \"textures\": %llu, \"note\": \"geometry at GpuScene v1 layout before clusters; textures uncompressed x 4/3; characters, grooms and authored textures are not in the scene yet\" }\n}\n",
                (unsigned long long)geometry, (unsigned long long)textures);
    return o.str();
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string out;
        uint64_t seed = 1;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--out") out = next();
            else if (a == "--seed") seed = std::stoull(next());
            else fail("unknown argument %s", a.c_str());
        }
        if (out.empty()) fail("usage: unx_rppbuild --out <dir> [--seed N]");
        std::filesystem::create_directories(out);
        const auto t0 = std::chrono::steady_clock::now();
        rpp::Layout layout;
        layout.city.translation = { -450.0f, 0.0f, -420.0f };
        layout.lake.translation = { 200.0f, -3.0f, 420.0f };
        rpp::World world = rpp::buildWorld(layout, seed);
        scene::validate(world.scene);
        const std::filesystem::path scenePath = std::filesystem::path(out) / "rpp1_world.unxscene";
        scene::save(world.scene, scenePath);
        const std::string sceneHash = scene::contentHash(world.scene);
        std::string bodiesHash[rpp::SectionCount];
        for (uint32_t k = 0; k < rpp::SectionCount; ++k)
        {
            const std::string text = rpp::sectionBodiesJson(world, (rpp::Section)k);
            writeTextFile(std::filesystem::path(out) / (std::string("rpp1_") + rpp::sectionId((rpp::Section)k) + "_bodies.json"), text);
            bodiesHash[k] = sha256Hex(text);
        }
        rpp::PathReport pathReport;
        const std::vector<rpp::CameraSample> camera = rpp::buildCameraTrack(world, pathReport);
        const std::string path = rpp::pathJson(world, camera, pathReport);
        writeTextFile(std::filesystem::path(out) / "rpp1_path.json", path);
        const std::string pathHash = sha256Hex(path);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::string rep = report(world, sceneHash, bodiesHash, seconds);
        rep.insert(rep.rfind("\n}"), format(",\n  \"path\": { \"file\": \"rpp1_path.json\", \"sha256\": \"%s\", \"minTrunkClearance\": %.3f, \"minCrownClearance\": %.3f, \"minStaticClearance\": %.3f, \"worstStatic\": \"%s\", \"worstStaticTick\": %u, \"maxSpeed\": %.2f }",
                                             pathHash.c_str(), pathReport.minTrunkClearance, pathReport.minCrownClearance, pathReport.minStaticClearance, pathReport.worstStatic.c_str(),
                                             pathReport.worstStaticTick, pathReport.maxSpeed));
        writeTextFile(std::filesystem::path(out) / "rpp1_build.json", rep);
        std::fputs(rep.c_str(), stdout);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "unx_rppbuild: %s\n", e.what());
        return 1;
    }
}
