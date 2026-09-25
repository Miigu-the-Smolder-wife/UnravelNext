// unx_reference: reference images, cache, census and comparisons (INTERFACES_KO.md 10.2, 10.3). CPU only.
//
//   unx_reference render  --scene <name|file.unxscene> (--camera <name> | --path <name> --time <s>) --res <WxH>
//                         [--seed N] [--scale S] [--no-wind] [--sun-illuminance lux] [--write-scene <file>] [--spp N] [--force]
//                         [--volume-order MIN:MAX]
//       Renders into / reuses Cache/Reference/<scene>/<camera>_<W>x<H>_<spp>_<sceneHash16>_<qualityHash16>.pfm (+ .json,
//       + .halfA.pfm / .halfB.pfm). --spp overrides reference.samples_per_pixel (recorded in the name and the hash);
//       cached references (the ones gates read) use the configured value (>= 4096). Checkpoints every 10 min and resumes.
//       --threads N: logical processors to use (default 3/4, the highest-numbered; the rest stay free for other sessions).
//       --also-hold <file>: an extra pause file (the render queue passes Cache/Reference/PAUSE_QUEUE so ad-hoc renders can
//       pause it instead of running beside it).
//       --volume-order MIN:MAX (diagnostics, MAX may be inf) keeps only light with MIN..MAX atmosphere scattering events;
//       surface and ground bounces are not counted (pure single scattering needs black surfaces and ground).
//       --surface-order MIN:MAX the same for surface scattering events (1 = reflected once, at the visible surface);
//       --volume-order 0:0 --surface-order 1:1 is direct light only.
//   unx_reference census  --scene ... (--camera|--path/--time) --res <WxH> [--engine <capture.unxids>] [--out report.json]
//       16-sub-sample identity census; without --engine the 1-sample (pixel centre) + 3x3 baseline.
//   unx_reference compare --scene ... (--camera|--path/--time) --res <WxH> --test <engine.pfm> [--out report.json]
//       Metrics against the cached reference and the scene thresholds of Config/quality/reference.toml.
//   unx_reference selfcheck
//       Measured error of the atmosphere optical-depth table against direct quadrature.
// render and census pause while another session holds the GPU measurement lock (.gpulock/current.json with a live
// holder) or while the manual marker .gpulock/HOLD exists (e.g. the user plays a game); --no-hold disables.
// Scenes by name come from Tools/SceneGen (seed 1, scale 1 unless given). --no-wind sets the wind speed to 0 before
// hashing (a different scene identity); --write-scene saves the exact scene that was rendered for the engine to use.
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/core/Sha256.h"
#include "unx/metrics/Census.h"
#include "unx/metrics/Metrics.h"
#include "unx/reference/PathTracer.h"
#include "unx/scenegen/SceneGen.h"

#include "../src/Atmosphere.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <thread>

#include <windows.h>

using namespace unx;

namespace
{
// Bumped whenever the estimator changes in a way that changes images: part of the cache key's quality hash.
constexpr const char* kEstimatorVersion = "unx-reference-1";

struct Args
{
    std::string command, scene, camera, path, out, engine, test, writeScene;
    float time = 0, scale = 1;
    uint64_t seed = 1;
    uint32_t width = 0, height = 0, spp = 0;
    bool noWind = false, force = false;
    float sunIlluminance = -1;
    bool noHold = false;
    uint32_t orderMin = 0, orderMax = 0xFFFFFFFFu;  // --volume-order MIN:MAX (diagnostics)
    uint32_t surfMin = 0, surfMax = 0xFFFFFFFFu;    // --surface-order MIN:MAX (diagnostics)
    uint32_t threads = 0;                            // --threads N (0 = 3/4 of the logical processors)
    std::vector<std::string> alsoHold;               // --also-hold <file> (repeatable)
};

Args parse(int argc, char** argv)
{
    Args a;
    if (argc < 2) fail("usage: unx_reference <render|census|compare|selfcheck> ...");
    a.command = argv[1];
    for (int i = 2; i < argc; ++i)
    {
        const std::string k = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", k.c_str()); return argv[++i]; };
        if (k == "--scene") a.scene = next();
        else if (k == "--camera") a.camera = next();
        else if (k == "--path") a.path = next();
        else if (k == "--time") a.time = std::stof(next());
        else if (k == "--seed") a.seed = std::stoull(next());
        else if (k == "--scale") a.scale = std::stof(next());
        else if (k == "--res")
        {
            const std::string r = next();
            const size_t x = r.find('x');
            if (x == std::string::npos) fail("--res expects WxH");
            a.width = (uint32_t)std::stoul(r.substr(0, x));
            a.height = (uint32_t)std::stoul(r.substr(x + 1));
        }
        else if (k == "--spp") a.spp = (uint32_t)std::stoul(next());
        else if (k == "--no-wind") a.noWind = true;
        else if (k == "--no-hold") a.noHold = true;
        else if (k == "--sun-illuminance") a.sunIlluminance = std::stof(next());
        else if (k == "--force") a.force = true;
        else if (k == "--out") a.out = next();
        else if (k == "--engine") a.engine = next();
        else if (k == "--test") a.test = next();
        else if (k == "--write-scene") a.writeScene = next();
        else if (k == "--threads") a.threads = (uint32_t)std::stoul(next());
        else if (k == "--also-hold") a.alsoHold.push_back(next());
        else if (k == "--volume-order" || k == "--surface-order")
        {
            const std::string r = next();
            const size_t c = r.find(':');
            if (c == std::string::npos) fail("%s expects MIN:MAX (MAX may be 'inf')", k.c_str());
            const std::string mx = r.substr(c + 1);
            uint32_t& lo = k == "--volume-order" ? a.orderMin : a.surfMin;
            uint32_t& hi = k == "--volume-order" ? a.orderMax : a.surfMax;
            lo = (uint32_t)std::stoul(r.substr(0, c));
            hi = mx == "inf" ? 0xFFFFFFFFu : (uint32_t)std::stoul(mx);
        }
        else fail("unknown argument %s", k.c_str());
    }
    return a;
}

scene::Scene loadScene(const Args& a)
{
    if (a.scene.empty()) fail("--scene is required");
    scene::Scene s;
    if (a.scene.size() > 9 && a.scene.substr(a.scene.size() - 9) == ".unxscene") s = scene::load(a.scene);
    else
    {
        bool found = false;
        for (scenegen::SceneId id : scenegen::allScenes())
            if (a.scene == scenegen::sceneName(id))
            {
                s = scenegen::generate({ id, a.seed, a.scale });
                found = true;
            }
        if (!found) fail("unknown scene '%s'", a.scene.c_str());
    }
    if (a.noWind) s.windSpeed = 0;
    if (a.sunIlluminance >= 0) s.sun.illuminance = a.sunIlluminance;
    if (!a.writeScene.empty()) scene::save(s, a.writeScene);
    return s;
}

reference::ResolvedCamera pickCamera(const scene::Scene& s, const Args& a, std::string& label)
{
    reference::CameraSelection sel;
    sel.camera = a.camera;
    sel.path = a.path;
    sel.time = a.time;
    if (a.camera.empty() && a.path.empty()) fail("--camera or --path is required");
    label = a.camera.empty() ? format("%s@%g", a.path.c_str(), a.time) : a.camera;
    return reference::resolveCamera(s, sel);
}

std::filesystem::path root() { return std::filesystem::path(UNX_SOURCE_DIR); }

// Files that pause render/census workers (see RenderSettings::pauseWhileExists).
std::vector<std::filesystem::path> holdFiles(const Args& a)
{
    std::vector<std::filesystem::path> f;
    if (!a.noHold) f = { root() / ".gpulock" / "current.json", root() / ".gpulock" / "HOLD" };
    for (const std::string& h : a.alsoHold) f.push_back(h);
    return f;
}

// The machine is shared with other sessions' builds and correctness tests (they run without the GPU lock): a render
// takes at most 3/4 of the logical processors (--threads overrides), the highest-numbered ones, leaving the lowest
// (P-cores on this machine) to the others. Applied as the process affinity, so the job pool, Embree's BVH builder and
// the atmosphere table all stay inside it.
void limitProcessors(const Args& a)
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw < 2 || hw > 64) return;
    const unsigned n = std::clamp(a.threads ? a.threads : hw * 3 / 4, 1u, hw);
    if (n == hw) return;
    const unsigned long long all = hw == 64 ? ~0ull : ((1ull << hw) - 1), low = (1ull << (hw - n)) - 1;
    if (SetProcessAffinityMask(GetCurrentProcess(), (DWORD_PTR)(all & ~low))) logf("reference: using %u of %u logical processors (%u..%u)\n", n, hw, hw - n, hw - 1);
}

struct ReferenceKeys
{
    uint32_t spp = 0, rrStart = 0;
    std::string hash16;
};
ReferenceKeys referenceKeys(const QualityConfig& q, uint32_t sppOverride, bool sunCaustics, uint32_t orderMin = 0, uint32_t orderMax = 0xFFFFFFFFu,
                            uint32_t surfMin = 0, uint32_t surfMax = 0xFFFFFFFFu)
{
    ReferenceKeys k;
    k.spp = sppOverride ? sppOverride : (uint32_t)q.integer("reference.samples_per_pixel");
    k.rrStart = (uint32_t)q.integer("reference.russian_roulette_start_bounce");
    // Only what changes the image: estimator version, sample count, Russian-roulette start.
    std::string canonical = format("%s\nreference.samples_per_pixel = %u\nreference.russian_roulette_start_bounce = %u\n", kEstimatorVersion, k.spp, k.rrStart);
    // A diagnostic scattering-order window changes the image, so it is part of the key (absent for full references).
    // Sun caustics by light tracing (2026-09-25): changes the images of scenes with sun-caustic surfaces only; the others
    // keep their key (their images are bitwise unchanged).
    if (sunCaustics) canonical += "estimator.sun_caustics = light_traced\n";
    if (orderMin != 0 || orderMax != 0xFFFFFFFFu) canonical += format("diagnostic.volume_order = %u:%u\n", orderMin, orderMax);
    if (surfMin != 0 || surfMax != 0xFFFFFFFFu) canonical += format("diagnostic.surface_order = %u:%u\n", surfMin, surfMax);
    k.hash16 = Sha256::hex(canonical).substr(0, 16);
    return k;
}

std::filesystem::path cachePath(const scene::Scene& s, const std::string& label, uint32_t w, uint32_t h, const ReferenceKeys& k)
{
    std::string safe = label;
    for (char& c : safe)
        if (c == '/' || c == '\\' || c == ':' || c == ' ') c = '_';
    return root() / "Cache" / "Reference" / s.name / format("%s_%ux%u_%u_%s_%s.pfm", safe.c_str(), w, h, k.spp, scene::contentHash(s).substr(0, 16).c_str(), k.hash16.c_str());
}

std::string nowIso()
{
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

std::filesystem::path renderCached(const Args& a, const scene::Scene& s, const std::string& label, const reference::ResolvedCamera& cam, const QualityConfig& q)
{
    const ReferenceKeys k = referenceKeys(q, a.spp, reference::hasSunCausticSurfaces(s), a.orderMin, a.orderMax, a.surfMin, a.surfMax);
    const std::filesystem::path pfm = cachePath(s, label, a.width, a.height, k);
    if (std::filesystem::exists(pfm) && !a.force)
    {
        logf("reference: cached %s\n", pfm.string().c_str());
        return pfm;
    }
    logf("reference: rendering %s camera %s %ux%u at %u spp (quality %s) -> %s\n", s.name.c_str(), label.c_str(), a.width, a.height, k.spp, k.hash16.c_str(), pfm.string().c_str());
    reference::waitWhileHeld(holdFiles(a));
    const auto t0 = std::chrono::steady_clock::now();
    reference::PathTracer pt(s);
    const double buildSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    reference::RenderSettings rs;
    rs.width = a.width;
    rs.height = a.height;
    rs.samplesPerPixel = k.spp;
    rs.russianRouletteStart = k.rrStart;
    rs.samplesPerPass = std::max(1u, std::min(32u, k.spp / 64));
    rs.checkpoint = pfm.string() + ".checkpoint";
    rs.pauseWhileExists = holdFiles(a);
    rs.volumeOrderMin = a.orderMin;
    rs.volumeOrderMax = a.orderMax;
    rs.surfaceOrderMin = a.surfMin;
    rs.surfaceOrderMax = a.surfMax;
    if (a.surfMin != 0 || a.surfMax != 0xFFFFFFFFu) logf("reference: diagnostic surface scattering order window %u:%u\n", a.surfMin, a.surfMax);
    if (a.orderMin != 0 || a.orderMax != 0xFFFFFFFFu) logf("reference: diagnostic volume scattering order window %u:%u\n", a.orderMin, a.orderMax);
    const reference::RenderOutput out = pt.render(cam, rs, [&](const reference::RenderStats& st) {
        const double rate = st.seconds > 0 ? st.rays / st.seconds / 1e6 : 0;
        const double eta = st.samplesDone ? st.seconds * (k.spp - st.samplesDone) / st.samplesDone : 0;
        logf("  %u/%u spp  %.0f s (paused %.0f s)  %.1f Mrays/s  ETA %.0f s\n", st.samplesDone, k.spp, st.seconds, st.pausedSeconds, rate, eta);
    });
    metrics::writePfm(pfm, out.image);
    const std::string stem = pfm.string().substr(0, pfm.string().size() - 4);
    metrics::writePfm(stem + ".halfA.pfm", out.halfA);
    metrics::writePfm(stem + ".halfB.pfm", out.halfB);
    const std::string json = format(
        "{\n  \"scene\": \"%s\",\n  \"scene_hash\": \"%s\",\n  \"camera\": \"%s\",\n  \"width\": %u,\n  \"height\": %u,\n  \"spp\": %u,\n"
        "  \"russian_roulette_start_bounce\": %u,\n  \"estimator\": \"%s\",\n  \"quality_hash16\": \"%s\",\n  \"ev100\": %.9g,\n  \"time\": %.9g,\n"
        "  \"halves_relmse\": %.9g,\n  \"render_seconds\": %.1f,\n  \"build_seconds\": %.1f,\n  \"paths\": %llu,\n  \"rays\": %llu,\n  \"truncated_paths\": %llu,\n"
        "  \"nan_samples\": %llu,\n  \"threads\": %u,\n  \"finished\": \"%s\"\n}\n",
        s.name.c_str(), scene::contentHash(s).c_str(), label.c_str(), a.width, a.height, k.spp, k.rrStart, kEstimatorVersion, k.hash16.c_str(), cam.ev100, cam.time,
        out.halvesRelMse, out.stats.seconds, buildSec, (unsigned long long)out.stats.paths, (unsigned long long)out.stats.rays, (unsigned long long)out.stats.truncatedPaths,
        (unsigned long long)out.stats.nanSamples, std::thread::hardware_concurrency(), nowIso().c_str());
    writeTextFile(stem + ".json", json);
    logf("reference: done in %.0f s, halves relMSE %.3g, %llu NaN samples, %llu truncated paths\n", out.stats.seconds, out.halvesRelMse,
         (unsigned long long)out.stats.nanSamples, (unsigned long long)out.stats.truncatedPaths);
    return pfm;
}

std::string censusJson(const char* kind, const metrics::CensusResult& r)
{
    return format("{\"kind\": \"%s\", \"pixels\": %llu, \"distinct_ge2\": %.6f, \"distinct_ge3\": %.6f, \"distinct_ge5\": %.6f, \"distinct_ge9\": %.6f, "
                  "\"missed_ge1\": %.6f, \"missed_ge4\": %.6f, \"missed_ge8\": %.6f}\n",
                  kind, (unsigned long long)r.pixels, r.distinct2, r.distinct3, r.distinct5, r.distinct9, r.missed1, r.missed4, r.missed8);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const Args a = parse(argc, argv);
        limitProcessors(a);
        const QualityConfig q = QualityConfig::loadDirectory(root() / "Config" / "quality");
        if (a.command == "selfcheck")
        {
            scene::Atmosphere atm;
            const auto t0 = std::chrono::steady_clock::now();
            reference::AtmosphereModel m(atm);
            const double build = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            double relT = 0;
            const double absTau = m.selfCheck(20000, &relT);
            logf("atmosphere table: built in %.2f s; max |tau error| %.3g, max relative transmittance error %.3g (T > 1e-4), 20000 samples\n", build, absTau, relT);
            return 0;
        }
        if (a.width == 0 || a.height == 0) fail("--res is required");
        const scene::Scene s = loadScene(a);
        std::string label;
        const reference::ResolvedCamera cam = pickCamera(s, a, label);
        if (a.command == "render")
        {
            renderCached(a, s, label, cam, q);
            return 0;
        }
        if (a.command == "census")
        {
            reference::waitWhileHeld(holdFiles(a));
            const auto t0 = std::chrono::steady_clock::now();
            reference::PathTracer pt(s);
            const std::vector<uint64_t> ids = pt.primaryIdentities(cam, a.width, a.height, holdFiles(a));
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::string json;
            if (a.engine.empty())
            {
                const metrics::CensusResult r = metrics::selfCensus(ids, a.width, a.height);
                logf("census (1-sample centre + 3x3) %s/%s %ux%u: >=2 ids %.2f%%, >=3 %.2f%%, >=5 %.2f%%, >=9 %.2f%% | missed >=1 %.2f%%, >=4 %.2f%%, >=8 %.2f%% (%.1f s)\n",
                     s.name.c_str(), label.c_str(), a.width, a.height, 100 * r.distinct2, 100 * r.distinct3, 100 * r.distinct5, 100 * r.distinct9, 100 * r.missed1,
                     100 * r.missed4, 100 * r.missed8, sec);
                json = censusJson("self_1spp_3x3", r);
            }
            else
            {
                const metrics::IdentityImage e = metrics::readIdentities(a.engine);
                if (e.width != a.width || e.height != a.height) fail("census: engine capture is %ux%u", e.width, e.height);
                const metrics::CensusResult r = metrics::census(ids, 17, e);
                logf("census (engine) %s/%s: >=2 ids %.2f%% | missed >=1 %.3f%%, >=4 %.3f%%, >=8 %.3f%%\n", s.name.c_str(), label.c_str(), 100 * r.distinct2, 100 * r.missed1,
                     100 * r.missed4, 100 * r.missed8);
                json = censusJson("engine", r);
            }
            if (!a.out.empty()) writeTextFile(a.out, json);
            return 0;
        }
        if (a.command == "compare")
        {
            if (a.test.empty()) fail("--test is required");
            const std::filesystem::path ref = renderCached(a, s, label, cam, q);
            const metrics::Image r = metrics::readPfm(ref), t = metrics::readPfm(a.test);
            const double rel = metrics::relMse(r, t);
            const metrics::FlipResult f = metrics::flipHdr(r, t);
            const std::string p = "reference.threshold." + s.name + ".";
            const double tMean = q.number(p + "flip_mean"), tP99 = q.number(p + "flip_p99"), tRel = q.number(p + "relmse");
            const bool pass = f.mean <= tMean && f.p99 <= tP99 && rel <= tRel;
            logf("compare %s/%s: HDR-FLIP mean %.4f (<= %.4f), p99 %.4f (<= %.4f), relMSE %.4g (<= %.4g) -> %s\n", s.name.c_str(), label.c_str(), f.mean, tMean, f.p99, tP99, rel,
                 tRel, pass ? "PASS" : "FAIL");
            if (!a.out.empty())
                writeTextFile(a.out, format("{\"scene\": \"%s\", \"camera\": \"%s\", \"reference\": \"%s\", \"flip_mean\": %.6f, \"flip_p99\": %.6f, \"relmse\": %.6g, "
                                            "\"threshold_flip_mean\": %.6f, \"threshold_flip_p99\": %.6f, \"threshold_relmse\": %.6g, \"pass\": %s}\n",
                                            s.name.c_str(), label.c_str(), ref.filename().string().c_str(), f.mean, f.p99, rel, tMean, tP99, tRel, pass ? "true" : "false"));
            return pass ? 0 : 1;
        }
        fail("unknown command '%s'", a.command.c_str());
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
