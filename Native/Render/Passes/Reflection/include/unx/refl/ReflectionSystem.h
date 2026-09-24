#pragma once
// Reflections of the R track (ARCHITECTURE 2.6), main view. One instance per FrameRenderer (track state "R.reflection").
// Per frame (after GI: the screen probes are the G path's control variate and the K path's source):
//   r.refl.begin -> r.refl.classify -> r.refl.args -> r.refl.trace (indirect DispatchRays) -> r.refl.resolve
// K pixels are left to M (Reflection.hlsli). Planar mirrors: triangles of mirror-smooth submeshes (static instances) are
// clustered by plane. Each frame classify counts, per visible candidate plane, the mirror-smooth pixels on it; a plane
// gets a reflection camera through FrameServices::renderView (INTERFACES 5.4; same geometry path as the main view, so
// the reflection matches the direct view exactly) when its count (framesInFlight frames old, read back without stalls) reaches
// reflection.planar_min_pixels: the break-even of the design's cost formula (2.6) between the view's fixed cost and the
// per-pixel ray cost it saves. Other planes' pixels take rays. Planes whose exact pixel bound (area, distance, corner
// pixel density; screen rectangle) is below that threshold are not candidates at all. The exact set (original BLASes of characters and wind
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
    uint32_t planarMinPixels = 0;// pixels on a plane above which a reflection camera is cheaper than rays
    static ReflectionSettings fromQuality(const QualityConfig& q);
};

class ReflectionSystem
{
public:
    static ReflectionSystem& get(FramePassContext& fc);
    ReflectionSystem(Device& device, ShaderLibrary& shaders, const QualityConfig& quality);
    ~ReflectionSystem();
    ReflectionSystem(const ReflectionSystem&) = delete;
    ReflectionSystem& operator=(const ReflectionSystem&) = delete;

    // Declares the reflection passes of the main view and creates view.reflection. Needs this frame's GI
    // (view.screenProbes, FrameResources::giCache) and ray scene.
    void record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays);
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
    };
    // Planar reflectors of the scene (built on first use). Tests disable the planar path to compare it with rays.
    void setPlanarEnabled(bool enabled) { m_planarEnabled = enabled; }
    size_t planarReflectorCount() const { return m_planes.size(); }
    // Counters of the last completed frame (blocking readback).
    Stats readStats();
    // This frame's per-pixel mode texture (ReflectionInternal.hlsli), for tests and diagnostics.
    TextureRef modes() const { return m_modes; }

private:
    void ensureHistory(uint32_t width, uint32_t height);
    Device& m_device;
    ReflectionSettings m_settings;
    ComPtr<ID3D12Resource> m_history;   // R16_FLOAT reflection hit distance of the last frame (G spacing)
    uint32_t m_historyWidth = 0, m_historyHeight = 0;
    ComPtr<ID3D12Resource> m_arguments;  // raw: job counter, then the two indirect dispatch descriptions (SKY0, SKY1)
    float3 m_skyRadiance{}, m_sunIlluminance{};
    TextureRef m_modes;

    struct PlanarReflector
    {
        float4 plane;        // world: n.p + w = 0, n on the reflective (front) side
        float3 lo, hi;       // world bounds of its triangles
        float area = 0;      // world area of its triangles (m^2)
        uint32_t triangles = 0;
    };
    // Bounding volume hierarchy over the planes: a node holds the bounds and the largest area of its planes, so a frame
    // visits only planes whose pixel-count bound can reach reflection.planar_min_pixels.
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
    bool m_planarEnabled = true;
    ComPtr<ID3D12Resource> m_planarRing;      // upload ring: per frame { candidates, views, pad, 64 x { plane, rect } }
    uint8_t* m_planarMapped = nullptr;
    uint32_t m_planarSrv = 0xFFFFFFFFu;
    ComPtr<ID3D12Resource> m_planarCounts;    // per candidate of this frame: mirror-smooth pixels on its plane
    ComPtr<ID3D12Resource> m_planarReadback;  // ring of the counts, read on the CPU framesInFlight frames later
    const uint32_t* m_readbackMapped = nullptr;
    std::vector<uint32_t> m_slotPlanes[4];    // plane of each candidate, per ring slot
    std::vector<uint64_t> m_slotFrame;        // frame that wrote each slot
    std::vector<uint32_t> m_planePixels;      // last read-back count per plane
    uint32_t m_lastPlanarViews = 0, m_lastPlanarPixels = 0, m_lastCandidates = 0;
    float m_lastSelectMs = 0;
};
} // namespace unx::render::refl
