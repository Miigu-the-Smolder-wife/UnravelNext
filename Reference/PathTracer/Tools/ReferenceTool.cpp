// unx_reference: reference images, cache, census and comparisons (INTERFACES_KO.md 10.2, 10.3). CPU only.
//
//   unx_reference render  --scene <name|file.unxscene> (--camera <name> | --path <name> --time <s>) --res <WxH>
//                         [--seed N] [--scale S] [--no-wind] [--sun-illuminance lux] [--write-scene <file>] [--spp N] [--force]
//                         [--device cpu|gpu] [--render-seed N]
//                         [--volume-order MIN:MAX]
//       Renders into / reuses Cache/Reference/<scene>/<camera>_<W>x<H>_<spp>_<sceneHash16>_<qualityHash16>.pfm (+ .json,
//       + .halfA.pfm / .halfB.pfm). --spp overrides reference.samples_per_pixel (recorded in the name and the hash);
//       cached references (the ones gates read) use the configured value (>= 4096). Checkpoints every 10 min and resumes.
//       --threads N: logical processors to use (default 3/4, the highest-numbered; the rest stay free for other sessions).
//       While Cache/Reference/LIGHT exists (the user is gaming but work may continue lightly): 8 processors, below normal.
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
//   unx_reference scenemeta --scene ... --res <WxH> [--wcap px (0)] [--out meta.md]
//       Per camera: surface pixels, triangles per visibility band (frustum, before occlusion), light counts.
//   unx_reference gatecompare --scene ... --camera ... --res WxH --cpu-image <pfm> --gpu-image <pfm> [--out md]
//       CPU/GPU validation gate: tile z-scores, per-mask bias, relMSE against both noises (halves beside the images).
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
#include "unx/reference/GpuPathTracer.h"
#include "unx/reference/PathTracer.h"
#include "unx/scenegen/SceneGen.h"

#include "../src/Atmosphere.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <sstream>
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
    float wcap = 0.0f;                               // scenemeta --wcap: face-on sheets below this width are band B (design default 0)
    std::vector<std::string> alsoHold;               // --also-hold <file> (repeatable)
    std::string cpuImage, gpuImage;                  // gatecompare --cpu-image / --gpu-image (full images; halves beside them)
    bool stripDynamic = false;                       // --strip-dynamic: drop the RPP-1 dynamic bodies (the scene before 2026-09-26)
    bool gpu = false;                                // --device gpu: the GPU reference path tracer (Reference/GpuTracer)
    uint64_t renderSeed = 0;                         // --render-seed N (0 = the device default)
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
        else if (k == "--wcap") a.wcap = std::stof(next());
        else if (k == "--also-hold") a.alsoHold.push_back(next());
        else if (k == "--device")
        {
            const std::string d = next();
            if (d != "cpu" && d != "gpu") fail("--device expects cpu or gpu");
            a.gpu = d == "gpu";
        }
        else if (k == "--render-seed") a.renderSeed = std::stoull(next());
        else if (k == "--strip-dynamic") a.stripDynamic = true;
        else if (k == "--cpu-image") a.cpuImage = next();
        else if (k == "--gpu-image") a.gpuImage = next();
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
    if (a.stripDynamic)
    {
        // The generator appends the RPP-1 bodies last (3 materials, the body meshes, the InstanceDynamic instances):
        // removing them gives the scene of the references rendered before they existed (validation against those).
        while (!s.instances.empty() && (s.instances.back().flags & scene::InstanceDynamic)) s.instances.pop_back();
        while (!s.meshes.empty() && s.meshes.back().name.rfind("body_", 0) == 0) s.meshes.pop_back();
        while (!s.materials.empty() && s.materials.back().name.rfind("body_", 0) == 0) s.materials.pop_back();
        for (const scene::Instance& in : s.instances)
            if (in.flags & scene::InstanceDynamic) fail("--strip-dynamic: dynamic instances are not the last ones");
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
    // Light mode (Cache/Reference/LIGHT exists, e.g. while the user plays a game): 8 processors, below-normal priority
    // (not idle: idle-priority threads can be starved while holding shared resources).
    const bool light = std::filesystem::exists(root() / "Cache" / "Reference" / "LIGHT");
    if (light) SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    const unsigned n = std::clamp(a.threads ? a.threads : light ? 8u : hw * 3 / 4, 1u, hw);
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
                            uint32_t surfMin = 0, uint32_t surfMax = 0xFFFFFFFFu, bool gpu = false, uint64_t renderSeed = 0)
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
    // GPU tracer images (2026-09-26): their own identity until the CPU/GPU validation gate has passed
    // (Reference/GpuTracer/README_KO.md); a non-default render seed too (independent estimates for comparisons).
    if (gpu) canonical += "estimator.device = gpu\n";
    if (renderSeed) canonical += format("estimator.render_seed = %llu\n", (unsigned long long)renderSeed);
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
    const ReferenceKeys k = referenceKeys(q, a.spp, reference::hasSunCausticSurfaces(s), a.orderMin, a.orderMax, a.surfMin, a.surfMax, a.gpu, a.renderSeed);
    const std::filesystem::path pfm = cachePath(s, label, a.width, a.height, k);
    if (std::filesystem::exists(pfm) && !a.force)
    {
        logf("reference: cached %s\n", pfm.string().c_str());
        return pfm;
    }
    logf("reference: rendering %s camera %s %ux%u at %u spp (quality %s, %s) -> %s\n", s.name.c_str(), label.c_str(), a.width, a.height, k.spp, k.hash16.c_str(),
         a.gpu ? "gpu" : "cpu", pfm.string().c_str());
    if (!a.gpu) reference::waitWhileHeld(holdFiles(a));
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_ptr<reference::PathTracer> pt;
    std::unique_ptr<reference::GpuPathTracer> gpt;
    if (a.gpu) gpt = std::make_unique<reference::GpuPathTracer>(s, root(), format("unx_reference gpu %s/%s %ux%u %u spp", s.name.c_str(), label.c_str(), a.width, a.height, k.spp));
    else pt = std::make_unique<reference::PathTracer>(s);
    const double buildSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    reference::RenderSettings rs;
    rs.seed = a.renderSeed ? a.renderSeed : a.gpu ? 0x6E5EEDull : 0x5EEDull;
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
    auto progress = [&](const reference::RenderStats& st) {
        const double rate = st.seconds > 0 ? st.rays / st.seconds / 1e6 : 0;
        const double eta = st.samplesDone ? st.seconds * (k.spp - st.samplesDone) / st.samplesDone : 0;
        logf("  %u/%u spp  %.0f s (paused %.0f s)  %.1f Mrays/s  ETA %.0f s\n", st.samplesDone, k.spp, st.seconds, st.pausedSeconds, rate, eta);
    };
    const reference::RenderOutput out = a.gpu ? gpt->render(cam, rs, progress) : pt->render(cam, rs, progress);
    std::string gpuJson;
    if (a.gpu)
    {
        const reference::GpuRenderInfo& g = gpt->info();
        gpuJson = format(",\n  \"device\": \"gpu\",\n  \"render_seed\": %llu,\n  \"adapter\": \"%s\",\n  \"driver\": \"%s\",\n  \"vram_scene_mb\": %.1f,\n"
                         "  \"vram_process_peak_mb\": %.1f,\n  \"gpu_build_seconds\": %.1f,\n  \"lock_wait_seconds\": %.1f,\n  \"slices\": %u,\n"
                         "  \"longest_slice_seconds\": %.2f,\n  \"dispatches\": %u,\n  \"longest_dispatch_ms\": %.2f,\n  \"mean_dispatch_ms\": %.2f",
                         (unsigned long long)rs.seed, g.adapter.c_str(), g.driver.c_str(), g.vramSceneBytes / 1048576.0, g.vramProcessPeakBytes / 1048576.0, g.buildSeconds,
                         g.lockWaitSeconds, g.slices, g.longestSliceSeconds, g.dispatches, g.longestDispatchMs, g.meanDispatchMs);
        logf("reference: gpu %s, scene %.0f MB, process peak %.0f MB, %u slices (longest %.1f s), %u dispatches (longest %.1f ms, mean %.1f ms), lock wait %.0f s\n",
             g.adapter.c_str(), g.vramSceneBytes / 1048576.0, g.vramProcessPeakBytes / 1048576.0, g.slices, g.longestSliceSeconds, g.dispatches, g.longestDispatchMs,
             g.meanDispatchMs, g.lockWaitSeconds);
    }
    metrics::writePfm(pfm, out.image);
    const std::string stem = pfm.string().substr(0, pfm.string().size() - 4);
    metrics::writePfm(stem + ".halfA.pfm", out.halfA);
    metrics::writePfm(stem + ".halfB.pfm", out.halfB);
    const std::string json = format(
        "{\n  \"scene\": \"%s\",\n  \"scene_hash\": \"%s\",\n  \"camera\": \"%s\",\n  \"width\": %u,\n  \"height\": %u,\n  \"spp\": %u,\n"
        "  \"russian_roulette_start_bounce\": %u,\n  \"estimator\": \"%s\",\n  \"quality_hash16\": \"%s\",\n  \"ev100\": %.9g,\n  \"time\": %.9g,\n"
        "  \"halves_relmse\": %.9g,\n  \"render_seconds\": %.1f,\n  \"build_seconds\": %.1f,\n  \"paths\": %llu,\n  \"rays\": %llu,\n  \"truncated_paths\": %llu,\n"
        "  \"nan_samples\": %llu,\n  \"threads\": %u,\n  \"finished\": \"%s\"%s\n}\n",
        s.name.c_str(), scene::contentHash(s).c_str(), label.c_str(), a.width, a.height, k.spp, k.rrStart, kEstimatorVersion, k.hash16.c_str(), cam.ev100, cam.time,
        out.halvesRelMse, out.stats.seconds, buildSec, (unsigned long long)out.stats.paths, (unsigned long long)out.stats.rays, (unsigned long long)out.stats.truncatedPaths,
        (unsigned long long)out.stats.nanSamples, std::thread::hardware_concurrency(), nowIso().c_str(), gpuJson.c_str());
    writeTextFile(stem + ".json", json);
    logf("reference: done in %.0f s, halves relMSE %.3g, %llu NaN samples, %llu truncated paths\n", out.stats.seconds, out.halvesRelMse,
         (unsigned long long)out.stats.nanSamples, (unsigned long long)out.stats.truncatedPaths);
    return pfm;
}

// Scene metadata for gate scenes (per camera): surface pixels (pixel-centre primary ray hits geometry), triangles per
// visibility band by projected minimum feature width (ARCHITECTURE 2.1 band rule: w_px = the triangle's smallest
// altitude x instance scale x focal / distance of the instance centre; flat features (Foliage class or two-sided
// materials) are band B up to 8 px, solid ones up to 1.5 px; below 0.25 px band C), counted over instances whose
// bounding sphere meets the view frustum (before occlusion), and light counts.
std::string sceneMeta(const scene::Scene& s, reference::PathTracer& pt, uint32_t W, uint32_t H, const std::vector<std::filesystem::path>& hold, float wcap)
{
    // Per mesh: object-space bounding sphere and a histogram of triangle minimum altitudes (log bins) split flat / solid.
    constexpr int kBins = 96;
    auto binOf = [](double w) { return std::clamp((int)std::floor((std::log2(std::max(w, 1e-6)) + 20.0) * 4.0), 0, kBins - 1); };
    auto binCentre = [](int b) { return std::exp2((b + 0.5) / 4.0 - 20.0); };
    struct MeshInfo
    {
        float3 centre;
        double radius = 0;
        std::array<double, kBins> flat{}, solid{};
        uint64_t tris = 0;
        std::vector<float> alt;        // per triangle: smallest altitude (object space)
        std::vector<uint8_t> isFlat;   // per triangle
        std::vector<float3> nrm, ctr;  // per triangle: unit normal and centroid (object space)
    };
    std::vector<MeshInfo> mi(s.meshes.size());
    for (size_t m = 0; m < s.meshes.size(); ++m)
    {
        const scene::Mesh& mesh = s.meshes[m];
        float3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
        for (const float3& q : mesh.positions)
        {
            lo = { std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z) };
            hi = { std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z) };
        }
        mi[m].centre = (lo + hi) * 0.5f;
        mi[m].radius = 0.5 * std::sqrt((double)dot(hi - lo, hi - lo));
        mi[m].alt.assign(mesh.indices.size() / 3, 0.0f);
        mi[m].isFlat.assign(mesh.indices.size() / 3, 0);
        mi[m].nrm.assign(mesh.indices.size() / 3, float3{ 0, 0, 1 });
        mi[m].ctr.assign(mesh.indices.size() / 3, float3{ 0, 0, 0 });
        for (const scene::Submesh& sub : mesh.submeshes)
        {
            const scene::Material& mat = s.materials[sub.material];
            const bool flat = mat.cls == scene::MaterialClass::Foliage || mat.twoSided;
            for (uint32_t k = sub.indexOffset; k + 2 < sub.indexOffset + sub.indexCount; k += 3)
            {
                const float3 a = mesh.positions[mesh.indices[k]], b = mesh.positions[mesh.indices[k + 1]], c = mesh.positions[mesh.indices[k + 2]];
                const float3 cr = cross(b - a, c - a);
                const double area2 = std::sqrt((double)dot(cr, cr));
                const double lmax = std::sqrt(std::max({ (double)dot(b - a, b - a), (double)dot(c - b, c - b), (double)dot(a - c, a - c) }));
                const double alt = lmax > 0 ? area2 / lmax : 0;  // smallest altitude = 2 area / longest edge
                (flat ? mi[m].flat : mi[m].solid)[binOf(alt)] += 1;
                mi[m].alt[k / 3] = (float)alt;
                mi[m].isFlat[k / 3] = flat ? 1 : 0;
                if (area2 > 0) mi[m].nrm[k / 3] = cr * (float)(1.0 / area2);
                mi[m].ctr[k / 3] = (a + b + c) * (1.0f / 3.0f);
                ++mi[m].tris;
            }
        }
    }
    std::ostringstream md;
    md << format("## %s at %ux%u, w_cap %.1f px\n\n", s.name.c_str(), W, H, wcap);
    md << format("Lights: sun %s, local lights %zu (%zu casting shadows).\n\n", s.sun.illuminance > 0 ? "on" : "off", s.lights.size(),
                 (size_t)std::count_if(s.lights.begin(), s.lights.end(), [](const scene::Light& l) { return l.castShadow; }));
    {
        const size_t dynamic = (size_t)std::count_if(s.instances.begin(), s.instances.end(), [](const scene::Instance& in) { return (in.flags & scene::InstanceDynamic) != 0; });
        md << format("Dynamic content (RPP-1): %zu rigid bodies (InstanceDynamic, placement from the generator = bodies.json). "
                     "References show them frozen at bodies.json t0 (the frozen-body variant); a moving-body variant is a separate "
                     "per-frame sequence. Character slots (256, 8 with hair) and VFX are empty in these references: nothing is "
                     "rendered for them.\n\n",
                     dynamic);
    }
    md << "Band of a visible triangle (COVERAGE_REDESIGN 14.9): w_px = smallest altitude x instance scale x focal / distance to the "
          "triangle centroid; C below 0.25 px; solid B below 1.5 px; flat (Foliage or two-sided) B only when |cos theta| x w_px < 1.5 px "
          "(theta between the view ray and the sheet normal) or w_px < w_cap, else A. Visible = hit by one of 16 stratified sub-samples per pixel (no "
          "wind). P_A / P_B / P_C = pixels whose sub-samples include a triangle of that band (ARCHITECTURE 2 table: P_B, P_C); T_A / T_B "
          "/ T_C = distinct visible triangles per band (before any LOD).\n\n"
          "| camera | surface px (centre ray) | surface % | P_A / P_B / P_C (M px) | P_B or P_C | T_A / T_B / T_C (visible) | instances in frustum | dynamic bodies in frustum |\n"
          "|---|---|---|---|---|---|---|---|\n";
    // Band B by distance (design revision request 2026-09-26: stored-layer model of COVERAGE_REDESIGN 14.9 per distance).
    static const double kDistEdges[4] = { 62, 94, 187, 374 };
    std::ostringstream dist;
    dist << "\nBand B by distance (camera to the triangle centroid, the distance of the band rule). P_B: pixels by their nearest band-B "
            "sub-sample; T_B: distinct visible band-B triangles.\n\n"
            "| camera | P_B < 62 / 62-94 / 94-187 / 187-374 / >= 374 m (M px) | T_B < 62 / 62-94 / 94-187 / 187-374 / >= 374 m (M) |\n|---|---|---|\n";
    for (const scene::Camera& c : s.cameras)
    {
        reference::CameraSelection sel;
        sel.camera = c.name;
        const reference::ResolvedCamera cam = reference::resolveCamera(s, sel);
        const std::vector<uint64_t> ids = pt.primaryIdentities(cam, W, H, hold);
        uint64_t surf = 0;
        for (size_t i = 0; i < (size_t)W * H; ++i)
            if (ids[i * 17 + 16] != reference::kSkyIdentity) ++surf;
        const float3 fw = normalize(cam.forward), right = normalize(cross(fw, cam.up)), up = cross(right, fw);
        const double th = std::tan(0.5 * cam.verticalFov), aspect = (double)W / H, focal = 0.5 * H / th;
        // Visible triangles by band (distinct ids over the 16 sub-samples) and band pixel coverage.
        // Band of a visible triangle (COVERAGE_REDESIGN 14.9 rule): w_px = smallest altitude x scale x focal / distance to
        // the triangle centroid; C below 0.25 px; solid B below 1.5 px; flat (sheet) B only when |cos theta| x w_px < 1.5 px
        // (theta between the view ray and the sheet normal: a sheet seen face-on is band A whatever its size).
        auto bandOfId = [&](uint64_t id) {
            const uint32_t instIdx = (uint32_t)(id >> 32), tri = (uint32_t)id;
            const scene::Instance& in = s.instances[instIdx];
            const MeshInfo& m = mi[in.mesh];
            if (tri >= m.alt.size()) return 0;
            const float3 col0{ in.transform.m[0][0], in.transform.m[1][0], in.transform.m[2][0] };
            const double scale = std::sqrt((double)dot(col0, col0));
            const float3 v = in.transform.transformPoint(m.ctr[tri]) - cam.position;
            const double d = std::max((double)std::sqrt(dot(v, v)), (double)cam.nearPlane);
            const double w = m.alt[tri] * scale * focal / d;
            if (w < 0.25) return 2;
            if (!m.isFlat[tri]) return w < 1.5 ? 1 : 0;
            if (w < wcap) return 1;  // face-on sheets narrower than w_cap: band B by cost (design request 16)
            const float3 n = normalize(in.transform.transformVector(m.nrm[tri]));
            const double cosT = std::fabs((double)dot(n, v)) / d;
            return cosT * w < 1.5 ? 1 : 0;
        };
        auto distOfId = [&](uint64_t id) {
            const scene::Instance& in = s.instances[(uint32_t)(id >> 32)];
            const MeshInfo& m = mi[in.mesh];
            const uint32_t tri = (uint32_t)id;
            if (tri >= m.ctr.size()) return 0.0;
            const float3 v = in.transform.transformPoint(m.ctr[tri]) - cam.position;
            return (double)std::sqrt(dot(v, v));
        };
        auto distBin = [&](double d) {
            int b = 0;
            while (b < 4 && d >= kDistEdges[b]) ++b;
            return b;
        };
        uint64_t pxBand[3] = {}, pxBC = 0, pxBDist[5] = {}, visBDist[5] = {};
        std::vector<uint64_t> seen;
        seen.reserve((size_t)W * H);
        for (size_t i = 0; i < (size_t)W * H; ++i)
        {
            bool has[3] = {};
            double nearestB = -1;
            for (int k = 0; k < 16; ++k)
            {
                const uint64_t id = ids[i * 17 + k];
                if (id == reference::kSkyIdentity) continue;
                const int band = bandOfId(id);
                has[band] = true;
                if (band == 1)
                {
                    const double d = distOfId(id);
                    if (nearestB < 0 || d < nearestB) nearestB = d;
                }
                seen.push_back(id);
            }
            for (int b = 0; b < 3; ++b) pxBand[b] += has[b];
            pxBC += has[1] || has[2];
            if (nearestB >= 0) ++pxBDist[distBin(nearestB)];
        }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        uint64_t visBand[3] = {};
        for (uint64_t id : seen)
        {
            const int band = bandOfId(id);
            ++visBand[band];
            if (band == 1) ++visBDist[distBin(distOfId(id))];
        }
        dist << format("| %s | %.2f / %.2f / %.2f / %.2f / %.2f | %.2f / %.2f / %.2f / %.2f / %.2f |\n", c.name.c_str(), pxBDist[0] / 1e6, pxBDist[1] / 1e6, pxBDist[2] / 1e6,
                       pxBDist[3] / 1e6, pxBDist[4] / 1e6, visBDist[0] / 1e6, visBDist[1] / 1e6, visBDist[2] / 1e6, visBDist[3] / 1e6, visBDist[4] / 1e6);
        // Frustum side-plane normals (inward) for the sphere test.
        const double tx = th * aspect;
        const float3 nL = normalize(right * 1.0f + fw * (float)tx), nR = normalize(right * -1.0f + fw * (float)tx);
        const float3 nB = normalize(up * 1.0f + fw * (float)th), nT = normalize(up * -1.0f + fw * (float)th);
        double bandA = 0, bandB = 0, bandBflat = 0, bandC = 0;
        uint64_t inst = 0, dyn = 0;
        for (const scene::Instance& in : s.instances)
        {
            const MeshInfo& m = mi[in.mesh];
            const float3 col0{ in.transform.m[0][0], in.transform.m[1][0], in.transform.m[2][0] };
            const double scale = std::sqrt((double)dot(col0, col0));
            const float3 wc = in.transform.transformPoint(m.centre);
            const double r = m.radius * scale;
            const float3 v = wc - cam.position;
            if (dot(v, fw) < -r) continue;
            if (dot(v, nL) < -r || dot(v, nR) < -r || dot(v, nB) < -r || dot(v, nT) < -r) continue;
            ++inst;
            dyn += (in.flags & scene::InstanceDynamic) ? 1 : 0;
            const double d = std::max((double)std::sqrt(dot(v, v)), (double)cam.nearPlane);
            for (int b = 0; b < kBins; ++b)
            {
                const double w = binCentre(b) * scale * focal / d;
                if (m.flat[b] > 0)
                {
                    if (w < 0.25) bandC += m.flat[b];
                    else if (w < 8.0) { bandB += m.flat[b]; bandBflat += m.flat[b]; }
                    else bandA += m.flat[b];
                }
                if (m.solid[b] > 0)
                {
                    if (w < 0.25) bandC += m.solid[b];
                    else if (w < 1.5) bandB += m.solid[b];
                    else bandA += m.solid[b];
                }
            }
        }
        const double tot = bandA + bandB + bandC;
        (void)bandBflat;
        (void)tot;
        (void)bandA;
        (void)bandB;
        (void)bandC;
        md << format("| %s | %.2f M | %.1f %% | %.2f / %.2f / %.2f | %.2f M | %.2f M / %.2f M / %.2f M | %llu | %llu |\n", c.name.c_str(), surf / 1e6, 100.0 * surf / ((double)W * H),
                     pxBand[0] / 1e6, pxBand[1] / 1e6, pxBand[2] / 1e6, pxBC / 1e6, visBand[0] / 1e6, visBand[1] / 1e6, visBand[2] / 1e6, (unsigned long long)inst, (unsigned long long)dyn);
        logf("  scenemeta %s/%s: surface %.2f M px (%.1f %%), P_A %.2f P_B %.2f P_C %.2f M px, visible triangles A %.2f B %.2f C %.2f M\n", s.name.c_str(), c.name.c_str(),
             surf / 1e6, 100.0 * surf / ((double)W * H), pxBand[0] / 1e6, pxBand[1] / 1e6, pxBand[2] / 1e6, visBand[0] / 1e6, visBand[1] / 1e6, visBand[2] / 1e6);
    }
    md << dist.str();
    return md.str();
}

// CPU/GPU validation gate (Reference/GpuTracer/README_KO.md): the GPU tracer's image against the CPU reference of the
// same scene, camera and resolution, each with its two independent halves. Noise per pixel from the halves:
// var(full) = (A - B)^2 / 4. Criteria: tile z-scores (16 x 16 tiles, z = sum(G - C) / sqrt(sum var)) with mean 0 and
// deviation 1; per-mask mean bias in units of its standard error (masks from the 17 primary sub-sample identities:
// sky, edge, leaf, metal, ground, other); relMSE(C, G) against the sum of both images' noise, (h_C + h_G) / 4 with
// h = relMSE(half A, half B).
std::string gateCompare(const scene::Scene& s, const reference::ResolvedCamera& cam, const std::string& label, uint32_t W, uint32_t H, const std::filesystem::path& cpuPfm,
                        const std::filesystem::path& gpuPfm, const std::vector<std::filesystem::path>& hold)
{
    auto stem = [](const std::filesystem::path& p) { const std::string x = p.string(); return x.substr(0, x.size() - 4); };
    const metrics::Image C = metrics::readPfm(cpuPfm), Ca = metrics::readPfm(stem(cpuPfm) + ".halfA.pfm"), Cb = metrics::readPfm(stem(cpuPfm) + ".halfB.pfm");
    const metrics::Image G = metrics::readPfm(gpuPfm), Ga = metrics::readPfm(stem(gpuPfm) + ".halfA.pfm"), Gb = metrics::readPfm(stem(gpuPfm) + ".halfB.pfm");
    for (const metrics::Image* im : { &C, &Ca, &Cb, &G, &Ga, &Gb })
        if (im->width != W || im->height != H) fail("gatecompare: image size %ux%u, expected %ux%u", im->width, im->height, W, H);
    const size_t n = (size_t)W * H;
    // Masks from the primary identities (centre sample 16; edge = the 16 sub-samples do not all see the centre's surface).
    reference::PathTracer pt(s);
    const std::vector<uint64_t> ids = pt.primaryIdentities(cam, W, H, hold);
    enum Mask { All, Sky, Edge, Leaf, Metal, Ground, Other, MaskCount };
    const char* maskNames[MaskCount] = { "all", "sky", "edge", "leaf (Foliage)", "metal (metallic >= 0.5)", "ground (terrain / street)", "other surfaces" };
    std::vector<uint8_t> mask(n);
    for (size_t p = 0; p < n; ++p)
    {
        const uint64_t c = ids[p * 17 + 16];
        bool edge = false;
        for (int k = 0; k < 16; ++k) edge |= ids[p * 17 + k] != c;
        if (edge)
        {
            mask[p] = Edge;
            continue;
        }
        if (c == reference::kSkyIdentity)
        {
            mask[p] = Sky;
            continue;
        }
        const scene::Instance& in = s.instances[(uint32_t)(c >> 32)];
        const scene::Mesh& m = s.meshes[in.mesh];
        const uint32_t tri = (uint32_t)c;
        uint32_t sub = 0;
        for (uint32_t k = 0; k < (uint32_t)m.submeshes.size(); ++k)
            if (m.submeshes[k].indexOffset / 3 <= tri) sub = k;
        const scene::Material& mat = s.materials[in.materialOverrides.empty() ? m.submeshes[sub].material : in.materialOverrides[sub]];
        if (mat.cls == scene::MaterialClass::Foliage) mask[p] = Leaf;
        else if (mat.metallic >= 0.5f) mask[p] = Metal;
        else if (m.name == "terrain" || m.name == "ground" || m.name.rfind("street", 0) == 0) mask[p] = Ground;
        else mask[p] = Other;
    }
    auto Y = [](const metrics::Image& im, size_t p) { return 0.2126 * im.rgb[3 * p] + 0.7152 * im.rgb[3 * p + 1] + 0.0722 * im.rgb[3 * p + 2]; };
    std::vector<double> varSum(n);
    for (size_t p = 0; p < n; ++p)
    {
        const double dc = Y(Ca, p) - Y(Cb, p), dg = Y(Ga, p) - Y(Gb, p);
        varSum[p] = 0.25 * (dc * dc + dg * dg);
    }
    std::ostringstream md;
    md << format("## %s / %s %ux%u: GPU vs CPU reference [measured]\n\n", s.name.c_str(), label.c_str(), W, H);
    md << "CPU: `" << cpuPfm.filename().string() << "`, GPU: `" << gpuPfm.filename().string() << "`. Luminance; noise of each image from its halves.\n\n";
    // Tile z-scores.
    {
        const uint32_t T = 16;
        std::vector<double> z;
        for (uint32_t ty = 0; ty + T <= H; ty += T)
            for (uint32_t tx = 0; tx + T <= W; tx += T)
            {
                double d = 0, v = 0;
                for (uint32_t y = ty; y < ty + T; ++y)
                    for (uint32_t x = tx; x < tx + T; ++x)
                    {
                        const size_t p = (size_t)y * W + x;
                        d += Y(G, p) - Y(C, p);
                        v += varSum[p];
                    }
                if (v > 0) z.push_back(d / std::sqrt(v));
            }
        double mean = 0, sq = 0;
        size_t over3 = 0, over5 = 0;
        for (double v : z)
        {
            mean += v;
            sq += v * v;
            over3 += std::fabs(v) > 3;
            over5 += std::fabs(v) > 5;
        }
        mean /= std::max<size_t>(1, z.size());
        const double sd = std::sqrt(std::max(0.0, sq / std::max<size_t>(1, z.size()) - mean * mean));
        md << format("Tile z-scores (16 x 16 px, %zu tiles): mean %.3f (standard error %.3f), deviation %.3f, |z| > 3: %.2f %% (normal 0.27 %%), |z| > 5: %zu\n\n", z.size(), mean,
                     1.0 / std::sqrt((double)std::max<size_t>(1, z.size())), sd, 100.0 * over3 / std::max<size_t>(1, z.size()), over5);
    }
    // Per-mask bias.
    md << "| mask | pixels | CPU mean Y | GPU - CPU (relative) | standard error | bias / error |\n|---|---|---|---|---|---|\n";
    for (int k = 0; k < MaskCount; ++k)
    {
        double sc = 0, sg = 0, v = 0;
        size_t cnt = 0;
        for (size_t p = 0; p < n; ++p)
            if (k == All || mask[p] == k)
            {
                sc += Y(C, p);
                sg += Y(G, p);
                v += varSum[p];
                ++cnt;
            }
        if (cnt == 0) continue;
        const double rel = (sg - sc) / sc, err = std::sqrt(v) / sc;
        md << format("| %s | %zu | %.5g | %+.4f %% | %.4f %% | %+.2f |\n", maskNames[k], cnt, sc / cnt, 100 * rel, 100 * err, rel / err);
    }
    // Image metrics.
    const double hC = metrics::relMse(Ca, Cb), hG = metrics::relMse(Ga, Gb), rel = metrics::relMse(C, G);
    const metrics::FlipResult f = metrics::flipHdr(C, G), fn = metrics::flipHdr(Ca, Cb);
    md << format("\nrelMSE(CPU, GPU) %.4g; expected from the two noises (h_CPU + h_GPU) / 4 = %.4g (h_CPU %.4g, h_GPU %.4g): ratio %.3f. HDR-FLIP(CPU, GPU) mean %.4f, P99 %.4f; "
                 "CPU halves (floor) mean %.4f, P99 %.4f.\n\n",
                 rel, (hC + hG) / 4, hC, hG, rel / ((hC + hG) / 4), f.mean, f.p99, fn.mean, fn.p99);
    return md.str();
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
        if (a.command == "scenemeta")
        {
            if (a.width == 0 || a.height == 0) fail("--res is required");
            reference::waitWhileHeld(holdFiles(a));
            reference::PathTracer pt(s);
            const std::string md = sceneMeta(s, pt, a.width, a.height, holdFiles(a), a.wcap);
            if (!a.out.empty()) writeTextFile(a.out, md);
            logf("%s", md.c_str());
            return 0;
        }
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
        if (a.command == "gatecompare")
        {
            if (a.cpuImage.empty() || a.gpuImage.empty()) fail("gatecompare needs --cpu-image and --gpu-image");
            const std::string md = gateCompare(s, cam, label, a.width, a.height, a.cpuImage, a.gpuImage, holdFiles(a));
            if (!a.out.empty()) writeTextFile(a.out, md);
            logf("%s", md.c_str());
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
