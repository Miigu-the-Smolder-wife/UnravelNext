// S performance gate through the real renderer (FrameRenderer: V's cluster pipeline and depth raster service, M's
// material resolve, S; other tracks as built): per-pass GPU time of every S pass at 4K / 1440p on a C-track scene, with
// a static or a moving camera (the scene's first camera path), and the quantities the design's cost formulas use:
// dirty pages and the triangles V rasterised into them (T_sun), page requests and pool use, visibility paths.
// Needs a build with tracks V, M, S and C (Build.ps1 -Track S -Tracks "V;M;S;C", or -Track all). GPU lock required:
//   powershell -File Tools/CI/GpuLock.ps1 -Track S -- build/S/bin/unx_gate_shadow_renderergate.exe
//       --scene city_block|forest_thin|... [--resolution 4K|1440p|both] [--frames 600] [--moving] [--sun-deg-per-s R]
//       [--wind-gust-period-s T] [--camera NAME] [--capture FILE.pfm | --capture-output FILE.pfm] [--out DIR] [--set k=v]
//       (--resolution also 1080p, and all = 4K, 1440p, 1080p)
// --capture: the main view's linear scene radiance (FrameContext::outputLinearHdr, x exposure) of the last frame as
// a PFM for unx_reference compare (one resolution; the frames still render as measured, plus one copy each). The frame
// renders at the output resolution (no temporal upscale).
// --capture-output: the same units (linear radiance x exposure, before the post chain's encoding) at the output
// resolution after the temporal upscale (ViewResources::upscaled: the frames render as in play, at the internal
// resolution output.render_scale gives): with --capture of the same scene, the upscaled image against the native one.
// --warmup-frames N: the warm-up by frame count (HarnessOptions::warmupFrames), so the last frame has the same index in
// every run; the default with --capture / --capture-output / --luminance-log is 300 (the time-based warm-up ended at a
// different frame each run and in each configuration: every temporal state differed at the captured frame).
// --luminance-log FILE.csv: per frame (warm-up included) the mean luminance of the gate output (GateLuminance.hlsl:
// exposed-linear source before display encoding), "frame,mean_linear_exposed,invalid_pixels".
// The logger reads the upscale output when active; native runs request linear output. GI split history stays opt-in.
#if __has_include("unx/clusterbuilder/ClusterBuilder.h") && defined(UNX_HAS_SCENEGEN)
#define S_RENDERER_GATE 1
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/scenegen/SceneGen.h"
#include "unx/visibility/Visibility.h"
#endif
#include "FroxelSystem.h"
#include "SResources.h"
#include "VsmSystem.h"
#include "../../Atmosphere/Celestial.h"

#if __has_include("unx/refl/ReflectionSystem.h")
#include "unx/refl/ReflectionSystem.h"
#endif
#if __has_include("unx/gi/GiSystem.h")
#include "unx/gi/GiSystem.h"
#endif
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

#if S_RENDERER_GATE
namespace
{
scene::Camera cameraAt(const scene::Scene& s, bool moving, double time, const std::string& name)
{
    scene::Camera c = s.cameras.at(0);
    for (const scene::Camera& k : s.cameras)
        if (k.name == name) c = k;
    if (!moving || s.paths.empty() || s.paths[0].keys.size() < 2) return c;
    const auto& keys = s.paths[0].keys;
    const double span = keys.back().time - keys.front().time;
    const float t = (float)(keys.front().time + std::fmod(time, span));
    size_t k = 0;
    while (k + 2 < keys.size() && keys[k + 1].time < t) ++k;
    const float u = std::clamp((t - keys[k].time) / std::max(keys[k + 1].time - keys[k].time, 1e-6f), 0.0f, 1.0f);
    c.position = keys[k].position + (keys[k + 1].position - keys[k].position) * u;
    c.forward = normalize(keys[k].forward + (keys[k + 1].forward - keys[k].forward) * u);
    c.up = normalize(keys[k].up + (keys[k + 1].up - keys[k].up) * u);
    return c;
}
} // namespace
#endif

#if S_RENDERER_GATE
namespace
{
// IEEE 754 binary16 -> float (the upscaled capture's RGBA16F).
float halfToFloat(uint16_t h)
{
    const uint32_t sign = (uint32_t)(h & 0x8000u) << 16, exponent = (h >> 10) & 0x1Fu, mantissa = h & 0x3FFu;
    uint32_t bits;
    if (exponent == 0)
    {
        if (mantissa == 0) bits = sign;
        else
        {
            uint32_t e = 113, m = mantissa;  // subnormal: normalise
            while ((m & 0x400u) == 0)
            {
                m <<= 1;
                --e;
            }
            bits = sign | (e << 23) | ((m & 0x3FFu) << 13);
        }
    }
    else if (exponent == 31) bits = sign | 0x7F800000u | (mantissa << 13);
    else bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
} // namespace
#endif

int main(int argc, char** argv)
{
    int gateFailures = 0;
    try
    {
#if !S_RENDERER_GATE
        (void)argc;
        (void)gateFailures;
        (void)argv;
        fail("this build lacks V's cluster builder or C's scene generator: Build.ps1 -Track S -Tracks \"V;M;S;C\" (or -Track all)");
#else
        std::string sceneName = "city_block", resolutionArg = "both", out;
        uint32_t frames = 600;
        bool moving = false;
        float sunDegPerS = 0;  // moving sun (time of day): the sun turns about the horizontal axis normal to it
        std::string capturePath;  // --capture: last frame's linear radiance as PFM
        bool captureUpscaled = false;  // --capture-output: the upscaled image (ViewResources::upscaled) instead of the native frame
        std::string cameraName;   // --camera: a camera of the scene by name (default: the first)
        float gustPeriodS = 0;  // wind change after commit (v1.23): every gustPeriodS the source scene's wind alternates
                                // between the scene's and +30 % speed / +20 degrees (no reload; the host's path)
        std::vector<std::string> overrides;
        double warmupSeconds = -1;  // --warmup-seconds: the harness default when negative
        int64_t warmupFrames = -1;  // --warmup-frames (captures and luminance logs: 300 when not given)
        std::string luminancePath;  // --luminance-log
        std::string cameraAt6, saveScene;
        std::string timeArg, placeArg;
        bool autoExposure = false;
        uint64_t shiftAt = UINT64_MAX;  // --origin-shift-at F --origin-shift x,y,z: a C9 rebase at frame F (repros)
        float cloudCoverage = 0;         // --clouds C: B5 cloud layer (FrameContext::clouds) with coverage C, other fields default
        float3 shiftBy{};  // --time YYYY-MM-DDTHH:MM (UT), --place lat,lon: sun, moon, stars (B4)
        uint32_t addShadowLights = 0;  // --add-shadow-lights N (game request 09-30: frames failed past ~97 shadowed local lights)
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") sceneName = next();
            else if (a == "--resolution") resolutionArg = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--camera") cameraName = next();
            else if (a == "--moving") moving = true;
            else if (a == "--sun-deg-per-s") sunDegPerS = std::stof(next());
            else if (a == "--wind-gust-period-s") gustPeriodS = std::stof(next());
            else if (a == "--capture") capturePath = next();
            else if (a == "--capture-output")
            {
                capturePath = next();
                captureUpscaled = true;
            }
            else if (a == "--out") out = next();
            else if (a == "--set") overrides.push_back(next());
            else if (a == "--warmup-frames") warmupFrames = std::stoll(next());
            else if (a == "--luminance-log") luminancePath = next();
            else if (a == "--warmup-seconds") warmupSeconds = std::stod(next());  // repro of early frames (never with timings)
            else if (a == "--camera-at") cameraAt6 = next();  // "px,py,pz,tx,ty,tz": camera 0 moved to look from p at t (repros)
            else if (a == "--save-scene") saveScene = next();  // the scene as rendered (with --camera-at) for unx_reference
            else if (a == "--time") timeArg = next();
            else if (a == "--place") placeArg = next();
            else if (a == "--auto-exposure") autoExposure = true;
            else if (a == "--origin-shift-at") shiftAt = std::stoull(next());
            else if (a == "--clouds") cloudCoverage = std::stof(next());
            else if (a == "--add-shadow-lights") addShadowLights = (uint32_t)std::stoul(next());
            else if (a == "--origin-shift")
            {
                const std::string v = next();
                const size_t c0 = v.find(','), c1 = v.find(',', c0 + 1);
                if (c0 == std::string::npos || c1 == std::string::npos) fail("--origin-shift x,y,z");
                shiftBy = { std::stof(v.substr(0, c0)), std::stof(v.substr(c0 + 1, c1 - c0 - 1)), std::stof(v.substr(c1 + 1)) };
            }  // A4 metering instead of the camera's EV100
            else fail("unknown argument %s", a.c_str());
        }
        requireGpuLock("unx_gate_shadow_renderergate");
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const std::string& o : overrides) quality.applyOverride(o);
        // --scene: a SceneGen scene by name, or a saved .unxscene file (the host's UnravelNextRenderer save: repro of an
        // engine capture in the gate).
        const bool sceneFile = sceneName.size() > 9 && sceneName.compare(sceneName.size() - 9, 9, ".unxscene") == 0;
        scenegen::Request request;
        bool found = sceneFile;
        for (scenegen::SceneId id : scenegen::allScenes())
            if (sceneName == scenegen::sceneName(id))
            {
                request.id = id;
                found = true;
            }
        if (!found) fail("unknown scene %s", sceneName.c_str());
        scene::Scene s = sceneFile ? scene::load(sceneName) : scenegen::generate(request);  // not const: --sun-deg-per-s turns its sun (GpuScene keeps &s)
        if (!cameraAt6.empty())
        {
            float v[6] = {};
            size_t at = 0;
            for (int k = 0; k < 6; ++k)
            {
                const size_t comma = cameraAt6.find(',', at);
                if ((comma == std::string::npos) != (k == 5)) fail("--camera-at expects px,py,pz,tx,ty,tz");
                v[k] = std::stof(cameraAt6.substr(at, comma - at));
                at = comma + 1;
            }
            if (s.cameras.empty()) s.cameras.push_back({});
            scene::Camera& c = s.cameras[0];
            c.position = { v[0], v[1], v[2] };
            c.forward = normalize(float3{ v[3] - v[0], v[4] - v[1], v[5] - v[2] });
            c.up = normalize(cross(cross(c.forward, float3{ 0, 1, 0 }), c.forward));
        }
        if (!saveScene.empty())
        {
            scene::save(s, saveScene);
            logf("saved the scene with its camera 0 to %s\n", saveScene.c_str());
        }
        // Time of day (B4): the directional light (sun, or the moon below civil twilight) and the celestial record.
        CelestialFrame celestial;
        if (!timeArg.empty())
        {
            sky::CelestialTime t;
            if (timeArg.size() != 16 || timeArg[4] != '-' || timeArg[7] != '-' || timeArg[10] != 'T' || timeArg[13] != ':') fail("--time expects YYYY-MM-DDTHH:MM (UT)");
            t.year = std::stoi(timeArg.substr(0, 4));
            t.month = std::stoi(timeArg.substr(5, 2));
            t.day = std::stoi(timeArg.substr(8, 2));
            t.hoursUt = std::stoi(timeArg.substr(11, 2)) + std::stoi(timeArg.substr(14, 2)) / 60.0;
            if (!placeArg.empty())
            {
                const size_t comma = placeArg.find(',');
                if (comma == std::string::npos) fail("--place expects lat,lon");
                t.latitudeDeg = std::stod(placeArg.substr(0, comma));
                t.longitudeDeg = std::stod(placeArg.substr(comma + 1));
            }
            const sky::CelestialState st = sky::celestial(t);
            const sky::DirectionalLight light = sky::directionalLight(st, s.sun);
            celestial = sky::celestialFrame(st, light, s.sun);
            s.sun = light.sun;
            logf("moon direction (%.4f, %.4f, %.4f), sun direction (%.4f, %.4f, %.4f)\n", st.moon.x, st.moon.y, st.moon.z, st.sun.x, st.sun.y, st.sun.z);
            logf("time %s at (%.4f, %.4f): sun altitude %.2f deg, moon altitude %.2f deg, phase angle %.1f deg; light = %s (%.4g lux)\n", timeArg.c_str(),
                 t.latitudeDeg, t.longitudeDeg, st.sunAltitudeDeg, st.moonAltitudeDeg, st.moonPhaseAngleDeg, light.moon ? "moon" : "sun", s.sun.illuminance);
        }
        const float3 sun0 = normalize(s.sun.direction);
        const float3 sunAxis = normalize(cross(sun0, float3{ 0, 1, 0 }));
        const float wind0 = s.windSpeed;
        const float3 windDir0 = s.windDirection;
        if (addShadowLights > 0 && !s.cameras.empty())
        {
            // N shadowed point lights on a grid around the host camera (1.5 m apart, 20 per row, 1 m above it, range 4 m)
            const scene::Camera& cam = s.cameras[0];  // (the gate renders camera 0)
            for (uint32_t i = 0; i < addShadowLights; ++i)
            {
                scene::Light l;
                l.type = scene::LightType::Point;
                l.position = { cam.position.x + 1.5f * ((float)(i % 20) - 9.5f), cam.position.y + 1.0f, cam.position.z + 1.5f * ((float)(i / 20) - 4.5f) };
                l.intensity = 20;
                l.range = 4;
                l.castShadow = true;
                s.lights.push_back(l);
            }
            logf("added %u shadowed point lights around the camera: %zu lights\n", addShadowLights, s.lights.size());
        }
        ClusterData clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality));
        logf("scene %s (%s), %zu instances, %zu clusters, camera %s\n", sceneName.c_str(), scene::contentHash(s).substr(0, 16).c_str(), s.instances.size(),
             clusters.clusters.size(), moving ? "path 0 (moving)" : (cameraName.empty() ? "0 (static)" : cameraName.c_str()));
        Device device({});
        ComPtr<ID3D12Resource> captureBuffer;  // --capture: readback of the gate output (last frame wins)
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT captureFootprint{};
        float captureEv100 = 0;  // the captured frame's exposure (the output is radiance x exposure)
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene gpuScene(device);
        gpuScene.upload(s);
        gpuScene.setClusters(std::move(clusters));
        Harness harness(device, quality);
        const std::vector<std::string> resolutions = resolutionArg == "both"  ? std::vector<std::string>{ "4K", "1440p" }
                                                   : resolutionArg == "all" ? std::vector<std::string>{ "4K", "1440p", "1080p" }
                                                                            : std::vector<std::string>{ resolutionArg };
        for (const std::string& rs : resolutions)
        {
            // A repro of an engine capture at its own size (WxH): with --capture only, and its timings are not measurements.
            Resolution res;
            if (const size_t x = rs.find('x'); x != std::string::npos && rs != "3840x2160" && rs != "2560x1440" && rs != "1920x1080")
            {
                if (capturePath.empty()) fail("--resolution %s: other sizes than 4K, 1440p and 1080p only for a --capture repro", rs.c_str());
                res = { (uint32_t)std::stoul(rs.substr(0, x)), (uint32_t)std::stoul(rs.substr(x + 1)), rs + " (repro, not a measurement)" };
            }
            else
                res = resolutionFromString(rs, quality);
            FrameRenderer renderer(device, shaders, quality, gpuScene, 2);
            shadow::setKeepFroxels(renderer.trackState(), true);  // no consumer of the volume yet (M): measure it anyway
            HarnessOptions options;
            options.frames = frames;
            if (warmupSeconds >= 0) options.warmupSeconds = warmupSeconds;
            if (warmupFrames >= 0) options.warmupFrames = (uint32_t)warmupFrames;
            else if (!capturePath.empty() || !luminancePath.empty()) options.warmupFrames = 300;
            // --luminance-log: uint64 fixed-point sum + invalid count per frame, read back after the run.
            constexpr uint32_t kLuminanceSlots = 1u << 15;
            ComPtr<ID3D12Resource> luminanceSums;
            uint64_t lastFrame = 0;
            if (!luminancePath.empty()) luminanceSums = s_detail::createBuffer(device, L"gate luminance sums", (uint64_t)kLuminanceSlots * 16);
            options.label = "S " + sceneName + (moving ? " moving " : " static ") + (sunDegPerS != 0 ? "sun " + std::to_string(sunDegPerS) + " deg/s " : "") + (gustPeriodS > 0 ? "gusts " : "") + rs;
            if (!out.empty()) options.outputDirectory = out;
            float4x4 prev = ViewDesc::fromCamera(cameraAt(s, moving, 0, cameraName), res.width, res.height, {}).viewProj;
            // Dirty pages and T_sun averaged over the measured frames (the counters lag the frame by two).
            double dirtySum = 0, trianglesSum = 0, requestedSum = 0;
            uint32_t samples = 0, exhausted = 0, requestedMax = 0, overTiles = 0, overflowWordsMax = 0, overflowLightsMax = 0;
            uint64_t lastStatsFrame = 0;
            uint32_t internalW = 0, internalH = 0;  // the main view's render size (the internal resolution when it upscales)
            // P0 workload (RENDERER_REDESIGN 0.4): the sizes and the counts the cost formulas use, into the result's JSON.
            options.extraJson = [&]() {
                std::string js = format("\"output\": \"%ux%u\", \"internal\": \"%ux%u\", \"render_scale\": %.4f", res.width, res.height, internalW, internalH,
                                        quality.has("output.render_scale") ? quality.number("output.render_scale") : 1.0);
                js += format(", \"vsm_dirty_pages_mean\": %.1f, \"vsm_raster_triangles_mean\": %.0f, \"vsm_requested_mean\": %.1f, \"vsm_requested_max\": %u, "
                             "\"vsm_exhausted_frames\": %u, \"overflow_lights_max\": %u",
                             samples ? dirtySum / samples : 0.0, samples ? trianglesSum / samples : 0.0, samples ? requestedSum / samples : 0.0, requestedMax, exhausted,
                             overflowLightsMax);
                const shadow::VsmStats& vs = shadow::stats(renderer.trackState());
                js += format(", \"vsm_cached_pages\": %u, \"vsm_local_active\": %u, \"vsm_paths\": [%u, %u, %u, %u, %u, %u, %u]", vs.cachedPages, vs.localActive,
                             vs.pathNoCaster, vs.pathRegionLit, vs.pathRegionUmbra, vs.pathSearchLit, vs.pathFiltered, vs.pathDiskLit, vs.pathDiskUmbra);
                js += format(", \"froxel_light_entries\": %u", shadow::froxelStats(renderer.trackState()).indexCount);
#if __has_include("unx/refl/ReflectionSystem.h")
                if (refl::ReflectionSystem* rs = refl::ReflectionSystem::find(renderer.trackState()))
                {
                    const refl::ReflectionSystem::Stats st = rs->readStats();
                    const uint32_t raysPerSample = (uint32_t)quality.integer("reflection.g_rays_per_sample");
                    js += format(", \"reflection_jobs\": %u, \"reflection_mirror_jobs\": %u, \"reflection_glossy_jobs\": %u, \"reflection_glossy_pixels\": %u, "
                                 "\"reflection_primary_rays\": %u, \"reflection_planar_views\": %u, \"reflection_planar_pixels\": %u",
                                 st.jobs, st.mirrorJobs, st.glossyJobs, st.glossyPixels, st.mirrorJobs + st.glossyJobs * raysPerSample, st.planarViews, st.planarPixels);
                }
#endif
#if __has_include("unx/gi/GiSystem.h")
                if (gi::GiSystem* gs = gi::GiSystem::find(renderer.trackState()))
                {
                    const gi::GiStats st = gs->readStats();
                    js += format(", \"gi_live_entries\": %u, \"gi_requested\": %u, \"gi_selected_updates\": %u, \"gi_primary_rays\": %u, \"gi_background\": %u, "
                                 "\"gi_reflection_hit_lookups\": %u",
                                 st.live, st.requested, st.selected, st.selected * 64u, st.background, st.hitLookups);
                }
#endif
                return js;
            };
            const HarnessResult r = harness.run(res, options, [&](RenderGraph& g, const Resolution& rr, uint64_t frame) {
                FrameContext fc;
                fc.frameIndex = frame;
                fc.time = frame / 60.0;
                fc.deltaTime = 1.0f / 60;
                scene::Camera cam = cameraAt(s, moving, fc.time, cameraName);
                if (frame == shiftAt)
                {
                    // The rebase (C9): the GPU scene moves by -delta, the frame says so, and the camera is given in the new
                    // coordinates from here on; the previous view moves with it (FrameRenderer).
                    gpuScene.rebase(shiftBy);
                    fc.originShift = shiftBy;
                }
                const float3 offset = gpuScene.originOffset();
                cam.position = cam.position - offset;
                fc.mainView = ViewDesc::fromCamera(cam, rr.width, rr.height, prev);
                fc.clouds.coverage = cloudCoverage;
                if (gustPeriodS > 0)
                {
                    const bool gust = ((uint64_t)(fc.time / gustPeriodS) & 1) != 0;
                    const float a = gust ? 20.0f * 0.01745329f : 0.0f;
                    s.windSpeed = gust ? wind0 * 1.3f : wind0;
                    s.windDirection = float3{ windDir0.x * std::cos(a) - windDir0.z * std::sin(a), windDir0.y, windDir0.x * std::sin(a) + windDir0.z * std::cos(a) };
                }
                if (sunDegPerS != 0)
                {
                    // Rodrigues rotation of the initial sun direction (the axis is normal to it).
                    const float a = sunDegPerS * 0.01745329252f * (float)fc.time, ca = std::cos(a), sa = std::sin(a);
                    s.sun.direction = normalize(sun0 * ca + cross(sunAxis, sun0) * sa);
                }
                prev = fc.mainView.viewProj;
                fc.outputLinearHdr = !capturePath.empty() && !captureUpscaled;
                if (luminanceSums && !fc.outputLinearHdr)
                {
                    // Match setupUpscale's resolution decision, preserving the actual upscale path.
                    const double scale = quality.number("output.render_scale");
                    uint32_t height = (int64_t)rr.height < quality.integer("output.render_scale_min_height") ? rr.height :
                        std::max(8u, (uint32_t)std::lround(rr.height * scale));
                    const int64_t cap = quality.integer("output.render_height_max");
                    if (cap > 0) height = std::min(height, (uint32_t)cap);
                    if (cap < 0 || height >= rr.height) fc.outputLinearHdr = true;
                }
                fc.celestial = celestial;
                fc.autoExposure = autoExposure;
                captureEv100 = fc.mainView.ev100;
                const TextureRef output = g.createTexture({ "gate output", rr.width, rr.height, 1, 1,
                                                            fc.outputLinearHdr ? DXGI_FORMAT_R32G32B32A32_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM });
                const ViewResources rendered = renderer.record(g, fc, output);
                internalW = rendered.view.width;
                internalH = rendered.view.height;
                lastFrame = frame;
                if (luminanceSums)
                {
                    const BufferRef sums = g.importBuffer(luminanceSums.Get(), { "gate luminance sums", (uint64_t)kLuminanceSlots * 16, 0 });
                    const TextureRef source = rendered.upscaled.valid() ? rendered.upscaled : output;
                    if (!rendered.upscaled.valid() && !fc.outputLinearHdr) fail("luminance log requires a linear colour source");
                    const uint32_t slot = (uint32_t)(frame % kLuminanceSlots), w = rr.width, h = rr.height;
                    if ((uint64_t)w * h > (1ull << 24)) fail("luminance log exceeds its fixed-point pixel bound");
                    for (const uint32_t clear : { 1u, 0u })
                        g.addPass(clear ? "s.gate.luminance.clear" : "s.gate.luminance", QueueType::Compute,
                                  [&](PassBuilder& b) {
                                      if (!clear) b.use(source, Use::SrvCompute);
                                      b.use(sums, Use::UavCompute);
                                      b.keep();
                                  },
                                  [&shaders, source, sums, slot, w, h, clear](PassContext& c) {
                                      const uint32_t k[8] = { clear ? 0xFFFFFFFFu : c.srv(source), c.uav(sums), slot, w, h, 0, clear, 0 };
                                      c.cmd->SetPipelineState(shaders.compute("Passes/Shadow/Gates/GateLuminance"));
                                      c.computeConstants(k, 8);
                                      c.cmd->Dispatch(clear ? 1 : (w + 7) / 8, clear ? 1 : (h + 7) / 8, 1);
                                  });
                }
                if (!capturePath.empty())
                {
                    if (captureUpscaled && !rendered.upscaled.valid())
                        fail("--capture-output: the frame renders at its output resolution (output.render_scale / render_scale_min_height): use --capture");
                    const TextureRef source = captureUpscaled ? rendered.upscaled : output;
                    if (!captureBuffer)
                    {
                        D3D12_RESOURCE_DESC td{};
                        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                        td.Width = rr.width;
                        td.Height = rr.height;
                        td.DepthOrArraySize = 1;
                        td.MipLevels = 1;
                        td.Format = g.desc(source).format;
                        if (td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT && td.Format != DXGI_FORMAT_R32G32B32A32_FLOAT)
                            fail("capture requires RGBA16F or RGBA32F linear colour");
                        td.SampleDesc.Count = 1;
                        UINT rows;
                        UINT64 rowBytes, total;
                        device.d3d()->GetCopyableFootprints(&td, 0, 1, 0, &captureFootprint, &rows, &rowBytes, &total);
                        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                        D3D12_RESOURCE_DESC bd{};
                        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                        bd.Width = total;
                        bd.Height = 1;
                        bd.DepthOrArraySize = 1;
                        bd.MipLevels = 1;
                        bd.SampleDesc.Count = 1;
                        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                        check(device.d3d()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                    IID_PPV_ARGS(&captureBuffer)),
                              "capture readback");
                    }
                    ID3D12Resource* rb = captureBuffer.Get();
                    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = captureFootprint;
                    g.addPass("s.gate.capture", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  b.use(source, Use::CopySrc);
                                  b.keep();
                              },
                              [=](PassContext& ctx) {
                                  D3D12_TEXTURE_COPY_LOCATION dst{ rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                                  dst.PlacedFootprint = fp;
                                  D3D12_TEXTURE_COPY_LOCATION src{ ctx.resource(source), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                                  src.SubresourceIndex = 0;
                                  ctx.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                              });
                }
                const shadow::VsmStats& st = shadow::stats(renderer.trackState());
                if (frame > 8 && st.frame != lastStatsFrame)
                {
                    lastStatsFrame = st.frame;
                    const visibility::Stats vs = visibility::latestStats(renderer.trackState(), "s.vsm.raster");
                    dirtySum += st.dirty;
                    trianglesSum += (double)vs.triangles[0] + vs.triangles[1] + vs.triangles[2];
                    requestedSum += st.requested;
                    requestedMax = std::max(requestedMax, st.requested);
                    exhausted += st.exhausted;
                    overTiles += st.overflowOverTiles;
                    overflowWordsMax = std::max(overflowWordsMax, st.overflowWords);
                    overflowLightsMax = std::max(overflowLightsMax, st.overflowLights);
                    ++samples;
                }
            });
            harness.printSummary(r);
            if (luminanceSums)
            {
                // The harness waited for the GPU at its end: copy the sums out and write one line per rendered frame.
                const uint64_t bytes = (uint64_t)kLuminanceSlots * 16;
                ComPtr<ID3D12Resource> rb = s_detail::createBuffer(device, L"gate luminance readback", bytes, D3D12_HEAP_TYPE_READBACK);
                CommandList cl = device.acquireCommandList(QueueType::Graphics);
                D3D12_BUFFER_BARRIER bb{ D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                         luminanceSums.Get(), 0, UINT64_MAX };
                D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_BUFFER, 1 };
                group.pBufferBarriers = &bb;
                cl.list->Barrier(1, &group);
                cl.list->CopyBufferRegion(rb.Get(), 0, luminanceSums.Get(), 0, bytes);
                device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
                void* mapped = nullptr;
                check(rb->Map(0, nullptr, &mapped), "map luminance sums");
                const uint64_t* sums = static_cast<const uint64_t*>(mapped);
                std::ofstream file(luminancePath);
                if (!file) fail("cannot write %s", luminancePath.c_str());
                file << "frame,mean_linear_exposed,invalid_pixels\n";
                const uint64_t first = lastFrame + 1 > kLuminanceSlots ? lastFrame + 1 - kLuminanceSlots : 0;
                for (uint64_t f = first; f <= lastFrame; ++f)
                {
                    const uint64_t slot = f % kLuminanceSlots;
                    const uint32_t invalid = (uint32_t)sums[slot * 2 + 1];
                    file << f << "," << format("%.9f", (double)sums[slot * 2] / (4096.0 * res.width * res.height)) << "," << invalid << "\n";
                }
                D3D12_RANGE none{ 0, 0 };
                rb->Unmap(0, &none);
                logf("luminance of frames %llu..%llu -> %s\n", (unsigned long long)first, (unsigned long long)lastFrame, luminancePath.c_str());
            }
            if (!capturePath.empty()) logf("captured frame index %llu (warm-up %llu frames)\n", (unsigned long long)lastFrame, (unsigned long long)r.firstMeasuredFrame);
            if (!capturePath.empty() && captureBuffer)
            {
                // The harness waits for the GPU at its end: the readback holds the last frame. PFM: "PF", width height,
                // -1 (little endian), rows bottom to top, RGB float.
                void* mapped = nullptr;
                check(captureBuffer->Map(0, nullptr, &mapped), "map capture");
                std::string header = "PF\n" + std::to_string(res.width) + " " + std::to_string(res.height) + "\n-1.0\n";
                std::vector<float> rgb((size_t)res.width * res.height * 3);
                // The linear-HDR output is radiance x exposure (Frame.h; 1 / (1.2 2^ev100)), the same units as C's
                // unx_reference images (their json records the camera's ev100): written as it is.
                const float toRadiance = 1.0f;
                for (uint32_t y = 0; y < res.height; ++y)
                {
                    const uint8_t* rowBytes = static_cast<const uint8_t*>(mapped) + captureFootprint.Offset + (size_t)y * captureFootprint.Footprint.RowPitch;
                    float* dstRow = &rgb[(size_t)(res.height - 1 - y) * res.width * 3];
                    for (uint32_t x = 0; x < res.width; ++x)
                        for (int c = 0; c < 3; ++c)
                        {
                            float v;
                            if (captureFootprint.Footprint.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
                            {
                                uint16_t hv;
                                std::memcpy(&hv, rowBytes + (x * 4 + c) * 2, 2);
                                v = halfToFloat(hv);
                            }
                            else
                                std::memcpy(&v, rowBytes + (x * 4 + c) * 4, 4);
                            dstRow[x * 3 + c] = v * toRadiance;
                        }
                }
                D3D12_RANGE none{ 0, 0 };
                captureBuffer->Unmap(0, &none);
                std::ofstream file(capturePath, std::ios::binary);
                if (!file) fail("cannot write %s", capturePath.c_str());
                file.write(header.data(), (std::streamsize)header.size());
                file.write(reinterpret_cast<const char*>(rgb.data()), (std::streamsize)(rgb.size() * sizeof(float)));
                logf("captured %ux%u linear radiance x exposure (ev100 %.2f, %s) -> %s\n", res.width, res.height, captureEv100,
                     captureUpscaled ? "after the temporal upscale" : "native", capturePath.c_str());
                captureBuffer.Reset();
            }
            const shadow::VsmStats& st = shadow::stats(renderer.trackState());
            logf("local shadows: %u slots assigned, %u raster-active, %u shadowed lights without a slot\n", st.localAssigned, st.localActive, st.localWithoutSlot);
            double sPasses = 0, raster = 0;
            for (const auto& [name, d] : r.passMs)
                if (name.rfind("s.", 0) == 0)
                {
                    logf("  %-32s median %.4f ms  P95 %.4f ms\n", name.c_str(), d.median, d.p95);
                    sPasses += d.median;
                    if (name.rfind("s.vsm.raster", 0) == 0) raster += d.median;
                }
            const double n = std::max(samples, 1u);
            logf("%s: S passes %.4f ms (page raster %.4f ms) of GPU frame %.4f ms | pages requested mean %.0f max %u, dirty mean %.1f, T_sun mean %.2f M, pool exhausted %u\n",
                 rs.c_str(), sPasses, raster, r.gpuFrameMs.median, requestedSum / n, requestedMax, dirtySum / n, trianglesSum / n / 1e6, exhausted);
            const shadow::FroxelStats& fs = shadow::froxelStats(renderer.trackState());
            logf("  froxels: %u light entries (%.2f per froxel), %u truncated lists, %u lights dropped, max %u per froxel\n", fs.indexCount,
                 (double)fs.indexCount / std::max(1.0, (double)shadow::froxelGridFor(quality, res.width, res.height).gridX *
                                                       shadow::froxelGridFor(quality, res.width, res.height).gridY *
                                                       shadow::froxelGridFor(quality, res.width, res.height).slices),
                 fs.overflowLists, fs.droppedLights, fs.maxCount);
            if (quality.integer("atmosphere.froxels.walk_stats") != 0)
                logf("  air walk: slices %u, mixed %u (%.1f %%), loads b32 %u b8 %u texel %u (%.1f/mixed)\n",
                     st.airSlices, st.airSlicesMixed, 100.0 * st.airSlicesMixed / std::max(st.airSlices, 1u), st.airBlocks32, st.airBlocks8, st.airTexels,
                     (double)st.airTexels / std::max(st.airSlicesMixed, 1u));
            if (quality.integer("atmosphere.froxels.walk_stats") != 0)
                logf("  local air walk: entries %u, cells %u (%.2f per entry), max %u per entry, lit runs %.2f per entry\n", st.localAirEntries,
                     st.localAirCells, (double)st.localAirCells / std::max(st.localAirEntries, 1u), st.localAirMaxCells,
                     (double)st.localAirRuns / std::max(st.localAirEntries, 1u));
            if (quality.integer("atmosphere.froxels.walk_stats") != 0)
                logf("  local air walk per wave: sum of lane maxima cells %u, entries %u (x 24 points = %u)\n", st.localAirWaveCells, st.localAirWaveEntries,
                     st.localAirWaveEntries * 24);
            {
                std::string pagesLine;
                for (uint32_t k = 0; k < shadow::kLevels; ++k) pagesLine += format(" L%u:%u", k, st.levelPages[k]);
                logf("  requested sun pages by level (last frame):%s\n", pagesLine.c_str());
                if (quality.integer("shadow.vsm.use_stats") != 0)
                    logf("  pages read: pixel %u/%u, air %u/%u, propagated %u/%u, cached %u\n",
                         st.usePixelRead, st.usePixel, st.useAirRead, st.useAir, st.usePropagatedRead, st.usePropagated, st.useCachedRead);
                if (quality.integer("shadow.vsm.use_stats") != 0)
                    logf("  surface px %u, N.L<=0 %u (%.1f %%), N.L<=0 mixed %u\n",
                         st.surfacePixels, st.backfacePixels, 100.0 * st.backfacePixels / std::max(st.surfacePixels, 1u), st.backfaceMixed);
                if (quality.integer("shadow.vsm.subtile_stats") != 0)
                    logf("  sampled 32^2 sub-tiles %u of %u in pixel-requested pages (%.1f %%)\n", st.sampledSubtiles, st.pixelRequested * 16,
                         100.0 * st.sampledSubtiles / std::max(st.pixelRequested * 16, 1u));
            }
            // Overflow list (INTERFACES 7.3): the gate requires no tile over the capacity in the measured frames.
            logf("  shadow overflow: lights past the third max %u, words needed max %u, tiles over capacity %u %s\n", overflowLightsMax, overflowWordsMax, overTiles,
                 overTiles ? "FAIL" : "ok");
            if (overTiles) ++gateFailures;
            logf("  S error bits (INTERFACES 3.6, shader loop caps) 0x%x %s\n", st.errorBitsSeen, st.errorBitsSeen ? "FAIL" : "ok");
            if (st.errorBitsSeen) ++gateFailures;
            // Fragment visibility of the coverage layer (ShadowFragments): pixels with records and pair pixels; with
            // shadow.vsm.fragment_check the settled pixels' values against the per-record SMRT (limit 1/255).
            logf("  coverage fragments: %u pixels with records, %u pair (%.1f %%)\n", st.fragmentPixels, st.fragmentPairs,
                 100.0 * st.fragmentPairs / std::max(st.fragmentPixels, 1u));
            if (quality.integer("shadow.vsm.fragment_check") != 0)
            {
                logf("  fragment check: %u records of settled pixels, %u differ by more than 1/255 (largest %u/255) %s\n", st.fragmentChecked,
                     st.fragmentMismatch, st.fragmentMaxDiff, st.fragmentMismatch == 0 && st.fragmentChecked > 0 ? "ok" : "FAIL");
                if (st.fragmentMismatch != 0)
                    logf("    first mismatch: pixel (%u, %u), settled %u, per-record %u, segment position %.2f of 3\n", (st.fragmentFirstPixel - 1) & 0xFFFF,
                         (st.fragmentFirstPixel - 1) >> 16, st.fragmentFirstValues & 0xFF, (st.fragmentFirstValues >> 8) & 0xFF, (st.fragmentFirstValues >> 16) / 64.0);
                if (st.fragmentMismatch != 0 || st.fragmentChecked == 0) ++gateFailures;
            }
            const double px = st.pathNoCaster + st.pathRegionLit + st.pathRegionUmbra + st.pathSearchLit + st.pathFiltered + st.pathDiskLit + st.pathDiskUmbra;
            logf("  visibility paths (%% of %.2f M pixels): no caster %.1f, reach lit %.1f, reach umbra %.1f, search lit %.1f, disk lit %.1f, disk umbra %.1f, filtered %.1f\n",
                 px / 1e6, 100 * st.pathNoCaster / px, 100 * st.pathRegionLit / px, 100 * st.pathRegionUmbra / px, 100 * st.pathSearchLit / px, 100 * st.pathDiskLit / px,
                 100 * st.pathDiskUmbra / px, 100 * st.pathFiltered / px);
        }
        return gateFailures ? 1 : 0;
#endif
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
