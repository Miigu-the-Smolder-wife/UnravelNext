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
    bool deterministic = false;      // gi.deterministic: same inputs -> bit-identical cache (selection by key priority, seeds by key)
    static GiSettings fromQuality(const QualityConfig& q);
};

struct GiStats  // header counters of the last completed frame (tests, diagnostics)
{
    uint32_t live = 0, free = 0, requested = 0, selected = 0, background = 0, hits = 0;
    uint32_t created = 0, allocationFailures = 0, tableFull = 0, evicted = 0, resets = 0;
    uint32_t epoch = 0;  // the lighting epoch (a whole-cache restart increments it)
    uint32_t hitLookups = 0, hitMisses = 0;  // reflection hits' cache lookups and those with no data at any level
    uint32_t gSamples = 0, gRatio = 0;       // reflection G samples, and those estimated by the ratio branch (reflLobeEstimate)
    uint32_t gHistogram[9] = {};             // G samples by log2(mean L / mean g), bins [-4, 5)
    uint32_t gZero = 0;                      // G samples with mean g = 0 (not in the histogram)
};

// The information quantity of M's per-pixel cache lookup on the main view (Gates/GiLookupStats.hlsl; gates only).
struct GiLookupStats
{
    uint32_t pixels = 0, levels = 0, lookups = 0, slots = 0, found = 0, multiLevelPixels = 0, tileDiffers = 0, tileMissed = 0;
    float tileMaxRel = 0;
    uint32_t evaluated = 0;  // entries evaluated (weight > 0 after the partner corners)
    // Against the reconstruction before the partner corners: largest and mean relative difference, pixels over 1 % / 5 %.
    float fillMaxRel = 0;
    uint32_t fillSumRel1e3 = 0, fillCompared = 0, fillOver1 = 0, fillOver5 = 0;
    uint32_t tiles = 0, tileKeys = 0, tileEntries = 0, maxTileKeys = 0, maxTileEntries = 0;
    uint32_t entryHistogram[64] = {};  // tiles by distinct entries (63 = 63 or more)
    // r.gi.screen's texture (view.giIrradiance) against the per-pixel lookup on M's inputs: pixels whose data flag differs,
    // pixels compared (both with data, value in the half's normal range), those over 1e-3 relative, the largest relative.
    uint32_t screenFlagDiffers = 0, screenCompared = 0, screenOver = 0;
    float screenMaxRel = 0;
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
    // Tests: the constant sky only in directions up to this sine of elevation (1 = the whole sky; SKY1 variant only).
    void setConstantSkyBand(float maxSinElevation)
    {
        if (maxSinElevation != m_skyBand) ++m_epoch;
        m_skyBand = maxSinElevation;
    }
    const GiSettings& settings() const { return m_settings; }
    // Blocking readback of the cache header (waits for the GPU).
    GiStats readStats();
    ID3D12Resource* cache() const { return m_cache.Get(); }
    // Gates: count the main view's cache lookups in the frames recorded while on (GiLookupStats.hlsl, after r.gi.maps:
    // the cache M reads); readLookupStats (blocking) returns the last such frame's counts.
    void setLookupStats(bool on) { m_lookupStatsOn = on; }
    GiLookupStats readLookupStats();
    uint64_t cacheBytes() const { return m_bytes; }

private:
    Device& m_device;
    GiSettings m_settings;
    ComPtr<ID3D12Resource> m_cache;
    ComPtr<ID3D12Resource> m_lookupStats;  // 128 uint counters (setLookupStats)
    bool m_lookupStatsOn = false;
    ComPtr<ID3D12CommandSignature> m_dispatchSignature;  // one D3D12_DISPATCH_ARGUMENTS, 16 B stride (radiance maps)
    ID3D12CommandSignature* dispatchSignature();
    uint64_t m_bytes = 0;
    float3 m_skyRadiance{}, m_sunIlluminance{};
    float m_skyBand = 1;
    uint32_t m_epoch = 1, m_sceneRevision = 0;
    // Change boxes for GiInvalidate (B3): a mapped upload ring, one slot per frame of kChangeSlots, raw SRVs.
    static constexpr uint32_t kChangeSlots = 4, kChangeBoxesMax = 256, kChangeSlotBytes = 16 + kChangeBoxesMax * 32 + 240;
    ComPtr<ID3D12Resource> m_changeRing;
    uint8_t* m_changeMapped = nullptr;
    uint32_t m_changeSrv[kChangeSlots] = {};
};
} // namespace unx::render::gi
