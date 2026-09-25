// unx_gamebench: the game bench scenes (Content/RPP1/GameBench/GAMEBENCH_KO.md).
//   unx_gamebench --out <dir> [--preset standard|fluid|all] | [--name N --objects 1000 --fluid 0 --destruction-per-minute 12
//                 --fragments 16 --debris-share 0.25 --seed 1 --duration 60]
// Per bench: <dir>/gamebench_<name>.unxscene, gamebench_<name>_bodies.json (C bodies format 1: the native host gate renders
// the rigid subset: unx_gate_host_hostdynamic --scene <dir>/gamebench_<name>.unxscene --characters 0), gamebench_<name>.json
// (parameters, population, events, camera cases, gaps) and <dir>/gamebench_index.json (hashes of every output). CPU only.
#include "../src/GameBench.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace unx;

int main(int argc, char** argv)
{
    try
    {
        std::string out, preset;
        rpp::GameBenchRequest one;
        bool custom = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--out") out = next();
            else if (a == "--preset") preset = next();
            else if (a == "--name") { one.name = next(); custom = true; }
            else if (a == "--objects") { one.movingObjects = (uint32_t)std::stoul(next()); custom = true; }
            else if (a == "--fluid") { one.fluidParticles = (uint32_t)std::stoul(next()); custom = true; }
            else if (a == "--destruction-per-minute") { one.destructionPerMinute = std::stof(next()); custom = true; }
            else if (a == "--fragments") { one.fragmentsPerBreak = (uint32_t)std::stoul(next()); custom = true; }
            else if (a == "--debris-share") { one.debrisShare = std::stof(next()); custom = true; }
            else if (a == "--seed") { one.seed = std::stoull(next()); custom = true; }
            else if (a == "--duration") { one.durationSeconds = std::stof(next()); custom = true; }
            else fail("unknown argument %s", a.c_str());
        }
        if (out.empty() || (preset.empty() && !custom)) fail("usage: unx_gamebench --out <dir> (--preset standard|fluid|all | --name N --objects K ...)");
        std::vector<rpp::GameBenchRequest> list;
        auto variant = [](const char* name, uint32_t objects, uint32_t fluid) {
            rpp::GameBenchRequest r;
            r.name = name;
            r.movingObjects = objects;
            r.fluidParticles = fluid;
            return r;
        };
        if (preset == "standard" || preset == "all")
            for (auto [n, k] : { std::pair{ "fp_250", 250u }, std::pair{ "fp_500", 500u }, std::pair{ "fp_1000", 1000u } }) list.push_back(variant(n, k, 0));
        if (preset == "fluid" || preset == "all")  // user 2026-09-26: fluid size unknown - sweep up to the design capacity (250k)
            for (auto [n, f] : { std::pair{ "fp_1000_fluid32k", 32768u }, std::pair{ "fp_1000_fluid64k", 65536u }, std::pair{ "fp_1000_fluid128k", 131072u }, std::pair{ "fp_1000_fluid250k", 250000u } })
                list.push_back(variant(n, 1000, f));
        if (!preset.empty() && list.empty()) fail("unknown preset %s", preset.c_str());
        if (custom) list.push_back(one);
        std::filesystem::create_directories(out);
        std::string index = "{\n  \"tool\": \"unx_gamebench\",\n  \"benches\": [\n";
        for (size_t i = 0; i < list.size(); ++i)
        {
            const rpp::GameBenchOutput b = rpp::buildGameBench(list[i]);
            scene::validate(b.scene);
            const std::filesystem::path base = std::filesystem::path(out) / b.scene.name;
            scene::save(b.scene, base.string() + ".unxscene");
            writeTextFile(base.string() + "_bodies.json", b.bodiesJson);
            writeTextFile(base.string() + ".json", b.benchJson);
            index += format("    { \"name\": \"%s\", \"scene\": \"%s.unxscene\", \"contentHash\": \"%s\", \"bodies\": \"%s\", \"bench\": \"%s\", \"movingObjects\": %u, \"spawned\": %u, \"debrisCap\": %u, \"rigidReplayed\": %u, \"fluidParticles\": %u }%s\n",
                            list[i].name.c_str(), b.scene.name.c_str(), scene::contentHash(b.scene).c_str(), Sha256::hex(b.bodiesJson).c_str(), Sha256::hex(b.benchJson).c_str(),
                            list[i].movingObjects, b.spawned, b.debrisCap, b.rigidReplayed, list[i].fluidParticles, i + 1 < list.size() ? "," : "");
        }
        index += "  ]\n}\n";
        writeTextFile(std::filesystem::path(out) / "gamebench_index.json", index);
        std::fputs(index.c_str(), stdout);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "unx_gamebench: %s\n", e.what());
        return 1;
    }
}
