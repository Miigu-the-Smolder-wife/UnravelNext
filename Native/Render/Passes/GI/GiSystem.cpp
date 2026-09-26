#include "unx/gi/GiSystem.h"

#include "unx/rt/RayPipeline.h"

#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>
#include <memory>
#include <vector>

namespace unx::render::gi
{
namespace
{
uint32_t nextPow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

uint32_t asU(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// Cache buffer layout (GiCache.hlsli header fields 4..11, 24..27).
struct Layout
{
    uint32_t table, freeList, meta, anchor, sh, texels, update, selected, hitStamp, hitList, shTable, mapOwner, anchorMin, irr, slotAnchor, end;
};

Layout layoutOf(const GiSettings& s)
{
    Layout l{};
    l.table = 1024;  // header 256 B + age histograms 512 B (2 tiers x 64) + spare
    l.freeList = l.table + s.tableSlots * 16;
    l.meta = l.freeList + s.capacity * 4;
    l.anchor = l.meta + s.capacity * 16;
    l.sh = l.anchor + s.capacity * 16;
    l.texels = l.sh + s.capacity * 80;
    l.update = l.texels + s.capacity * 512;
    l.selected = l.update + s.capacity * 4;
    l.hitStamp = l.selected + s.capacity * 4;
    l.hitList = l.hitStamp + s.capacity * 4;
    l.shTable = l.hitList + 2 * s.capacity * 4;
    l.mapOwner = l.shTable + 64 * 36;
    l.anchorMin = l.mapOwner + s.capacity * 4;
    l.irr = l.anchorMin + s.capacity * 8;
    l.slotAnchor = l.irr + s.capacity * 336;  // GI_IRR_STRIDE
    l.end = l.slotAnchor + s.tableSlots * 8;  // deterministic anchors per table slot (GiDetFold)
    return l;
}

// Exact per-texel integrals of the real L2 SH basis over the 8 x 8 hemispherical octahedral texels (entry-local frame):
// integral over uv of Y(v/|v|) * 2/|v|^3, v = (p, 1 - |p.x| - |p.y|), p = rotate45(2uv - 1). 64 x 64 subsamples/texel.
std::vector<float> texelShIntegrals()
{
    std::vector<float> table(64 * 9, 0.0f);
    const int n = 64;
    for (int ty = 0; ty < 8; ++ty)
        for (int tx = 0; tx < 8; ++tx)
        {
            double acc[9] = {};
            for (int sy = 0; sy < n; ++sy)
                for (int sx = 0; sx < n; ++sx)
                {
                    const double u = (tx + (sx + 0.5) / n) / 8.0, v = (ty + (sy + 0.5) / n) / 8.0;
                    const double qx = u * 2 - 1, qy = v * 2 - 1;
                    const double px = (qx + qy) * 0.5, py = (qx - qy) * 0.5;
                    const double vz = 1 - std::fabs(px) - std::fabs(py);
                    const double len = std::sqrt(px * px + py * py + vz * vz);
                    const double x = px / len, y = py / len, z = vz / len;
                    const double dw = 2.0 / (len * len * len) / (64.0 * n * n);
                    const double Y[9] = { 0.282095, 0.488603 * y, 0.488603 * z, 0.488603 * x, 1.092548 * x * y, 1.092548 * y * z, 0.315392 * (3 * z * z - 1), 1.092548 * x * z,
                                          0.546274 * (x * x - y * y) };
                    for (int k = 0; k < 9; ++k) acc[k] += Y[k] * dw;
                }
            for (int k = 0; k < 9; ++k) table[(ty * 8 + tx) * 9 + k] = (float)acc[k];
        }
    return table;
}
} // namespace

GiSettings GiSettings::fromQuality(const QualityConfig& q)
{
    GiSettings s;
    s.raysPerFrame = (uint32_t)q.integer("gi.rays_per_frame");
    s.capacity = (uint32_t)q.integer("gi.cache_entries");
    s.tableSlots = nextPow2(2 * s.capacity);  // load <= 50 %
    s.probeSpacing = (uint32_t)q.integer("gi.screen_probe_spacing_px");
    s.maxAge = (uint32_t)q.integer("gi.cache_max_age_frames");
    s.jacobiUpdates = (uint32_t)q.integer("gi.jacobi_updates");
    s.historyMax = (uint32_t)q.integer("gi.history_updates_max");
    s.maxLevel = (uint32_t)q.integer("gi.cache_levels_max");
    s.cellAngleDeg = (float)q.number("gi.cache_cell_angle_deg");
    s.cellMin = (float)q.number("gi.cache_cell_min_m");
    s.nearRadius = (float)q.number("gi.near_occlusion_radius_m");
    s.rayLength = (float)q.number("gi.ray_length_m");
    s.hitUpdateShare = (float)q.number("gi.hit_update_share");
    s.hitCellFootprintScale = (float)q.number("gi.hit_cell_footprint_scale");
    s.experimentDisable = (uint32_t)q.integer("gi.experiment_disable");
    s.deterministic = q.boolean("gi.deterministic");
    // Fixed by the kernels (GiCache.hlsli, GiProbeGather.hlsl, GiInternal.hlsli probe offsets).
    if (q.integer("gi.cache_octahedral_texels") != 8) fail("gi.cache_octahedral_texels must be 8 (GI_TEXELS)");
    if (q.integer("gi.near_occlusion_taps") != 16) fail("gi.near_occlusion_taps must be 16 (GiProbeGather)");
    if (s.tableSlots < s.capacity) fail("gi.cache table slots (%u) must be >= capacity (%u): GiTableClear resets the map owners", s.tableSlots, s.capacity);
    if (s.probeSpacing != 8) fail("gi.screen_probe_spacing_px must be 8 (3-bit probe offsets, 4 candidate points)");
    if (s.capacity == 0 || s.raysPerFrame < 64 || s.historyMax == 0) fail("gi: zero capacity, rays or history");
    if (s.maxLevel > 31) fail("gi.cache_levels_max must be <= 31 (5-bit key field)");
    s.updatesPerFrame = s.raysPerFrame / 64;  // whole-hemisphere updates
    return s;
}

namespace
{
struct GiSystemSlot
{
    std::unique_ptr<GiSystem> gi;
};
} // namespace

GiSystem& GiSystem::get(FramePassContext& fc)
{
    GiSystemSlot& slot = fc.state<GiSystemSlot>("R.gi");
    if (!slot.gi) slot.gi = std::make_unique<GiSystem>(fc.device, fc.quality);
    return *slot.gi;
}

GiSystem* GiSystem::find(TrackState& state) { return state.get<GiSystemSlot>("R.gi").gi.get(); }

GiSystem::GiSystem(Device& device, const QualityConfig& quality) : m_device(device), m_settings(GiSettings::fromQuality(quality))
{
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 o11{};
    check(device.d3d()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &o11, sizeof o11), "OPTIONS11");
    if (!o11.AtomicInt64OnDescriptorHeapResourceSupported) fail("GI cache: 64-bit atomics on descriptor-heap resources are required (hash keys)");
    const Layout l = layoutOf(m_settings);
    m_bytes = l.end;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, upload{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = m_bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_cache)), "GI cache");
    m_cache->SetName(L"GI world radiance cache");

    // Initial image: everything up to the anchors (header, histogram, table, free list = every entry, free metas), and the
    // hit stamps, hit lists and SH integral table at the end. Anchors, SH blocks and texels are written on creation.
    const std::vector<float> shTable = texelShIntegrals();
    std::vector<uint32_t> head(l.anchor / 4, 0), tail((l.end - l.hitStamp) / 4, 0);
    uint32_t* h = head.data();
    h[0] = m_settings.capacity;
    h[1] = m_settings.tableSlots;
    h[2] = asU(m_settings.cellMin);
    h[3] = asU(std::tan(m_settings.cellAngleDeg * 3.14159265358979f / 180.0f));
    h[4] = l.table;
    h[5] = l.freeList;
    h[6] = l.meta;
    h[7] = l.anchor;
    h[8] = l.sh;
    h[9] = l.texels;
    h[10] = l.update;
    h[11] = m_settings.maxAge;
    h[12] = m_settings.capacity;  // free count
    h[23] = m_settings.maxLevel;
    h[24] = l.selected;
    h[25] = l.hitStamp;
    h[26] = l.hitList;
    h[27] = l.shTable;
    h[30] = m_settings.jacobiUpdates;
    h[31] = m_settings.historyMax;
    h[37] = l.mapOwner;  // GI_H_MAP_OWNER
    h[60] = l.anchorMin; // deterministic anchors (GiHeader.offAnchorMin)
    h[62] = l.irr;       // irradiance maps (GiHeader.offIrr)
    h[63] = l.slotAnchor;  // deterministic anchors per table slot (GiHeader.offSlotAnchor)
    for (uint32_t e = 0; e < m_settings.capacity; ++e) head[l.freeList / 4 + e] = m_settings.capacity - 1 - e;  // pops 0, 1, 2 ...
    for (uint32_t t = 0; t < m_settings.tableSlots; ++t) head[l.table / 4 + t * 4 + 2] = 0xFFFFFFFFu;
    std::memcpy(&tail[(l.shTable - l.hitStamp) / 4], shTable.data(), shTable.size() * 4);
    auto uploadRange = [&](uint64_t offset, const std::vector<uint32_t>& data) {
        const uint64_t bytes = (uint64_t)data.size() * 4;
        D3D12_RESOURCE_DESC1 sd = d;
        sd.Width = bytes;
        sd.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> staging;
        check(device.d3d()->CreateCommittedResource3(&upload, D3D12_HEAP_FLAG_NONE, &sd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
              "GI staging");
        void* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(staging->Map(0, &none, &mapped), "map GI staging");
        std::memcpy(mapped, data.data(), bytes);
        staging->Unmap(0, nullptr);
        CommandList cl = device.acquireCommandList(QueueType::Graphics);
        cl.list->CopyBufferRegion(m_cache.Get(), offset, staging.Get(), 0, bytes);
        device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    };
    uploadRange(0, head);
    uploadRange(l.hitStamp, tail);
    logf("GI cache: %u entries, %u table slots, %.1f MB (texels %.1f MB), finest cell %.3f m, cell %.2f deg, %u rays = %u hemisphere updates per frame\n",
         m_settings.capacity, m_settings.tableSlots, m_bytes / 1048576.0, m_settings.capacity * 512.0 / 1048576.0, m_settings.cellMin, m_settings.cellAngleDeg,
         m_settings.raysPerFrame, m_settings.updatesPerFrame);
}

GiSystem::~GiSystem()
{
    m_device.deferRelease(m_cache);
    m_device.deferRelease(m_dispatchSignature);
}

void GiSystem::record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays)
{
    RenderGraph& g = fc.graph;
    const GiSettings& s = m_settings;
    // Lighting epoch: a new scene upload (geometry, materials, lights), new sky constants or a restore discontinuity
    // (snapshot restore, save load: every temporal state resets, world-space caches included; Frame.h) reset every
    // entry's history. A camera cut keeps the cache.
    if (fc.scene.revision() != m_sceneRevision || (fc.frame.discontinuity & kDiscontinuityRestore) != 0)
    {
        m_sceneRevision = fc.scene.revision();
        ++m_epoch;
    }
    const BufferRef cache = g.importBuffer(m_cache.Get(), { "GI cache", m_bytes, 0 });
    fc.resources.giCache = cache;
    // Probes at the tile corners (design revision 12.3): one more column and row than tiles.
    const uint32_t probesX = (main.view.width + s.probeSpacing - 1) / s.probeSpacing + 1;
    const uint32_t probesY = (main.view.height + s.probeSpacing - 1) / s.probeSpacing + 1;
    main.screenProbes = g.createTexture({ "GI screen probes", probesX * 8, probesY * 5 + 1, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT });
    {
        // K-path maps atlas (INTERFACES v1.13, layout in ScreenProbes.hlsli): written as R32_UINT, filtered as RGB9E5.
        TextureDesc maps{ "GI screen probe maps", probesX * 14, probesY * 8, 1, 1, DXGI_FORMAT_R32_UINT };
        maps.srvFormat = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
        main.screenProbeMaps = g.createTexture(maps);
    }
    const TextureRef atlas = main.screenProbeMaps;
    const TextureRef probes = main.screenProbes, depth = main.depth, gbuffer = main.gbuffer;
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = main.frameConstants;
    const uint32_t frame = (uint32_t)fc.frame.frameIndex + 1;  // 0 never matches a stamp
    const float3 camera = fc.frame.mainView.position;
    ShaderLibrary& shaders = fc.shaders;
    auto groups = [](uint32_t n) { return (n + 63) / 64; };

    auto compute = [&](const char* name, const char* kernel, uint32_t dispatch, std::vector<uint32_t> extra) {
        g.addPass(name, QueueType::Compute, [&](PassBuilder& b) { b.use(cache, Use::UavCompute); },
                  [&shaders, cache, kernel, dispatch, extra, frameConstants](PassContext& c) {
                      uint32_t k[8] = { c.uav(cache), 0, 0, 0, 0, 0, 0, 0 };
                      for (size_t i = 0; i < extra.size(); ++i) k[1 + i] = extra[i];
                      c.cmd->SetPipelineState(shaders.compute(kernel));
                      c.computeConstants(k, 8);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(dispatch, 1, 1);
                  });
    };
    uint32_t cam[3];
    std::memcpy(cam, &camera, 12);
    compute("r.gi.begin", "Passes/GI/GiBegin", 1, { frame, m_epoch, s.deterministic ? 1u : 0u, cam[0], cam[1], cam[2] });
    // Deterministic anchors: last frame's per-slot candidates (its ray passes) into the entries before the table clears.
    if (s.deterministic) compute("r.gi.det.fold", "Passes/GI/GiDetFold", groups(s.tableSlots), {});
    compute("r.gi.evict", "Passes/GI/GiEvict", groups(s.capacity), {});
    compute("r.gi.clear", "Passes/GI/GiTableClear", groups(s.tableSlots), {});
    compute("r.gi.rehash", "Passes/GI/GiRehash", groups(s.capacity), {});
    g.addPass("r.gi.place", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::UavCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
              },
              [&shaders, cache, depth, gbuffer, probesX, probesY, frameConstants, s, main](PassContext& c) {
                  const uint32_t k[8] = { c.uav(cache), c.srv(depth), c.srv(gbuffer), s.probeSpacing, probesX, probesY, main.view.width, main.view.height };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiProbePlace"));
                  c.computeConstants(k, 8);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch((probesX + 7) / 8, (probesY + 7) / 8, 1);
              });
    if (s.deterministic)
    {
        compute("r.gi.det.foldplace", "Passes/GI/GiDetFold", groups(s.tableSlots), {});  // the probe placement's candidates
        compute("r.gi.det.anchors", "Passes/GI/GiDetAnchors", groups(s.capacity), {});  // before any ray leaves an anchor
    }
    compute("r.gi.carry", "Passes/GI/GiCarry", groups(s.capacity), {});
    compute("r.gi.age", "Passes/GI/GiAgeHistogram", (s.capacity + 127) / 128, {});
    compute("r.gi.setup", "Passes/GI/GiUpdateSetup", 1, { s.updatesPerFrame, asU(s.hitUpdateShare) });
    // Selection: in deterministic mode the threshold bucket's quota goes by key priority (4-level radix select), not by
    // the order of atomic fills; the state is a small transient buffer.
    std::optional<BufferRef> detState;
    if (s.deterministic)
    {
        const BufferRef state = g.createBuffer({ "GI deterministic selection", 3ull * 1040, 0 });
        detState = state;
        for (uint32_t level = 0; level < 4; ++level)
        {
            if (level == 0)
                g.addPass("r.gi.det.clear", QueueType::Compute, [&](PassBuilder& b) { b.use(state, Use::UavCompute); },
                          [&shaders, state](PassContext& c) {
                              const uint32_t k[4] = { c.uav(state), 3 * 1040 / 4, 0, 0 };
                              c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiDetClear"));
                              c.computeConstants(k, 4);
                              c.cmd->Dispatch((3 * 1040 / 4 + 63) / 64, 1, 1);
                          });
            for (const char* kernel : { "Passes/GI/GiDetDigits", "Passes/GI/GiDetResolve" })
            {
                const bool digits = std::string_view(kernel).ends_with("Digits");
                g.addPass(digits ? "r.gi.det.digits" : "r.gi.det.resolve", QueueType::Compute,
                          [&](PassBuilder& b) {
                              b.use(cache, Use::UavCompute);
                              b.use(state, Use::UavCompute);
                          },
                          [&shaders, cache, state, kernel, digits, level, dispatch = groups(s.capacity), frameConstants](PassContext& c) {
                              const uint32_t k[4] = { c.uav(cache), c.uav(state), level, 0 };  // mode 0: tiers 0, 1
                              c.cmd->SetPipelineState(shaders.compute(kernel));
                              c.computeConstants(k, 4);
                              c.bindFrameConstants(frameConstants);
                              c.cmd->Dispatch(digits ? dispatch : 2, 1, 1);
                          });
            }
        }
    }
    g.addPass("r.gi.select", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::UavCompute);
                  if (detState) b.use(*detState, Use::UavCompute);
              },
              [&shaders, cache, detState, dispatch = groups(s.capacity), frameConstants](PassContext& c) {
                  const uint32_t k[4] = { c.uav(cache), detState ? c.uav(*detState) : 0xFFFFFFFFu, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiSelect"));
                  c.computeConstants(k, 4);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(dispatch, 1, 1);
              });
    if (detState)
    {
        // Background updates by key priority among the live entries not selected (tier 2), then appended.
        const BufferRef state = *detState;
        for (uint32_t level = 0; level < 4; ++level)
            for (const bool digits : { true, false })
                g.addPass(digits ? "r.gi.det.bg.digits" : "r.gi.det.bg.resolve", QueueType::Compute,
                          [&](PassBuilder& b) {
                              b.use(cache, Use::UavCompute);
                              b.use(state, Use::UavCompute);
                          },
                          [&shaders, cache, state, digits, level, dispatch = groups(s.capacity)](PassContext& c) {
                              const uint32_t k[4] = { c.uav(cache), c.uav(state), level, 1 };  // mode 1: tier 2
                              c.cmd->SetPipelineState(shaders.compute(digits ? "Passes/GI/GiDetDigits" : "Passes/GI/GiDetResolve"));
                              c.computeConstants(k, 4);
                              c.cmd->Dispatch(digits ? dispatch : 1, 1, 1);
                          });
        g.addPass("r.gi.det.bg", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(cache, Use::UavCompute);
                      b.use(state, Use::UavCompute);
                  },
                  [&shaders, cache, state, dispatch = groups(s.capacity)](PassContext& c) {
                      const uint32_t k[4] = { c.uav(cache), c.uav(state), 0, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiDetBackground"));
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(dispatch, 1, 1);
                  });
        g.addPass("r.gi.det.bg.done", QueueType::Compute, [&](PassBuilder& b) { b.use(cache, Use::UavCompute); },
                  [&shaders, cache, state](PassContext& c) {
                      const uint32_t k[4] = { c.uav(cache), 0, 1, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiDetBackground"));
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    }

    uint32_t scene[8];
    rays.rootConstants(scene);
    // Escaping rays see S's sky and sun when the atmosphere LUTs exist this frame (variant SKY0); otherwise the constant
    // sky of setConstantSky (SKY1: tests, and builds without the S track).
    const FrameResources& fr = fc.resources;
    const bool atmosphere = fr.transmittanceLut.valid() && fr.multiScatterLut.valid() && fr.skyViewLut.valid() && fr.aerialPerspective.valid();
    const TextureRef luts[4] = { fr.transmittanceLut, fr.multiScatterLut, fr.skyViewLut, fr.aerialPerspective };
    rt::RayPipeline& pipeline =
        rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(atmosphere ? "Passes/GI/GiTrace.SKY0" : "Passes/GI/GiTrace.SKY1", { "GiTraceGen" }));
    const float3 sky = m_skyRadiance, sun = m_sunIlluminance;
    const float skyBand = m_skyBand;
    const uint32_t rayCount = s.updatesPerFrame * 64;
    // Each ray's radiance and hemispherical octahedral coordinates, for the per-ray irradiance map and SH (GiIntegrate).
    const BufferRef samples = g.createBuffer({ "GI ray samples", (uint64_t)rayCount * 2 * 16, 16 });  // texel samples, then emitter samples
    g.addPass("r.gi.trace", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::UavGraphics);
                  b.use(samples, Use::UavGraphics);
                  rays.declareTraversal(b);
                  if (atmosphere)
                      for (const TextureRef& t : luts) b.use(t, Use::SrvGraphics);
              },
              [&pipeline, cache, samples, rayCount, s, sky, sun, skyBand, scene, frameConstants, atmosphere, luts](PassContext& c) {
                  uint32_t k[32] = {};
                  k[0] = c.uav(cache);
                  k[1] = rayCount;
                  k[2] = asU(s.hitCellFootprintScale);
                  k[3] = s.deterministic ? 1u : 0u;  // bit 0: ray seeds from the entry's key
                  k[4] = asU(sky.x);
                  k[5] = asU(sky.y);
                  k[6] = asU(sky.z);
                  k[7] = asU(s.rayLength);
                  for (int i = 0; i < 4; ++i) k[8 + i] = atmosphere ? c.srv(luts[i]) : 0xFFFFFFFFu;
                  k[12] = asU(sun.x);
                  k[13] = asU(sun.y);
                  k[14] = asU(sun.z);
                  k[15] = s.experimentDisable;
                  k[16] = asU(skyBand);
                  k[17] = c.uav(samples);
                  std::memcpy(&k[24], scene, sizeof scene);
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  pipeline.dispatch(c.cmd, 0, rayCount, 1, 1);
              });
    g.addPass("r.gi.integrate", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::UavCompute);
                  b.use(samples, Use::SrvCompute);
              },
              [&shaders, cache, samples, updates = s.updatesPerFrame, frameConstants](PassContext& c) {
                  const uint32_t k[4] = { c.uav(cache), updates, c.srv(samples), 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiIntegrate"));
                  c.computeConstants(k, 4);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(updates, 1, 1);  // one group per update slot
              });

    // Map owner list (count, then probe indices) and its indirect dispatch arguments, reset by the gather.
    const BufferRef owners = g.createBuffer({ "GI map owners", 4ull + 4ull * probesX * probesY, 0 });
    const BufferRef mapArgs = g.createBuffer({ "GI map dispatch args", 16, 0 });
    g.addPass("r.gi.gather", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::UavCompute);  // radiance map owners
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(probes, Use::UavCompute);
                  b.use(owners, Use::UavCompute);
                  b.use(mapArgs, Use::UavCompute);
              },
              [&shaders, cache, depth, gbuffer, probes, owners, mapArgs, probesX, probesY, frameConstants, s, main](PassContext& c) {
                  const uint32_t k[12] = { c.uav(cache), c.srv(depth), c.srv(gbuffer), c.uav(probes), probesX, probesY, main.view.width, main.view.height,
                                           s.probeSpacing, asU(s.nearRadius), c.uav(owners), c.uav(mapArgs) };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiProbeGather"));
                  c.computeConstants(k, 12);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch((probesX + 7) / 8, (probesY + 7) / 8, 1);
              });
    g.addPass("r.gi.mapowners", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::SrvCompute);
                  b.use(probes, Use::UavCompute);
                  b.use(owners, Use::UavCompute);
                  b.use(mapArgs, Use::UavCompute);
              },
              [&shaders, cache, probes, owners, mapArgs, probesX, probesY](PassContext& c) {
                  const uint32_t k[8] = { c.srv(cache), c.uav(probes), probesX, probesY, c.uav(owners), c.uav(mapArgs), 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiProbeMapOwners"));
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((probesX + 7) / 8, (probesY + 7) / 8, 1);
              });
    ID3D12CommandSignature* signature = dispatchSignature();
    g.addPass("r.gi.maps", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::SrvCompute);
                  b.use(probes, Use::UavCompute);
                  b.use(owners, Use::SrvCompute);
                  b.use(mapArgs, Use::IndirectArgs);
                  b.use(atlas, Use::UavCompute);
              },
              [&shaders, cache, probes, owners, mapArgs, probesX, probesY, signature, atlas](PassContext& c) {
                  const uint32_t k[8] = { c.srv(cache), c.uav(probes), probesX, probesY, c.srv(owners), c.uav(atlas), 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiProbeMaps"));
                  c.computeConstants(k, 8);
                  c.cmd->ExecuteIndirect(signature, 1, c.resource(mapArgs), 0, nullptr, 0);
              });
}

ID3D12CommandSignature* GiSystem::dispatchSignature()
{
    if (!m_dispatchSignature)
    {
        D3D12_INDIRECT_ARGUMENT_DESC a{};
        a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC d{};
        d.ByteStride = 16;
        d.NumArgumentDescs = 1;
        d.pArgumentDescs = &a;
        check(m_device.d3d()->CreateCommandSignature(&d, nullptr, IID_PPV_ARGS(&m_dispatchSignature)), "GI dispatch signature");
    }
    return m_dispatchSignature.Get();
}

GiStats GiSystem::readStats()
{
    m_device.waitIdle();
    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 256;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(m_device.d3d()->CreateCommittedResource3(&rb, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)), "GI stats readback");
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(readback.Get(), 0, m_cache.Get(), 0, 256);
    m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(cl));
    uint32_t h[64];
    void* mapped = nullptr;
    D3D12_RANGE all{ 0, 256 };
    check(readback->Map(0, &all, &mapped), "map GI stats");
    std::memcpy(h, mapped, 256);
    D3D12_RANGE none{ 0, 0 };
    readback->Unmap(0, &none);
    GiStats st;
    st.free = h[12];
    st.requested = h[13];
    st.selected = h[14];
    st.background = h[17];
    st.live = h[19];
    st.hits = h[28] + h[29];
    st.created = h[32];
    st.allocationFailures = h[33];
    st.tableFull = h[34];
    st.evicted = h[35];
    st.resets = h[36];
    st.hitLookups = h[38];  // GI_H_STAT_HIT_LOOKUPS
    st.hitMisses = h[39];
    st.gSamples = h[48];  // GI_H_STAT_G_SAMPLES
    st.gRatio = h[49];
    for (int i = 0; i < 9; ++i) st.gHistogram[i] = h[50 + i];  // GI_H_STAT_G_HIST
    st.gZero = h[59];                                            // GI_H_STAT_G_ZERO
    return st;
}
} // namespace unx::render::gi
