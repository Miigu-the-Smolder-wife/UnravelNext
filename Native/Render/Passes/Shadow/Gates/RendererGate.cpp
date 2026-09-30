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
//
// Motion, cuts, lighting changes and several captures per run (RENDERER_REDESIGN_V2 3.3, P0): the camera follows a pose
// function P(t) of a time t: path 0 of the scene (--moving), or a synthetic motion of the chosen camera
// (--path-rotate deg/s: yaw about world up; --path-translate v[,strafe]: m/s along the horizontal forward and to the
// right). t = (frame - --motion-start F) / 60 while moving; --path-time T holds the camera still at P(T) (the reference of a
// moving sequence's frame at that time: 600 still frames there). --cut-at F[:T2]: from frame F on, t = T2 (+ the frames
// since F while moving), and frame F is a camera cut (FrameContext::discontinuity kDiscontinuityCut, no previous view).
// --light-toggle-at F,i: at frame F light i goes off (on again when it was off; GpuScene::setLights). --sun-step-at F,deg:
// the sun turns by deg at frame F (about the same axis as --sun-deg-per-s).
// --capture-frames a,b,c: captures of these frame indices (the files get _f<frame> before .pfm) instead of the last
// frame; --capture-layers final,gi,refl,shadow,reflmode,depth: besides the capture (final), the main view's internal
// layers of the same frames as PFM (_<layer>): gi = view.giIrradiance (E x near occlusion x exposure; an _alpha file with
// its data flag), refl = view.reflection (radiance, weight in _alpha), shadow = the first three light slots of
// view.shadowVisibility (0..1), reflmode = R's per-pixel mode texture (r = mode 0 K 1 M 2 G 3 planar, g = log2 of the G
// spacing, b = own job bit), depth = device depth (reversed Z).
// --frame-log FILE.csv: per frame the camera, the camera-motion disocclusion d (GateDisocclusion.hlsl: surface pixels the
// previous frame did not see) and the tracks' counters of their last completed frame (GI: live, requested, selected,
// created, resets, evicted; reflection: jobs, mirror, glossy, planar pixels; VSM: dirty, requested) - blocking readbacks
// every frame: correctness runs only, never with timings.
// --gi-cache-stats FILE.json: after the last frame, R's GI cache read back and analysed on the CPU (GateGiCache.h: parent
// against child converged irradiance, the entries read in the last frame by their updates).
#include "GateGiCache.h"
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

// Synthetic game-speed motion of a camera (3.3 --path-rotate / --path-translate): yaw about world up by rate x t, then a
// move along the turned camera's horizontal forward (and right: strafe) at the given speeds.
scene::Camera cameraMotion(scene::Camera c, double t, float yawDegPerS, float forwardMps, float strafeMps)
{
    const float a = yawDegPerS * 0.01745329252f * (float)t, ca = std::cos(a), sa = std::sin(a);
    auto yaw = [&](float3 v) { return float3{ v.x * ca + v.z * sa, v.y, -v.x * sa + v.z * ca }; };
    c.forward = normalize(yaw(c.forward));
    c.up = normalize(yaw(c.up));
    float3 f{ c.forward.x, 0, c.forward.z };
    const float fl = std::sqrt(f.x * f.x + f.z * f.z);
    f = fl > 1e-6f ? f * (1.0f / fl) : float3{ 0, 0, -1 };
    const float3 right = normalize(cross(f, float3{ 0, 1, 0 }));
    c.position = c.position + f * (forwardMps * (float)t) + right * (strafeMps * (float)t);
    return c;
}

std::vector<uint64_t> parseFrameList(const std::string& v)
{
    std::vector<uint64_t> r;
    size_t at = 0;
    while (at < v.size())
    {
        const size_t comma = v.find(',', at);
        r.push_back(std::stoull(v.substr(at, comma == std::string::npos ? std::string::npos : comma - at)));
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return r;
}

// A texture read back for a capture (--capture, --capture-frames, --capture-layers): its copy footprint and file.
struct CaptureSlot
{
    std::string layer, path;
    uint64_t frame = UINT64_MAX;  // UINT64_MAX: every frame copies (the last one wins)
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0, height = 0;
    bool written = false;
};
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

// PFM ("PF", width height, -1 = little endian, rows bottom to top, RGB float).
void writePfm(const std::string& path, uint32_t w, uint32_t h, const std::vector<float>& rgbTopDown)
{
    const std::string header = "PF\n" + std::to_string(w) + " " + std::to_string(h) + "\n-1.0\n";
    std::ofstream file(path, std::ios::binary);
    if (!file) fail("cannot write %s", path.c_str());
    file.write(header.data(), (std::streamsize)header.size());
    for (uint32_t y = h; y-- > 0;) file.write(reinterpret_cast<const char*>(&rgbTopDown[(size_t)y * w * 3]), (std::streamsize)w * 3 * sizeof(float));
}

// A capture slot's readback as RGB floats (and the fourth channel when the format has one): RGBA32F and RGBA16F as they
// are, R32_UINT as the 8-bit fields 0..2 (shadow slots / 255) - or, for the reflection mode texture, its mode, spacing and
// own-job bit - and one-channel floats (depth) repeated.
void convertCapture(const CaptureSlot& c, const uint8_t* mapped, std::vector<float>& rgb, std::vector<float>& alpha)
{
    rgb.assign((size_t)c.width * c.height * 3, 0.0f);
    alpha.clear();
    const bool four = c.format == DXGI_FORMAT_R32G32B32A32_FLOAT || c.format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (four) alpha.assign((size_t)c.width * c.height, 0.0f);
    for (uint32_t y = 0; y < c.height; ++y)
    {
        const uint8_t* row = mapped + c.footprint.Offset + (size_t)y * c.footprint.Footprint.RowPitch;
        for (uint32_t x = 0; x < c.width; ++x)
        {
            float* o = &rgb[((size_t)y * c.width + x) * 3];
            float a = 0;
            switch (c.format)
            {
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                std::memcpy(o, row + (size_t)x * 16, 12);
                std::memcpy(&a, row + (size_t)x * 16 + 12, 4);
                break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            {
                uint16_t hv[4];
                std::memcpy(hv, row + (size_t)x * 8, 8);
                for (int k = 0; k < 3; ++k) o[k] = halfToFloat(hv[k]);
                a = halfToFloat(hv[3]);
                break;
            }
            case DXGI_FORMAT_R32_UINT:
            {
                uint32_t v;
                std::memcpy(&v, row + (size_t)x * 4, 4);
                if (c.layer == "reflmode")
                {
                    o[0] = (float)(v & 3u);
                    o[1] = (float)((v >> 2) & 7u);
                    o[2] = (float)((v >> 5) & 1u);
                }
                else
                    for (int k = 0; k < 3; ++k) o[k] = (float)((v >> (8 * k)) & 0xFFu) / 255.0f;
                break;
            }
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT:
            case DXGI_FORMAT_R32_TYPELESS:
                std::memcpy(o, row + (size_t)x * 4, 4);
                o[1] = o[2] = o[0];
                break;
            default:
                fail("capture of layer %s: format %d is not handled", c.layer.c_str(), (int)c.format);
            }
            if (four) alpha[(size_t)y * c.width + x] = a;
        }
    }
}

std::string capturePathFor(const std::string& base, const std::string& layer, uint64_t frame, bool perFrame)
{
    std::string stem = base, ext;
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".pfm") == 0)
    {
        stem = base.substr(0, base.size() - 4);
        ext = ".pfm";
    }
    else
        ext = ".pfm";
    if (layer != "final") stem += "_" + layer;
    if (perFrame) stem += "_f" + std::to_string(frame);
    return stem + ext;
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
        // P0 motion and change options (see the head comment).
        double pathTime = -1;  // --path-time T: still at P(T); negative = not given
        // --path-time-list T1,T2,.. --segment-frames N: still references of several times in one run (N frames at P(T1),
        // then a cut to P(T2), ...; the world-space caches carry over, a converged still does not depend on its history).
        std::vector<double> pathTimes;
        uint64_t segmentFrames = 600;
        float rotateDegPerS = 0, translateMps = 0, strafeMps = 0;
        bool synthetic = false;
        uint64_t motionStart = 0, cutAt = UINT64_MAX, lightToggleAt = UINT64_MAX, sunStepAt = UINT64_MAX;
        double cutTime = 0;
        uint32_t lightToggleIndex = 0;
        float sunStepDeg = 0;
        std::vector<uint64_t> captureFrames;
        std::vector<std::string> captureLayers{ "final" };
        std::string frameLogPath, giCacheStatsPath;
        bool listLights = false;
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
            else if (a == "--path-time") pathTime = std::stod(next());
            else if (a == "--path-time-list")
            {
                const std::string v = next();
                size_t at = 0;
                while (at < v.size())
                {
                    const size_t comma = v.find(',', at);
                    pathTimes.push_back(std::stod(v.substr(at, comma == std::string::npos ? std::string::npos : comma - at)));
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
            }
            else if (a == "--segment-frames") segmentFrames = std::stoull(next());
            else if (a == "--path-rotate")
            {
                rotateDegPerS = std::stof(next());
                synthetic = true;
            }
            else if (a == "--path-translate")
            {
                const std::string v = next();
                const size_t comma = v.find(',');
                translateMps = std::stof(v.substr(0, comma));
                if (comma != std::string::npos) strafeMps = std::stof(v.substr(comma + 1));
                synthetic = true;
            }
            else if (a == "--motion-start") motionStart = std::stoull(next());
            else if (a == "--cut-at")
            {
                const std::string v = next();
                const size_t colon = v.find(':');
                cutAt = std::stoull(v.substr(0, colon));
                if (colon != std::string::npos) cutTime = std::stod(v.substr(colon + 1));
            }
            else if (a == "--light-toggle-at")
            {
                const std::string v = next();
                const size_t comma = v.find(',');
                if (comma == std::string::npos) fail("--light-toggle-at F,light");
                lightToggleAt = std::stoull(v.substr(0, comma));
                lightToggleIndex = (uint32_t)std::stoul(v.substr(comma + 1));
            }
            else if (a == "--sun-step-at")
            {
                const std::string v = next();
                const size_t comma = v.find(',');
                if (comma == std::string::npos) fail("--sun-step-at F,deg");
                sunStepAt = std::stoull(v.substr(0, comma));
                sunStepDeg = std::stof(v.substr(comma + 1));
            }
            else if (a == "--capture-frames") captureFrames = parseFrameList(next());
            else if (a == "--capture-layers")
            {
                captureLayers.clear();
                const std::string v = next();
                size_t at = 0;
                while (at <= v.size())
                {
                    const size_t comma = v.find(',', at);
                    const std::string l = v.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                    if (l != "final" && l != "gi" && l != "refl" && l != "shadow" && l != "reflmode" && l != "depth")
                        fail("--capture-layers: unknown layer '%s' (final, gi, refl, shadow, reflmode, depth)", l.c_str());
                    captureLayers.push_back(l);
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
            }
            else if (a == "--list-lights") listLights = true;  // print the scene's lights and cameras and stop (no GPU)
            else if (a == "--frame-log") frameLogPath = next();
            else if (a == "--gi-cache-stats") giCacheStatsPath = next();
            else if (a == "--origin-shift")
            {
                const std::string v = next();
                const size_t c0 = v.find(','), c1 = v.find(',', c0 + 1);
                if (c0 == std::string::npos || c1 == std::string::npos) fail("--origin-shift x,y,z");
                shiftBy = { std::stof(v.substr(0, c0)), std::stof(v.substr(c0 + 1, c1 - c0 - 1)), std::stof(v.substr(c1 + 1)) };
            }  // A4 metering instead of the camera's EV100
            else fail("unknown argument %s", a.c_str());
        }
        if (capturePath.empty() && (!captureFrames.empty() || captureLayers.size() != 1 || captureLayers[0] != "final"))
            fail("--capture-frames / --capture-layers name files after --capture or --capture-output");
        if (moving && synthetic) fail("--moving (path 0) and --path-rotate / --path-translate are two different motions");
        std::sort(captureFrames.begin(), captureFrames.end());
        captureFrames.erase(std::unique(captureFrames.begin(), captureFrames.end()), captureFrames.end());
        if (listLights)
        {
            const scene::Scene ls = scene::load(sceneName);
            for (size_t i = 0; i < ls.lights.size(); ++i)
            {
                const scene::Light& l = ls.lights[i];
                logf("light %zu: type %u at (%.3f, %.3f, %.3f), intensity %g, colour (%.3f, %.3f, %.3f), range %g, size (%.3f, %.3f), shadow %d\n", i, (uint32_t)l.type,
                     l.position.x, l.position.y, l.position.z, l.intensity, l.color.x, l.color.y, l.color.z, l.range, l.size.x, l.size.y, (int)l.castShadow);
            }
            for (const scene::Camera& c : ls.cameras)
                logf("camera '%s' at (%.3f, %.3f, %.3f) forward (%.3f, %.3f, %.3f), ev100 %.2f\n", c.name.c_str(), c.position.x, c.position.y, c.position.z, c.forward.x,
                     c.forward.y, c.forward.z, c.ev100);
            for (const scene::CameraPath& p : ls.paths)
                logf("path '%s': %zu keys over %.2f s\n", p.name.c_str(), p.keys.size(), p.keys.empty() ? 0.0f : p.keys.back().time - p.keys.front().time);
            logf("sun direction (%.4f, %.4f, %.4f), %g lux\n", ls.sun.direction.x, ls.sun.direction.y, ls.sun.direction.z, ls.sun.illuminance);
            return 0;
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
        if (lightToggleAt != UINT64_MAX && lightToggleIndex >= s.lights.size())
            fail("--light-toggle-at: light %u of %zu", lightToggleIndex, s.lights.size());
        const std::vector<scene::Light> lights0 = s.lights;
        if (lightToggleAt != UINT64_MAX)
        {
            const scene::Light& l = s.lights[lightToggleIndex];
            logf("light %u: type %u at (%.3f, %.3f, %.3f), intensity %g, range %g, shadow %d; toggled at frame %llu\n", lightToggleIndex, (uint32_t)l.type, l.position.x,
                 l.position.y, l.position.z, l.intensity, l.range, (int)l.castShadow, (unsigned long long)lightToggleAt);
        }
        if (moving && (s.paths.empty() || s.paths[0].keys.size() < 2))
            logf("--moving: the scene has no camera path 0: the camera stays still (use --path-rotate / --path-translate for motion)\n");
        // The pose function and its time per frame (head comment).
        if (!pathTimes.empty() && (pathTime >= 0 || cutAt != UINT64_MAX)) fail("--path-time-list replaces --path-time and --cut-at");
        if (!pathTimes.empty() && segmentFrames == 0) fail("--segment-frames 0");
        if (!pathTimes.empty()) pathTime = pathTimes[0];
        const bool advancing = (moving || synthetic) && pathTime < 0;
        const double baseTime = pathTime < 0 ? 0.0 : pathTime;
        auto timeOf = [&](uint64_t frame) {
            if (!pathTimes.empty()) return pathTimes[std::min<uint64_t>(frame / segmentFrames, pathTimes.size() - 1)];
            const bool cut = frame >= cutAt;
            const double base = cut ? cutTime : baseTime;
            const int64_t since = (int64_t)frame - (int64_t)(cut ? cutAt : motionStart);
            return advancing ? base + (double)std::max<int64_t>(since, 0) / 60.0 : base;
        };
        auto cameraFor = [&](uint64_t frame) {
            const double t = timeOf(frame);
            if (synthetic)
            {
                scene::Camera c = cameraAt(s, false, 0, cameraName);
                return cameraMotion(c, t, rotateDegPerS, translateMps, strafeMps);
            }
            return cameraAt(s, moving || pathTime >= 0 || cutAt != UINT64_MAX ? !s.paths.empty() && s.paths[0].keys.size() >= 2 : false, t, cameraName);
        };
        ClusterData clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality));
        logf("scene %s (%s), %zu instances, %zu clusters, camera %s\n", sceneName.c_str(), scene::contentHash(s).substr(0, 16).c_str(), s.instances.size(),
             clusters.clusters.size(), moving ? "path 0 (moving)" : (cameraName.empty() ? "0 (static)" : cameraName.c_str()));
        Device device({});
        std::vector<CaptureSlot> captures;  // --capture (last frame wins) or --capture-frames, per layer
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
            float4x4 prev = ViewDesc::fromCamera(cameraFor(0), res.width, res.height, {}).viewProj;
            // Each resolution starts from the scene's own lights and sun (--light-toggle-at changed the source).
            {
                std::vector<uint32_t> changed;
                for (uint32_t i = 0; i < (uint32_t)s.lights.size(); ++i)
                    if (s.lights[i].intensity != lights0[i].intensity)
                    {
                        s.lights[i] = lights0[i];
                        changed.push_back(i);
                    }
                if (!changed.empty()) gpuScene.setLights(changed);
            }
            captures.clear();
            // --frame-log: camera-motion disocclusion counts per frame (GateDisocclusion.hlsl) into slots read after the run,
            // the tracks' counters read every frame (blocking).
            constexpr uint32_t kFrameSlots = 1u << 14;
            ComPtr<ID3D12Resource> disoccCounts, depthCopy[2];
            uint32_t depthCopyW = 0, depthCopyH = 0;
            bool depthCopyValid = false;
            float4x4 prevInternalViewProj{};
            struct FrameLogRow
            {
                uint64_t frame = 0;
                double t = 0;
                float3 position{}, forward{};
                uint32_t cut = 0, lightOn = 1;
                float sunDeg = 0;
                std::string tracks;
            };
            std::vector<FrameLogRow> frameLog;
            if (!frameLogPath.empty()) disoccCounts = s_detail::createBuffer(device, L"gate disocclusion counts", (uint64_t)kFrameSlots * 16);
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
                scene::Camera cam = cameraFor(frame);
                const bool cutNow = frame == cutAt || (!pathTimes.empty() && frame > 0 && frame % segmentFrames == 0 && frame / segmentFrames < pathTimes.size());
                if (cutNow)
                {
                    fc.discontinuity |= kDiscontinuityCut;
                    prev = ViewDesc::fromCamera(cam, rr.width, rr.height, {}).viewProj;  // no previous view
                }
                if (frame == lightToggleAt)
                {
                    scene::Light& l = s.lights[lightToggleIndex];
                    l.intensity = l.intensity > 0 ? 0.0f : lights0[lightToggleIndex].intensity;
                    const uint32_t index = lightToggleIndex;
                    gpuScene.setLights(std::span<const uint32_t>(&index, 1));
                    logf("frame %llu: light %u intensity -> %g\n", (unsigned long long)frame, index, l.intensity);
                }
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
                float sunTurnDeg = 0;
                if (sunDegPerS != 0 || sunStepAt != UINT64_MAX)
                {
                    // Rodrigues rotation of the initial sun direction (the axis is normal to it); --sun-step-at adds a step.
                    sunTurnDeg = sunDegPerS * (float)fc.time + (frame >= sunStepAt ? sunStepDeg : 0.0f);
                    const float a = sunTurnDeg * 0.01745329252f, ca = std::cos(a), sa = std::sin(a);
                    s.sun.direction = normalize(sun0 * ca + cross(sunAxis, sun0) * sa);
                }
                prev = fc.mainView.viewProj;
                if (!frameLogPath.empty())
                {
                    FrameLogRow row;
                    row.frame = frame;
                    row.t = timeOf(frame);
                    row.position = cam.position;
                    row.forward = cam.forward;
                    row.cut = cutNow ? 1u : 0u;
                    row.lightOn = lightToggleAt != UINT64_MAX ? (s.lights[lightToggleIndex].intensity > 0 ? 1u : 0u) : 1u;
                    row.sunDeg = sunTurnDeg;
                    frameLog.push_back(row);
                }
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
                const bool captureThisFrame = captureFrames.empty() || std::binary_search(captureFrames.begin(), captureFrames.end(), frame);
                if (!capturePath.empty() && captureThisFrame)
                {
                    if (captureUpscaled && !rendered.upscaled.valid())
                        fail("--capture-output: the frame renders at its output resolution (output.render_scale / render_scale_min_height): use --capture");
                    for (const std::string& layer : captureLayers)
                    {
                        TextureRef source;
                        if (layer == "final") source = captureUpscaled ? rendered.upscaled : output;
                        else if (layer == "gi") source = rendered.giIrradiance;
                        else if (layer == "refl") source = rendered.reflection;
                        else if (layer == "shadow") source = rendered.shadowVisibility;
                        else if (layer == "depth") source = rendered.depth;
#if __has_include("unx/refl/ReflectionSystem.h")
                        else if (layer == "reflmode")
                        {
                            if (refl::ReflectionSystem* rsys = refl::ReflectionSystem::find(renderer.trackState())) source = rsys->modes();
                        }
#endif
                        if (!source.valid())
                        {
                            if (frame == (captureFrames.empty() ? frame : captureFrames.front())) logf("capture layer %s: no such texture this frame (skipped)\n", layer.c_str());
                            continue;
                        }
                        const uint64_t key = captureFrames.empty() ? UINT64_MAX : frame;
                        CaptureSlot* slot = nullptr;
                        for (CaptureSlot& c : captures)
                            if (c.layer == layer && c.frame == key) slot = &c;
                        if (!slot)
                        {
                            captures.emplace_back();
                            slot = &captures.back();
                            slot->layer = layer;
                            slot->frame = key;
                            slot->path = capturePathFor(capturePath, layer, frame, !captureFrames.empty());
                            const TextureDesc& td = g.desc(source);
                            slot->format = td.format;
                            slot->width = td.width;
                            slot->height = td.height;
                            D3D12_RESOURCE_DESC rd{};
                            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                            rd.Width = td.width;
                            rd.Height = td.height;
                            rd.DepthOrArraySize = 1;
                            rd.MipLevels = 1;
                            rd.Format = td.format;
                            rd.SampleDesc.Count = 1;
                            UINT rows;
                            UINT64 rowBytes, total;
                            device.d3d()->GetCopyableFootprints(&rd, 0, 1, 0, &slot->footprint, &rows, &rowBytes, &total);
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
                                                                        IID_PPV_ARGS(&slot->buffer)),
                                  "capture readback");
                        }
                        slot->written = true;
                        ID3D12Resource* rb = slot->buffer.Get();
                        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = slot->footprint;
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
                }
                if (disoccCounts && rendered.depth.valid())
                {
                    // --frame-log: the camera-motion disocclusion of this frame (GateDisocclusion.hlsl).
                    const uint32_t w = rendered.view.width, h = rendered.view.height;
                    if (w != depthCopyW || h != depthCopyH)
                    {
                        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                        D3D12_RESOURCE_DESC1 d{};
                        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                        d.Width = w;
                        d.Height = h;
                        d.DepthOrArraySize = d.MipLevels = 1;
                        d.Format = DXGI_FORMAT_R32_FLOAT;
                        d.SampleDesc.Count = 1;
                        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                        for (ComPtr<ID3D12Resource>& t : depthCopy)
                        {
                            if (t) device.deferRelease(t);
                            check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0,
                                                                         nullptr, IID_PPV_ARGS(&t)),
                                  "gate depth copy");
                        }
                        depthCopyW = w;
                        depthCopyH = h;
                        depthCopyValid = false;
                    }
                    const bool hasPrev = depthCopyValid && !cutNow;
                    const uint32_t parity = (uint32_t)(frame & 1);
                    const TextureDesc cd{ "gate depth copy", w, h, 1, 1, DXGI_FORMAT_R32_FLOAT };
                    const TextureRef copyNow = g.importTexture(depthCopy[parity].Get(), cd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
                    const TextureRef copyPrev = g.importTexture(depthCopy[parity ^ 1].Get(), cd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
                    const BufferRef counts = g.importBuffer(disoccCounts.Get(), { "gate disocclusion counts", (uint64_t)kFrameSlots * 16, 0 });
                    const float4x4 m = mul(prevInternalViewProj, rendered.view.invViewProj);
                    const uint32_t slot = (uint32_t)(frame % kFrameSlots);
                    const TextureRef depth = rendered.depth;
                    for (const uint32_t clear : { 1u, 0u })
                        g.addPass(clear ? "s.gate.disocclusion.clear" : "s.gate.disocclusion", QueueType::Compute,
                                  [&](PassBuilder& b) {
                                      if (!clear)
                                      {
                                          b.use(depth, Use::SrvCompute);
                                          b.use(copyNow, Use::UavCompute);
                                          b.use(copyPrev, Use::UavCompute);
                                      }
                                      b.use(counts, Use::UavCompute);
                                      b.keep();
                                  },
                                  [&shaders, depth, copyNow, copyPrev, counts, slot, w, h, clear, hasPrev, m](PassContext& c) {
                                      uint32_t k[24] = { clear ? 0u : c.srv(depth), clear || !hasPrev ? 0xFFFFFFFFu : c.uav(copyPrev), clear ? 0u : c.uav(copyNow), c.uav(counts),
                                                         w, h, slot, clear };
                                      std::memcpy(&k[8], &m.m[0][0], 64);
                                      c.cmd->SetPipelineState(shaders.compute("Passes/Shadow/Gates/GateDisocclusion"));
                                      c.computeConstants(k, 24);
                                      c.cmd->Dispatch(clear ? 1 : (w + 7) / 8, clear ? 1 : (h + 7) / 8, 1);
                                  });
                    depthCopyValid = true;
                    prevInternalViewProj = rendered.view.viewProj;
                }
                if (!frameLogPath.empty())
                {
                    // The tracks' counters of their last completed frame (blocking readbacks).
                    std::string tr;
                    const shadow::VsmStats& vst = shadow::stats(renderer.trackState());
                    tr += format(",%llu,%u,%u", (unsigned long long)vst.frame, vst.dirty, vst.requested);
#if __has_include("unx/gi/GiSystem.h")
                    if (gi::GiSystem* gs = gi::GiSystem::find(renderer.trackState()))
                    {
                        const gi::GiStats gst = gs->readStats();
                        tr += format(",%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%.5f", gst.live, gst.requested, gst.selected, gst.created, gst.resets, gst.evicted, gst.epoch,
                                     gst.priors, gst.restarts, gst.selectedYoung, gst.selectedT1, gst.parentDelta);
                    }
                    else
                        tr += ",,,,,,,,,,,,";
#else
                    tr += ",,,,,,,,,,,,";
#endif
#if __has_include("unx/refl/ReflectionSystem.h")
                    if (refl::ReflectionSystem* rsys = refl::ReflectionSystem::find(renderer.trackState()))
                    {
                        const refl::ReflectionSystem::Stats rst = rsys->readStats();
                        tr += format(",%u,%u,%u,%u,%u", rst.jobs, rst.mirrorJobs, rst.glossyJobs, rst.glossyPixels, rst.planarPixels);
                    }
                    else
                        tr += ",,,,,";
#else
                    tr += ",,,,,";
#endif
                    frameLog.back().tracks = tr;
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
            // The harness waits for the GPU at its end: every readback holds its frame. The linear-HDR output is radiance x
            // exposure (Frame.h; 1 / (1.2 2^ev100)), the same units as C's unx_reference images: written as it is.
            for (CaptureSlot& c : captures)
            {
                if (!c.written) continue;
                void* mapped = nullptr;
                check(c.buffer->Map(0, nullptr, &mapped), "map capture");
                std::vector<float> rgb, alpha;
                convertCapture(c, static_cast<const uint8_t*>(mapped), rgb, alpha);
                D3D12_RANGE none{ 0, 0 };
                c.buffer->Unmap(0, &none);
                writePfm(c.path, c.width, c.height, rgb);
                const bool withAlpha = !alpha.empty() && (c.layer == "gi" || c.layer == "refl" || (c.layer == "final" && captureUpscaled));
                if (withAlpha)
                {
                    std::vector<float> a3(alpha.size() * 3);
                    for (size_t k = 0; k < alpha.size(); ++k) a3[k * 3] = a3[k * 3 + 1] = a3[k * 3 + 2] = alpha[k];
                    writePfm(c.path.substr(0, c.path.size() - 4) + "_alpha.pfm", c.width, c.height, a3);
                }
                logf("captured %s %ux%u (%s, frame %s, ev100 %.2f) -> %s\n", c.layer.c_str(), c.width, c.height,
                     c.layer != "final" ? "internal layer" : captureUpscaled ? "after the temporal upscale" : "native",
                     c.frame == UINT64_MAX ? std::to_string(lastFrame).c_str() : std::to_string(c.frame).c_str(), captureEv100, c.path.c_str());
                c.buffer.Reset();
            }
            for (uint64_t f : captureFrames)
                if (f > lastFrame) logf("capture frame %llu was not rendered (last frame %llu)\n", (unsigned long long)f, (unsigned long long)lastFrame);
            if (!frameLogPath.empty())
            {
                // Disocclusion counts from the slots, the rest as recorded.
                const uint64_t bytes = (uint64_t)kFrameSlots * 16;
                ComPtr<ID3D12Resource> rb = s_detail::createBuffer(device, L"gate disocclusion readback", bytes, D3D12_HEAP_TYPE_READBACK);
                CommandList cl = device.acquireCommandList(QueueType::Graphics);
                D3D12_BUFFER_BARRIER bb{ D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                         disoccCounts.Get(), 0, UINT64_MAX };
                D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_BUFFER, 1 };
                group.pBufferBarriers = &bb;
                cl.list->Barrier(1, &group);
                cl.list->CopyBufferRegion(rb.Get(), 0, disoccCounts.Get(), 0, bytes);
                device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
                void* mapped = nullptr;
                check(rb->Map(0, nullptr, &mapped), "map disocclusion counts");
                const uint32_t* counts = static_cast<const uint32_t*>(mapped);
                std::ofstream file(frameLogPath);
                if (!file) fail("cannot write %s", frameLogPath.c_str());
                file << "frame,t,px,py,pz,fx,fy,fz,cut,light_on,sun_turn_deg,surface_px,disoccluded_px,offscreen_px,sky_px,d,"
                        "vsm_stats_frame,vsm_dirty,vsm_requested,gi_live,gi_requested,gi_selected,gi_created,gi_resets,gi_evicted,gi_epoch,gi_priors,gi_restarts,gi_t0,gi_t1,gi_parent_delta,"
                        "refl_jobs,refl_mirror_jobs,refl_glossy_jobs,refl_glossy_pixels,refl_planar_pixels\n";
                for (const FrameLogRow& row : frameLog)
                {
                    const uint64_t slot = row.frame % kFrameSlots;
                    const bool fresh = row.frame + kFrameSlots > lastFrame;
                    const uint32_t sp = fresh ? counts[slot * 4] : 0, dp = fresh ? counts[slot * 4 + 1] : 0, op = fresh ? counts[slot * 4 + 2] : 0,
                                   kp = fresh ? counts[slot * 4 + 3] : 0;
                    file << format("%llu,%.5f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%u,%u,%.3f,%u,%u,%u,%u,%.5f", (unsigned long long)row.frame, row.t, row.position.x, row.position.y,
                                   row.position.z, row.forward.x, row.forward.y, row.forward.z, row.cut, row.lightOn, row.sunDeg, sp, dp, op, kp,
                                   sp ? (double)dp / sp : 0.0)
                         << row.tracks << "\n";
                }
                D3D12_RANGE none{ 0, 0 };
                rb->Unmap(0, &none);
                logf("frame log of %zu frames -> %s\n", frameLog.size(), frameLogPath.c_str());
            }
#if __has_include("unx/gi/GiSystem.h")
            if (!giCacheStatsPath.empty())
            {
                gi::GiSystem* gs = gi::GiSystem::find(renderer.trackState());
                if (!gs || !gs->cache()) fail("--gi-cache-stats: no GI cache in this renderer");
                const uint64_t bytes = gs->cacheBytes();
                ComPtr<ID3D12Resource> rb = s_detail::createBuffer(device, L"gate GI cache readback", bytes, D3D12_HEAP_TYPE_READBACK);
                CommandList cl = device.acquireCommandList(QueueType::Graphics);
                D3D12_BUFFER_BARRIER bb{ D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE,
                                         gs->cache(), 0, UINT64_MAX };
                D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_BUFFER, 1 };
                group.pBufferBarriers = &bb;
                cl.list->Barrier(1, &group);
                cl.list->CopyBufferRegion(rb.Get(), 0, gs->cache(), 0, bytes);
                device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
                void* mapped = nullptr;
                check(rb->Map(0, nullptr, &mapped), "map GI cache");
                const std::string js = "{" + gate_gi::analyse(static_cast<const uint8_t*>(mapped), (size_t)bytes) + "}";
                D3D12_RANGE none{ 0, 0 };
                rb->Unmap(0, &none);
                std::ofstream file(giCacheStatsPath);
                if (!file) fail("cannot write %s", giCacheStatsPath.c_str());
                file << js << "\n";
                logf("GI cache stats (frame %llu) -> %s\n%s\n", (unsigned long long)lastFrame, giCacheStatsPath.c_str(), js.c_str());
            }
#endif
            const shadow::VsmStats& st = shadow::stats(renderer.trackState());
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
