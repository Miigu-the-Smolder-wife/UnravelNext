#pragma once
// CPU reference path tracer (INTERFACES_KO.md 10.2; ARCHITECTURE 3 quality definition). Owner: C.
//
// Renders a scene::Scene with the models of INTERFACES_KO.md 8 and no approximation beyond floating point:
//   materials   scene::model::evaluate (the authoritative C++ BRDF), mip-0 bilinear textures, alpha test, normal maps
//   lights      point/spot (analytic, windowed), rect/disk/sphere/tube area lights sampled exactly with MIS
//   sun         uniform disk of angular radius theta_s, radiance E_TOA * color / (pi sin^2 theta_s)
//   atmosphere  participating medium on every path segment (Rayleigh, Mie HG, ozone tent, planet ground albedo):
//               null-free tracking with exact optical depth (Gauss-Legendre on short segments, a verified optical-
//               depth table on long ones), forced in-scattering next-event estimation with sun and lights
//   camera      pinhole, pixel box filter, near plane as in the rasteriser, output radiance (nit) x exposure
// The estimator is unbiased (Russian roulette from reference.russian_roulette_start_bounce). Two independent halves
// of the samples are accumulated so the convergence of every image is measured (relMSE between the halves).
#include "unx/metrics/Metrics.h"
#include "unx/scene/SceneData.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace unx::reference
{
struct CameraSelection
{
    std::string camera;     // scene camera name, or
    std::string path;       // camera path name with 'time' (seconds)
    float time = 0;
};

// The effective pinhole camera (after resolving a path time).
struct ResolvedCamera
{
    float3 position, forward, up;
    float verticalFov = 1.0471976f, nearPlane = 0.05f, ev100 = 14.0f;
    float time = 0;  // scene time for deformations
};
ResolvedCamera resolveCamera(const scene::Scene& scene, const CameraSelection& selection);

// Blocks while any of the hold files is in place (same rule as RenderSettings::pauseWhileExists); callers use it before
// heavy setup (BVH and atmosphere-table builds run on every core).
void waitWhileHeld(const std::vector<std::filesystem::path>& files);

struct RenderSettings
{
    uint32_t width = 0, height = 0;
    uint32_t samplesPerPixel = 0;          // reference.samples_per_pixel (>= 4096 for cached references)
    uint32_t russianRouletteStart = 0;     // reference.russian_roulette_start_bounce
    uint32_t samplesPerPass = 64;          // progress / checkpoint granularity (per half); does not change the result
    uint64_t seed = 0x5EED;
    std::filesystem::path checkpoint;      // optional: resumable accumulation state
    double checkpointSeconds = 600;        // write the checkpoint at most this often
    // Pause all render workers while any of these files exists: the GPU measurement lock's holder record
    // (.gpulock/current.json; a record whose holder process is gone is stale and ignored) and a manual hold marker
    // (.gpulock/HOLD, e.g. while the user plays a game). Performance measurements and the user's foreground work must
    // not share the CPU with a reference render. Polled every 200 ms; workers check before every pixel row. Paused
    // time is excluded from RenderStats::seconds.
    std::vector<std::filesystem::path> pauseWhileExists;
    // Testing only: false replaces forced in-scattering NEE with NEE at the tracked collision points. Both estimators
    // have the same expectation; the reference always uses the forced one (lower variance for sky light).
    bool forcedInScattering = true;
    // Diagnostics only: keep the light that underwent between volumeOrderMin and volumeOrderMax atmosphere scattering
    // events. The default keeps everything; the reference tool puts a non-default window in the cache key. Only
    // atmosphere events are counted: surface bounces (the planet ground and scene surfaces) are not, so the window 1:1
    // also holds light scattered once in the air after any number of surface reflections. Pure single scattering
    // needs a scene with black surfaces and ground (e.g. a --write-scene .unxscene edited to albedo 0).
    uint32_t volumeOrderMin = 0, volumeOrderMax = 0xFFFFFFFFu;
};

struct RenderStats
{
    double seconds = 0;        // rendering time, pauses excluded
    double pausedSeconds = 0;  // time spent paused (GPU measurement lock, manual hold)
    uint64_t paths = 0, rays = 0, truncatedPaths = 0, nanSamples = 0;
    uint32_t samplesDone = 0;  // per pixel, both halves together
};

struct RenderOutput
{
    metrics::Image image;       // mean of both halves: radiance (nit) x exposure, before tonemapping
    metrics::Image halfA, halfB;
    double halvesRelMse = 0;    // relMSE(halfA, halfB): the sample-noise level of each half
    RenderStats stats;
};

// Identity of the surface a primary sub-sample sees (census, ARCHITECTURE 1.2): instance << 32 | mesh triangle,
// kSkyIdentity for no hit.
constexpr uint64_t kSkyIdentity = ~0ull;

class PathTracer
{
public:
    explicit PathTracer(const scene::Scene& scene, uint32_t threads = 0);
    ~PathTracer();
    PathTracer(const PathTracer&) = delete;
    PathTracer& operator=(const PathTracer&) = delete;

    using Progress = std::function<void(const RenderStats&)>;
    RenderOutput render(const ResolvedCamera& camera, const RenderSettings& settings, const Progress& progress = {});

    // 16 stratified (4 x 4) primary sub-samples per pixel plus the pixel-centre sample (index 16), with alpha test.
    // Returns width * height * 17 identities.
    std::vector<uint64_t> primaryIdentities(const ResolvedCamera& camera, uint32_t width, uint32_t height,
                                            const std::vector<std::filesystem::path>& pauseWhileExists = {});

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};
} // namespace unx::reference
