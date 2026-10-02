#include "unx/refl/ReflectionSystem.h"
#include "unx/refl/SurfaceCacheLightPairs.h"

#include "unx/rt/RayPipeline.h"
#if UNX_R_HAS_SHADING
#include "unx/shading/Upscale.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <array>
#include <chrono>
#include <memory>
#include <unordered_map>

namespace unx::render::refl
{
namespace
{
uint32_t asU(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

constexpr uint32_t kDescStride = (uint32_t)((sizeof(D3D12_DISPATCH_RAYS_DESC) + 7) / 8 * 8);
constexpr uint32_t kPlanarMax = 4, kCandidatesMax = 64, kPlanarSlotBytes = 4096, kPlanarSlots = 4;  // ring slots > frames in flight
// Read-back slot: candidate pixel counts, timestamps (trace begin/end, then begin/end per view), job counters (args 0..16).
constexpr uint32_t kTicks = 2 + 2 * kPlanarMax, kTicksOffset = kCandidatesMax * 4, kJobsOffset = kTicksOffset + kTicks * 8, kReadbackStride = 512;
static_assert(kJobsOffset + 16 <= kReadbackStride);
struct PlanarGpu  // ReflectionInternal.hlsli: candidates [0, views) have a reflection camera
{
    uint32_t candidates, views, pad[2];
    struct
    {
        float4 plane;
        uint32_t rect[4];  // x, y, width, height in main-view pixels
    } planes[kCandidatesMax];
};
static_assert(sizeof(PlanarGpu) <= kPlanarSlotBytes);
static_assert(kPlanarMax == 4, "ReflectionSystem::m_viewRects and ReflectionClassify's mask constants hold 4 views");

float3 xform(const float4 rows[3], float3 p)
{
    return { rows[0].x * p.x + rows[0].y * p.y + rows[0].z * p.z + rows[0].w, rows[1].x * p.x + rows[1].y * p.y + rows[1].z * p.z + rows[1].w,
             rows[2].x * p.x + rows[2].y * p.y + rows[2].z * p.z + rows[2].w };
}
// Arguments buffer: counters, trace descriptions (SKY0, SKY1), shadow description, shade and combine Dispatch arguments.
constexpr uint32_t kShadowDescOffset = 16 + 2 * kDescStride, kShadeArgsOffset = kShadowDescOffset + kDescStride, kCombineArgsOffset = kShadeArgsOffset + 16;
constexpr uint32_t kLocalDescOffset = kCombineArgsOffset + 16;  // local-light shadow rays (ReflectionLocalShadow)
constexpr uint32_t kInlineDescOffset = kLocalDescOffset + kDescStride;  // jobs over the ray capacity (ReflectionTraceInline, 2 x 2 variants)
constexpr uint32_t kPenumbraArgsOffset = kInlineDescOffset + 4 * kDescStride;  // ReflectionPenumbra Dispatch arguments
constexpr uint32_t kLumenDescOffset = kPenumbraArgsOffset + 16;  // the Lumen trace's descriptions (reflection.lumen_only: SKY0, SKY1)
constexpr uint32_t kArgumentsBytes = kLumenDescOffset + 2 * kDescStride;
// Bands (ReflectionRay.hlsli REFL_BAND, REFL_INLINE_BAND): one DispatchRays launches at most kBand threads (the inline
// pass kInlineBand jobs), so its time is bounded whatever a frame's counts are; the arguments buffer holds kMaxBands
// copies of the descriptions, one per band (band b's at b x kArgumentsBytes). The surface cache's direct light:
// kCellBand cells a dispatch, bounded in TraceRay calls: a cell traces up to 8 light rays, the remainder light and the
// sun, each over the static and the dynamic TLAS = 20 calls, so 163,840 calls a dispatch (under kBand).
constexpr uint32_t kSlotRowGroups = 16384;  // SurfaceCache.hlsli SC_ROW_THREADS / 64: the per-slot passes' dispatch rows
constexpr uint32_t kBand = 262144, kInlineBand = 65536, kMaxBands = 128, kCellBand = 8192;
constexpr uint32_t bandsFor(uint64_t count, uint32_t band) { return (uint32_t)std::clamp<uint64_t>((count + band - 1) / band, 1, kMaxBands); }
const char* const kTraceLibrary[2] = { "Passes/Reflection/ReflectionTrace.SKY0", "Passes/Reflection/ReflectionTrace.SKY1" };
const char* const kInlineLibrary[2][2][2] = {
    { { "Passes/Reflection/ReflectionTraceInline.SKY0.JOB1.CORNERS0", "Passes/Reflection/ReflectionTraceInline.SKY0.JOB1.CORNERS1" },
      { "Passes/Reflection/ReflectionTraceInline.SKY0.JOB2.CORNERS0", "Passes/Reflection/ReflectionTraceInline.SKY0.JOB2.CORNERS1" } },
    { { "Passes/Reflection/ReflectionTraceInline.SKY1.JOB1.CORNERS0", "Passes/Reflection/ReflectionTraceInline.SKY1.JOB1.CORNERS1" },
      { "Passes/Reflection/ReflectionTraceInline.SKY1.JOB2.CORNERS0", "Passes/Reflection/ReflectionTraceInline.SKY1.JOB2.CORNERS1" } }
};
const char* const kShadeKernel[2][2] = {
    { "Passes/Reflection/ReflectionShadeRays.SKY0.CORNERS0", "Passes/Reflection/ReflectionShadeRays.SKY0.CORNERS1" },
    { "Passes/Reflection/ReflectionShadeRays.SKY1.CORNERS0", "Passes/Reflection/ReflectionShadeRays.SKY1.CORNERS1" }
};
const char* const kRefractLibrary[2] = { "Passes/Reflection/RefractionTrace.SKY0", "Passes/Reflection/RefractionTrace.SKY1" };
const char* const kLumenTraceLibrary[2] = { "Passes/Reflection/ReflectionLumenTrace.SKY0", "Passes/Reflection/ReflectionLumenTrace.SKY1" };
const char* const kLumenRefractLibrary[2] = { "Passes/Reflection/RefractionLumenTrace.SKY0", "Passes/Reflection/RefractionLumenTrace.SKY1" };
constexpr const char* kShadowLibrary = "Passes/Reflection/ReflectionShadow";
constexpr const char* kLocalShadowLibrary = "Passes/Reflection/ReflectionLocalShadow";
const char* const kSurfaceCacheLightLibrary[2] = { "Passes/SurfaceCache/SurfaceCacheLight.SKY0", "Passes/SurfaceCache/SurfaceCacheLight.SKY1" };
constexpr uint32_t kScreenTraceLevels = 6;  // ScreenTrace.hlsli SCT_LEVELS
constexpr uint32_t kSurfaceCacheHeaderBytes = 64, kSurfaceCacheCellBytes = 56, kSurfaceCacheProbeBytes = 36;  // SurfaceCache.hlsli
} // namespace

ReflectionSettings ReflectionSettings::fromQuality(const QualityConfig& q)
{
    ReflectionSettings s;
    s.kHalfAngle = (float)(q.number("reflection.cache_lobe_half_angle_min_deg") * 3.14159265358979 / 180.0);
    s.mirrorRoughness = (float)q.number("reflection.mirror_roughness_max");
    const int64_t rayCount = q.integer("reflection.g_rays_per_sample");
    // Ray ownership stores the within-job index in four bits. Reject invalid
    // configuration instead of truncating samples or aliasing their identities.
    if (rayCount < 1 || rayCount > 16) fail("reflection.g_rays_per_sample must be in [1, 16] (4-bit ray index)");
    s.raysPerSample = (uint32_t)rayCount;
    const std::vector<double> spacing = q.numbers("reflection.g_sample_spacing_px");
    if (spacing.size() != 2 || spacing[0] != 1) fail("reflection.g_sample_spacing_px must be [1, max]");
    // Samples live on per-tile grids: spacings above 8 px are sampled at 8 (denser than the bound, never sparser).
    s.maxSpacing = (uint32_t)std::min(spacing[1], 8.0);
    s.planarViewsMax = (uint32_t)q.integer("reflection.planar_views_max");
    if (s.planarViewsMax > kPlanarMax) fail("reflection.planar_views_max must be <= %u", kPlanarMax);
    s.planarViewFixedMs = (float)q.number("reflection.planar_view_fixed_ms");
    s.experimentDisable = (uint32_t)q.integer("reflection.experiment_disable");
    s.batchGiCorners = q.has("reflection.batch_gi_corners") && q.boolean("reflection.batch_gi_corners");
    s.statsLogFrames = (uint32_t)q.integer("reflection.stats_log_frames");
    s.planarViewNsPerPixel = (float)q.number("reflection.planar_view_ns_per_px");
    s.temporalHistoryMax = (uint32_t)q.integer("reflection.temporal_history_max");
    s.temporalLobeShift = (float)q.number("reflection.temporal_lobe_shift");
    // (keys newer than deployed configurations: absent = the previous path, as GiSystem's newer keys)
    s.layers = q.has("reflection.layers") && q.boolean("reflection.layers");
    s.layerFilter = !q.has("reflection.layer_filter") || q.boolean("reflection.layer_filter");
    s.layerHistoryFrames = q.has("reflection.layer_history_frames") ? (uint32_t)std::max<int64_t>(q.integer("reflection.layer_history_frames"), 1) : 8u;
    s.layerView = q.has("reflection.layer_view") ? (uint32_t)q.integer("reflection.layer_view") : 0u;
    s.layerHistoryBound = !q.has("reflection.layer_history_bound") || q.boolean("reflection.layer_history_bound");
    s.layerMirrorLobe = q.has("reflection.layer_mirror_lobe") && q.boolean("reflection.layer_mirror_lobe");
    s.layerCrossMode = !q.has("reflection.layer_cross_mode") || q.boolean("reflection.layer_cross_mode");
    s.layerWholeValue = !q.has("reflection.layer_whole_value") || q.boolean("reflection.layer_whole_value");
    s.hitConeLobes = !q.has("reflection.hit_cone_lobes") || q.boolean("reflection.hit_cone_lobes");
    s.layerResidualWhole = !q.has("reflection.layer_residual_whole") || q.boolean("reflection.layer_residual_whole");
    s.hitOrientedLights = q.has("reflection.hit_oriented_lights") && q.boolean("reflection.hit_oriented_lights");
    s.hitAccumulator = !q.has("reflection.hit_accumulator") || q.boolean("reflection.hit_accumulator");
    s.hitStrictRead = !q.has("reflection.hit_strict_read") || q.boolean("reflection.hit_strict_read");
    // The ray-reuse pipeline (ReflectionReuse.hlsli); defaults are the reference's (ue6-main LumenReflections.cpp).
    const auto num = [&q](const char* key, double fallback) { return (float)(q.has(key) ? q.number(key) : fallback); };
    const auto flag = [&q](const char* key, bool fallback) { return q.has(key) ? q.boolean(key) : fallback; };
    s.lumen = flag("reflection.lumen", false);
    s.lumenOnly = s.lumen && flag("reflection.lumen_only", false);
    s.lumenMaxRoughness = num("reflection.lumen_max_roughness_to_trace", 0.4);
    s.lumenFadeLength = num("reflection.lumen_roughness_fade_length", 0.1);
    s.lumenMaxRayIntensity = num("reflection.lumen_max_ray_intensity", 40.0);
    s.lumenTonemapRange = num("reflection.lumen_tonemap_range", 10.0);
    s.lumenReconstruction = flag("reflection.lumen_reconstruction", true);
    s.lumenReconstructionSamples = (uint32_t)num("reflection.lumen_reconstruction_samples", 5);
    s.lumenReconstructionRadius = num("reflection.lumen_reconstruction_radius", 8.0);
    s.lumenTemporal = flag("reflection.lumen_temporal", true);
    s.lumenTemporalMaxFrames = num("reflection.lumen_temporal_max_frames", 12.0);
    s.lumenClampScale = num("reflection.lumen_neighborhood_clamp_scale", 1.0);
    s.lumenDistanceThreshold = num("reflection.lumen_history_distance_threshold", 0.03);
    s.lumenBilateral = flag("reflection.lumen_bilateral", true);
    s.lumenBilateralSamples = (uint32_t)num("reflection.lumen_bilateral_samples", 4);
    s.lumenBilateralRadius = num("reflection.lumen_bilateral_radius", 8.0);
    s.lumenBilateralDepthWeight = num("reflection.lumen_bilateral_depth_weight", 10000.0);
    s.lumenDisocclusionFrames = num("reflection.lumen_bilateral_disocclusion_frames", 2.0);
    s.lumenDisocclusionTonemap = flag("reflection.lumen_disocclusion_tonemap", true);
    s.lumenRoughFromGather = flag("reflection.lumen_rough_specular_from_gather", true);
    s.lumenScreenTraces = flag("reflection.lumen_screen_traces", true);
    s.lumenRefractionSurfaceCache = flag("reflection.lumen_refraction_hit_surface_cache", true);
    s.lumenScreenContinue = flag("reflection.lumen_screen_trace_continue", true);
    s.lumenScreenPullback = num("reflection.lumen_screen_trace_pullback", 0.08);
    s.lumenSceneColorAtHit = flag("reflection.lumen_sample_scene_color_at_hit", true);
    s.lumenSceneColorThickness = num("reflection.lumen_sample_scene_color_relative_depth_thickness", 0.01);
    s.lumenSceneColorNormalDegrees = num("reflection.lumen_sample_scene_color_normal_threshold", 85.0);
    s.lumenSamplingBias = std::clamp(num("reflection.lumen_ggx_sampling_bias", 0.1), 0.0f, 0.99f);
    s.lumenScreenIterations = (uint32_t)num("reflection.lumen_screen_trace_max_iterations", 50);
    s.lumenScreenThickness = num("reflection.lumen_screen_trace_relative_depth_thickness", 0.005);
    // The surface cache (Passes/SurfaceCache/SurfaceCache.hlsli); defaults are the reference's (ue6-main LumenScene*.cpp,
    // LumenRadiosity.cpp).
    s.surfaceCache = flag("surface_cache.enabled", false);
    s.scEntriesLog2 = (uint32_t)num("surface_cache.entries_log2", 22);
    if (s.scEntriesLog2 < 12 || s.scEntriesLog2 > 24) fail("surface_cache.entries_log2 must be in [12, 24]");
    s.scMaxUnused = std::min((uint32_t)num("surface_cache.max_unused_frames", 255), 255u);
    s.scCaptureFactor = std::max((uint32_t)num("surface_cache.capture_factor", 64), 1u);
    s.scCaptureBounces = std::min((uint32_t)num("surface_cache.capture_bounces", 3), 8u);
    s.scDirectFactor = std::max((uint32_t)num("surface_cache.direct_update_factor", 32), 1u);
    s.scRadiosityFactor = std::max((uint32_t)num("surface_cache.radiosity_update_factor", 64), 1u);
    s.scRadiosityCap = num("surface_cache.radiosity_max_ray_intensity", 40.0);
    s.scRadiosityFrames = num("surface_cache.radiosity_max_frames_accumulated", 4.0);
    s.scDirect = flag("surface_cache.direct_lighting", true);
    s.scRadiosity = flag("surface_cache.radiosity", true);
    s.scRemainderLight = flag("surface_cache.remainder_light", false);
    s.scDirectStochastic = flag("surface_cache.direct_stochastic", false);
    s.scLightingFeedback = flag("surface_cache.lighting_feedback", true);
    s.scDirectAnalytic = flag("surface_cache.direct_analytic", true);
    s.scBilinearRead = flag("surface_cache.bilinear_read", true);
    s.scBaseCells = flag("surface_cache.base_cells", true);
    s.scDebugSkip = (uint32_t)num("surface_cache.debug_skip", 0);
    s.scShadowRaysOpaque = flag("surface_cache.shadow_rays_opaque", false);
    s.scDirectShadowInline = flag("surface_cache.direct_shadow_inline", false);
    s.scDirectPairs = flag("surface_cache.direct_pairs", true);
    s.scMeshCards = s.surfaceCache && flag("surface_cache.mesh_cards", false);
    // (the pairs hold direct_analytic lighting alone; the other modes are r.sc.cells' - asked for together, neither a silent
    // drop of the mode nor a silent return to the path that stops the device is right)
    if (s.surfaceCache && !s.scMeshCards && s.scDirectPairs && (s.scDirectStochastic || s.scRemainderLight || !s.scDirectAnalytic))
        fail("surface_cache.direct_pairs lights cells by direct_analytic alone: direct_stochastic, remainder_light and direct_analytic = false need "
             "surface_cache.direct_pairs = false (r.sc.cells - the path that hung the device in the bath lounge, 2026-10-02)");
    if (s.lumenOnly && s.surfaceCache && !s.scMeshCards)
        fail("reflection.lumen_only lights its hits from the mesh cards: surface_cache.enabled needs surface_cache.mesh_cards");
    s.scDebugCount = (uint32_t)num("surface_cache.debug_count", 0);
    s.scDirectStochasticFrames = num("surface_cache.direct_stochastic_max_frames", 12.0);
    s.scDirectMinWeight = num("surface_cache.direct_stochastic_min_sample_weight", 0.001);
    s.lumenHitSurfaceCache = flag("reflection.lumen_hit_surface_cache", true);
    s.lumenSurfaceCacheView = flag("reflection.lumen_surface_cache_view", false);
    s.lumenSurfaceCacheViewComponent = (uint32_t)num("reflection.lumen_surface_cache_view_component", 0);
    s.deterministic = q.has("debug.deterministic") && q.boolean("debug.deterministic");
    s.planarRayNs = (float)q.number("reflection.planar_ray_ns");
    return s;
}

namespace
{
struct ReflectionSystemSlot
{
    std::unique_ptr<ReflectionSystem> system;
};
} // namespace

ReflectionSystem& ReflectionSystem::get(FramePassContext& fc)
{
    ReflectionSystemSlot& slot = fc.state<ReflectionSystemSlot>("R.reflection");
    if (!slot.system) slot.system = std::make_unique<ReflectionSystem>(fc.device, fc.shaders, fc.quality);
    return *slot.system;
}

ReflectionSystem* ReflectionSystem::find(TrackState& state) { return state.get<ReflectionSystemSlot>("R.reflection").system.get(); }

ReflectionSystem::ReflectionSystem(Device& device, ShaderLibrary& shaders, const QualityConfig& quality)
    : m_device(device), m_settings(ReflectionSettings::fromQuality(quality))
{
    // Indirect dispatch arguments: counter + one description per sky variant (shader tables fixed; Width per frame).
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, upload{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = (uint64_t)kMaxBands * kArgumentsBytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_arguments)),
          "reflection arguments");
    m_arguments->SetName(L"R reflection dispatch arguments");
    uint8_t image[kArgumentsBytes] = {};
    // (only the pipelines of the path that runs: reflection.lumen_only never makes the older path's state objects)
    for (int v = 0; v < 2 && m_settings.lumenOnly; ++v)
    {
        const D3D12_DISPATCH_RAYS_DESC desc =
            rt::RayPipeline::get(device, shaders, rt::standardRayPipeline(kLumenTraceLibrary[v], { "ReflectionLumenTraceGen" })).dispatchDesc(0, 0, 1, 1);
        std::memcpy(image + kLumenDescOffset + v * kDescStride, &desc, sizeof desc);
    }
    for (int v = 0; v < 2 && !m_settings.lumenOnly; ++v)
    {
        const D3D12_DISPATCH_RAYS_DESC desc = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline(kTraceLibrary[v], { "ReflectionTraceGen" })).dispatchDesc(0, 0, 1, 1);
        std::memcpy(image + 16 + v * kDescStride, &desc, sizeof desc);
    }
    if (!m_settings.lumenOnly)
    {
        const D3D12_DISPATCH_RAYS_DESC desc = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline(kShadowLibrary, { "ReflectionShadowGen" })).dispatchDesc(0, 0, 1, 1);
        std::memcpy(image + kShadowDescOffset, &desc, sizeof desc);
    }
    if (!m_settings.lumenOnly)
    {
        const D3D12_DISPATCH_RAYS_DESC desc =
            rt::RayPipeline::get(device, shaders, rt::standardRayPipeline(kLocalShadowLibrary, { "ReflectionLocalShadowGen" })).dispatchDesc(0, 0, 1, 1);
        std::memcpy(image + kLocalDescOffset, &desc, sizeof desc);
    }
    for (int v = 0; v < 4 && !m_settings.lumenOnly; ++v)
    {
        const D3D12_DISPATCH_RAYS_DESC desc =
            rt::RayPipeline::get(device, shaders, rt::standardRayPipeline(kInlineLibrary[v / 2][v % 2][m_settings.batchGiCorners], { "ReflectionTraceInlineGen" })).dispatchDesc(0, 0, 1, 1);
        std::memcpy(image + kInlineDescOffset + v * kDescStride, &desc, sizeof desc);
    }
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sd{};
        sd.ByteStride = 16;
        sd.NumArgumentDescs = 1;
        sd.pArgumentDescs = &arg;
        check(device.d3d()->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&m_dispatchSignature)), "reflection dispatch signature");
    }
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource3(&upload, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "reflection arguments staging");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &mapped), "map arguments staging");
    for (uint32_t band = 0; band < kMaxBands; ++band) std::memcpy(static_cast<uint8_t*>(mapped) + band * kArgumentsBytes, image, sizeof image);
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(m_arguments.Get(), 0, staging.Get(), 0, (uint64_t)kMaxBands * kArgumentsBytes);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));

    // Planar reflector parameters: CPU-written upload ring, read by classify/resolve through a raw SRV.
    d.Width = kPlanarSlots * kPlanarSlotBytes;
    check(device.d3d()->CreateCommittedResource3(&upload, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_planarRing)),
          "planar reflector ring");
    check(m_planarRing->Map(0, &none, reinterpret_cast<void**>(&m_planarMapped)), "map planar ring");
    std::memset(m_planarMapped, 0, kPlanarSlots * kPlanarSlotBytes);
    DescriptorHeaps& h = device.descriptors();
    m_planarSrv = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.Buffer.NumElements = kPlanarSlots * kPlanarSlotBytes / 4;
    sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device.d3d()->CreateShaderResourceView(m_planarRing.Get(), &sd, h.resourceCpu(m_planarSrv));

    // Per-candidate pixel counts (UAV) and their read-back ring.
    d.Width = kCandidatesMax * 4;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_planarCounts)),
          "planar counts");
    D3D12_HEAP_PROPERTIES readback{ D3D12_HEAP_TYPE_READBACK };
    d.Width = kPlanarSlots * kReadbackStride;
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    check(device.d3d()->CreateCommittedResource3(&readback, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_planarReadback)),
          "planar count readback");
    D3D12_RANGE all{ 0, (SIZE_T)d.Width };
    check(m_planarReadback->Map(0, &all, reinterpret_cast<void**>(const_cast<uint8_t**>(&m_readbackMapped))), "map planar readback");
    m_slotFrame.assign(kPlanarSlots, UINT64_MAX);

    // Own timestamps for the cost choice (the frame profiler is not visible to tracks).
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kPlanarSlots * kTicks;
    check(device.d3d()->CreateQueryHeap(&qd, IID_PPV_ARGS(&m_timestamps)), "reflection timestamps");
    uint64_t frequency = 0;
    check(device.queue(QueueType::Graphics).get()->GetTimestampFrequency(&frequency), "timestamp frequency");
    m_tickMs = 1000.0 / (double)frequency;
    m_viewNsPerPixel = m_settings.planarViewNsPerPixel;
    m_viewFixedNs = m_settings.planarViewFixedMs * 1e6f;
    // The prior as two points of the fit (at 0 and at 1 M mirror pixels); measured views outweigh them after a few.
    addViewSample(0, m_viewFixedNs, 1);
    addViewSample(1e6, m_viewFixedNs + 1e6 * m_viewNsPerPixel, 1);
    if (m_settings.deterministic)
    {
        // debug.deterministic: the priors alone (no measurement ever replaces them).
        m_viewNsPerPixel = m_settings.planarViewNsPerPixel;
        m_viewFixedNs = m_settings.planarViewFixedMs * 1e6f;
        m_rayNs = m_settings.planarRayNs;
    }
}

void ReflectionSystem::addViewSample(double pixels, double ns, double weight)
{
    double* f = m_viewFit;
    for (int i = 0; i < 5; ++i) f[i] *= 15.0 / 16.0;
    f[0] += weight;
    f[1] += weight * pixels;
    f[2] += weight * ns;
    f[3] += weight * pixels * pixels;
    f[4] += weight * pixels * ns;
    // y = a + b x by least squares; a, b >= 0 (a negative term refits the other alone).
    const double det = f[0] * f[3] - f[1] * f[1];
    double b = det > 1e-9 * f[0] * f[3] ? (f[0] * f[4] - f[1] * f[2]) / det : m_viewNsPerPixel;
    double a = (f[2] - b * f[1]) / f[0];
    if (b < 0) b = 0, a = f[2] / f[0];
    if (a < 0) a = 0, b = f[3] > 0 ? f[4] / f[3] : m_viewNsPerPixel;
    m_viewFixedNs = (float)a;
    m_viewNsPerPixel = (float)b;
}

// Planar reflector candidates: triangles on which a reflection camera is exact. The material (with instance overrides)
// is mirror-smooth (roughness <= reflection.mirror_roughness_max), not alpha-tested and has no normal map, and the
// triangle's three vertex normals are its face normal (cos >= 0.999): then the shading normal is the plane normal and
// the mirrored view is the reflection. Smooth-shaded curved or wavy surfaces fail that test and stay on rays, which
// are exact for them. Static instances only; triangles are clustered by world plane (normal quantised to 1e-3,
// offset to 1 mm; the side is the one the normals face), so a facade's window panes share one plane.
void ReflectionSystem::buildPlanes(const GpuScene& gpuScene)
{
    const auto start = std::chrono::steady_clock::now();
    m_planes.clear();
    m_planesRevision = gpuScene.revision();
    const scene::Scene* src = gpuScene.source();
    const auto& instances = gpuScene.instances();
    struct KeyHash
    {
        size_t operator()(const std::array<int64_t, 4>& k) const
        {
            uint64_t h = 1469598103934665603ull;
            for (int64_t x : k) h = (h ^ (uint64_t)x) * 1099511628211ull;
            return (size_t)h;
        }
    };
    std::unordered_map<std::array<int64_t, 4>, uint32_t, KeyHash> clusters;
    auto mirrorMaterial = [&](const scene::Instance& in, uint32_t k) -> bool {
        const scene::Mesh& m = src->meshes[in.mesh];
        const scene::Material& material = src->materials[k < in.materialOverrides.size() ? in.materialOverrides[k] : m.submeshes[k].material];
        return material.roughness <= m_settings.mirrorRoughness && material.alphaCutoff <= 0 && material.normalTexture == scene::kNone;
    };
    auto staticInstance = [](const scene::Instance& in) { return !(in.flags & (scene::InstanceDynamic | scene::InstanceSkinned | scene::InstanceWind)); };
    size_t candidateTriangles = 0;
    for (uint32_t i = 0; src && i < (uint32_t)src->instances.size(); ++i)
        if (staticInstance(src->instances[i]))
            for (uint32_t k = 0; k < (uint32_t)src->meshes[src->instances[i].mesh].submeshes.size(); ++k)
                if (mirrorMaterial(src->instances[i], k)) candidateTriangles += src->meshes[src->instances[i].mesh].submeshes[k].indexCount / 3;
    clusters.reserve(candidateTriangles);
    uint64_t mirrorTriangles = 0, curvedTriangles = 0;
    for (uint32_t i = 0; src && i < (uint32_t)src->instances.size(); ++i)
    {
        const scene::Instance& in = src->instances[i];
        if (!staticInstance(in)) continue;
        const scene::Mesh& m = src->meshes[in.mesh];
        const gpu::Instance& gi = instances[i];
        const float4* r = gi.objectToWorld;
        const float det = r[0].x * (r[1].y * r[2].z - r[1].z * r[2].y) - r[0].y * (r[1].x * r[2].z - r[1].z * r[2].x) + r[0].z * (r[1].x * r[2].y - r[1].y * r[2].x);
        for (uint32_t k = 0; k < (uint32_t)m.submeshes.size(); ++k)
        {
            if (!mirrorMaterial(in, k)) continue;
            const scene::Submesh& sm = m.submeshes[k];
            for (uint32_t t = sm.indexOffset; t + 2 < sm.indexOffset + sm.indexCount; t += 3)
            {
                const uint32_t ia = m.indices[t], ib = m.indices[t + 1], ic = m.indices[t + 2];
                const float3 objectCross = cross(m.positions[ib] - m.positions[ia], m.positions[ic] - m.positions[ia]);
                if (length(objectCross) <= 1e-20f) continue;
                const float3 objectNormal = normalize(objectCross);
                if (!m.normals.empty() && (dot(m.normals[ia], objectNormal) < 0.999f || dot(m.normals[ib], objectNormal) < 0.999f || dot(m.normals[ic], objectNormal) < 0.999f))
                {
                    ++curvedTriangles;
                    continue;
                }
                const float3 a = xform(gi.objectToWorld, m.positions[ia]), b = xform(gi.objectToWorld, m.positions[ib]), c = xform(gi.objectToWorld, m.positions[ic]);
                const float3 cr = cross(b - a, c - a);
                const float doubleArea = length(cr);
                if (doubleArea <= 1e-20f) continue;
                const float3 n = cr * ((det < 0 ? -1.0f : 1.0f) / doubleArea);  // a mirroring transform flips the winding, not the normals
                const float w = -dot(n, a);
                const std::array<int64_t, 4> key{ std::lround(n.x * 1000), std::lround(n.y * 1000), std::lround(n.z * 1000), std::llround(w * 1000) };
                auto [it, added] = clusters.try_emplace(key, (uint32_t)m_planes.size());
                if (added) m_planes.push_back({ { n.x, n.y, n.z, w }, { 1e30f, 1e30f, 1e30f }, { -1e30f, -1e30f, -1e30f }, 0, 0 });
                PlanarReflector& p = m_planes[it->second];
                for (const float3& q : { a, b, c })
                {
                    p.lo = { std::min(p.lo.x, q.x), std::min(p.lo.y, q.y), std::min(p.lo.z, q.z) };
                    p.hi = { std::max(p.hi.x, q.x), std::max(p.hi.y, q.y), std::max(p.hi.z, q.z) };
                }
                p.area += 0.5f * doubleArea;
                ++p.triangles;
                ++mirrorTriangles;
            }
        }
    }
    m_planeOrder.resize(m_planes.size());
    for (uint32_t k = 0; k < (uint32_t)m_planes.size(); ++k) m_planeOrder[k] = k;
    m_planeNodes.clear();
    if (!m_planes.empty())
    {
        m_planeNodes.reserve(m_planes.size() / 2 + 2);
        buildPlaneNodes(0, (uint32_t)m_planes.size());
    }
    m_planePixels.assign(m_planes.size(), 0);
    m_planeViewMs.assign(m_planes.size(), 0);
    m_planeViewFrame.assign(m_planes.size(), UINT64_MAX);
    m_planeCameraFrame.assign(m_planes.size(), UINT64_MAX - 1);
    m_planeLastSeen.assign(m_planes.size(), UINT64_MAX - 1);
    m_planeRunStart.assign(m_planes.size(), UINT64_MAX);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    logf("R reflections: %zu planar reflector planes from %llu flat mirror triangles (%llu smooth-shaded mirror triangles stay on rays), %.1f ms\n",
         m_planes.size(), (unsigned long long)mirrorTriangles, (unsigned long long)curvedTriangles, ms);
}

// Node over m_planeOrder[begin, end) and its subtree, appended to m_planeNodes: leaves of up to 8 planes, split at the
// centroid median of the longest axis.
uint32_t ReflectionSystem::buildPlaneNodes(uint32_t begin, uint32_t end)
{
    const uint32_t index = (uint32_t)m_planeNodes.size();
    m_planeNodes.emplace_back();
    PlaneNode node;
    node.lo = { 1e30f, 1e30f, 1e30f };
    node.hi = { -1e30f, -1e30f, -1e30f };
    for (uint32_t k = begin; k < end; ++k)
    {
        const PlanarReflector& p = m_planes[m_planeOrder[k]];
        node.lo = { std::min(node.lo.x, p.lo.x), std::min(node.lo.y, p.lo.y), std::min(node.lo.z, p.lo.z) };
        node.hi = { std::max(node.hi.x, p.hi.x), std::max(node.hi.y, p.hi.y), std::max(node.hi.z, p.hi.z) };
        node.maxArea = std::max(node.maxArea, p.area);
    }
    if (end - begin <= 8)
    {
        node.first = begin;
        node.count = end - begin;
        m_planeNodes[index] = node;
        return index;
    }
    const float3 extent = node.hi - node.lo;
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
    auto centre = [&](uint32_t k) {
        const PlanarReflector& p = m_planes[k];
        return axis == 0 ? p.lo.x + p.hi.x : (axis == 1 ? p.lo.y + p.hi.y : p.lo.z + p.hi.z);
    };
    const uint32_t mid = begin + (end - begin) / 2;
    std::nth_element(m_planeOrder.begin() + begin, m_planeOrder.begin() + mid, m_planeOrder.begin() + end, [&](uint32_t a, uint32_t b) { return centre(a) < centre(b); });
    node.first = buildPlaneNodes(begin, mid);
    node.second = buildPlaneNodes(mid, end);
    m_planeNodes[index] = node;
    return index;
}

ReflectionSystem::~ReflectionSystem()
{
    m_device.deferRelease(m_arguments);
    if (m_statsReadback) m_statsReadback->Unmap(0, nullptr);
    m_device.deferRelease(m_statsReadback);
    m_device.deferRelease(m_history);
    for (int k = 0; k < 2; ++k)
    {
        m_device.deferRelease(m_accum[k]);
        m_device.deferRelease(m_accumKeys[k]);
        m_device.deferRelease(m_layerStochastic[k]);
        m_device.deferRelease(m_layerResidual[k]);
        m_device.deferRelease(m_layerKeys[k]);
    }
    if (m_planarRing) m_planarRing->Unmap(0, nullptr);
    m_device.deferRelease(m_planarRing);
    if (m_planarReadback) m_planarReadback->Unmap(0, nullptr);
    m_device.deferRelease(m_planarReadback);
    m_device.deferRelease(m_planarCounts);
    m_device.deferRelease(m_timestamps);
    DescriptorHeaps* h = &m_device.descriptors();
    const uint32_t srv = m_planarSrv;
    if (srv != 0xFFFFFFFFu) m_device.deferCall([h, srv] { h->freeResource(srv); });
}

// The surface cache's buffer in this frame's graph (SurfaceCache.hlsli), imported once per frame: for the passes of this
// system and for other tracks whose ray hits mark and read it (GI: before this system records, so its marks are of the
// frame and its reads see the lighting of the frame before). Invalid until the first frame with surface_cache.enabled.
BufferRef ReflectionSystem::surfaceCacheBuffer(FramePassContext& fc)
{
    if (!m_surfaceCache || m_settings.scMeshCards) return BufferRef{};
    if (m_surfaceCacheFrame != fc.frame.frameIndex || !m_surfaceCacheRef.valid())
    {
        const uint64_t bytes = kSurfaceCacheHeaderBytes + (uint64_t)m_surfaceCacheEntries * kSurfaceCacheCellBytes +
                               (uint64_t)(m_surfaceCacheEntries / 4) * kSurfaceCacheProbeBytes;
        m_surfaceCacheRef = fc.graph.importBuffer(m_surfaceCache.Get(), { "R surface cache", bytes, 0 });
        m_surfaceCacheFrame = fc.frame.frameIndex;
    }
    return m_surfaceCacheRef;
}

// The screen traces' inputs of this frame (Passes/Reflection/ScreenTrace.hlsli), made once per frame by whoever asks
// first - GI's probe trace or this system's record: the depth pyramid of the main view's depth (ReflectionHzb.hlsl: one
// pass per level into one atlas) and the previous frame's colour (ViewResources::prevSceneColor, set here from M's
// upscale history; invalid without it - the caller then skips its screen traces).
ScreenTraceInputs ReflectionSystem::screenTraceInputs(FramePassContext& fc, ViewResources& main)
{
    if (m_screenFrame == fc.frame.frameIndex) return m_screenInputs;
    m_screenFrame = fc.frame.frameIndex;
    m_screenInputs = ScreenTraceInputs{};
    if (!main.depth.valid()) return m_screenInputs;
#if UNX_R_HAS_SHADING
    main.prevSceneColor = shading::upscalePreviousColor(fc, main);
#endif
    m_screenInputs.prevColor = main.prevSceneColor;
    const uint32_t width = main.view.width, height = main.view.height;
    auto levelSize = [&](uint32_t level, uint32_t extent) { return std::max((extent + ((1u << level) - 1u)) >> level, 1u); };  // sctLevelSize
    uint32_t tall = 0;
    for (uint32_t level = 2; level <= kScreenTraceLevels; ++level) tall += levelSize(level, height);
    const TextureRef atlas = fc.graph.createTexture({ "R screen trace depth pyramid", levelSize(1, width) + levelSize(2, width), std::max(levelSize(1, height), tall), 1, 1,
                                                      DXGI_FORMAT_R32_FLOAT });
    const TextureRef depth = main.depth;
    ShaderLibrary& shaders = fc.shaders;
    for (uint32_t level = 1; level <= kScreenTraceLevels; ++level)
    {
        const uint32_t lw = levelSize(level, width), lh = levelSize(level, height);
        fc.graph.addPass("r.hzb", QueueType::Compute,
                         [&](PassBuilder& b) {
                             b.use(depth, Use::SrvCompute);
                             b.use(atlas, Use::UavCompute);
                         },
                         [&shaders, depth, atlas, level, width, height, lw, lh](PassContext& c) {
                             const uint32_t k[8] = { c.srv(depth), c.uav(atlas), level, 0, width, height, 0, 0 };
                             c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionHzb"));
                             c.computeConstants(k, 8);
                             c.cmd->Dispatch((lw + 7) / 8, (lh + 7) / 8, 1);
                         });
    }
    m_screenInputs.hzb = atlas;
    return m_screenInputs;
}

void ReflectionSystem::ensureHistory(uint32_t width, uint32_t height)
{
    if (m_history && m_historyWidth == width && m_historyHeight == height) return;
    if (m_history) m_device.deferRelease(m_history);
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16_FLOAT;  // distance, hit motion (ReflectionResolve)
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_history)),
          "reflection distance history");
    m_history->SetName(L"R reflection distance history");
    for (int k = 0; k < 2; ++k)
    {
        if (m_accum[k]) m_device.deferRelease(m_accum[k]);
        if (m_accumKeys[k]) m_device.deferRelease(m_accumKeys[k]);
        if (m_layerStochastic[k]) m_device.deferRelease(m_layerStochastic[k]);
        if (m_layerResidual[k]) m_device.deferRelease(m_layerResidual[k]);
        if (m_layerKeys[k]) m_device.deferRelease(m_layerKeys[k]);
        m_accum[k] = nullptr, m_accumKeys[k] = nullptr, m_layerStochastic[k] = nullptr, m_layerResidual[k] = nullptr, m_layerKeys[k] = nullptr;
        if (m_settings.layers || m_settings.lumen)
        {
            // The layers' history in place of the value's (LayerTemporal.hlsl); none when the layers keep no history.
            // reflection.lumen keeps its history in the same textures (value + second moment, frames, keys).
            if (!m_settings.lumen && m_settings.layerHistoryFrames <= 1) continue;
            ComPtr<ID3D12Resource>* targets[3] = { &m_layerStochastic[k], &m_layerResidual[k], &m_layerKeys[k] };
            const wchar_t* names[3][2] = { { L"R reflection layer history stochastic 0", L"R reflection layer history stochastic 1" },
                                           { L"R reflection layer history residual 0", L"R reflection layer history residual 1" },
                                           { L"R reflection layer history keys 0", L"R reflection layer history keys 1" } };
            for (int t = 0; t < 3; ++t)
            {
                d.Format = t == 2 ? DXGI_FORMAT_R32G32_UINT : DXGI_FORMAT_R16G16B16A16_FLOAT;
                check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                               IID_PPV_ARGS(targets[t]->ReleaseAndGetAddressOf())),
                      "reflection layer history");
                (*targets[t])->SetName(names[t][k]);
            }
            continue;
        }
        d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&m_accum[k])),
              "reflection accumulation");
        m_accum[k]->SetName(k ? L"R reflection accumulation 1" : L"R reflection accumulation 0");
        d.Format = DXGI_FORMAT_R32G32_UINT;
        check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&m_accumKeys[k])),
              "reflection accumulation keys");
        m_accumKeys[k]->SetName(k ? L"R reflection accumulation keys 1" : L"R reflection accumulation keys 0");
    }
    m_accumReset = true;
    m_historyWidth = width;
    m_historyHeight = height;
}

void ReflectionSystem::recordRefraction(FramePassContext& fc, BufferRef jobs, BufferRef results, uint32_t maxJobs)
{
    if (!m_refract.valid || m_refract.frameIndex != fc.frame.frameIndex || maxJobs == 0 || !jobs.valid() || !results.valid()) return;
    const RefractionInputs in = m_refract;
    RenderGraph& g = fc.graph;
    if (!m_streamTable)
    {
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 4 * 256;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(m_device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_streamTable)),
              "R refraction stream table");
        D3D12_RANGE none{ 0, 0 };
        check(m_streamTable->Map(0, &none, reinterpret_cast<void**>(&m_streamTableMapped)), "map R refraction stream table");
        for (uint32_t k = 0; k < 4; ++k)
        {
            m_streamTableSrv[k] = m_device.descriptors().allocateResource();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.Buffer.FirstElement = k * 64;
            sd.Buffer.NumElements = 64;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            m_device.d3d()->CreateShaderResourceView(m_streamTable.Get(), &sd, m_device.descriptors().resourceCpu(m_streamTableSrv[k]));
        }
    }
    const std::vector<std::pair<uint32_t, BufferRef>> streams = in.rays->streams();
    const bool lumenOnly = m_settings.lumenOnly;
    const char* const* refractLibrary = lumenOnly ? kLumenRefractLibrary : kRefractLibrary;
    rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, fc.shaders, rt::standardRayPipeline(refractLibrary[in.variant], { "RefractionGen" }));
    if (!m_refractTemplate)
    {
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 2 * kDescStride;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(m_device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&m_refractTemplate)),
              "R refraction dispatch template");
        uint8_t* m = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(m_refractTemplate->Map(0, &none, reinterpret_cast<void**>(&m)), "map R refraction template");
        for (int v = 0; v < 2; ++v)
        {
            const D3D12_DISPATCH_RAYS_DESC desc =
                rt::RayPipeline::get(fc.device, fc.shaders, rt::standardRayPipeline(refractLibrary[v], { "RefractionGen" })).dispatchDesc(0, 0, 1, 1);
            std::memcpy(m + v * kDescStride, &desc, sizeof desc);
        }
        m_refractTemplate->Unmap(0, nullptr);
    }
    // The dispatch size from the list's header (an empty list launches nothing): template copy -> count -> indirect rays.
    const BufferRef args = g.createBuffer(BufferDesc{ "R refraction dispatch", kDescStride, 0 });
    ID3D12Resource* templ = m_refractTemplate.Get();
    const uint32_t variant = (uint32_t)in.variant;
    ID3D12PipelineState* countPso = fc.shaders.compute("Passes/Reflection/RefractionArgs");
    g.addPass("r.refract.template", QueueType::Graphics,
              [&](PassBuilder& b) { b.use(args, Use::CopyDst); },
              [args, templ, variant](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(args), 0, templ, variant * kDescStride, kDescStride); });
    g.addPass("r.refract.count", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::UavCompute);
                  b.use(jobs, Use::SrvCompute);
              },
              [args, jobs, maxJobs, countPso](PassContext& c) {
                  const uint32_t k[4] = { c.uav(args), c.srv(jobs), maxJobs, (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width) };
                  c.cmd->SetPipelineState(countPso);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(1, 1, 1);
              });
    uint8_t* table = m_streamTableMapped + (in.frameIndex % 4) * 256;
    const uint32_t tableSrv = m_streamTableSrv[in.frameIndex % 4];
    g.addPass("r.refract", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  b.use(jobs, Use::SrvGraphics);
                  b.use(results, Use::UavGraphics);
                  if (lumenOnly)
                  {
                      if (in.cards.valid()) declareSurfaceCacheCards(b, in.cards, Use::SrvGraphics);
                  }
                  else
                  {
                      b.use(in.cache, Use::UavGraphics);
                      if (in.surfaceCache.valid()) b.use(in.surfaceCache, Use::UavGraphics);
                      rt::RayScene::declareVsm(b, in.vsm);
                  }
                  in.rays->declareTraversal(b);
                  in.rays->declareDecals(b);
                  for (const auto& st : streams) b.use(st.second, Use::SrvGraphics);
                  if (in.atmosphere)
                      for (const TextureRef& t : in.luts) b.use(t, Use::SrvGraphics);
              },
              [&pipeline, in, jobs, results, maxJobs, streams, table, tableSrv, args, lumenOnly](PassContext& c) {
                  // The stream table: each traced stream slot's vertex buffer SRV (known at execution).
                  uint32_t* t = reinterpret_cast<uint32_t*>(table);
                  for (uint32_t k = 0; k < 64; ++k) t[k] = 0xFFFFFFFFu;
                  for (const auto& st : streams) t[st.first] = c.srv(st.second);
                  uint32_t k[32] = {};
                  k[0] = c.srv(jobs), k[1] = c.uav(results), k[2] = maxJobs, k[3] = tableSrv;
                  k[4] = asU(in.sky.x), k[5] = asU(in.sky.y), k[6] = asU(in.sky.z), k[7] = asU(in.rayLength);
                  for (int i = 0; i < 4; ++i) k[8 + i] = in.atmosphere ? c.srv(in.luts[i]) : 0xFFFFFFFFu;
                  k[12] = asU(in.sun.x), k[13] = asU(in.sun.y), k[14] = asU(in.sun.z), k[15] = 0xFFFFFFFFu;
                  if (lumenOnly)
                  {
                      // RefractionLumenTrace.hlsl: P[4] = { card frame SRV, frame, 0, 0 }
                      k[16] = in.cards.valid() ? c.srv(in.cards.frame) : 0xFFFFFFFFu;
                      k[17] = in.frame & 0xFFFFFFu;
                      std::memcpy(&k[24], in.scene, sizeof in.scene);
                      c.computeConstants(k, 32);
                      c.bindFrameConstants(in.frameConstants);
                      pipeline.dispatchIndirect(c.cmd, c.resource(args), 0);
                      return;
                  }
                  k[16] = k[17] = 0xFFFFFFFFu;
                  k[18] = c.uav(in.cache);
                  k[19] = 0;
                  k[20] = (in.frame & 0xFFFFFFu) | (in.experiment << 24);
                  k[21] = in.surfaceCache.valid() ? c.uav(in.surfaceCache) : 0xFFFFFFFFu;  // (RefractionTrace.hlsl: no rays buffer here)
                  k[22] = in.rays->vsmSrvs(c, in.vsm, in.frameIndex, 1);
                  k[23] = 0xFFFFFFFFu;
                  std::memcpy(&k[24], in.scene, sizeof in.scene);
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(in.frameConstants);
                  pipeline.dispatchIndirect(c.cmd, c.resource(args), 0);
              });
}

void ReflectionSystem::record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays)
{
    RenderGraph& g = fc.graph;
    const ReflectionSettings& s = m_settings;
    const uint32_t width = main.view.width, height = main.view.height;
    const uint32_t tilesX = (width + 7) / 8, tilesY = (height + 7) / 8;
    const bool fresh = !m_history || m_historyWidth != width || m_historyHeight != height;
    ensureHistory(width, height);

    main.reflection = g.createTexture({ "R reflection", width, height + tilesY, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef reflection = main.reflection, depth = main.depth, gbuffer = main.gbuffer, probes = main.screenProbes, lobes = main.reflectionLobeTiles;
    const TextureRef modes = g.createTexture({ "R reflection modes", width, height, 1, 1, DXGI_FORMAT_R32_UINT });
    m_modes = modes;
    const TextureRef history = g.importTexture(m_history.Get(), { "R reflection distance history", width, height, 1, 1, DXGI_FORMAT_R16G16_FLOAT },
                                               D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const BufferRef jobs = g.createBuffer({ "R reflection jobs", (uint64_t)width * height * 4, 4 });
    const BufferRef results = g.createBuffer({ "R reflection results", (uint64_t)width * height * 12, 12 });
    // Reconstruction layers (ReflectionInternal.hlsli): per-job records beside the results, per-ray records beside the rays.
    const bool lumen = s.lumen;  // the ray-reuse pipeline (ReflectionReuse.hlsli): its own resolve, history and filter
    const bool lumenOnly = s.lumenOnly;  // ... with the Lumen trace as its only world rays (ReflectionLumenTrace.hlsl)
    // the lobe tail's share that is not sampled, as the trace, its screen traces and the resolve's replays read it
    const uint32_t samplingBias16 = lumen ? (uint32_t)std::lround(s.lumenSamplingBias * 65535.0f) : 0u;
    const bool layers = s.layers && !lumen;
    const BufferRef jobLayers = layers ? g.createBuffer({ "R reflection job layers", (uint64_t)width * height * 24, 0 }) : BufferRef{};  // REFL_LAYER_JOB_BYTES
    const BufferRef args = g.importBuffer(m_arguments.Get(), { "R reflection dispatch arguments", (uint64_t)kMaxBands * kArgumentsBytes, 0 });
    const BufferRef cache = fc.resources.giCache;
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = main.frameConstants;
    ShaderLibrary& shaders = fc.shaders;
    const float focal = height / (2.0f * std::tan(main.view.verticalFov * 0.5f));

    // Planar mirrors (design 2.6 cost formula, header comment): read back the per-plane pixel counts and the measured
    // costs of the frame that last used the slot framesInFlight frames ago; visible planes become candidates (counted
    // this frame), the ones whose rays cost more than their view get a reflection camera and their pixels take no rays.
    if (m_planesRevision != fc.scene.revision()) buildPlanes(fc.scene);
    const uint32_t ringSlot = (uint32_t)(fc.frame.frameIndex % kPlanarSlots);
    {
        // Frame f - framesInFlight is complete when frame f is recorded (FramePassContext::framesInFlight).
        if (fc.framesInFlight >= kPlanarSlots) fail("R reflections: %u frames in flight need more planar ring slots", fc.framesInFlight);
        const uint32_t oldSlot = (uint32_t)((fc.frame.frameIndex + kPlanarSlots - fc.framesInFlight) % kPlanarSlots);
        if (m_slotFrame[oldSlot] != UINT64_MAX && fc.frame.frameIndex >= m_slotFrame[oldSlot] + fc.framesInFlight)
        {
            const uint8_t* slot = m_readbackMapped + oldSlot * kReadbackStride;
            const uint32_t* counts = reinterpret_cast<const uint32_t*>(slot);
            for (size_t c = 0; c < m_slotPlanes[oldSlot].size(); ++c)
            {
                const uint32_t p = m_slotPlanes[oldSlot][c];
                // Only counts of the plane's current run of consecutive candidate frames (its view may have changed).
                if (p < m_planePixels.size() && m_planeRunStart[p] <= m_slotFrame[oldSlot]) m_planePixels[p] = counts[c];
            }
            // Costs: the trace per ray, each view per mirror pixel it drew (running averages over ~16 frames).
            const uint64_t* ticks = reinterpret_cast<const uint64_t*>(slot + kTicksOffset);
            const uint32_t* counters = reinterpret_cast<const uint32_t*>(slot + kJobsOffset);  // total jobs, M, G samples, G pixels
            const double traced = (double)counters[1] + (double)counters[2] * m_settings.raysPerSample;
            // Ray slots for the split passes: 1.5 x the traced rays once they pass 3/4 of the capacity (a frame beyond it
            // traces the overflowing jobs inline: the same values, slower). At most 2^24 slots (60 B each, 1.01 GB): at 2^26
            // slots (3.8 GB) and still at 2^25 (1.9 GB) slots lost their stores [measured: a 64-ray reference against the
            // inline path, -3.9 % and -1.8 %]; 4 K frames trace at most ~9 M rays (full-screen mirror plus G).
            if (traced > 0.75 * m_rayCapacity)
                while (m_rayCapacity < 1.5 * traced && m_rayCapacity < (1u << 24)) m_rayCapacity *= 2;
            if (m_settings.deterministic) {}  // (measured times are not used: the choice from the priors)
            else if (ticks[1] > ticks[0] && traced >= 4096)
            {
                const float sample = (float)((ticks[1] - ticks[0]) * m_tickMs * 1e6 / traced);
                m_rayNs = m_rayNs > 0 ? m_rayNs + (sample - m_rayNs) / 16 : sample;
            }
            m_lastViewMs = 0;
            for (size_t v = 0; v < (m_settings.deterministic ? 0 : m_slotViewPlanes[oldSlot].size()); ++v)
            {
                const uint64_t t0 = ticks[2 + 2 * v], t1 = ticks[3 + 2 * v];
                if (t1 <= t0) continue;  // not bracketed (the view's passes ran on another queue): no measurement
                const float ms = (float)((t1 - t0) * m_tickMs);
                m_lastViewMs += ms;
                const uint32_t p = m_slotViewPlanes[oldSlot][v], mirror = m_slotViewPixels[oldSlot][v];
                if (p < m_planeViewMs.size())
                {
                    m_planeViewMs[p] = ms;
                    m_planeViewFrame[p] = m_slotFrame[oldSlot];
                }
                addViewSample(mirror, ms * 1e6, 1);
            }
        }
    }
    PlanarGpu planar{};
    TextureRef planarColor[kPlanarMax];
    // Views chosen this frame: recorded after the classification, which writes their mirror masks (INTERFACES v1.22).
    struct PlanarView
    {
        ViewDesc desc;
        TextureRef mask, tileMask;
    } planarViews[kPlanarMax];
    m_lastPlanarViews = m_lastPlanarPixels = m_lastCandidates = m_lastRectPixels = 0;
    std::vector<uint32_t>& slotPlanes = m_slotPlanes[ringSlot];
    slotPlanes.clear();
    m_slotViewPlanes[ringSlot].clear();
    m_slotViewPixels[ringSlot].clear();
    // Exact threshold of the cost choice: a view costs at least a + b x pixels, rays c x pixels, so a plane can pay off
    // only when c > b and pixels > a / (c - b). Until the trace has been measured no plane is chosen.
    const float rayNs = m_rayNs, viewNs = m_viewNsPerPixel, viewFixedNs = m_viewFixedNs;
    const bool planarCanWin = fc.services.renderView && (m_planarForced || rayNs > viewNs);
    const double minPixels = m_planarForced ? 1.0 : planarCanWin ? viewFixedNs / (rayNs - viewNs) : 1e30;
    const auto selectStart = std::chrono::steady_clock::now();
    if (m_planarEnabled && !m_planes.empty() && planarCanWin)
    {
        struct Candidate
        {
            uint32_t plane, x, y, w, h;
            bool eligible;  // a current read-back count whose rays cost more than the plane's view
            bool current;   // its read-back count is of this run of candidate frames (m_planePixels is its size on screen)
        };
        std::vector<Candidate> candidates;
        const float4x4& vp = main.view.viewProj;
        const float3 cam = main.view.position;
        const uint64_t frame = fc.frame.frameIndex;
        // Exact bound: a plane of area A whose bounds are at distance d covers a solid angle <= A / d^2, and a pixel
        // subtends >= cos^3(corner) / f^2 sr, so pixels <= A f^2 / (d^2 cos^3). Planes whose bound is below minPixels
        // can never pay for a camera and are not visited (BVH on bounds and max area).
        const float tanY = std::tan(main.view.verticalFov * 0.5f), tanX = tanY * width / height;
        const float cosCorner = 1.0f / std::sqrt(1 + tanX * tanX + tanY * tanY);
        const float reach = minPixels > 0 ? (float)(focal * focal / (minPixels * cosCorner * cosCorner * cosCorner)) : 1e30f;  // d^2 <= A * reach
        auto distance2 = [&](const float3& lo, const float3& hi) {
            const float dx = std::max({ lo.x - cam.x, 0.0f, cam.x - hi.x }), dy = std::max({ lo.y - cam.y, 0.0f, cam.y - hi.y }),
                        dz = std::max({ lo.z - cam.z, 0.0f, cam.z - hi.z });
            return dx * dx + dy * dy + dz * dz;
        };
        auto consider = [&](uint32_t k) {
            const PlanarReflector& r = m_planes[k];
            if (r.plane.x * cam.x + r.plane.y * cam.y + r.plane.z * cam.z + r.plane.w <= 0) return;
            if (distance2(r.lo, r.hi) > r.area * reach) return;
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            bool behind = false, visible = false;
            for (int c = 0; c < 8; ++c)
            {
                const float3 p{ (c & 1) ? r.hi.x : r.lo.x, (c & 2) ? r.hi.y : r.lo.y, (c & 4) ? r.hi.z : r.lo.z };
                const float cx = vp.m[0][0] * p.x + vp.m[0][1] * p.y + vp.m[0][2] * p.z + vp.m[0][3];
                const float cy = vp.m[1][0] * p.x + vp.m[1][1] * p.y + vp.m[1][2] * p.z + vp.m[1][3];
                const float cw = vp.m[3][0] * p.x + vp.m[3][1] * p.y + vp.m[3][2] * p.z + vp.m[3][3];
                if (cw <= main.view.nearPlane)
                {
                    behind = true;
                    continue;
                }
                visible = true;
                const float sx = (cx / cw * 0.5f + 0.5f) * width, sy = (0.5f - cy / cw * 0.5f) * height;
                x0 = std::min(x0, sx);
                x1 = std::max(x1, sx);
                y0 = std::min(y0, sy);
                y1 = std::max(y1, sy);
            }
            if (!visible && !behind) return;
            if (behind) x0 = y0 = 0, x1 = (float)width, y1 = (float)height;  // crosses the near plane: whole view
            // The origin on the 8 x 8 grid: a classification tile is one tile of the view's tile mask (ReflectionClassify).
            const uint32_t ix0 = (uint32_t)std::clamp(std::floor(x0) - 1, 0.0f, (float)width) & ~7u, iy0 = (uint32_t)std::clamp(std::floor(y0) - 1, 0.0f, (float)height) & ~7u;
            uint32_t ix1 = (uint32_t)std::clamp(std::ceil(x1) + 1, 0.0f, (float)width), iy1 = (uint32_t)std::clamp(std::ceil(y1) + 1, 0.0f, (float)height);
            // The size in 64-pixel steps (within the view): the reflection camera's textures and its whole chain take it,
            // and the render graph's plan key their sizes (an exact rectangle changed them with every camera move). The
            // camera draws only the mirror's pixels (planarMask): the margin costs mask texels.
            if (ix1 > ix0) ix1 = std::min(width, ix0 + (ix1 - ix0 + 63) / 64 * 64);
            if (iy1 > iy0) iy1 = std::min(height, iy0 + (iy1 - iy0 + 63) / 64 * 64);
            const uint64_t rect = (uint64_t)(ix1 - ix0) * (iy1 - iy0);
            if (ix1 <= ix0 || iy1 <= iy0 || (double)rect < minPixels) return;  // the rectangle bounds the count too
            const bool current = m_planeLastSeen[k] + 1 == frame && m_planeRunStart[k] + fc.framesInFlight <= frame;
            // The view's cost: measured within the last second, else the model a + b x mirror pixels (the rectangle, an
            // upper bound, until the plane has a current count; it is not eligible before).
            const bool measured = m_planeViewFrame[k] != UINT64_MAX && frame < m_planeViewFrame[k] + 60;
            const double viewCost = measured ? m_planeViewMs[k] * 1e6 : viewFixedNs + (double)viewNs * (current ? m_planePixels[k] : rect);
            const double rayCost = (double)rayNs * m_planePixels[k];
            const bool hadCamera = m_planeCameraFrame[k] + 1 == frame;
            const bool cheaper = hadCamera ? viewCost < rayCost * 1.1 : viewCost * 1.1 < rayCost;  // hysteresis
            candidates.push_back({ k, ix0, iy0, ix1 - ix0, iy1 - iy0, current && (m_planarForced ? m_planePixels[k] > 0 : cheaper), current });
        };
        uint32_t stack[64], top = 0;
        stack[top++] = 0;
        while (top)
        {
            const PlaneNode& node = m_planeNodes[stack[--top]];
            if (distance2(node.lo, node.hi) > node.maxArea * reach) continue;
            if (node.count)
                for (uint32_t k = node.first; k < node.first + node.count; ++k) consider(m_planeOrder[k]);
            else
            {
                if (top + 2 > 64) fail("R reflections: plane hierarchy deeper than 64");
                stack[top++] = node.first;
                stack[top++] = node.second;
            }
        }
        // The kCandidatesMax planes counted this frame are the largest on screen: by the read-back pixel count where it
        // is current, else by the rectangle (an upper bound: a plane that just came into view, or whose count has not
        // come back yet - framesInFlight frames - competes with its rectangle, so it stays in the list until it is
        // counted). Defect queue 5 (game request 78): with eligible planes first and the cut after that order, the first
        // 64 counted planes (a bath's glazed tiles) kept the list for good, and a larger plane that came into view later
        // (the shower mirror) was never counted, so never eligible.
        auto screenSize = [&](const Candidate& c) { return c.current ? (uint64_t)m_planePixels[c.plane] : (uint64_t)c.w * c.h; };
        std::sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
            const uint64_t sa = screenSize(a), sb = screenSize(b);
            return sa != sb ? sa > sb : a.plane < b.plane;
        });
        if (candidates.size() > kCandidatesMax) candidates.resize(kCandidatesMax);
        // Among them: eligible planes by count first (the leading ones get the cameras), then the others by rectangle
        // (they are counted and may qualify framesInFlight later).
        std::stable_sort(candidates.begin(), candidates.end(), [&](const Candidate& a, const Candidate& b) {
            if (a.eligible != b.eligible) return a.eligible;
            if (a.eligible && m_planePixels[a.plane] != m_planePixels[b.plane]) return m_planePixels[a.plane] > m_planePixels[b.plane];
            return (uint64_t)a.w * a.h > (uint64_t)b.w * b.h;
        });
        for (const Candidate& c : candidates)
        {
            const bool view = fc.services.renderView && c.eligible && planar.views == planar.candidates && planar.views < s.planarViewsMax;
            auto& slotData = planar.planes[planar.candidates];
            slotData.plane = m_planes[c.plane].plane;
            slotData.rect[0] = c.x;
            slotData.rect[1] = c.y;
            slotData.rect[2] = c.w;
            slotData.rect[3] = c.h;
            if (view)
            {
                PlanarView& pv = planarViews[planar.views];
                pv.desc = ViewDesc::planarReflection(main.view, m_planes[c.plane].plane, c.x, c.y, c.w, c.h);
                pv.mask = g.createTexture({ "R planar mirror mask", c.w, c.h, 1, 1, DXGI_FORMAT_R8_UINT });
                pv.tileMask = g.createTexture({ "R planar mirror tile mask", (c.w + 7) / 8, (c.h + 7) / 8, 1, 1, DXGI_FORMAT_R8_UINT });
                pv.desc.planarMask = pv.mask;
                pv.desc.planarTileMask = pv.tileMask;
                m_viewRects[planar.views] = { c.x, c.y, c.w, c.h };
                m_slotViewPlanes[ringSlot].push_back(c.plane);
                m_slotViewPixels[ringSlot].push_back(m_planePixels[c.plane]);
                m_planeCameraFrame[c.plane] = frame;
                m_lastPlanarPixels += m_planePixels[c.plane];
                m_lastRectPixels += c.w * c.h;
                ++planar.views;
            }
            if (m_planeLastSeen[c.plane] + 1 != frame)
            {
                m_planeRunStart[c.plane] = frame;
                m_planePixels[c.plane] = 0;
            }
            m_planeLastSeen[c.plane] = frame;
            slotPlanes.push_back(c.plane);
            ++planar.candidates;
        }
        m_lastSelectMs = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - selectStart).count();
        m_lastPlanarViews = planar.views;
        m_lastCandidates = planar.candidates;
    }
    m_slotFrame[ringSlot] = fc.frame.frameIndex;
    const uint32_t planarOffset = ringSlot * kPlanarSlotBytes;
    std::memcpy(m_planarMapped + planarOffset, &planar, sizeof planar);
    const BufferRef planarCounts = g.importBuffer(m_planarCounts.Get(), { "R planar counts", kCandidatesMax * 4, 0 });
    const BufferRef planarReadback = g.importBuffer(m_planarReadback.Get(), { "R planar readback", kPlanarSlots * kReadbackStride, 0 });
    const uint64_t readbackOffset = (uint64_t)ringSlot * kReadbackStride;
    const uint32_t planarSrv = m_planarSrv;

    if (fresh)
        g.addPass("r.refl.history.clear", QueueType::Compute, [&](PassBuilder& b) { b.use(history, Use::UavCompute); },
                  [&shaders, history, width, height](PassContext& c) {
                      const uint32_t k[4] = { c.uav(history), width, height, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionHistoryClear"));
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                  });
    g.addPass("r.refl.begin", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(reflection, Use::UavCompute);
                  b.use(args, Use::UavCompute);
                  b.use(planarCounts, Use::UavCompute);
              },
              [&shaders, reflection, args, planarCounts, tilesX, tilesY, height](PassContext& c) {
                  const uint32_t k[8] = { c.uav(reflection), c.uav(args), tilesX, tilesY, height, c.uav(planarCounts), 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionBegin"));
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((std::max(tilesX, kCandidatesMax) + 7) / 8, (tilesY + 7) / 8, 1);
              });
    uint32_t spacingLog2 = 0;  // reflection.g_sample_spacing_px bound (1, 2, 4 or 8)
    while ((2u << spacingLog2) <= s.maxSpacing && spacingLog2 < 3) ++spacingLog2;
    // reflection.lumen_only: M's material word - a clearcoat pixel's reflection is traced, resolved and filtered at its
    // coat's roughness (ReflectionInternal.hlsli reflTopLayerRoughness; M composes it as the top layer)
    const TextureRef words = lumenOnly ? main.materialWord : TextureRef{};
    g.addPass("r.refl.classify", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  if (words.valid()) b.use(words, Use::SrvCompute);
                  if (lobes.valid() && !lumen) b.use(lobes, Use::SrvCompute);
                  b.use(history, Use::SrvCompute);
                  b.use(modes, Use::UavCompute);
                  b.use(jobs, Use::UavCompute);
                  b.use(args, Use::UavCompute);
                  b.use(reflection, Use::UavCompute);
                  b.use(planarCounts, Use::UavCompute);
                  for (uint32_t v = 0; v < planar.views; ++v)
                  {
                      b.use(planarViews[v].mask, Use::UavCompute);
                      b.use(planarViews[v].tileMask, Use::UavCompute);
                  }
              },
              [&shaders, depth, gbuffer, lobes, history, modes, jobs, args, reflection, s, focal, width, height, tilesX, tilesY, frameConstants, planarSrv,
               planarOffset, planarCounts, planarViews, viewCount = planar.views, spacingLog2, lumen, words, roughSpecularValid = main.giRoughSpecular.valid()](PassContext& c) {
                  uint32_t k[32] = { c.srv(depth), c.srv(gbuffer), lobes.valid() && !lumen ? c.srv(lobes) : 0xFFFFFFFFu, c.srv(history),
                                     c.uav(modes), c.uav(jobs), c.uav(args), c.uav(reflection),
                                     asU(s.kHalfAngle), asU(s.mirrorRoughness), asU(focal), height,
                                     width, height, planarSrv, planarOffset, c.uav(planarCounts), spacingLog2,
                                     (lumen ? 1u : 0u) | (lumen && s.lumenRoughFromGather && roughSpecularValid ? 2u : 0u), asU(s.lumenMaxRoughness) };
                  for (uint32_t v = 0; v < kPlanarMax; ++v)
                  {
                      k[20 + v] = v < viewCount ? c.uav(planarViews[v].mask) : 0xFFFFFFFFu;
                      k[24 + v] = v < viewCount ? c.uav(planarViews[v].tileMask) : 0xFFFFFFFFu;
                  }
                  k[28] = words.valid() ? c.srv(words) : 0xFFFFFFFFu;
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionClassify"));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(tilesX, tilesY, 1);
              });
    g.addPass("r.refl.jobs", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(modes, Use::UavCompute);
                  b.use(jobs, Use::UavCompute);
                  b.use(args, Use::UavCompute);
                  b.use(reflection, Use::UavCompute);
              },
              [&shaders, modes, jobs, args, reflection, width, height, tilesX, tilesY](PassContext& c) {
                  const uint32_t k[12] = { c.uav(modes), c.uav(jobs), c.uav(args), 0, width, height, tilesX, tilesY, c.uav(reflection), height, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionJobs"));
                  c.computeConstants(k, 12);
                  c.cmd->Dispatch(tilesX, tilesY, 1);
              });
    // Mask aprons (v1.28): 3 x 3 dilation of each view's mirror pixels as value 2, tile masks from the dilated masks.
    for (uint32_t v = 0; v < planar.views; ++v)
    {
        const TextureRef mask = planarViews[v].mask, tileMask = planarViews[v].tileMask;
        const uint32_t w = planarViews[v].desc.width, h = planarViews[v].desc.height;
        g.addPass("r.refl.planar.apron", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(mask, Use::UavCompute);
                      b.use(tileMask, Use::UavCompute);
                  },
                  [&shaders, mask, tileMask, w, h](PassContext& c) {
                      const uint32_t k[4] = { c.uav(mask), c.uav(tileMask), w, h };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionPlanarApron"));
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                  });
    }
    // The reflection cameras (their masks come from the classification and the aprons above).
    for (uint32_t v = 0; v < planar.views; ++v)
    {
        ID3D12QueryHeap* heap = m_timestamps.Get();
        const uint32_t tick = ringSlot * kTicks + 2 + 2 * v;
        g.addPass("r.refl.view.begin", QueueType::Graphics, [&](PassBuilder& b) { b.keep(); },
                  [heap, tick](PassContext& pc) { pc.cmd->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, tick); });
        planarColor[v] = fc.services.renderView(fc, planarViews[v].desc).color;
        g.addPass("r.refl.view.end", QueueType::Graphics, [&](PassBuilder& b) { b.keep(); },
                  [heap, tick](PassContext& pc) { pc.cmd->EndQuery(heap, D3D12_QUERY_TYPE_TIMESTAMP, tick + 1); });
    }
    // Rays buffer of the split passes (ReflectionRay.hlsli): header, hit records, ray -> job, values, shadow rays.
    const uint32_t rayCapacity = (s.experimentDisable & 64) || lumenOnly ? 0 : m_rayCapacity;  // (lumen_only: no rays buffer)
    static_assert(48 + (1ull << 24) * 60 < (1ull << 30), "the rays buffer stays under 1 GiB");  // 64: every job inline (A/B of the split)
    // (bands of the ray passes: by what the frame can hold - a job per pixel, a slot per unit of capacity)
    // (the trace pass: a job traces up to raysPerSample rays in its thread, so its band is kBand rays, not kBand jobs)
    const uint32_t jobBand = kBand / std::max(s.raysPerSample & 0xFFu, 1u);
    const uint32_t jobBands = bandsFor((uint64_t)width * height, jobBand), slotBands = bandsFor(rayCapacity, kBand),
                   inlineBands = bandsFor((uint64_t)width * height, kInlineBand);
    const BufferRef raysBuffer = g.createBuffer({ "R reflection rays", 48 + (uint64_t)rayCapacity * 60, 0 });  // REFL_RAYS_HEADER + REFL_RAYS_SLOT_BYTES
    // The GI hit accumulator pool (GiAccPool.hlsli; valid when gi.hit_accumulator's pool runs): reflection hits read it.
    const BufferRef accPool = s.hitAccumulator ? fc.resources.giAccumulator : BufferRef{};
    // The surface cache (Passes/SurfaceCache/SurfaceCache.hlsli): one persistent raw buffer; cleared when new and on a scene
    // revision (materials, geometry). It is world space: a camera cut does not reset it.
    BufferRef surfaceCache;
    bool surfaceCacheClear = false;
    const uint32_t surfaceCacheEntries = 1u << s.scEntriesLog2;
    if (s.surfaceCache && !s.scMeshCards)
    {
        const uint64_t bytes = kSurfaceCacheHeaderBytes + (uint64_t)surfaceCacheEntries * kSurfaceCacheCellBytes +
                               (uint64_t)(surfaceCacheEntries / 4) * kSurfaceCacheProbeBytes;
        if (!m_surfaceCache || m_surfaceCacheEntries != surfaceCacheEntries)
        {
            if (m_surfaceCache) m_device.deferRelease(m_surfaceCache);
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC1 d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = bytes;
            d.Height = d.DepthOrArraySize = d.MipLevels = 1;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                           IID_PPV_ARGS(m_surfaceCache.ReleaseAndGetAddressOf())),
                  "surface cache");
            m_surfaceCache->SetName(L"R surface cache");
            m_surfaceCacheEntries = surfaceCacheEntries;
            m_surfaceCacheRef = BufferRef{};  // (an import of the buffer just released)
            m_surfaceCacheRevision = fc.scene.revision();
            surfaceCacheClear = true;
        }
        if (fc.scene.revision() != m_surfaceCacheRevision)
        {
            m_surfaceCacheRevision = fc.scene.revision();
            surfaceCacheClear = true;
        }
        surfaceCache = surfaceCacheBuffer(fc);
    }
    const bool hitsUseSurfaceCache = surfaceCache.valid() && lumen && s.lumenHitSurfaceCache;
    // surface_cache.mesh_cards (unx/refl/SurfaceCacheCards.h, recorded before GI): the card frame hits read through
    const SurfaceCacheCardRefs cardRefs = s.scMeshCards ? fc.resources.cards : SurfaceCacheCardRefs{};
    const BufferRef cardFrame = cardRefs.frame;
    const bool hitsUseCards = cardFrame.valid() && lumen && s.lumenHitSurfaceCache;
    // Screen traces before the world rays (ReflectionScreenTrace.hlsl): a ray that meets a visible surface takes the
    // previous frame's colour there and its job traces no world ray. Without that colour (no upscale history) every
    // ray is a world ray.
    ScreenTraceInputs screen;
    if (lumen && s.lumenScreenTraces) screen = screenTraceInputs(fc, main);
    const bool screenTraces = screen.hzb.valid() && screen.prevColor.valid();
    const bool screenContinue = screenTraces && s.lumenScreenContinue;  // world rays start at their screen traces' ends
    // output.screen_trace_source = 0: the previous colour's alpha is its frame's depth (the history depth test, ScreenTrace.hlsli)
    const bool historyDepth = fc.quality.has("output.screen_trace_source") && fc.quality.integer("output.screen_trace_source") == 0;
    const BufferRef rayLayers = layers ? g.createBuffer({ "R reflection ray layers", std::max<uint64_t>((uint64_t)rayCapacity * 16, 16), 0 }) : BufferRef{};  // REFL_LAYER_RAY_BYTES
    if (!lumenOnly)
    g.addPass("r.refl.args", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::UavCompute);
                  b.use(raysBuffer, Use::UavCompute);
                  if (layers)
                  {
                      b.use(rayLayers, Use::UavCompute);
                      b.use(jobLayers, Use::UavCompute);
                  }
                  if (accPool.valid()) b.use(accPool, Use::SrvCompute);
                  if (hitsUseSurfaceCache) b.use(surfaceCache, Use::UavCompute);
              },
              [&shaders, args, raysBuffer, rayCapacity, layers, rayLayers, jobLayers, accPool, surfaceCache, hitsUseSurfaceCache, jobBands, jobBand, cardFrame, hitsUseCards,
               hitFlags = (s.hitConeLobes ? 1u : 0u) | (s.hitOrientedLights ? 2u : 0u) | (layers && s.layerFilter && s.hitStrictRead ? 4u : 0u) |
                          (s.lumenSurfaceCacheView ? 8u | ((s.lumenSurfaceCacheViewComponent & 7u) << 8) : 0u) | (screenContinue ? 16u : 0u)](PassContext& c) {
                  // (the layer buffers' UAVs and the hit shading's flags into the rays header: the shade, combine and inline
                  // passes find them there)
                  const uint32_t k[16] = { c.uav(args), 2, kDescStride, (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width), c.uav(raysBuffer), rayCapacity,
                                           layers ? c.uav(rayLayers) : 0xFFFFFFFFu, layers ? c.uav(jobLayers) : 0xFFFFFFFFu, hitFlags,
                                           accPool.valid() ? c.srv(accPool) : 0xFFFFFFFFu, hitsUseSurfaceCache ? c.uav(surfaceCache) : 0xFFFFFFFFu, jobBands,
                                           kArgumentsBytes, jobBand, hitsUseCards ? c.srv(cardFrame) : 0xFFFFFFFFu, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionArgs"));
                  c.computeConstants(k, 16);
                  c.cmd->Dispatch(1, 1, 1);
              });

    uint32_t scene[8];
    rays.recordDecals(fc, main);  // decals at hits (HitDecals.hlsli): header words before rootConstants
    rays.rootConstants(scene);
    const FrameResources& fr = fc.resources;
    const bool atmosphere = fr.transmittanceLut.valid() && fr.multiScatterLut.valid() && fr.skyViewLut.valid() && fr.aerialPerspective.valid();
    const TextureRef luts[4] = { fr.transmittanceLut, fr.multiScatterLut, fr.skyViewLut, fr.aerialPerspective };
    const int variant = atmosphere ? 0 : 1;
    rt::RayPipeline& pipeline = rt::RayPipeline::get(
        fc.device, shaders, rt::standardRayPipeline(lumenOnly ? kLumenTraceLibrary[variant] : kTraceLibrary[variant], { lumenOnly ? "ReflectionLumenTraceGen" : "ReflectionTraceGen" }));
    const float3 sky = m_skyRadiance, sun = m_sunIlluminance;
    const float rayLength = (float)fc.quality.number("gi.ray_length_m");
    const uint32_t frame = (uint32_t)fc.frame.frameIndex;
    ID3D12Resource* argumentResource = m_arguments.Get();
    const uint32_t experiment = s.experimentDisable;
    const BufferRef exactCounts = rays.exactHitCounts();
    const TextureRef probeMaps = main.screenProbeMaps;
    const rt::RayScene::VsmRefs vsm = rt::RayScene::vsmRefs(fc.resources);  // S's shadowPages recorded before
    rt::RayScene* rayScene = &rays;
    const uint64_t frameIndex = fc.frame.frameIndex;
    ID3D12QueryHeap* timestamps = m_timestamps.Get();
    const uint32_t firstTick = ringSlot * kTicks;
    // The inputs a refraction list traced later this frame binds (recordRefraction).
    m_refract = {};
    m_refract.valid = true, m_refract.atmosphere = atmosphere, m_refract.frameIndex = frameIndex;
    m_refract.sky = sky, m_refract.sun = sun, m_refract.rayLength = rayLength;
    for (int i = 0; i < 4; ++i) m_refract.luts[i] = luts[i];
    m_refract.cache = cache, m_refract.vsm = vsm, m_refract.frame = frame, m_refract.experiment = experiment;
    std::memcpy(m_refract.scene, scene, sizeof scene);
    m_refract.rays = &rays, m_refract.frameConstants = frameConstants, m_refract.variant = variant;
    if (hitsUseSurfaceCache && s.lumenRefractionSurfaceCache) m_refract.surfaceCache = surfaceCache;  // water's and glass's ray hits
    if (lumenOnly && hitsUseCards && s.lumenRefractionSurfaceCache) m_refract.cards = cardRefs;
    // Root constants shared by the trace, shade, shadow and combine passes (ReflectionRay.hlsli).
    // gi = false (the traversal and the local-light shadow rays): GI's cache and screen probes are not bound (UNX_NONE)
    // nor declared, so those passes do not wait for GI's block (output.async_compute_passes).
    auto constantsFor = [jobs, results, modes, probes, depth, gbuffer, cache, luts, atmosphere, sky, sun, rayLength, s, frame, scene, experiment, exactCounts, samplingBias16,
                         probeMaps, vsm, rayScene, frameIndex, raysBuffer](PassContext& c, uint32_t k[32], bool gi = true, uint32_t band = 0) {
        k[0] = c.srv(jobs);
        k[1] = c.uav(results);
        k[2] = c.srv(modes);
        k[3] = gi ? c.srv(probes) : 0xFFFFFFFFu;
        k[4] = asU(sky.x);
        k[5] = asU(sky.y);
        k[6] = asU(sky.z);
        k[7] = asU(rayLength);
        for (int i = 0; i < 4; ++i) k[8 + i] = atmosphere ? c.srv(luts[i]) : 0xFFFFFFFFu;
        k[12] = asU(sun.x);
        k[13] = asU(sun.y);
        k[14] = asU(sun.z);
        k[15] = gi ? c.srv(probeMaps) : 0xFFFFFFFFu;
        k[16] = c.srv(depth);
        k[17] = c.srv(gbuffer);
        k[18] = gi ? c.uav(cache) : 0xFFFFFFFFu;
        // (the band of a ray dispatch: ReflectionRay.hlsli reflBand; the bias: reflNextDirection)
        k[19] = (s.raysPerSample & 0xFFu) | ((band & 0xFFu) << 8) | (samplingBias16 << 16);
        k[20] = (frame & 0xFFFFFFu) | (experiment << 24);
        k[21] = c.uav(raysBuffer);
        k[22] = rayScene->vsmSrvs(c, vsm, frameIndex, 1);  // S's VSM for sun visibility at hits (UNX_NONE: rays)
        k[23] = exactCounts.valid() ? c.uav(exactCounts) : 0xFFFFFFFFu;
        std::memcpy(&k[24], scene, sizeof scene);
    };
    // Every resource constantsFor names, declared by each pass that binds it (all-shading uses cover DispatchRays and compute).
    auto declareShared = [&](PassBuilder& b, bool gi = true) {
        b.use(raysBuffer, Use::UavGraphics);
        b.use(jobs, Use::SrvGraphics);
        b.use(modes, Use::SrvGraphics);
        if (gi)
        {
            b.use(probes, Use::SrvGraphics);
            b.use(probeMaps, Use::SrvGraphics);
        }
        b.use(depth, Use::SrvGraphics);
        b.use(gbuffer, Use::SrvGraphics);
        if (gi) b.use(cache, Use::UavGraphics);
        if (gi && accPool.valid()) b.use(accPool, Use::SrvGraphics);  // (the passes that shade: the accumulator's cell means)
        if (gi && hitsUseSurfaceCache) b.use(surfaceCache, Use::UavGraphics);  // (the passes that shade: hits mark and read their cells)
        if (gi && hitsUseCards) declareSurfaceCacheCards(b, cardRefs, Use::SrvGraphics);  // (the passes that shade: hits read their cards)
        if (gi && layers)  // (the passes that shade or combine: the layer records)
        {
            b.use(rayLayers, Use::UavGraphics);
            b.use(jobLayers, Use::UavGraphics);
        }
        b.use(results, Use::UavGraphics);
        if (exactCounts.valid()) b.use(exactCounts, Use::UavGraphics);
        rt::RayScene::declareVsm(b, vsm);
        rays.declareTraversal(b);
        rays.declareDecals(b);
        if (atmosphere)
            for (const TextureRef& t : luts) b.use(t, Use::SrvGraphics);
    };
    {
        if (screenTraces)
        {
            const FrameContext::Upscale& up = fc.frame.upscale;
            g.addPass("r.refl.screentrace", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(modes, Use::SrvCompute);
                          b.use(results, Use::UavCompute);
                          b.use(depth, Use::SrvCompute);
                          b.use(gbuffer, Use::SrvCompute);
                          b.use(jobs, Use::UavCompute);
                          b.use(screen.hzb, Use::SrvCompute);
                          b.use(screen.prevColor, Use::SrvCompute);
                          if (words.valid()) b.use(words, Use::SrvCompute);
                      },
                      [&shaders, modes, results, depth, gbuffer, jobs, screen, frame, width, height, rayLength, frameConstants, s, samplingBias16, screenContinue, words, historyDepth,
                       outW = g.desc(screen.prevColor).width, outH = g.desc(screen.prevColor).height, ratio = up.exposureRatio,
                       prevViewProj = up.prevViewProj](PassContext& c) {
                          uint32_t k[36] = { c.srv(modes), c.uav(results), c.srv(depth), c.srv(gbuffer), c.uav(jobs), c.srv(screen.hzb), c.srv(screen.prevColor), frame,
                                             width, height, (outW & 0xFFFFu) | (outH << 16), asU(screenContinue ? std::max(s.lumenScreenPullback, 0.0f) : -1.0f), asU(rayLength), (s.lumenScreenIterations & 0xFFFFu) | (samplingBias16 << 16), asU(s.lumenScreenThickness),
                                             asU(ratio) };
                          for (int r = 0; r < 4; ++r)
                              for (int col = 0; col < 4; ++col) k[16 + 4 * r + col] = asU(prevViewProj.m[r][col]);
                          k[32] = words.valid() ? c.srv(words) : 0xFFFFFFFFu;
                          k[33] = historyDepth ? 1u : 0u;
                          c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionScreenTrace"));
                          c.computeConstants(k, 36);
                          c.bindFrameConstants(frameConstants);
                          c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                      });
        }
    }
    if (lumenOnly)
    {
        // reflection.lumen_only: the jobs' world rays in bands of kBand threads, one ray each; a hit's value is final
        // (ReflectionLumenTrace.hlsl) - the resolve passes below read the results as they read the combine pass's.
        const uint32_t lumenBands = bandsFor((uint64_t)width * height, kBand);
        g.addPass("r.refl.lumen.args", QueueType::Compute, [&](PassBuilder& b) { b.use(args, Use::UavCompute); },
                  [&shaders, args, lumenBands](PassContext& c) {
                      const uint32_t k[8] = { c.uav(args), 2, kDescStride, kLumenDescOffset + (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width),
                                              kArgumentsBytes, kBand, lumenBands, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionLumenArgs"));
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch(1, 1, 1);
                  });
        const bool sceneColour = screenTraces && s.lumenSceneColorAtHit;
        const FrameContext::Upscale& up = fc.frame.upscale;
        const TextureRef prevColor = sceneColour ? screen.prevColor : TextureRef{};
        const uint32_t prevW = sceneColour ? g.desc(screen.prevColor).width : 0, prevH = sceneColour ? g.desc(screen.prevColor).height : 0;
        g.addPass("r.refl.lumen.trace", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(args, Use::IndirectArgs);
                      b.use(jobs, Use::SrvGraphics);
                      b.use(results, Use::UavGraphics);
                      b.use(depth, Use::SrvGraphics);
                      b.use(gbuffer, Use::SrvGraphics);
                      if (hitsUseCards) declareSurfaceCacheCards(b, cardRefs, Use::SrvGraphics);
                      if (sceneColour) b.use(prevColor, Use::SrvGraphics);
                      if (words.valid()) b.use(words, Use::SrvGraphics);
                      if (exactCounts.valid()) b.use(exactCounts, Use::UavGraphics);
                      rays.declareTraversal(b);
                      rays.declareDecals(b);
                      if (atmosphere)
                          for (const TextureRef& t : luts) b.use(t, Use::SrvGraphics);
                  },
                  [&pipeline, jobs, results, depth, gbuffer, cardFrame, hitsUseCards, prevColor, prevW, prevH, sceneColour, screenContinue, exactCounts, luts, atmosphere, sky, sun,
                   rayLength, frame, scene, samplingBias16, s, frameConstants, argumentResource, variant, timestamps, firstTick, lumenBands, words, historyDepth, ratio = up.exposureRatio,
                   prevViewProj = up.prevViewProj](PassContext& c) {
                      c.bindFrameConstants(frameConstants);
                      c.cmd->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstTick);
                      uint32_t k[48] = {};
                      k[0] = c.srv(jobs);
                      k[1] = c.uav(results);
                      k[2] = asU(ratio);
                      k[3] = (frame & 0xFFFFFFu) | ((screenContinue ? 1u : 0u) | (sceneColour ? 2u : 0u) | (historyDepth ? 4u : 0u)) << 24;
                      k[4] = asU(sky.x), k[5] = asU(sky.y), k[6] = asU(sky.z), k[7] = asU(rayLength);
                      for (int i = 0; i < 4; ++i) k[8 + i] = atmosphere ? c.srv(luts[i]) : 0xFFFFFFFFu;
                      k[12] = asU(sun.x), k[13] = asU(sun.y), k[14] = asU(sun.z);
                      k[15] = exactCounts.valid() ? c.uav(exactCounts) : 0xFFFFFFFFu;
                      k[16] = c.srv(depth);
                      k[17] = c.srv(gbuffer);
                      k[18] = hitsUseCards ? c.srv(cardFrame) : 0xFFFFFFFFu;
                      k[20] = sceneColour ? c.srv(prevColor) : 0xFFFFFFFFu;
                      k[21] = (prevW & 0xFFFFu) | (prevH << 16);
                      k[22] = asU(s.lumenSceneColorThickness);
                      k[23] = words.valid() ? c.srv(words) : 0xFFFFFFFFu;
                      // (the scene colour's normal threshold: its cosine as snorm8 beside the band)
                      const uint32_t cosThreshold =
                          (uint32_t)(int32_t)std::lround(std::cos(std::clamp(s.lumenSceneColorNormalDegrees, 0.0f, 180.0f) * 0.01745329252f) * 127.0f) & 0xFFu;
                      std::memcpy(&k[24], scene, sizeof scene);
                      for (int r = 0; r < 4; ++r)
                          for (int col = 0; col < 4; ++col) k[32 + 4 * r + col] = asU(prevViewProj.m[r][col]);
                      for (uint32_t band = 0; band < lumenBands; ++band)
                      {
                          k[19] = (band & 0xFFu) | (cosThreshold << 8) | (samplingBias16 << 16);
                          c.computeConstants(k, 48);
                          pipeline.dispatchIndirect(c.cmd, argumentResource, (uint64_t)band * kArgumentsBytes + kLumenDescOffset + variant * kDescStride);
                      }
                      c.cmd->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstTick + 1);
                  });
    }
    else
    {
    g.addPass("r.refl.trace", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b, false);
              },
              [&pipeline, constantsFor, frameConstants, argumentResource, variant, timestamps, firstTick, jobBands](PassContext& c) {
                  c.bindFrameConstants(frameConstants);
                  c.cmd->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstTick);
                  for (uint32_t band = 0; band < jobBands; ++band)
                  {
                      uint32_t k[32] = {};
                      constantsFor(c, k, false, band);
                      c.computeConstants(k, 32);
                      pipeline.dispatchIndirect(c.cmd, argumentResource, (uint64_t)band * kArgumentsBytes + 16 + variant * kDescStride);
                  }
              });
    if (screenTraces && s.lumenSceneColorAtHit)
    {
        // The world rays' hits on surfaces the view sees take the previous frame's colour (ReflectionSceneColorAtHit.hlsl)
        // and are not shaded.
        {
            const FrameContext::Upscale& up = fc.frame.upscale;
            g.addPass("r.refl.scenecolor", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(modes, Use::SrvCompute);
                          b.use(results, Use::UavCompute);
                          b.use(depth, Use::SrvCompute);
                          b.use(gbuffer, Use::SrvCompute);
                          b.use(jobs, Use::SrvCompute);
                          b.use(raysBuffer, Use::UavCompute);
                          b.use(screen.prevColor, Use::SrvCompute);
                      },
                      [&shaders, modes, results, depth, gbuffer, jobs, raysBuffer, screen, frame, width, height, frameConstants, s, samplingBias16,
                       outW = g.desc(screen.prevColor).width, outH = g.desc(screen.prevColor).height, ratio = up.exposureRatio,
                       prevViewProj = up.prevViewProj](PassContext& c) {
                          uint32_t k[32] = { c.srv(modes), c.uav(results), c.srv(depth), c.srv(gbuffer), c.srv(jobs), c.uav(raysBuffer), c.srv(screen.prevColor), frame,
                                             width, height, outW, outH, asU(s.lumenSceneColorThickness),
                                             asU(std::cos(std::clamp(s.lumenSceneColorNormalDegrees, 0.0f, 180.0f) * 0.01745329252f)),
                                             asU(samplingBias16 / 65535.0f), asU(ratio) };
                          for (int r = 0; r < 4; ++r)
                              for (int col = 0; col < 4; ++col) k[16 + 4 * r + col] = asU(prevViewProj.m[r][col]);
                          c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionSceneColorAtHit"));
                          c.computeConstants(k, 32);
                          c.bindFrameConstants(frameConstants);
                          c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                      });
        }
    }
    // Split passes (ARCHITECTURE 2.6 revision 1): hit shading in compute, off-screen sun visibility, the jobs' values.
    auto rayArgs = [&](const char* name, uint32_t stage) {
        g.addPass(name, QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(args, Use::UavCompute);
                      b.use(raysBuffer, Use::UavCompute);
                  },
                  [&shaders, args, raysBuffer, stage, slotBands, inlineBands](PassContext& c) {
                      const uint32_t k[16] = { c.uav(args), c.uav(raysBuffer), stage, kPenumbraArgsOffset, kShadeArgsOffset, kCombineArgsOffset,
                                               kShadowDescOffset + (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width),
                                               kLocalDescOffset + (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width),
                                               kInlineDescOffset + (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width), kDescStride, 4, inlineBands,
                                               kArgumentsBytes, kBand, slotBands, kInlineBand };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionRayArgs"));
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    };
    rayArgs("r.refl.rayargs", 0);
    if (surfaceCache.valid())
    {
        // The surface cache's frame (SurfaceCache.hlsli): the upkeep of cells and probes marked up to last frame, the
        // capture rays from the camera position, the cells' direct light and indirect gather, the probes' radiosity -
        // before this frame's hits read and mark. Budgets as the reference's: capture capacity / 64 texels a frame,
        // direct capacity / 32, radiosity capacity / 64 (one probe per 16 texels).
        const uint32_t n = surfaceCacheEntries;
        g.addPass("r.sc.begin", QueueType::Compute, [&](PassBuilder& b) { b.use(surfaceCache, Use::UavCompute); },
                  [&shaders, surfaceCache, n, surfaceCacheClear, frame, frameConstants, s](PassContext& c) {
                      const uint32_t k[8] = { c.uav(surfaceCache), n, frame, surfaceCacheClear ? 1u : 0u, s.scMaxUnused, 1u | (s.scBilinearRead ? 2u : 0u) | (s.scBaseCells ? 4u : 0u), asU(s.scRadiosityCap),
                                              asU(s.scRadiosityFrames) };
                      c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/SurfaceCacheBegin"));
                      c.computeConstants(k, 8);
                      c.bindFrameConstants(frameConstants);
                      // (rows of kSlotRowGroups groups: 2^22 slots are 65536 groups, one over a dispatch dimension's limit)
                      const uint32_t groups = surfaceCacheClear ? (n + 63) / 64 : 1;
                      c.cmd->Dispatch(std::min(groups, kSlotRowGroups), (groups + kSlotRowGroups - 1) / kSlotRowGroups, 1);
                  });
        for (uint32_t table = 0; table < 2; ++table)
            g.addPass(table ? "r.sc.update.probes" : "r.sc.update.cells", QueueType::Compute, [&](PassBuilder& b) { b.use(surfaceCache, Use::UavCompute); },
                      [&shaders, surfaceCache, n, table, frameConstants](PassContext& c) {
                          const uint32_t k[4] = { c.uav(surfaceCache), n, table, 0 };
                          c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/SurfaceCacheUpdate"));
                          c.computeConstants(k, 4);
                          c.bindFrameConstants(frameConstants);
                          const uint32_t groups = ((table ? n / 4 : n) + 63) / 64;
                          c.cmd->Dispatch(std::min(groups, kSlotRowGroups), (groups + kSlotRowGroups - 1) / kSlotRowGroups, 1);
                      });
        rt::RayPipeline& surfaceCacheLight = rt::RayPipeline::get(
            fc.device, shaders, rt::standardRayPipeline(kSurfaceCacheLightLibrary[variant], { "SurfaceCacheSeedGen", "SurfaceCacheCellsGen", "SurfaceCacheProbesGen" }));
        const uint32_t lightFlags = (s.scDirect ? 1u : 0u) | (s.scRadiosity ? 2u : 0u) | (s.scRemainderLight ? 8u : 0u) | (s.scDirectStochastic ? 16u : 0u) | (s.scLightingFeedback ? 32u : 0u) |
                                    (s.scDirectAnalytic ? 64u : 0u) | ((s.scDebugSkip & 15u) << 7) | (s.scShadowRaysOpaque ? 2048u : 0u) | ((s.scDebugSkip & 16u) ? 4096u : 0u) |
                                    (s.scDirectShadowInline ? 8192u : 0u) | (s.scDebugCount ? 16384u : 0u) | (s.scDebugCount == 2 ? 32768u : 0u) |
                                    (s.scDebugCount == 3 ? 65536u : 0u);
        const uint32_t budgets[3] = { std::max(n / s.scCaptureFactor / (s.scCaptureBounces + 1), 1u), std::max(n / s.scDirectFactor, 1u),
                                      std::max(n / s.scRadiosityFactor / 16, 1u) };
        static const char* const kLightNames[3] = { "r.sc.seed", "r.sc.cells", "r.sc.probes" };
        for (uint32_t pass = 0; pass < 3; ++pass)
        {
            if (pass == 2 && !s.scRadiosity) continue;
            if (pass == 1 && s.scDirectPairs)
            {
                // surface_cache.direct_pairs (A, SurfaceCacheLightPairs.cpp): the cells direct light as (cell, light) pairs -
                // select without rays, one shadow ray per pair in bands, store - in place of r.sc.cells
                SurfaceCachePairsInputs pin;
                pin.surfaceCache = surfaceCache;
                pin.budget = budgets[1];
                pin.frame = frame;
                pin.lightFlags = lightFlags;
                pin.skyVariant = variant;
                pin.frameConstants = frameConstants;
                pin.declareShared = [&](PassBuilder& b) { declareShared(b, false); };
                pin.sharedConstants = [constantsFor](PassContext& c, uint32_t* k) { constantsFor(c, k, false); };
                recordSurfaceCacheLightPairs(fc, pin);
                continue;
            }
            g.addPass(kLightNames[pass], QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(surfaceCache, Use::UavGraphics);
                          declareShared(b, false);
                      },
                      [&surfaceCacheLight, constantsFor, frameConstants, surfaceCache, frame, pass, lightFlags, budget = budgets[pass], bounces = s.scCaptureBounces,
                       stochasticFrames = s.scDirectStochasticFrames, minWeight = s.scDirectMinWeight](PassContext& c) {
                          uint32_t k[32] = {};
                          constantsFor(c, k, false);
                          k[0] = c.uav(surfaceCache);
                          k[1] = budget;
                          k[2] = frame;
                          k[3] = lightFlags;
                          k[15] = bounces;
                          k[16] = asU(stochasticFrames);
                          k[17] = asU(minWeight);
                          c.bindFrameConstants(frameConstants);
                          // One dispatch of the capture paths (budget x (1 + bounces) rays: N / 64) and of the radiosity
                          // (budget x 16 rays: N / 64); the direct light in bands of kCellBand cells (each up to 20 TraceRay calls).
                          const uint32_t band = pass == 1 ? kCellBand : budget;
                          for (uint32_t first = 0; first < budget; first += band)
                          {
                              k[18] = first;  // (SurfaceCacheCellsGen: the dispatch's first thread)
                              c.computeConstants(k, 32);
                              surfaceCacheLight.dispatch(c.cmd, pass, std::min(band, budget - first), 1);
                          }
                      });
        }
    }
    // Local-light samples of the hits: their shadow rays before the compute shading (bit 30 of the hit records). Declared
    // before the inline passes (which read GI's cache; they never touch the rays buffer), so it runs while GI's block is
    // still on the async queue.
    rt::RayPipeline& localShadowPipeline = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(kLocalShadowLibrary, { "ReflectionLocalShadowGen" }));
    g.addPass("r.refl.localshadow", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b, false);
              },
              [&localShadowPipeline, constantsFor, frameConstants, argumentResource, slotBands](PassContext& c) {
                  c.bindFrameConstants(frameConstants);
                  for (uint32_t band = 0; band < slotBands; ++band)
                  {
                      uint32_t k[32] = {};
                      constantsFor(c, k, false, band);
                      c.computeConstants(k, 32);
                      localShadowPipeline.dispatchIndirect(c.cmd, argumentResource, (uint64_t)band * kArgumentsBytes + kLocalDescOffset);
                  }
              });
    // Jobs over the ray capacity: traced, shaded and combined in their own ray generation library.
    for (uint32_t mode = 0; mode < 2; ++mode)
    {
        rt::RayPipeline& inlinePipeline =
            rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(kInlineLibrary[variant][mode][s.batchGiCorners], { "ReflectionTraceInlineGen" }));
        g.addPass(mode == 0 ? "r.refl.inline.m" : "r.refl.inline.g", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(args, Use::IndirectArgs);
                      declareShared(b);
                  },
                  [&inlinePipeline, constantsFor, frameConstants, argumentResource, variant, mode, inlineBands](PassContext& c) {
                      c.bindFrameConstants(frameConstants);
                      for (uint32_t band = 0; band < inlineBands; ++band)
                      {
                          uint32_t k[32] = {};
                          constantsFor(c, k, true, band);
                          c.computeConstants(k, 32);
                          inlinePipeline.dispatchIndirect(c.cmd, argumentResource, (uint64_t)band * kArgumentsBytes + kInlineDescOffset + (variant * 2 + mode) * kDescStride);
                      }
                  });
    }
    ID3D12CommandSignature* dispatchSignature = m_dispatchSignature.Get();
    g.addPass("r.refl.shade", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b);
              },
              [&shaders, constantsFor, frameConstants, argumentResource, variant, dispatchSignature, batchCorners = s.batchGiCorners](PassContext& c) {
                  uint32_t k[32] = {};
                  constantsFor(c, k);
                  c.cmd->SetPipelineState(shaders.compute(kShadeKernel[variant][batchCorners]));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->ExecuteIndirect(dispatchSignature, 1, argumentResource, kShadeArgsOffset, nullptr, 0);
              });
    rayArgs("r.refl.shadowargs", 1);
    rt::RayPipeline& shadowPipeline = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(kShadowLibrary, { "ReflectionShadowGen" }));
    g.addPass("r.refl.shadow", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b);
              },
              [&shadowPipeline, constantsFor, frameConstants, argumentResource, slotBands](PassContext& c) {
                  c.bindFrameConstants(frameConstants);
                  for (uint32_t band = 0; band < slotBands; ++band)
                  {
                      uint32_t k[32] = {};
                      constantsFor(c, k, true, band);
                      c.computeConstants(k, 32);
                      shadowPipeline.dispatchIndirect(c.cmd, argumentResource, (uint64_t)band * kArgumentsBytes + kShadowDescOffset);
                  }
              });
    // The penumbra filter of the hits the shade pass queued (ReflectionPenumbra.hlsl; the args from r.refl.shadowargs).
    g.addPass("r.refl.penumbra", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b);
              },
              [&shaders, constantsFor, frameConstants, argumentResource, dispatchSignature](PassContext& c) {
                  uint32_t k[32] = {};
                  constantsFor(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionPenumbra"));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->ExecuteIndirect(dispatchSignature, 1, argumentResource, kPenumbraArgsOffset, nullptr, 0);
              });
    g.addPass("r.refl.combine", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  declareShared(b);
              },
              [&shaders, constantsFor, frameConstants, argumentResource, dispatchSignature, timestamps, firstTick](PassContext& c) {
                  uint32_t k[32] = {};
                  constantsFor(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionCombine"));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->ExecuteIndirect(dispatchSignature, 1, argumentResource, kCombineArgsOffset, nullptr, 0);
                  c.cmd->EndQuery(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstTick + 1);
              });
    }  // (!lumenOnly)
    rays.recordExactReadback(fc);  // after the trace: the counts pick next frames' exact set
    const uint32_t planarCount = planar.views;
    // The pixels' layers (Passes/Reconstruct/LayerCommon.hlsli): the resolve writes them beside the value.
    TextureRef layerStochastic, layerResidual, layerGuide;
    if (layers)
    {
        layerStochastic = g.createTexture({ "R reflection layer stochastic", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        layerResidual = g.createTexture({ "R reflection layer residual", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        layerGuide = g.createTexture({ "R reflection layer guide", width, height, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT });
    }
    g.addPass("r.refl.resolve", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(modes, Use::SrvCompute);
                  b.use(results, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  for (uint32_t k = 0; k < planarCount; ++k) b.use(planarColor[k], Use::SrvCompute);
                  b.use(reflection, Use::UavCompute);
                  b.use(history, Use::UavCompute);
                  if (layers)
                  {
                      b.use(jobLayers, Use::SrvCompute);
                      b.use(layerStochastic, Use::UavCompute);
                      b.use(layerResidual, Use::UavCompute);
                      b.use(layerGuide, Use::UavCompute);
                  }
              },
              [&shaders, modes, results, depth, gbuffer, reflection, history, width, height, tilesX, tilesY, frameConstants, planarSrv, planarOffset, planarCount,
               planarColor, layers, jobLayers, layerStochastic, layerResidual, layerGuide,
               layerFlags = (s.layerMirrorLobe ? 1u : 0u) | (s.layerResidualWhole ? 2u : 0u) | (s.layerWholeValue ? 4u : 0u), focal](PassContext& c) {
                  uint32_t k[24] = { c.srv(modes), c.srv(results), c.srv(depth), c.srv(gbuffer), c.uav(reflection), c.uav(history), height, planarSrv,
                                     width, height, planarOffset, layerFlags, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu,
                                     asU(focal), 0, 0, 0 };
                  for (uint32_t v = 0; v < planarCount; ++v) k[12 + v] = c.srv(planarColor[v]);
                  if (layers) k[16] = c.srv(jobLayers), k[17] = c.uav(layerStochastic), k[18] = c.uav(layerResidual), k[19] = c.uav(layerGuide);
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionResolve"));
                  c.computeConstants(k, 24);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(tilesX, tilesY, 1);
              });
    if (layers)
    {
        // Reconstruction (RENDERER_REDESIGN_V2 1.2): the layers' spatial filter every frame (three a-trous levels), the
        // short history where V's vis id gives the surfaces' motion, and the composition back into view.reflection.
        TextureRef newStochastic = layerStochastic, newResidual = layerResidual;
        if (s.layerFilter)
        {
            TextureRef ping[2][2];
            for (int p = 0; p < 2; ++p)
            {
                ping[p][0] = g.createTexture({ p ? "R reflection layer stochastic filter 1" : "R reflection layer stochastic filter 0", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
                ping[p][1] = g.createTexture({ p ? "R reflection layer residual filter 1" : "R reflection layer residual filter 0", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            }
            // three a-trous levels in the pixels' units, then the dense pass (LayerDenoise.hlsl)
            static const char* const kLevelNames[4] = { "r.refl.layer.filter0", "r.refl.layer.filter1", "r.refl.layer.filter2", "r.refl.layer.filter.dense" };
            for (uint32_t level = 0; level < 4; ++level)
            {
                const TextureRef inS = newStochastic, inR = newResidual, outS = ping[level & 1][0], outR = ping[level & 1][1];
                g.addPass(kLevelNames[level], QueueType::Compute,
                          [&](PassBuilder& b) {
                              b.use(inS, Use::SrvCompute);
                              b.use(inR, Use::SrvCompute);
                              b.use(layerGuide, Use::SrvCompute);
                              b.use(outS, Use::UavCompute);
                              b.use(outR, Use::UavCompute);
                          },
                          [&shaders, inS, inR, layerGuide, outS, outR, width, height, focal, level, frameConstants,
                           flags = (s.layerMirrorLobe ? 1u : 0u) | (s.layerCrossMode ? 2u : 0u)](PassContext& c) {
                              const bool dense = level == 3;
                              const uint32_t k[12] = { c.srv(inS), c.srv(inR), c.srv(layerGuide), dense ? 1u : 1u << level, c.uav(outS), c.uav(outR), width, height, asU(focal),
                                                       flags, dense ? 1u : 0u, 0 };
                              c.cmd->SetPipelineState(shaders.compute("Passes/Reconstruct/LayerDenoise"));
                              c.computeConstants(k, 12);
                              c.bindFrameConstants(frameConstants);
                              c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                          });
                newStochastic = outS;
                newResidual = outR;
            }
        }
        if (s.layerHistoryFrames > 1 && main.visId.valid() && main.visibleClusters.valid())
        {
            // Reset as ReflectionAccumulate's history: new textures, a scene revision, a history discontinuity.
            if (fc.scene.revision() != m_accumSceneRevision || fc.frame.discontinuity != 0) m_accumReset = true;
            m_accumSceneRevision = fc.scene.revision();
            const uint32_t prev = m_accumParity, next = m_accumParity ^ 1u;
            m_accumParity = next;
            auto import = [&](ComPtr<ID3D12Resource>& r, const char* name, DXGI_FORMAT format) {
                return g.importTexture(r.Get(), { name, width, height, 1, 1, format }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            };
            const TextureRef prevS = import(m_layerStochastic[prev], "R reflection layer history stochastic (previous)", DXGI_FORMAT_R16G16B16A16_FLOAT);
            const TextureRef prevR = import(m_layerResidual[prev], "R reflection layer history residual (previous)", DXGI_FORMAT_R16G16B16A16_FLOAT);
            const TextureRef prevKeys = import(m_layerKeys[prev], "R reflection layer history keys (previous)", DXGI_FORMAT_R32G32_UINT);
            const TextureRef nextS = import(m_layerStochastic[next], "R reflection layer history stochastic", DXGI_FORMAT_R16G16B16A16_FLOAT);
            const TextureRef nextR = import(m_layerResidual[next], "R reflection layer history residual", DXGI_FORMAT_R16G16B16A16_FLOAT);
            const TextureRef nextKeys = import(m_layerKeys[next], "R reflection layer history keys", DXGI_FORMAT_R32G32_UINT);
            const TextureRef visId = main.visId;
            const BufferRef visibleClusters = main.visibleClusters;
            const uint32_t flags = (m_accumReset ? 1u : 0u) | (s.layerHistoryBound ? 0u : 2u) | (s.layerMirrorLobe ? 4u : 0u);
            m_accumReset = false;
            const float3 prevCamera = m_prevCamera;
            m_prevCamera = main.view.position;
            const float pixelAngle = 2.0f * std::tan(main.view.verticalFov * 0.5f) / height;
            const TextureRef inS = newStochastic, inR = newResidual;
            g.addPass("r.refl.layer.temporal", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(inS, Use::SrvCompute);
                          b.use(inR, Use::SrvCompute);
                          b.use(layerGuide, Use::SrvCompute);
                          b.use(visId, Use::SrvCompute);
                          b.use(visibleClusters, Use::SrvCompute);
                          b.use(prevS, Use::SrvCompute);
                          b.use(prevR, Use::SrvCompute);
                          b.use(prevKeys, Use::SrvCompute);
                          b.use(nextS, Use::UavCompute);
                          b.use(nextR, Use::UavCompute);
                          b.use(nextKeys, Use::UavCompute);
                          b.use(history, Use::SrvCompute);
                      },
                      [&shaders, inS, inR, layerGuide, visId, visibleClusters, prevS, prevR, prevKeys, nextS, nextR, nextKeys, history, width, height, flags, prevCamera,
                       pixelAngle, frameConstants, s](PassContext& c) {
                          const uint32_t k[24] = { c.srv(inS), c.srv(inR), c.srv(layerGuide), c.srv(visId), c.srv(visibleClusters), c.srv(prevS), c.srv(prevR), c.srv(prevKeys),
                                                   c.uav(nextS), c.uav(nextR), c.uav(nextKeys), s.layerHistoryFrames, width, height, asU(s.temporalLobeShift), flags,
                                                   asU(prevCamera.x), asU(prevCamera.y), asU(prevCamera.z), asU(pixelAngle), c.srv(history), 0, 0, 0 };
                          c.cmd->SetPipelineState(shaders.compute("Passes/Reconstruct/LayerTemporal"));
                          c.computeConstants(k, 24);
                          c.bindFrameConstants(frameConstants);
                          c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                      });
            newStochastic = nextS;
            newResidual = nextR;
        }
        const TextureRef composeS = newStochastic, composeR = newResidual;
        const bool composeOwn = composeS.id != layerStochastic.id;
        g.addPass("r.refl.layer.compose", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(reflection, Use::UavCompute);
                      b.use(layerGuide, Use::SrvCompute);
                      b.use(layerStochastic, Use::SrvCompute);
                      b.use(layerResidual, Use::SrvCompute);
                      if (composeOwn)
                      {
                          b.use(composeS, Use::SrvCompute);
                          b.use(composeR, Use::SrvCompute);
                      }
                  },
                  [&shaders, reflection, layerGuide, layerStochastic, layerResidual, composeS, composeR, width, height, view = s.layerView,
                   mirrorLobe = s.layerMirrorLobe](PassContext& c) {
                      const uint32_t k[12] = { c.uav(reflection), c.srv(layerGuide), c.srv(layerStochastic), c.srv(layerResidual), c.srv(composeS), c.srv(composeR), width, height, view,
                                               mirrorLobe ? 1u : 0u, 0, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reconstruct/LayerCompose"));
                      c.computeConstants(k, 12);
                      c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                  });
    }
    else if (lumen)
    {
        // The ray-reuse pipeline (ReflectionReuse.hlsli): resolve over neighbour rays, time accumulation, bilateral filter.
        // ReflectionResolve above left each traced pixel's own ray value and the planar mirrors in view.reflection; the
        // filter pass replaces the traced pixels with the pipeline's value and their share of the specular light.
        if (fc.scene.revision() != m_accumSceneRevision || fc.frame.discontinuity != 0) m_accumReset = true;
        m_accumSceneRevision = fc.scene.revision();
        const uint32_t prev = m_accumParity, next = m_accumParity ^ 1u;
        m_accumParity = next;
        auto import = [&](ComPtr<ID3D12Resource>& r, const char* name, DXGI_FORMAT format) {
            return g.importTexture(r.Get(), { name, width, height, 1, 1, format }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        };
        const TextureRef prevValue = import(m_layerStochastic[prev], "R reflection reuse history (previous)", DXGI_FORMAT_R16G16B16A16_FLOAT);
        const TextureRef prevFrames = import(m_layerResidual[prev], "R reflection reuse frames (previous)", DXGI_FORMAT_R16G16B16A16_FLOAT);
        const TextureRef prevKeys = import(m_layerKeys[prev], "R reflection reuse keys (previous)", DXGI_FORMAT_R32G32_UINT);
        const TextureRef nextValue = import(m_layerStochastic[next], "R reflection reuse history", DXGI_FORMAT_R16G16B16A16_FLOAT);
        const TextureRef nextFrames = import(m_layerResidual[next], "R reflection reuse frames", DXGI_FORMAT_R16G16B16A16_FLOAT);
        const TextureRef nextKeys = import(m_layerKeys[next], "R reflection reuse keys", DXGI_FORMAT_R32G32_UINT);
        const TextureRef resolved = g.createTexture({ "R reflection reuse resolved", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        const TextureRef visId = main.visId;
        const BufferRef visibleClusters = main.visibleClusters;
        const bool motion = visId.valid() && visibleClusters.valid();
        const uint32_t noHistory = (m_accumReset || !s.lumenTemporal) ? 1u : 0u;
        m_accumReset = false;
        m_prevCamera = main.view.position;
        const uint32_t reuseFrame = (uint32_t)fc.frame.frameIndex;
        // GI's rough specular (gi.lumen's final gather, view.giRoughSpecular): the value of the pixels above the roughness
        // limit and the other side of the fade (ReflectionReuseFilter). Absent: those pixels stay on the K path.
        // (reflection.lumen_only: M mixes the rough specular itself, with its specular occlusion - ShadeOpaque.hlsl - so
        // the filter leaves the untraced pixels and the fade's share to it)
        const TextureRef roughSpecular = s.lumenRoughFromGather && !lumenOnly ? main.giRoughSpecular : TextureRef{};
        g.addPass("r.refl.reuse.resolve", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(modes, Use::SrvCompute);
                      b.use(results, Use::SrvCompute);
                      b.use(depth, Use::SrvCompute);
                      b.use(gbuffer, Use::SrvCompute);
                      b.use(reflection, Use::SrvCompute);
                      b.use(resolved, Use::UavCompute);
                      if (words.valid()) b.use(words, Use::SrvCompute);
                  },
                  [&shaders, modes, results, depth, gbuffer, reflection, resolved, width, height, tilesX, tilesY, frameConstants, reuseFrame, s, samplingBias16, words](PassContext& c) {
                      const uint32_t k[20] = { c.srv(modes), c.srv(results), c.srv(depth), c.srv(gbuffer), c.srv(reflection), c.uav(resolved), height, reuseFrame,
                                               width, height, s.lumenReconstructionSamples, s.lumenReconstruction ? 0u : 1u,
                                               asU(s.lumenReconstructionRadius), asU(s.lumenMaxRayIntensity), asU(s.lumenTonemapRange),
                                               asU(samplingBias16 / 65535.0f), words.valid() ? c.srv(words) : 0xFFFFFFFFu, 0, 0, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionReuseResolve"));
                      c.computeConstants(k, 20);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(tilesX, tilesY, 1);
                  });
        g.addPass("r.refl.reuse.temporal", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(resolved, Use::SrvCompute);
                      b.use(depth, Use::SrvCompute);
                      b.use(gbuffer, Use::SrvCompute);
                      if (motion)
                      {
                          b.use(visId, Use::SrvCompute);
                          b.use(visibleClusters, Use::SrvCompute);
                      }
                      b.use(prevValue, Use::SrvCompute);
                      b.use(prevFrames, Use::SrvCompute);
                      b.use(prevKeys, Use::SrvCompute);
                      b.use(nextValue, Use::UavCompute);
                      b.use(nextFrames, Use::UavCompute);
                      b.use(nextKeys, Use::UavCompute);
                      if (words.valid()) b.use(words, Use::SrvCompute);
                  },
                  [&shaders, resolved, depth, gbuffer, visId, visibleClusters, motion, prevValue, prevFrames, prevKeys, nextValue, nextFrames, nextKeys, width, height,
                   noHistory, frameConstants, reuseFrame, s, words](PassContext& c) {
                      const uint32_t k[20] = { c.srv(resolved), c.srv(depth), c.srv(gbuffer), motion ? c.srv(visId) : 0xFFFFFFFFu,
                                               motion ? c.srv(visibleClusters) : 0xFFFFFFFFu, c.srv(prevValue), c.srv(prevFrames), c.srv(prevKeys),
                                               c.uav(nextValue), c.uav(nextFrames), c.uav(nextKeys), noHistory,
                                               width, height, asU(s.lumenTemporalMaxFrames), asU(s.lumenClampScale),
                                               asU(s.lumenTonemapRange), asU(s.lumenDistanceThreshold), reuseFrame, words.valid() ? c.srv(words) : 0xFFFFFFFFu };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionReuseTemporal"));
                      c.computeConstants(k, 20);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                  });
        g.addPass("r.refl.reuse.filter", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(nextValue, Use::SrvCompute);
                      b.use(nextFrames, Use::SrvCompute);
                      b.use(depth, Use::SrvCompute);
                      b.use(gbuffer, Use::SrvCompute);
                      b.use(reflection, Use::UavCompute);
                      b.use(modes, Use::SrvCompute);
                      if (roughSpecular.valid()) b.use(roughSpecular, Use::SrvCompute);
                      if (words.valid()) b.use(words, Use::SrvCompute);
                  },
                  [&shaders, nextValue, nextFrames, depth, gbuffer, reflection, modes, width, height, tilesX, tilesY, frameConstants, reuseFrame, s, roughSpecular, words](PassContext& c) {
                      const uint32_t k[24] = { c.srv(nextValue), c.srv(nextFrames), c.srv(depth), c.srv(gbuffer), c.uav(reflection), c.srv(modes), height, reuseFrame,
                                               width, height, s.lumenBilateralSamples, (s.lumenBilateral ? 0u : 1u) | (s.lumenDisocclusionTonemap ? 0u : 2u),
                                               asU(s.lumenBilateralRadius), asU(s.lumenBilateralDepthWeight), asU(s.lumenDisocclusionFrames), asU(s.lumenTemporalMaxFrames),
                                               asU(s.lumenMaxRoughness), asU(s.lumenFadeLength), asU(s.lumenTonemapRange),
                                               roughSpecular.valid() ? c.srv(roughSpecular) : 0xFFFFFFFFu, words.valid() ? c.srv(words) : 0xFFFFFFFFu, 0, 0, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionReuseFilter"));
                      c.computeConstants(k, 24);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(tilesX, tilesY, 1);
                  });
    }
    else if (main.visId.valid() && main.visibleClusters.valid())
    {
        // Time integration (ReflectionAccumulate.hlsl): reset on new textures, a scene revision (upload, materials) or a
        // history discontinuity (restore, camera cut); ping-pong by parity. Needs V's vis id (surface identity and exact
        // motion); frames without it (stand-in visibility in tests) keep each frame's own estimate.
        if (fc.scene.revision() != m_accumSceneRevision || fc.frame.discontinuity != 0) m_accumReset = true;
        m_accumSceneRevision = fc.scene.revision();
        const uint32_t prev = m_accumParity, next = m_accumParity ^ 1u;
        m_accumParity = next;
        const TextureRef accumPrev = g.importTexture(m_accum[prev].Get(), { "R reflection accumulation (previous)", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                                     D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const TextureRef keysPrev = g.importTexture(m_accumKeys[prev].Get(), { "R reflection accumulation keys (previous)", width, height, 1, 1, DXGI_FORMAT_R32G32_UINT },
                                                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const TextureRef accumNext = g.importTexture(m_accum[next].Get(), { "R reflection accumulation", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT },
                                                     D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const TextureRef keysNext = g.importTexture(m_accumKeys[next].Get(), { "R reflection accumulation keys", width, height, 1, 1, DXGI_FORMAT_R32G32_UINT },
                                                    D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const TextureRef visId = main.visId;
        const BufferRef visibleClusters = main.visibleClusters;
        const uint32_t flags = (m_accumReset ? 1u : 0u) | ((s.experimentDisable & 512) ? 2u : 0u) | ((s.experimentDisable & 1024) ? 4u : 0u);
        m_accumReset = false;
        const float3 prevCamera = m_prevCamera;
        m_prevCamera = main.view.position;
        const float pixelAngle = 2.0f * std::tan(main.view.verticalFov * 0.5f) / height;
        g.addPass("r.refl.accumulate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(reflection, Use::UavCompute);
                      b.use(modes, Use::SrvCompute);
                      b.use(depth, Use::SrvCompute);
                      b.use(gbuffer, Use::SrvCompute);
                      b.use(visId, Use::SrvCompute);
                      b.use(visibleClusters, Use::SrvCompute);
                      b.use(history, Use::UavCompute);
                      b.use(accumPrev, Use::UavCompute);
                      b.use(keysPrev, Use::UavCompute);
                      b.use(accumNext, Use::UavCompute);
                      b.use(keysNext, Use::UavCompute);
                  },
                  [&shaders, reflection, modes, depth, gbuffer, visId, visibleClusters, history, accumPrev, keysPrev, accumNext, keysNext, width, height, flags,
                   prevCamera, pixelAngle, frameConstants, s](PassContext& c) {
                      const float shift = s.temporalLobeShift;
                      uint32_t shiftBits, px, py, pz, angleBits;
                      std::memcpy(&shiftBits, &shift, 4);
                      std::memcpy(&px, &prevCamera.x, 4);
                      std::memcpy(&py, &prevCamera.y, 4);
                      std::memcpy(&pz, &prevCamera.z, 4);
                      std::memcpy(&angleBits, &pixelAngle, 4);
                      const uint32_t k[20] = { c.uav(reflection), c.srv(modes), c.srv(depth), c.srv(gbuffer), c.srv(visId), c.srv(visibleClusters), c.uav(history),
                                               c.uav(accumPrev), c.uav(keysPrev), c.uav(accumNext), c.uav(keysNext), s.temporalHistoryMax, width, height, shiftBits,
                                               flags, px, py, pz, angleBits };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionAccumulate"));
                      c.computeConstants(k, 20);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                  });
    }
    // One copy into this frame's read-back slot (read framesInFlight later): candidate pixel counts, timestamps (trace,
    // views) and job counters. A single copy-destination use per frame: the read-back heap buffer takes no barrier.
    const uint32_t tickCount = 2 + 2 * planarCount;
    g.addPass("r.refl.readback", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(planarCounts, Use::CopySrc);
                  b.use(args, Use::CopySrc);
                  b.use(planarReadback, Use::CopyDst);
                  b.keep();
              },
              [timestamps, firstTick, tickCount, args, planarCounts, planarReadback, readbackOffset](PassContext& c) {
                  c.cmd->CopyBufferRegion(c.resource(planarReadback), readbackOffset, c.resource(planarCounts), 0, kCandidatesMax * 4);
                  c.cmd->ResolveQueryData(timestamps, D3D12_QUERY_TYPE_TIMESTAMP, firstTick, tickCount, c.resource(planarReadback), readbackOffset + kTicksOffset);
                  c.cmd->CopyBufferRegion(c.resource(planarReadback), readbackOffset + kJobsOffset, c.resource(args), 0, 16);
              });
    // Counters for runs without the gate (Player, host gates): the GI header's reflection statistics (GI_H_STAT_HIT_*,
    // GI_H_STAT_G_*) of the frame framesInFlight ago, every reflection.stats_log_frames frames.
    if (s.statsLogFrames > 0 && !lumenOnly)  // (the counters live in the world GI cache's header)
    {
        if (!m_statsReadback)
        {
            D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC1 d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = kPlanarSlots * 256;
            d.Height = d.DepthOrArraySize = d.MipLevels = 1;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(m_device.d3d()->CreateCommittedResource3(&rb, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_statsReadback)),
                  "reflection stats readback");
            D3D12_RANGE all{ 0, (SIZE_T)d.Width };
            check(m_statsReadback->Map(0, &all, reinterpret_cast<void**>(const_cast<uint32_t**>(&m_statsMapped))), "map reflection stats");
        }
        const uint64_t statsFrame = fc.frame.frameIndex;
        if (statsFrame >= fc.framesInFlight && (statsFrame - fc.framesInFlight) % s.statsLogFrames == 0)
        {
            const uint32_t* h = m_statsMapped + ((statsFrame - fc.framesInFlight) % kPlanarSlots) * 64;
            logf("R stats frame %llu: reflection hit cache lookups %u, no data %u; G samples %u, ratio branch %u; mean L / mean g log2 bins [<-3 .. >=4] %u %u %u %u %u %u %u %u %u, mean g = 0 %u\n",
                 (unsigned long long)(statsFrame - fc.framesInFlight), h[38], h[39], h[48], h[49], h[50], h[51], h[52], h[53], h[54], h[55], h[56], h[57], h[58], h[59]);
        }
        const BufferRef statsRef = g.importBuffer(m_statsReadback.Get(), { "R reflection stats readback", kPlanarSlots * 256, 0 });
        const uint64_t statsOffset = (statsFrame % kPlanarSlots) * 256;
        g.addPass("r.refl.stats", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(cache, Use::CopySrc);
                      b.use(statsRef, Use::CopyDst);
                      b.keep();
                  },
                  [cache, statsRef, statsOffset](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(statsRef), statsOffset, c.resource(cache), 0, 256); });
    }
}
} // namespace unx::render::refl

namespace unx::render::refl
{
ReflectionSystem::Stats ReflectionSystem::readStats()
{
    m_device.waitIdle();
    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 16;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(m_device.d3d()->CreateCommittedResource3(&rb, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)),
          "reflection stats readback");
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(readback.Get(), 0, m_arguments.Get(), 0, 16);
    m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(cl));
    uint32_t v[4];
    void* mapped = nullptr;
    D3D12_RANGE all{ 0, 16 };
    check(readback->Map(0, &all, &mapped), "map reflection stats");
    std::memcpy(v, mapped, 16);
    D3D12_RANGE none{ 0, 0 };
    readback->Unmap(0, &none);
    const uint32_t largest = m_planePixels.empty() ? 0 : *std::max_element(m_planePixels.begin(), m_planePixels.end());
    return { v[0], v[1], v[2], v[3], m_lastPlanarViews, m_lastPlanarPixels, m_lastCandidates, largest, m_lastSelectMs, m_lastRectPixels, m_rayNs, m_viewNsPerPixel, m_viewFixedNs * 1e-6f,
             m_lastViewMs };
}
} // namespace unx::render::refl
