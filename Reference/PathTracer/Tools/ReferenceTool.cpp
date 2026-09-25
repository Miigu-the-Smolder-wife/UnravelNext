// unx_reference: reference images, cache, census and comparisons (INTERFACES_KO.md 10.2, 10.3). CPU only.
//
//   unx_reference render  --scene <name|file.unxscene> (--camera <name> | --path <name> --time <s>) --res <WxH>
//                         [--seed N] [--scale S] [--no-wind] [--sun-illuminance lux] [--write-scene <file>] [--spp N] [--force]
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
    md << "Band of a visible triangle (COVERAGE_REDESIGN 14.9): w_px = smallest altitude x instance scale x focal / distance to the "
          "triangle centroid; C below 0.25 px; solid B below 1.5 px; flat (Foliage or two-sided) B only when |cos theta| x w_px < 1.5 px "
          "(theta between the view ray and the sheet normal) or w_px < w_cap, else A. Visible = hit by one of 16 stratified sub-samples per pixel (no "
          "wind). P_A / P_B / P_C = pixels whose sub-samples include a triangle of that band (ARCHITECTURE 2 table: P_B, P_C); T_A / T_B "
          "/ T_C = distinct visible triangles per band (before any LOD).\n\n"
          "| camera | surface px (centre ray) | surface % | P_A / P_B / P_C (M px) | P_B or P_C | T_A / T_B / T_C (visible) | instances in frustum |\n"
          "|---|---|---|---|---|---|---|\n";
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
        uint64_t pxBand[3] = {}, pxBC = 0;
        std::vector<uint64_t> seen;
        seen.reserve((size_t)W * H);
        for (size_t i = 0; i < (size_t)W * H; ++i)
        {
            bool has[3] = {};
            for (int k = 0; k < 16; ++k)
            {
                const uint64_t id = ids[i * 17 + k];
                if (id == reference::kSkyIdentity) continue;
                has[bandOfId(id)] = true;
                seen.push_back(id);
            }
            for (int b = 0; b < 3; ++b) pxBand[b] += has[b];
            pxBC += has[1] || has[2];
        }
        std::sort(seen.begin(), seen.end());
        seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
        uint64_t visBand[3] = {};
        for (uint64_t id : seen) ++visBand[bandOfId(id)];
        // Frustum side-plane normals (inward) for the sphere test.
        const double tx = th * aspect;
        const float3 nL = normalize(right * 1.0f + fw * (float)tx), nR = normalize(right * -1.0f + fw * (float)tx);
        const float3 nB = normalize(up * 1.0f + fw * (float)th), nT = normalize(up * -1.0f + fw * (float)th);
        double bandA = 0, bandB = 0, bandBflat = 0, bandC = 0;
        uint64_t inst = 0;
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
        md << format("| %s | %.2f M | %.1f %% | %.2f / %.2f / %.2f | %.2f M | %.2f M / %.2f M / %.2f M | %llu |\n", c.name.c_str(), surf / 1e6, 100.0 * surf / ((double)W * H),
                     pxBand[0] / 1e6, pxBand[1] / 1e6, pxBand[2] / 1e6, pxBC / 1e6, visBand[0] / 1e6, visBand[1] / 1e6, visBand[2] / 1e6, (unsigned long long)inst);
        logf("  scenemeta %s/%s: surface %.2f M px (%.1f %%), P_A %.2f P_B %.2f P_C %.2f M px, visible triangles A %.2f B %.2f C %.2f M\n", s.name.c_str(), c.name.c_str(),
             surf / 1e6, 100.0 * surf / ((double)W * H), pxBand[0] / 1e6, pxBand[1] / 1e6, pxBand[2] / 1e6, visBand[0] / 1e6, visBand[1] / 1e6, visBand[2] / 1e6);
    }
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
