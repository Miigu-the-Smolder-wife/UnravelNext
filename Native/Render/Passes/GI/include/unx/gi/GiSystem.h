#pragma once
// GI of the R track (ARCHITECTURE 2.5): world radiance cache (persistent, GiCache.hlsli), screen probes and near
// occlusion (ScreenProbes.hlsli). One instance per FrameRenderer (track state "R.gi").
//
// Per frame (main view, after M's G-buffer; ARCHITECTURE 4.1 C3, C5):
//   r.gi.begin -> r.gi.evict -> r.gi.clear -> r.gi.rehash     cache housekeeping (frame stamp, LRU eviction, rehash)
//   r.gi.place -> r.gi.carry                                  requests: probe surfaces (find/create), last frame's ray hits
//   r.gi.age -> r.gi.setup -> r.gi.select                     stalest-first choice of budget / 64 hemisphere updates
//   r.gi.trace (DispatchRays) -> r.gi.integrate               64 rays per update -> texels -> exact SH irradiance
//   r.gi.gather                                               probes: trilinear SH + near occlusion -> view.screenProbes
#include "unx/render/Frame.h"
#include "unx/rt/RayScene.h"

namespace unx::render::gi
{
struct GiSettings  // from Config/quality/gi.toml
{
    uint32_t raysPerFrame = 0, capacity = 0, tableSlots = 0, probeSpacing = 0, maxAge = 0, jacobiUpdates = 0, historyMax = 0, maxLevel = 0;
    uint32_t updatesPerFrame = 0;  // raysPerFrame / 64 whole-hemisphere updates
    float cellAngleDeg = 0, cellMin = 0, nearRadius = 0, rayLength = 0, hitUpdateShare = 0, hitCellFootprintScale = 0;
    uint32_t experimentDisable = 0;  // gi.experiment_disable (cost attribution only)
    static GiSettings fromQuality(const QualityConfig& q);
};

struct GiStats  // header counters of the last completed frame (tests, diagnostics)
{
    uint32_t live = 0, free = 0, requested = 0, selected = 0, background = 0, hits = 0;
    uint32_t created = 0, allocationFailures = 0, tableFull = 0, evicted = 0, resets = 0;
    uint32_t hitLookups = 0, hitMisses = 0;  // reflection hits' cache lookups and those with no data at any level
    uint32_t gSamples = 0, gRatio = 0;       // reflection G samples, and those estimated by the ratio branch (reflLobeEstimate)
    uint32_t gHistogram[9] = {};             // G samples by log2(mean L / mean g), bins [-4, 5)
    uint32_t gZero = 0;                      // G samples with mean g = 0 (not in the histogram)
};

class GiSystem
{
public:
    static GiSystem& get(FramePassContext& fc);
    // This renderer's instance, or null before its first frame (diagnostics, gates).
    static GiSystem* find(TrackState& state);
    GiSystem(Device& device, const QualityConfig& quality);
    ~GiSystem();
    GiSystem(const GiSystem&) = delete;
    GiSystem& operator=(const GiSystem&) = delete;

    // Declares the GI passes of the main view and creates view.screenProbes; FrameResources::giCache is imported here.
    void record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays);

    // Constant sky radiance (nits) and sun illuminance (lux) for escaping rays instead of the atmosphere (tests, and
    // until S's atmosphere header is committed: GiTrace variant SKY1).
    void setConstantSky(float3 radiance, float3 sunIlluminance)
    {
        if (radiance.x != m_skyRadiance.x || radiance.y != m_skyRadiance.y || radiance.z != m_skyRadiance.z || sunIlluminance.x != m_sunIlluminance.x ||
            sunIlluminance.y != m_sunIlluminance.y || sunIlluminance.z != m_sunIlluminance.z)
            ++m_epoch;  // lighting changed: every entry's history restarts
        m_skyRadiance = radiance;
        m_sunIlluminance = sunIlluminance;
    }
    const GiSettings& settings() const { return m_settings; }
    // Blocking readback of the cache header (waits for the GPU).
    GiStats readStats();
    ID3D12Resource* cache() const { return m_cache.Get(); }
    uint64_t cacheBytes() const { return m_bytes; }

private:
    Device& m_device;
    GiSettings m_settings;
    ComPtr<ID3D12Resource> m_cache;
    ComPtr<ID3D12CommandSignature> m_dispatchSignature;  // one D3D12_DISPATCH_ARGUMENTS, 16 B stride (radiance maps)
    ID3D12CommandSignature* dispatchSignature();
    uint64_t m_bytes = 0;
    float3 m_skyRadiance{}, m_sunIlluminance{};
    uint32_t m_epoch = 1, m_sceneRevision = 0;
};
} // namespace unx::render::gi
