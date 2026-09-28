#pragma once
// Reflections of the R track (ARCHITECTURE 2.6), main view. One instance per FrameRenderer (track state "R.reflection").
// Per frame (after GI: the screen probes are the G path's control variate and the K path's source):
//   r.refl.begin -> r.refl.classify -> r.refl.args -> r.refl.trace (indirect DispatchRays) -> r.refl.resolve
// K pixels are left to M (Reflection.hlsli). Planar mirrors: triangles on which a reflection camera is exact are
// clustered by plane. Each frame classify counts, per visible candidate plane, the mirror-smooth pixels on it (read back
// framesInFlight frames later). Raster or rays is chosen per plane by measured cost (design 2.6 cost formula, user
// policy "raster vs rays by cost"): a plane gets a reflection camera (FrameServices::renderView, INTERFACES 5.4; the
// same geometry and shading path as the main view) when
//     pixels x c_ray  >  cost of its view,
// c_ray = this system's own GPU timestamps around the reflection trace / its rays (running average), the view's cost =
// its own bracketing timestamps when it ran within the last second, else a + b x (its mirror pixels: the view draws only
// those, ViewDesc::planarMask, INTERFACES v1.22/v1.25), with a and b a least-squares line through the measured views
// (weights decaying by 1/16 per view) seeded with two prior points from reflection.planar_view_fixed_ms and
// reflection.planar_view_ns_per_px. Both terms are fitted: charging a large fixed view cost to b alone made b so large
// that no plane could win and no view ran again to correct it. 10 % hysteresis. Since cost >= a + b x pixels, a
// plane can pay off only if c_ray > b and pixels > a / (c_ray - b): that exact bound prunes the plane hierarchy (with
// the pixel bound from area, distance and corner pixel density, and the screen rectangle). The exact set (original BLASes of characters and wind
// foliage near curved mirrors) is not implemented yet.
#include "unx/gi/GiSystem.h"
#include "unx/render/Frame.h"
#include "unx/rt/RayScene.h"

namespace unx::render::refl
{
struct ReflectionSettings  // from Config/quality/reflection.toml
{
    float kHalfAngle = 0;        // radians: narrow-lobe half-angle at or above which the K path applies
    float mirrorRoughness = 0;   // perceptual roughness below which a pixel takes one ray (M path)
    uint32_t raysPerSample = 0;  // G path rays per sample
    uint32_t maxSpacing = 0;     // G sample spacing bound in px (tile-limited to 8)
    uint32_t planarViewsMax = 0; // reflection cameras per frame (largest pixel counts first; the rest use rays)
    float planarViewFixedMs = 0;     // prior fixed cost of a reflection view (a)
    float planarViewNsPerPixel = 0;  // prior cost per mirror pixel of a view (b)
    uint32_t experimentDisable = 0;  // cost attribution only (ReflectionHit.hlsli); 0 in the shipped configuration
    uint32_t statsLogFrames = 0;     // reflection.stats_log_frames: log the GI/reflection counters every N frames (0 = off)
    uint32_t temporalHistoryMax = 0; // reflection.temporal_history_max: running mean over at most this many frames
    float temporalLobeShift = 0;     // reflection.temporal_lobe_shift: reflected-direction travel over the window / lobe
    // debug.deterministic: the planar view / ray choice from the priors alone (planarRayNs, the view's prior a + b x),
    // never from measured GPU times (they differ between runs, and a plane drawn by a camera or by rays differs in value).
    bool deterministic = false;
    float planarRayNs = 0;  // reflection.planar_ray_ns: prior cost per traced reflection ray
    static ReflectionSettings fromQuality(const QualityConfig& q);
};

class ReflectionSystem
{
public:
    static ReflectionSystem& get(FramePassContext& fc);
    // This renderer's instance, or null before its first frame (diagnostics, gates).
    static ReflectionSystem* find(TrackState& state);
    ReflectionSystem(Device& device, ShaderLibrary& shaders, const QualityConfig& quality);
    ~ReflectionSystem();
    ReflectionSystem(const ReflectionSystem&) = delete;
    ReflectionSystem& operator=(const ReflectionSystem&) = delete;

    // Declares the reflection passes of the main view and creates view.reflection. Needs this frame's GI
    // (view.screenProbes, FrameResources::giCache) and ray scene.
    void record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays);
    // Refraction rays of a caller's job list (FrameServices::traceRefractions: W's water R-W2, A's glass R-2;
    // RefractionTrace.hlsl), with the constants this frame's record() used (sky, sun, cache, VSM, scene). Nothing when
    // record() did not run this frame (the caller keeps its fallback: alpha 0 in its results).
    void recordRefraction(FramePassContext& fc, BufferRef jobs, BufferRef results, uint32_t maxJobs);
    // Constant sky radiance (nits) and sun illuminance (lux) when S's atmosphere LUTs are absent (tests).
    void setConstantSky(float3 radiance, float3 sunIlluminance)
    {
        m_skyRadiance = radiance;
        m_sunIlluminance = sunIlluminance;
    }
    const ReflectionSettings& settings() const { return m_settings; }
    struct Stats
    {
        uint32_t jobs = 0, mirrorJobs = 0, glossyJobs = 0, glossyPixels = 0;
        uint32_t planarViews = 0, planarPixels = 0;  // views rendered last frame and their pixel counts (as last read back)
        uint32_t planarCandidates = 0;                // visible candidate planes counted last frame
        uint32_t planarLargestPixels = 0;             // largest read-back pixel count of one plane
        float planarSelectMs = 0;                     // CPU time of last frame's plane query and camera choice
        uint32_t planarRectPixels = 0;                // screen rectangles of the views rendered last frame
        float rayNs = 0;                              // measured reflection trace cost per ray (running average)
        float viewNsPerPixel = 0;                     // view cost per mirror pixel, b (fit of the measured views; prior until views ran)
        float viewFixedMs = 0;                        // view cost independent of its pixels, a (same fit)
        float viewMs = 0;                             // measured cost of last read-back frame's views (bracketing timestamps)
    };
    // Planar reflectors of the scene (built on first use). Tests disable the planar path to compare it with rays.
    void setPlanarEnabled(bool enabled) { m_planarEnabled = enabled; }
    // Tests and capture modes: every counted candidate plane gets a camera (up to planar_views_max), without the cost choice.
    void setPlanarForced(bool forced) { m_planarForced = forced; }
    size_t planarReflectorCount() const { return m_planes.size(); }
    // Counters of the last completed frame (blocking readback).
    Stats readStats();
    // This frame's per-pixel mode texture (ReflectionInternal.hlsli), for tests and diagnostics.
    TextureRef modes() const { return m_modes; }
    // Main-view pixel rectangle of this frame's reflection camera 'view' (0 .. planar_views_max - 1), from the camera
    // choice until the next record: tests map a view pixel to its main-view pixel (view pixel + origin).
    struct Rect
    {
        uint32_t x = 0, y = 0, width = 0, height = 0;
    };
    Rect planarViewRect(uint32_t view) const { return m_viewRects[view]; }

private:
    void ensureHistory(uint32_t width, uint32_t height);
    struct RefractionInputs  // the frame's reflection constants, kept for recordRefraction
    {
        bool valid = false, atmosphere = false;
        uint64_t frameIndex = 0;
        float3 sky{}, sun{};
        float rayLength = 0;
        TextureRef luts[4];
        BufferRef cache;
        rt::RayScene::VsmRefs vsm;
        uint32_t frame = 0, experiment = 0, scene[8] = {};
        rt::RayScene* rays = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;
        int variant = 0;
    } m_refract;
    ComPtr<ID3D12Resource> m_streamTable;  // per frame slot: each triangle stream slot's vertex buffer SRV (256 B)
    uint8_t* m_streamTableMapped = nullptr;
    uint32_t m_streamTableSrv[4] = {};
    ComPtr<ID3D12Resource> m_refractTemplate;  // the refraction library's DispatchRays description per sky variant (upload)
    Device& m_device;
    ReflectionSettings m_settings;
    ComPtr<ID3D12Resource> m_history;   // R16G16_FLOAT: last frame's reflection hit distance (G spacing), hit motion
    uint32_t m_historyWidth = 0, m_historyHeight = 0;
    // Time integration (ReflectionAccumulate.hlsl), ping-pong by frame parity: RGBA16F running mean + n, RG32_UINT key
    // (scene instance + 1, linear depth). m_accumReset: the next frame ignores the history (new textures, a scene
    // revision, a discontinuity).
    ComPtr<ID3D12Resource> m_accum[2], m_accumKeys[2];
    uint32_t m_accumParity = 0, m_accumSceneRevision = 0;
    bool m_accumReset = true;
    float3 m_prevCamera{};
    ComPtr<ID3D12Resource> m_arguments;  // raw: job counters, the trace descriptions (SKY0, SKY1), the shadow description,
                                         // the shade and combine Dispatch arguments (ReflectionSystem.cpp offsets)
    ComPtr<ID3D12CommandSignature> m_dispatchSignature;  // one D3D12_DISPATCH_ARGUMENTS
    ComPtr<ID3D12Resource> m_statsReadback;  // 4 slots x 256 B of the GI header (reflection.stats_log_frames)
    const uint32_t* m_statsMapped = nullptr;
    uint32_t m_rayCapacity = 1u << 20;   // ray slots of the rays buffer (grows with the traced rays read back)
    float3 m_skyRadiance{}, m_sunIlluminance{};
    TextureRef m_modes;
    Rect m_viewRects[4];  // kPlanarMax (ReflectionSystem.cpp)

    struct PlanarReflector
    {
        float4 plane;        // world: n.p + w = 0, n on the reflective (front) side
        float3 lo, hi;       // world bounds of its triangles
        float area = 0;      // world area of its triangles (m^2)
        uint32_t triangles = 0;
    };
    // Bounding volume hierarchy over the planes: a node holds the bounds and the largest area of its planes, so a frame
    // visits only planes whose pixel-count bound can reach the cost choice's threshold.
    struct PlaneNode
    {
        float3 lo, hi;
        float maxArea = 0;
        uint32_t first = 0, count = 0;  // leaf: m_planeOrder[first, first + count); inner (count 0): children first, second
        uint32_t second = 0;
    };
    void buildPlanes(const GpuScene& scene);
    uint32_t buildPlaneNodes(uint32_t begin, uint32_t end);
    std::vector<PlanarReflector> m_planes;
    std::vector<PlaneNode> m_planeNodes;
    std::vector<uint32_t> m_planeOrder;
    std::vector<uint64_t> m_planeLastSeen, m_planeRunStart;  // frames a plane was a candidate: last, start of the current run
    uint32_t m_planesRevision = 0xFFFFFFFFu;
    bool m_planarEnabled = true, m_planarForced = false;
    ComPtr<ID3D12Resource> m_planarRing;      // upload ring: per frame { candidates, views, pad, 64 x { plane, rect } }
    uint8_t* m_planarMapped = nullptr;
    uint32_t m_planarSrv = 0xFFFFFFFFu;
    ComPtr<ID3D12Resource> m_planarCounts;    // per candidate of this frame: mirror-smooth pixels on its plane
    ComPtr<ID3D12Resource> m_planarReadback;  // per slot: counts, timestamps (trace, views), job counters; read framesInFlight later
    const uint8_t* m_readbackMapped = nullptr;
    ComPtr<ID3D12QueryHeap> m_timestamps;     // per slot: trace begin/end, view begin/end x kPlanarMax
    double m_tickMs = 0;
    float m_rayNs = 0, m_viewNsPerPixel = 0, m_viewFixedNs = 0, m_lastViewMs = 0;
    double m_viewFit[5] = {};  // decaying weighted sums over measured views (x = mirror pixels, y = ns): w, x, y, xx, xy
    void addViewSample(double pixels, double ns, double weight);
    std::vector<uint32_t> m_slotViewPlanes[4], m_slotViewPixels[4];  // plane and mirror pixels of each view, per slot
    std::vector<float> m_planeViewMs;         // last measured cost of the plane's view
    std::vector<uint64_t> m_planeViewFrame;   // frame of that measurement
    std::vector<uint64_t> m_planeCameraFrame; // last frame the plane had a camera (hysteresis)
    uint32_t m_lastRectPixels = 0;
    std::vector<uint32_t> m_slotPlanes[4];    // plane of each candidate, per ring slot
    std::vector<uint64_t> m_slotFrame;        // frame that wrote each slot
    std::vector<uint32_t> m_planePixels;      // last read-back count per plane
    uint32_t m_lastPlanarViews = 0, m_lastPlanarPixels = 0, m_lastCandidates = 0;
    float m_lastSelectMs = 0;
};
} // namespace unx::render::refl
