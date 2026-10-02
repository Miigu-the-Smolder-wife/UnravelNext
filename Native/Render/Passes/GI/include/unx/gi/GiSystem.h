#pragma once
// GI of the R track (ARCHITECTURE 2.5): world radiance cache (persistent, GiCache.hlsli), screen probes and near
// occlusion (ScreenProbes.hlsli). One instance per FrameRenderer (track state "R.gi").
//
// Per frame (main view, after M's G-buffer; ARCHITECTURE 4.1 C3, C5):
//   r.gi.begin -> r.gi.evict -> r.gi.clear -> r.gi.rehash     cache housekeeping (frame stamp, LRU eviction, rehash)
//   r.gi.place -> r.gi.carry                                  requests: probe surfaces (find/create), last frame's ray hits
//   r.gi.age -> r.gi.setup -> r.gi.select                     stalest-first choice of budget / 64 hemisphere updates
//   r.gi.trace (DispatchRays) -> r.gi.integrate               64 rays per update -> texels -> exact SH irradiance
//   r.gi.gather                                               probes: trilinear SH + near occlusion (integrated over
//                                                             time per probe) -> view.screenProbes
//   r.gi.screen -> r.gi.screen.filter                         per-pixel cache irradiance, edge-preserving spatial filter
//                                                             -> view.giIrradiance
#include "unx/render/Frame.h"
#include "unx/rt/RayScene.h"

namespace unx::render::gi
{
struct GiSettings  // from Config/quality/gi.toml
{
    uint32_t raysPerFrame = 0, capacity = 0, tableSlots = 0, probeSpacing = 0, maxAge = 0, jacobiUpdates = 0, historyMax = 0, maxLevel = 0;
    uint32_t historyStatic = 0;  // gi.history_updates_max_static (GiIntegrate's change test)
    uint32_t updatesPerFrame = 0;  // raysPerFrame / 64 whole-hemisphere updates
    float cellAngleDeg = 0, cellMin = 0, nearRadius = 0, rayLength = 0, hitUpdateShare = 0, hitCellFootprintScale = 0;
    uint32_t screenOcclusionHistory = 0;  // gi.screen_occlusion_history_frames (GiProbeGather's time integration; 1 = off)
    float screenFilterCells = 0;          // gi.screen_filter_cells (GiScreenFilter's radius in cell edges; 0 = off)
    uint32_t screenUpdateFrames = 1;      // gi.screen_update_frames (r.gi.screen's frame split, GiScreenIrradiance; 1 = off)
    uint32_t experimentDisable = 0;  // gi.experiment_disable (cost attribution only)
    bool deterministic = false;      // gi.deterministic: same inputs -> bit-identical cache (selection by key priority, seeds by key)
    bool anchorVisibility = false;   // gi.anchor_visibility: lookups skip entries whose anchor does not see the point (GiHeader.flags bit 1)
    bool splitBounceHistory = false;
    uint32_t bounceHistoryUpdates = 4;
    // Redesign V2 P1 (RENDERER_REDESIGN_V2 1.1; A/B switches, gi.toml): update tiers (young entries first, at most
    // youngUpdateShare of a tier's updates), the parent prior of first updates (parentDeltaInitial: the parent-child
    // difference assumed until measured), relight restarts.
    bool updateTiers = false, parentPrior = false, relightRestart = false;
    bool hitLightFootprint = false;
    bool lightInvalidation = false;
    // gi.hit_light_footprint: point / spot lights' diffuse term at GI hits as its footprint mean; gi.light_invalidation: a
    // changed light restarts the entries that see its range; gi.hit_light_footprint_scale: the footprint's size (diagnostics).
    float hitLightFootprintScale = 1.0f;
    // Redesign V2.2 11.2 (P1'-b): gi.bounce_split (the bounce part as a current L1 pair beside the long mean) and the
    // current bounce part's window (gi.bounce_split_updates, 1 = replacement).
    bool bounceSplit = false;
    // gi.hit_accumulator_pool (V2.3 12.8): the accumulator in its own cells and levels (GiAccPool.hlsli), window ratio
    // with the frame weight (1 - alpha)^age; gi.hit_accumulator_pool_slots (a power of two), _alpha, _fine_scale (the
    // finest cell over the ray footprint); the minimum samples are gi.hit_accumulator_min_samples.
    bool hitAccumulatorPool = false;
    uint32_t hitAccumulatorPoolSlots = 524288;
    float hitAccumulatorAlpha = 0.125f, hitAccumulatorFineScale = 0.25f;
    // gi.miss_closure (V2 1.3, cold start): bounce reads without data are closed with the entry's own irradiance
    // (GiIntegrate.hlsl). gi.bounce_visibility: GiTrace's bounce reads (the hit's own cell and the fallback levels) count
    // only cells whose anchor sees the hit.
    bool missClosure = false, bounceVisibility = false;
    bool hitOrientedLights = false;  // gi.hit_oriented_lights (GI_P1_FLAGS bit 7)
    // gi.lumen (LumenGather.cpp, Passes/GI/Lumen): the screen-probe final gather in the structure of Unreal's Lumen in
    // place of r.gi.screen and its filters. The values are Unreal's defaults (ue6-main); the ones marked (Q) trade
    // accuracy for stability and are the user's to decide (Docs/Status/LUMEN_GATHER_KO.md).
    struct Lumen
    {
        bool enabled = false;
        uint32_t tile = 16;                // screen probe spacing (px)
        float adaptiveFraction = 0.5f;     // adaptive probes over uniform probes, at most
        float minPdfToTrace = 0.1f;        // structured importance sampling: rays under it are given to the brightest
        bool importanceSampleLighting = true;
        float maxRayIntensity = 10.0f;     // (Q) a trace's largest exposed channel after its share of the texel
        uint32_t filterPasses = 3;         // probe-space spatial filter
        float filterMaxHitAngleDeg = 10.0f;
        float filterPositionWeight = 1000.0f;
        float temporalMaxFrames = 10.0f;   // (Q) pixel history length
        bool temporalFilterProbes = false; // probe-space blend with last frame's probes before the spatial filter
        float temporalFilterProbesWeight = 0.5f;
        float temporalDistanceThreshold = 0.01f;
        float temporalFastFraction = 0.1f; // share of moving lighting at which the history is at its shortest
        float temporalMaxFast = 0.9f;
        float jitterWidth = 1.0f;          // per-pixel offset of the probe interpolation, in tiles
        bool stochasticInterpolation = true;
        float maxRoughnessRoughSpecular = 0.8f;  // (Q) above it the rough specular is irradiance / pi
        float disocclusionMaxFrames = 4.0f;
        float disocclusionFraction = 0.4f;
        uint32_t rayDirections = 8;        // the frames the direction jitter cycles through
        float movingSpeed = 0.005f;        // relative speed difference that makes a trace "moving"
        float normalBias = 0.001f;         // m: the rays' origin off the surface (Unreal: 0.1 cm)
        bool capSnapExposure = true;       // on a snap frame (first frames, cut, restore) the intensity cap is taken in
                                           // the exposure metered on the frame's traces (LgMeter.hlsl; not Unreal's)
        bool hitSurfaceCache = true;       // the hits read the surface cache when it exists (surface_cache.enabled)
        bool only = false;                 // gi.lumen_only: the final gather alone - the world cache and its screen probes
                                           // are neither updated nor published (their readers take the gather's outputs
                                           // and the translucency volume)
        bool hitFallback = false;          // a hit without a lit cell is shaded from the world cache and a light sample;
                                           // false: it takes no cached light (Unreal's rule for an invalid sample)
        uint32_t raysPerDispatch = 262144; // the probe rays are traced in row bands of at most this many rays per DispatchRays
        bool screenTraces = true;          // the rays walk the depth pyramid first (ScreenTraces; needs the colour history)
        uint32_t screenTraceIterations = 50;        // HierarchicalScreenTraces.MaxIterations
        float screenTraceThickness = 0.02f;         // HierarchicalScreenTraces.RelativeDepthThickness
        uint32_t screenTraceThicknessSteps = 4;     // NumThicknessStepsToDetermineCertainty
        bool screenTraceSkipAfterCut = false;       // no screen traces in the frame after a cut either (A/B; not Unreal's)
    } lumen;
    uint32_t bounceSplitUpdates = 1;
    bool anchorResample = false;
    bool anchorCentroid = false;  // gi.anchor_centroid (V2.3 12.2, P1''-b): the anchor is the lookups' centroid (GiInternal giCentroidOffer)
    bool screenFilterAdaptive = false;  // gi.screen_filter_adaptive (V2 1.2 L_gi): filter radius x clamp(sigma / sigma0, 0.5, 3)
    bool screenWideFilter = false;  // gi.screen_wide_filter (V2 1.2 L_gi): the probes' SH filtered over many cells (GiProbeFilter.hlsl)
    uint32_t screenWidePasses = 3;  // gi.screen_wide_passes: a-trous passes (tap spacing x 1, 2, 4: reach +-2, 6, 14 spacings)
    float screenWideSigmaLo = 0.02f, screenWideSigmaHi = 0.06f;  // gi.screen_wide_sigma_lo / _hi: the narrow value's sigma where the wide share is 0 / 1
    uint32_t screenTemporalFrames = 0;  // gi.screen_temporal_frames (V2 1.2 L_gi's history, GiLayerTemporal.hlsl; 0 = off)
    // gi.history_window_rule "lighting": the running mean's window from the scene (history_updates_max while the sun
    // changed within lightingRecentFrames, else the static window); "samples": from the entry's own statistics.
    bool windowByLighting = true;
    uint32_t lightingRecentFrames = 256;
    // gi.path_guiding (D-12, off: a decision item): the rays' texels from a mixture of the uniform choice
    // (pathGuidingUniformShare) and one proportional to the texels' irradiance share (GiGuide.hlsl).
    bool pathGuiding = false;
    // gi.hit_accumulator (V2.2 12.1, P1''-a): hits read their cell's direct-light means (GiInternal GI_ACC_*) once it holds
    // hitAccumulatorMinSamples; the means' window in samples (hitAccumulatorWindowRecent while the sun changes).
    bool hitAccumulator = false;
    uint32_t hitAccumulatorMinSamples = 32;
    uint32_t hitAccumulatorLevels = 4;  // gi.hit_accumulator_levels (pool form): 1 = a reader's own cell only, no pass-up
    float hitAccumulatorWindow = 1024, hitAccumulatorWindowRecent = 128;
    float hitAccumulatorCellScale = 2;  // gi.hit_accumulator_cell_scale (default: the bounce cell's, gi.hit_cell_footprint_scale)
    bool hitAccumulatorFrame = false;   // gi.hit_accumulator_frame: this frame's cell means (GiAccFix), not a window's
    bool hitAccumulatorRatio = false;   // gi.hit_accumulator_ratio: each term's mean weighted by its readers' factors
    float pathGuidingUniformShare = 0.5f;
    float youngUpdateShare = 0.6f, parentDeltaInitial = 0.05f;
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
    // Redesign V2 P1: updates that started from a parent prior, relight restarts, young (T0) and T1 entries selected, and
    // the running estimate of the parent-child relative difference (sqrt of GI_P1_DELTA2).
    uint32_t priors = 0, restarts = 0, selectedYoung = 0, selectedT1 = 0;
    float parentDelta = 0;
    uint64_t audit[8] = {};
    uint64_t accAudit[2] = {};  // gi.hit_accumulator audit: sum of lum(point) and lum(accumulator) diffuse direct x 1024 (GI_ACC_AUDIT)  // energy audit sums (GiInternal.hlsli GI_AUDIT_SUMS, x 1024; gi.experiment_disable 32768)
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
    // A secondary (planar reflection) view's per-pixel cache irradiance and its filter into view.giIrradiance, after the
    // view's material resolve (depth, G-buffer) and after record (this frame's cache); nothing without the cache.
    void recordSecondaryScreen(FramePassContext& fc, ViewResources& view);

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
    // gi.hit_accumulator_pool (GiAccPool.hlsli): the accumulator's buffer; cleared before its first use and after an origin shift
    ComPtr<ID3D12Resource> m_accPool;
    uint64_t m_accPoolBytes = 0;
    bool m_accPoolClear = true;
    ComPtr<ID3D12Resource> m_lookupStats;  // 128 uint counters (setLookupStats)
    bool m_lookupStatsOn = false;
    ComPtr<ID3D12CommandSignature> m_dispatchSignature;  // one D3D12_DISPATCH_ARGUMENTS, 16 B stride (radiance maps)
    ID3D12CommandSignature* dispatchSignature();
    uint64_t m_bytes = 0;
    uint32_t m_admissionCapacity = 0, m_reflectionRays = 1;
    void ensureAdmission(FramePassContext& fc, const ViewResources& main);
    void recordAdmission(FramePassContext& fc, BufferRef cache);
    float3 m_skyRadiance{}, m_sunIlluminance{};
    float m_skyBand = 1;
    uint32_t m_epoch = 1, m_sceneRevision = 0;
    // Screen probe occlusion history (GiProbeGather): ping-pong by parity, RGBA32_UINT (2 probesX) x probesY.
    ComPtr<ID3D12Resource> m_probeHistory[2];
    uint32_t m_probeHistoryX = 0, m_probeHistoryY = 0, m_probeHistoryParity = 0, m_probeHistoryRevision = 0;
    bool m_probeHistoryReset = true;
    void ensureProbeHistory(uint32_t probesX, uint32_t probesY);
    // r.gi.screen's frame split (gi.screen_update_frames): the main view's value (RGBA16F) and keys (R32G32_UINT: device
    // depth, normal and age) ping-pong by parity; the previous frame's inverse view-projection and exposure.
    ComPtr<ID3D12Resource> m_screenValue[2], m_screenKeys[2];
    uint32_t m_screenX = 0, m_screenY = 0, m_screenParity = 0, m_screenRevision = 0, m_screenEpoch = 0;
    bool m_screenValid = false;
    float4x4 m_screenPrevInvViewProj{};
    float m_screenPrevExposure = 0;
    void ensureScreenHistory(uint32_t width, uint32_t height);
    // L_gi's temporal step (GiLayerTemporal.hlsl): value and keys, ping-pong
    ComPtr<ID3D12Resource> m_layerValue[2], m_layerKeys[2];
    uint32_t m_layerX = 0, m_layerY = 0, m_layerParity = 0, m_layerRevision = 0, m_layerEpoch = 0;
    bool m_layerValid = false;
    float4x4 m_layerPrevInvViewProj{};
    float m_layerPrevExposure = 0;
    TextureRef recordScreen(FramePassContext& fc, ViewResources& view, BufferRef cache);
    // gi.lumen (LumenGather.cpp): the probes of the last two frames (depth, position, filtered radiance: the next frame's
    // lighting density) and the pixels' histories (diffuse, rough specular, keys), ping-pong by parity.
    struct LumenState
    {
        ComPtr<ID3D12Resource> probeDepth[2], probePosition[2], probeRadiance[2], diffuse[2], specular[2], keys[2];
        ComPtr<ID3D12Resource> backface[2];  // Foliage back-side irradiance and its history (made when the scene has Foliage)
        uint32_t backfaceWidth = 0, backfaceHeight = 0;
        bool backfaceHistory = false;
        uint32_t width = 0, height = 0, parity = 0, revision = 0, epoch = 0, prevTemporalIndex = 0;
        bool valid = false;
        bool previousHadHistory = false;  // the frame before had a valid history (it was not a first frame, cut or restore)
        float prevExposure = 0;
        float4x4 prevInvViewProj{};
    } m_lumen;
    void ensureLumen(uint32_t width, uint32_t height, uint32_t atlasX, uint32_t atlasY);
    void recordLumen(FramePassContext& fc, ViewResources& view, BufferRef cache, rt::RayScene& rays);
    // Change boxes for GiInvalidate (B3): a mapped upload ring, one slot per frame of kChangeSlots, raw SRVs.
    static constexpr uint32_t kChangeSlots = 4, kChangeBoxesMax = 256, kChangeSlotBytes = 16 + kChangeBoxesMax * 32 + 240;
    ComPtr<ID3D12Resource> m_changeRing;
    uint8_t* m_changeMapped = nullptr;
    uint32_t m_changeSrv[kChangeSlots] = {};
    float m_sunSeen[8] = {};                // the sun of the last frame (window rule "lighting")
    uint64_t m_lightingChangedFrame = 0;   // the frame it last changed
    std::vector<gpu::Light> m_lightSeen;  // P1: last frame's light records (changed lights invalidate their range)
};
} // namespace unx::render::gi
