#include "unx/gi/GiSystem.h"

#include "unx/rt/RayPipeline.h"

#include <algorithm>
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
    uint32_t table, freeList, meta, anchor, sh, texels, update, selected, hitStamp, hitList, shTable, mapOwner, anchorMin, irr, slotAnchor, emit, split, end;
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
    l.emit = l.slotAnchor + s.tableSlots * 8;  // deterministic anchors per table slot (GiDetFold)
    l.split = l.emit + s.capacity * 256;       // emitter texels: 64 x RGB9E5 per entry (GiCache.hlsli giEmitterOffset)
    const uint64_t end = l.split + (s.splitBounceHistory ? (uint64_t)s.capacity * 3712 : 0); // GiSplitHistory.hlsli
    if (end >= (1ull << 32)) fail("GI split history exceeds raw-buffer address space");
    l.end = (uint32_t)end;
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
    s.historyStatic = (uint32_t)q.integer("gi.history_updates_max_static");
    s.splitBounceHistory = q.has("gi.split_bounce_history") && q.boolean("gi.split_bounce_history");
    if (q.has("gi.bounce_history_updates")) s.bounceHistoryUpdates = (uint32_t)q.integer("gi.bounce_history_updates");
    if (s.bounceHistoryUpdates == 0 || s.bounceHistoryUpdates > 32) fail("gi.bounce_history_updates must be in [1, 32]");
    s.maxLevel = (uint32_t)q.integer("gi.cache_levels_max");
    s.cellAngleDeg = (float)q.number("gi.cache_cell_angle_deg");
    s.cellMin = (float)q.number("gi.cache_cell_min_m");
    s.nearRadius = (float)q.number("gi.near_occlusion_radius_m");
    s.rayLength = (float)q.number("gi.ray_length_m");
    s.hitUpdateShare = (float)q.number("gi.hit_update_share");
    s.hitCellFootprintScale = (float)q.number("gi.hit_cell_footprint_scale");
    s.screenOcclusionHistory = (uint32_t)q.integer("gi.screen_occlusion_history_frames");
    s.screenFilterCells = (float)q.number("gi.screen_filter_cells");
    s.screenUpdateFrames = q.has("gi.screen_update_frames") ? (uint32_t)q.integer("gi.screen_update_frames") : 1;
    s.experimentDisable = (uint32_t)q.integer("gi.experiment_disable");
    s.deterministic = q.boolean("gi.deterministic") || (q.has("debug.deterministic") && q.boolean("debug.deterministic"));  // (debug.deterministic implies it)
    s.anchorVisibility = q.has("gi.anchor_visibility") ? q.boolean("gi.anchor_visibility") : false;
    // Fixed by the kernels (GiCache.hlsli, GiProbeGather.hlsl, GiInternal.hlsli probe offsets).
    if (q.integer("gi.cache_octahedral_texels") != 8) fail("gi.cache_octahedral_texels must be 8 (GI_TEXELS)");
    if (q.integer("gi.near_occlusion_taps") != 16) fail("gi.near_occlusion_taps must be 16 (GiProbeGather)");
    if (s.tableSlots < s.capacity) fail("gi.cache table slots (%u) must be >= capacity (%u): GiTableClear resets the map owners", s.tableSlots, s.capacity);
    if (s.probeSpacing != 8) fail("gi.screen_probe_spacing_px must be 8 (3-bit probe offsets, 4 candidate points)");
    if (s.capacity == 0 || s.raysPerFrame < 64 || s.historyMax == 0) fail("gi: zero capacity, rays or history");
    if (s.historyStatic < s.historyMax || s.historyStatic > 4096 || s.historyMax > 4096) fail("gi: history_updates_max_static below history_updates_max, or a window above 4096 (12-bit count)");
    if (s.maxLevel > 31) fail("gi.cache_levels_max must be <= 31 (5-bit key field)");
    if (s.screenOcclusionHistory < 1 || s.screenOcclusionHistory > 255) fail("gi.screen_occlusion_history_frames must be in [1, 255]");
    if (!(s.screenFilterCells >= 0 && s.screenFilterCells <= 2)) fail("gi.screen_filter_cells must be in [0, 2] (cell edges)");
    if (s.screenUpdateFrames < 1 || s.screenUpdateFrames > 4) fail("gi.screen_update_frames must be in [1, 4] (2-bit value age, 2 x 2 tile order)");
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
    if (quality.has("reflection.g_rays_per_sample"))
        m_reflectionRays = (uint32_t)std::max<int64_t>(1, quality.integer("reflection.g_rays_per_sample"));
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
    h[31] = m_settings.historyMax | (m_settings.historyStatic << 16);
    h[37] = l.mapOwner;  // GI_H_MAP_OWNER
    h[60] = l.anchorMin; // deterministic anchors (GiHeader.offAnchorMin)
    h[62] = l.irr;       // irradiance maps (GiHeader.offIrr)
    h[63] = l.slotAnchor;  // deterministic anchors per table slot (GiHeader.offSlotAnchor)
    h[192] = l.end; // deterministic admission tail, grown before its first use
    h[193] = m_settings.splitBounceHistory ? l.split : 0;
    h[194] = m_settings.bounceHistoryUpdates;
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

void GiSystem::ensureProbeHistory(uint32_t probesX, uint32_t probesY)
{
    if (m_probeHistory[0] && m_probeHistoryX == probesX && m_probeHistoryY == probesY) return;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 2ull * probesX;
    d.Height = probesY;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (int k = 0; k < 2; ++k)
    {
        if (m_probeHistory[k]) m_device.deferRelease(m_probeHistory[k]);
        check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&m_probeHistory[k])),
              "GI probe occlusion history");
        m_probeHistory[k]->SetName(k ? L"R GI probe occlusion history 1" : L"R GI probe occlusion history 0");
    }
    m_probeHistoryX = probesX;
    m_probeHistoryY = probesY;
    m_probeHistoryReset = true;  // (new textures hold nothing; the previous grid had other dimensions)
}

void GiSystem::ensureScreenHistory(uint32_t width, uint32_t height)
{
    if (m_screenValue[0] && m_screenX == width && m_screenY == height) return;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    const auto create = [&](ComPtr<ID3D12Resource>& t, DXGI_FORMAT format) {
        if (t) m_device.deferRelease(t);
        d.Format = format;
        check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&t)),
              "GI screen history");
    };
    for (int k = 0; k < 2; ++k)
    {
        create(m_screenValue[k], DXGI_FORMAT_R16G16B16A16_FLOAT);
        create(m_screenKeys[k], DXGI_FORMAT_R32G32_UINT);
        m_screenValue[k]->SetName(k ? L"R GI screen irradiance 1" : L"R GI screen irradiance 0");
        m_screenKeys[k]->SetName(k ? L"R GI screen keys 1" : L"R GI screen keys 0");
    }
    m_screenX = width;
    m_screenY = height;
    m_screenValid = false;  // (new textures hold nothing)
}

GiSystem::~GiSystem()
{
    for (auto& t : m_probeHistory)
        if (t) m_device.deferRelease(t);
    for (int k = 0; k < 2; ++k)
    {
        if (m_screenValue[k]) m_device.deferRelease(m_screenValue[k]);
        if (m_screenKeys[k]) m_device.deferRelease(m_screenKeys[k]);
    }
    if (m_changeRing)
    {
        m_changeRing->Unmap(0, nullptr);
        m_device.deferRelease(m_changeRing);
        DescriptorHeaps* dh = &m_device.descriptors();
        for (uint32_t srv : m_changeSrv) m_device.deferCall([dh, srv] { dh->freeResource(srv); });
    }
    m_device.deferRelease(m_cache);
    m_device.deferRelease(m_dispatchSignature);
}

// r.gi.screen (GiScreenIrradiance.hlsl) and r.gi.screen.filter (GiScreenFilter.hlsl) of a view: its per-pixel cache
// irradiance into view.giIrradiance (the filtered texture; the unfiltered one is returned for the lookup-stats gate).
// The main view, and the planar reflection views (recordSecondaryScreen: the same lookup and filter as the main view,
// where they looked the cache up per pixel with the plain trilinear rule and no filter). Secondary views' passes carry
// ".planar" (their cost is the reflection budget's, as M's planar passes).
TextureRef GiSystem::recordScreen(FramePassContext& fc, ViewResources& view, BufferRef cache)
{
    RenderGraph& g = fc.graph;
    const GiSettings& s = m_settings;
    ShaderLibrary& shaders = fc.shaders;
    const bool planar = view.view.kind != gpu::ViewKind::Main;
    const TextureRef depth = view.depth, gbuffer = view.gbuffer;
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = view.frameConstants;
    const uint32_t width = view.view.width, height = view.view.height;
    // Frame split (gi.screen_update_frames, GiScreenIrradiance.hlsl): the main view with V's vis buffer (the surface's
    // exact motion); not while the lookup statistics compare the texture with the lookup. The history restarts on new
    // textures, a scene revision, a lighting epoch, a history discontinuity (restore, camera cut) or an origin shift.
    const TextureRef visId = view.visId;
    const BufferRef visibleClusters = view.visibleClusters;
    const bool split = !planar && s.screenUpdateFrames > 1 && visId.valid() && visibleClusters.valid() && !m_lookupStatsOn;
    TextureRef screen, keys, prevScreen, prevKeys;
    uint32_t splitFlags = 0;
    float exposureRatio = 1;
    float4x4 invPrev{};
    if (split)
    {
        ensureScreenHistory(width, height);
        const float3 shift = fc.frame.originShift;
        if (fc.scene.revision() != m_screenRevision || m_epoch != m_screenEpoch || fc.frame.discontinuity != 0 || shift.x != 0 || shift.y != 0 ||
            shift.z != 0)
            m_screenValid = false;
        m_screenRevision = fc.scene.revision();
        m_screenEpoch = m_epoch;
        const float exposure = 1.0f / (1.2f * std::exp2(view.view.ev100));
        exposureRatio = m_screenValid && m_screenPrevExposure > 0 ? exposure / m_screenPrevExposure : 1.0f;
        invPrev = m_screenPrevInvViewProj;
        splitFlags = 1u | (m_screenValid ? 2u : 0u) | ((uint32_t)(fc.frame.frameIndex % s.screenUpdateFrames) << 2) | (s.screenUpdateFrames << 4);
        m_screenPrevExposure = exposure;
        m_screenPrevInvViewProj = view.view.invViewProj;
        m_screenValid = true;
        const uint32_t prev = m_screenParity, next = prev ^ 1u;
        m_screenParity = next;
        const TextureDesc vd{ "GI screen irradiance (unfiltered)", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT };
        const TextureDesc kd{ "GI screen keys", width, height, 1, 1, DXGI_FORMAT_R32G32_UINT };
        TextureDesc pvd = vd, pkd = kd;
        pvd.name = "GI screen irradiance (previous)";
        pkd.name = "GI screen keys (previous)";
        screen = g.importTexture(m_screenValue[next].Get(), vd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        keys = g.importTexture(m_screenKeys[next].Get(), kd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        prevScreen = g.importTexture(m_screenValue[prev].Get(), pvd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        prevKeys = g.importTexture(m_screenKeys[prev].Get(), pkd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    }
    else
    {
        if (!planar) m_screenValid = false;  // (a frame without the split leaves no history)
        screen = g.createTexture({ "GI screen irradiance (unfiltered)", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    }
    // The main view's lookups read the cache concurrently with the reflection hits that follow (useConcurrentRead): those
    // write only what the lookups do not read or read as missing - hit stamps, age, request lists, statistics, and new
    // entries (a new entry has no update yet: giScreenCellUpdates takes it as a missing cell, as before it existed; an
    // insertion fills an empty table slot, which ends no other key's probe run). So the reflections do not wait for the
    // lookup and its filter (~0.6 ms at internal 960p [measured 974c6bb]), which overlap them from the async queue with
    // the same values. The planar views' lookups keep the plain order.
    const auto readCache = [&](PassBuilder& b) {
        if (planar) b.use(cache, Use::SrvCompute);
        else b.useConcurrentRead(cache);
    };
    g.addPass(planar ? "r.gi.screen.planar" : "r.gi.screen", QueueType::Compute,
              [&](PassBuilder& b) {
                  readCache(b);
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(screen, Use::UavCompute);
                  if (split)
                  {
                      b.use(keys, Use::UavCompute);
                      b.use(prevScreen, Use::SrvCompute);
                      b.use(prevKeys, Use::SrvCompute);
                      b.use(visId, Use::SrvCompute);
                      b.use(visibleClusters, Use::SrvCompute);
                  }
              },
              [&shaders, cache, depth, gbuffer, screen, frameConstants, width, height, split, keys, prevScreen, prevKeys, visId, visibleClusters, splitFlags,
               exposureRatio, invPrev](PassContext& c) {
                  const uint32_t none = 0xFFFFFFFFu;
                  uint32_t k[32] = { c.srv(cache), c.srv(depth), c.srv(gbuffer), c.uav(screen), width, height, splitFlags, asU(exposureRatio),
                                     split ? c.srv(prevKeys) : none, split ? c.uav(keys) : none, split ? c.srv(prevScreen) : none, split ? c.srv(visId) : none,
                                     split ? c.srv(visibleClusters) : none, 0, 0, 0 };
                  std::memcpy(&k[16], &invPrev, 64);  // rows (row_major in HLSL)
                  c.cmd->SetPipelineState(shaders.compute(split ? "Passes/GI/GiScreenIrradiance.SPLIT1" : "Passes/GI/GiScreenIrradiance.SPLIT0"));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
              });
    // The filter pass always runs: besides the spatial filter (gi.screen_filter_cells, 0 = none) it multiplies the main
    // view's probe near occlusion in (view.giIrradiance = the pixel's whole front diffuse indirect irradiance, which M's
    // shading kernel takes without a probe gather).
    const TextureRef filtered = g.createTexture({ "GI screen irradiance", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const float pixelAngle = 2.0f * std::tan(view.view.verticalFov * 0.5f) / (float)height;
    const TextureRef probes = planar ? TextureRef{} : view.screenProbes;
    g.addPass(planar ? "r.gi.screen.filter.planar" : "r.gi.screen.filter", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(screen, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  readCache(b);
                  if (probes.valid()) b.use(probes, Use::SrvCompute);
                  b.use(filtered, Use::UavCompute);
              },
              [&shaders, screen, depth, gbuffer, cache, probes, filtered, frameConstants, width, height, pixelAngle, s](PassContext& c) {
                  const uint32_t k[12] = { c.srv(screen), c.srv(depth), c.srv(gbuffer), c.uav(filtered), width, height, c.srv(cache),
                                           asU(pixelAngle), asU(s.screenFilterCells), probes.valid() ? c.srv(probes) : 0xFFFFFFFFu, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiScreenFilter"));
                  c.computeConstants(k, 12);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
              });
    view.giIrradiance = filtered;
    return screen;
}

void GiSystem::recordSecondaryScreen(FramePassContext& fc, ViewResources& view)
{
    if (!fc.resources.giCache.valid() || !view.depth.valid() || !view.gbuffer.valid()) return;  // (no cache this frame yet: the view keeps its direct lookups)
    recordScreen(fc, view, fc.resources.giCache);
}

void GiSystem::ensureAdmission(FramePassContext& fc, const ViewResources& main)
{
    // One request per traced GI/reflection hit, and two levels per probe on
    // both placement walks. No budget truncation: reserve the producer bound.
    const uint64_t pixels = (uint64_t)main.view.width * main.view.height;
    const uint64_t probes = ((main.view.width + m_settings.probeSpacing - 1) / m_settings.probeSpacing + 1ull) *
                            ((main.view.height + m_settings.probeSpacing - 1) / m_settings.probeSpacing + 1ull);
    const uint64_t wanted = pixels * m_reflectionRays + m_settings.raysPerFrame + 4 * probes;
    if (wanted <= m_admissionCapacity) return;
    const uint64_t total = wanted + m_settings.capacity;
    const Layout layout = layoutOf(m_settings);
    uint64_t scanWords = 0;
    for (uint64_t count = (total + 255) / 256;; count = (count + 255) / 256)
    {
        scanWords += count;
        if (count <= 1) break;
    }
    const uint64_t bytes = layout.end + 256ull + total * 64 + (scanWords + 1) * 4;
    if (bytes >= (1ull << 32)) fail("GI deterministic admission exceeds the raw-buffer address space; producer bound needs partitioning");
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> grown;
    check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                   IID_PPV_ARGS(&grown)), "GI deterministic admission workspace");
    grown->SetName(L"GI cache with deterministic admission");
    RenderGraph& graph = fc.graph;
    const BufferRef oldCache = graph.importBuffer(m_cache.Get(), { "GI cache before admission growth", m_bytes, 0 });
    const BufferRef newCache = graph.importBuffer(grown.Get(), { "GI cache", bytes, 0 });
    const uint64_t copyBytes = m_bytes;
    graph.addPass("r.gi.admit.grow", QueueType::Graphics,
                  [&](PassBuilder& b) { b.use(oldCache, Use::CopySrc); b.use(newCache, Use::CopyDst); },
                  [oldCache, newCache, copyBytes](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(newCache), 0, c.resource(oldCache), 0, copyBytes); });
    ShaderLibrary& shaders = fc.shaders;
    const uint32_t capacity = (uint32_t)wanted, first = m_admissionCapacity == 0 ? 1u : 0u;
    graph.addPass("r.gi.admit.init", QueueType::Compute, [&](PassBuilder& b) { b.use(newCache, Use::UavCompute); },
                  [&shaders, newCache, capacity, first](PassContext& c) {
                      const uint32_t k[4] = { c.uav(newCache), capacity, first, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiAdmissionInit"));
                      c.computeConstants(k, 4); c.cmd->Dispatch(1, 1, 1);
                  });
    m_device.deferRelease(m_cache);
    m_cache = std::move(grown);
    m_bytes = bytes;
    m_admissionCapacity = capacity;
}

void GiSystem::recordAdmission(FramePassContext& fc, BufferRef cache)
{
    RenderGraph& graph = fc.graph;
    ShaderLibrary& shaders = fc.shaders;
    const uint32_t requestCapacity = m_admissionCapacity, pool = m_settings.capacity;
    const uint32_t total = requestCapacity + pool;
    const BufferRef args = graph.createBuffer({ "GI admission dispatch", 16, 0 });
    ID3D12CommandSignature* signature = dispatchSignature();
    graph.addPass("r.gi.admit.prepare", QueueType::Compute,
                  [&](PassBuilder& b) { b.use(cache, Use::UavCompute); b.use(args, Use::UavCompute); },
                  [&shaders, cache, args, pool, requestCapacity](PassContext& c) {
                      const uint32_t k[4] = { c.uav(cache), c.uav(args), requestCapacity, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiAdmissionPrepare"));
                      c.computeConstants(k, 4); c.cmd->Dispatch((pool + 255) / 256, 1, 1);
                  });
    auto indirect = [&](const char* name, const char* kernel, uint32_t parity, uint32_t span) {
        graph.addPass(name, QueueType::Compute,
                      [&](PassBuilder& b) { b.use(cache, Use::UavCompute); b.use(args, Use::IndirectArgs); },
                      [&shaders, cache, args, signature, requestCapacity, kernel, parity, span](PassContext& c) {
                          const uint32_t k[4] = { c.uav(cache), requestCapacity, parity, span };
                          c.cmd->SetPipelineState(shaders.compute(kernel)); c.computeConstants(k, 4);
                          c.cmd->ExecuteIndirect(signature, 1, c.resource(args), 0, nullptr, 0);
                      });
    };
    uint32_t parity = 0;
    for (uint32_t span = 1; span < total; span *= 2)
    {
        indirect("r.gi.admit.merge", "Passes/GI/GiAdmissionMerge", parity, span);
        parity = 1 - parity;
    }
    indirect("r.gi.admit.mark", "Passes/GI/GiAdmissionMark", parity, 0);
    uint32_t level = 0;
    for (uint32_t count = (total + 255) / 256;; count = (count + 255) / 256, ++level)
    {
        const uint32_t groups = (count + 255) / 256;
        graph.addPass("r.gi.admit.scan", QueueType::Compute, [&](PassBuilder& b) { b.use(cache, Use::UavCompute); },
                      [&shaders, cache, requestCapacity, level, groups](PassContext& c) {
                          const uint32_t k[4] = { c.uav(cache), requestCapacity, level, 0 };
                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiAdmissionScan"));
                          c.computeConstants(k, 4); c.cmd->Dispatch(groups, 1, 1);
                      });
        if (count <= 256) break;
    }
    indirect("r.gi.admit.publish", "Passes/GI/GiAdmissionPublish", parity, 0);
}

void GiSystem::record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays)
{
    RenderGraph& g = fc.graph;
    const GiSettings& s = m_settings;
    // Lighting epoch: a new scene upload (geometry, materials, lights), new sky constants or a restore discontinuity
    // (snapshot restore, save load: every temporal state resets, world-space caches included; Frame.h) reset every
    // entry's history. A camera cut keeps the cache.
    // An instance edit (B3: destruction events; the ray scene rebuilt incrementally with the same meshes, materials and
    // lights) keeps the epoch: GiInvalidate restarts only the entries whose texel rays can see a changed box.
    if ((fc.frame.discontinuity & kDiscontinuityRestore) != 0 || (fc.scene.revision() != m_sceneRevision && !rays.incrementalRebuild())) ++m_epoch;
    m_sceneRevision = fc.scene.revision();
    if (s.deterministic) ensureAdmission(fc, main);
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
    compute("r.gi.begin", "Passes/GI/GiBegin", 1, { frame, m_epoch, (s.deterministic ? 1u : 0u) | (s.anchorVisibility ? 2u : 0u), cam[0], cam[1], cam[2] });
    // Deterministic anchors: last frame's per-slot candidates (its ray passes) into the entries before the table clears.
    // C9 origin rebase: the entries move by whole cells before this frame's rehash files them (GiShift.hlsl).
    if (fc.frame.originShift.x != 0 || fc.frame.originShift.y != 0 || fc.frame.originShift.z != 0)
    {
        uint32_t d[3];
        std::memcpy(d, &fc.frame.originShift, 12);
        compute("r.gi.shift", "Passes/GI/GiShift", groups(s.capacity), { d[0], d[1], d[2] });
        if (s.deterministic)
        {
            const uint32_t dx = d[0], dy = d[1], dz = d[2], count = (m_admissionCapacity + 255) / 256;
            g.addPass("r.gi.admit.shift", QueueType::Compute, [&](PassBuilder& b) { b.use(cache, Use::UavCompute); },
                      [&shaders, cache, dx, dy, dz, count](PassContext& c) {
                          const uint32_t k[4] = { c.uav(cache), dx, dy, dz };
                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiAdmissionShift")); c.computeConstants(k, 4);
                          c.cmd->Dispatch(std::min(count, 65535u), (count + 65534) / 65535, 1);
                      });
        }
    }
    compute("r.gi.evict", "Passes/GI/GiEvict", groups(s.capacity), {});
    compute("r.gi.clear", "Passes/GI/GiTableClear", groups(s.tableSlots), {});
    if (s.deterministic) recordAdmission(fc, cache);
    else compute("r.gi.rehash", "Passes/GI/GiRehash", groups(s.capacity), {});
    // First placement collects missing keys; after admission a second placement
    // requests the complete stable footprint, including this frame's new cells.
    for (uint32_t placement = 0; placement < (s.deterministic ? 2u : 1u); ++placement)
    {
    if (placement == 1) recordAdmission(fc, cache);
    g.addPass(placement == 0 ? "r.gi.place" : "r.gi.place.admitted", QueueType::Compute,
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
    }
    if (s.deterministic)
    {
        compute("r.gi.det.anchors", "Passes/GI/GiDetAnchors", groups(s.capacity), {});  // before any ray leaves an anchor
    }
    // Local invalidation (B3): the ray scene's change boxes of this frame (instance edits keep the epoch).
    if (!rays.changes().empty())
    {
        if (!m_changeRing)
        {
            D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC1 d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = (uint64_t)kChangeSlots * kChangeSlotBytes;
            d.Height = d.DepthOrArraySize = d.MipLevels = 1;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(fc.device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                           IID_PPV_ARGS(&m_changeRing)),
                  "GI change boxes");
            m_changeRing->SetName(L"GI change boxes ring");
            D3D12_RANGE nothing{ 0, 0 };
            check(m_changeRing->Map(0, &nothing, reinterpret_cast<void**>(&m_changeMapped)), "map GI change boxes");
            DescriptorHeaps& dh = fc.device.descriptors();
            for (uint32_t k = 0; k < kChangeSlots; ++k)
            {
                m_changeSrv[k] = dh.allocateResource();
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Format = DXGI_FORMAT_R32_TYPELESS;
                sd.Buffer.FirstElement = k * kChangeSlotBytes / 4;  // slots are 16-byte multiples (raw views need 16 B alignment)
                sd.Buffer.NumElements = kChangeSlotBytes / 4;
                sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                fc.device.d3d()->CreateShaderResourceView(m_changeRing.Get(), &sd, dh.resourceCpu(m_changeSrv[k]));
            }
        }
        // Slot of frame % kChangeSlots: that slot's frame has completed (frames in flight <= kChangeSlots). More boxes
        // than the slot holds: the rest merge into the last one (a larger box invalidates more, never less).
        static_assert(kChangeSlotBytes % 16 == 0, "raw view offsets");
        const uint32_t slot = (uint32_t)(fc.frame.frameIndex % kChangeSlots);
        uint8_t* dst = m_changeMapped + (size_t)slot * kChangeSlotBytes;
        const auto& changes = rays.changes();
        const uint32_t count = (uint32_t)std::min<size_t>(changes.size(), kChangeBoxesMax);
        for (uint32_t i = 0; i < count; ++i)
        {
            float3 lo = changes[i].first, hi = changes[i].second;
            if (i == count - 1)
                for (size_t j = count; j < changes.size(); ++j)
                {
                    lo = { std::min(lo.x, changes[j].first.x), std::min(lo.y, changes[j].first.y), std::min(lo.z, changes[j].first.z) };
                    hi = { std::max(hi.x, changes[j].second.x), std::max(hi.y, changes[j].second.y), std::max(hi.z, changes[j].second.z) };
                }
            const float box[8] = { lo.x, lo.y, lo.z, 0, hi.x, hi.y, hi.z, 0 };
            std::memcpy(dst + 16 + i * 32, box, 32);
        }
        std::memcpy(dst, &count, 4);
        const uint32_t boxesSrv = m_changeSrv[slot];
        g.addPass("r.gi.invalidate", QueueType::Compute, [&](PassBuilder& b) { b.use(cache, Use::UavCompute); },
                  [&shaders, cache, boxesSrv, capacity = s.capacity](PassContext& c) {
                      const uint32_t k[4] = { c.uav(cache), boxesSrv, 0, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiInvalidate"));
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(512, (capacity + 511) / 512, 1);
                  });
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
    if (!detState)
    {
        // Background updates of the index range into the selected list, then the range emptied (GiBackgroundList.hlsl:
        // the trace and the integration read one list; each testing the range on its own could disagree).
        for (const uint32_t mode : { 0u, 1u })
            g.addPass(mode == 0 ? "r.gi.background" : "r.gi.background.done", QueueType::Compute, [&](PassBuilder& b) { b.use(cache, Use::UavCompute); },
                      [&shaders, cache, mode, dispatch = groups(s.updatesPerFrame)](PassContext& c) {
                          const uint32_t k[4] = { c.uav(cache), mode, 0, 0 };
                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiBackgroundList"));
                          c.computeConstants(k, 4);
                          c.cmd->Dispatch(mode == 0 ? dispatch : 1, 1, 1);
                      });
    }
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
    rays.recordDecals(fc, main);  // decals at hits (HitDecals.hlsli): header words before rootConstants
    rays.rootConstants(scene);
    // Escaping rays see S's sky and sun when the atmosphere LUTs exist this frame (variant SKY0); otherwise the constant
    // sky of setConstantSky (SKY1: tests, and builds without the S track).
    const FrameResources& fr = fc.resources;
    const bool atmosphere = fr.transmittanceLut.valid() && fr.multiScatterLut.valid() && fr.skyViewLut.valid() && fr.aerialPerspective.valid();
    const TextureRef luts[4] = { fr.transmittanceLut, fr.multiScatterLut, fr.skyViewLut, fr.aerialPerspective };
    const std::string traceKernel = std::string("Passes/GI/GiTrace.SKY") + (atmosphere ? "0" : "1") + (s.splitBounceHistory ? ".SPLIT1" : ".SPLIT0");
    rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(traceKernel, { "GiTraceGen" }));
    const float3 sky = m_skyRadiance, sun = m_sunIlluminance;
    const float skyBand = m_skyBand;
    const uint32_t rayCount = s.updatesPerFrame * 64;
    // Each ray's radiance and hemispherical octahedral coordinates, for the per-ray irradiance map and SH (GiIntegrate).
    // Four blocks: texel samples (irradiance), emitter samples, texel values (radiance, distance, bounce; GiIntegrate blends),
    // the texel's analytic-emitter radiance (the emitter texels, K path only).
    const BufferRef samples = g.createBuffer({ "GI ray samples", (uint64_t)rayCount * (s.splitBounceHistory ? 7 : 4) * 16, 16 });
    g.addPass("r.gi.trace", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::UavGraphics);
                  b.use(samples, Use::UavGraphics);
                  rays.declareTraversal(b);
                  rays.declareDecals(b);
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
              [&shaders, cache, samples, updates = s.updatesPerFrame, split = s.splitBounceHistory, frameConstants](PassContext& c) {
                  const uint32_t k[4] = { c.uav(cache), updates, c.srv(samples), 0 };
                  c.cmd->SetPipelineState(shaders.compute(split ? "Passes/GI/GiIntegrate.SPLIT1" : "Passes/GI/GiIntegrate.SPLIT0"));
                  c.computeConstants(k, 4);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(updates, 1, 1);  // one group per update slot
              });

    // Map owner list (count, then probe indices) and its indirect dispatch arguments, reset by the gather.
    const BufferRef owners = g.createBuffer({ "GI map owners", 4ull + 4ull * probesX * probesY, 0 });
    const BufferRef mapArgs = g.createBuffer({ "GI map dispatch args", 16, 0 });
    // The probes' occlusion over time (GiProbeGather): needs V's vis id (the surface's exact motion); frames without it
    // (stand-in visibility in tests) take each frame's own 16 points at a fixed rotation. The history restarts on new
    // textures, a scene revision (geometry may have changed) or a history discontinuity (restore, camera cut).
    const bool probeHistory = main.visId.valid() && main.visibleClusters.valid() && s.screenOcclusionHistory > 1;
    TextureRef historyPrev, historyNext;
    uint32_t historyFlags = 0, historyPrevX = 0, historyPrevY = 0;
    if (probeHistory)
    {
        const uint32_t oldX = m_probeHistoryX, oldY = m_probeHistoryY;
        ensureProbeHistory(probesX, probesY);
        if (fc.scene.revision() != m_probeHistoryRevision || fc.frame.discontinuity != 0) m_probeHistoryReset = true;
        m_probeHistoryRevision = fc.scene.revision();
        historyFlags = m_probeHistoryReset ? 1u : 0u;
        m_probeHistoryReset = false;
        historyPrevX = oldX == probesX ? probesX : 0;
        historyPrevY = oldY == probesY ? probesY : 0;
        const uint32_t prev = m_probeHistoryParity, next = prev ^ 1u;
        m_probeHistoryParity = next;
        historyPrev = g.importTexture(m_probeHistory[prev].Get(), { "GI probe occlusion history (previous)", 2 * probesX, probesY, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT },
                                      D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        historyNext = g.importTexture(m_probeHistory[next].Get(), { "GI probe occlusion history", 2 * probesX, probesY, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT },
                                      D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    }
    const TextureRef visId = main.visId;
    const BufferRef visibleClusters = main.visibleClusters;
    g.addPass("r.gi.gather", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::UavCompute);  // radiance map owners
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(probes, Use::UavCompute);
                  b.use(owners, Use::UavCompute);
                  b.use(mapArgs, Use::UavCompute);
                  if (probeHistory)
                  {
                      b.use(visId, Use::SrvCompute);
                      b.use(visibleClusters, Use::SrvCompute);
                      b.use(historyPrev, Use::UavCompute);
                      b.use(historyNext, Use::UavCompute);
                  }
              },
              [&shaders, cache, depth, gbuffer, probes, owners, mapArgs, probesX, probesY, frameConstants, s, main, probeHistory, visId, visibleClusters,
               historyPrev, historyNext, historyFlags, historyPrevX, historyPrevY](PassContext& c) {
                  const uint32_t none = 0xFFFFFFFFu;
                  const uint32_t k[20] = { c.uav(cache), c.srv(depth), c.srv(gbuffer), c.uav(probes), probesX, probesY, main.view.width, main.view.height,
                                           s.probeSpacing, asU(s.nearRadius), c.uav(owners), c.uav(mapArgs),
                                           probeHistory ? c.srv(visId) : none, probeHistory ? c.srv(visibleClusters) : none,
                                           probeHistory ? c.uav(historyPrev) : none, probeHistory ? c.uav(historyNext) : none,
                                           historyFlags, s.screenOcclusionHistory, historyPrevX, historyPrevY };
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiProbeGather"));
                  c.computeConstants(k, 20);
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
    // M's per-pixel cache irradiance (front side) as a pass of its own (GiScreenIrradiance.hlsl; R_STATUS 0, GI tile path
    // verdict): M reads view.giIrradiance once instead of the lookup inside its shading kernel. Culled while nothing reads it.
    // Then the edge-preserving spatial filter over the cells' blotches (GiScreenFilter.hlsl) makes view.giIrradiance.
    const TextureRef screenRaw = recordScreen(fc, main, cache);
    if (m_lookupStatsOn)
    {
        if (!m_lookupStats)
        {
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC1 d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            d.Width = 512;
            d.Height = d.DepthOrArraySize = d.MipLevels = 1;
            d.SampleDesc.Count = 1;
            d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                           IID_PPV_ARGS(&m_lookupStats)),
                  "GI lookup stats");
        }
        const BufferRef counters = g.importBuffer(m_lookupStats.Get(), { "GI lookup stats", 512, 0 });
        for (uint32_t clear : { 1u, 0u })
            g.addPass(clear ? "r.gi.lookupstats.clear" : "r.gi.lookupstats", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(cache, Use::SrvCompute);
                          b.use(depth, Use::SrvCompute);
                          b.use(gbuffer, Use::SrvCompute);
                          b.use(counters, Use::UavCompute);
                          b.use(screenRaw, Use::SrvCompute);  // r.gi.screen's own values (before the filter)
                          b.keep();
                      },
                      [&shaders, cache, depth, gbuffer, counters, frameConstants, main, clear, screenRaw](PassContext& c) {
                          const uint32_t k[8] = { c.srv(cache), c.srv(depth), c.srv(gbuffer), c.uav(counters), main.view.width, main.view.height, clear,
                                                  c.srv(screenRaw) };
                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/Gates/GiLookupStats"));
                          c.computeConstants(k, 8);
                          c.bindFrameConstants(frameConstants);
                          if (clear) c.cmd->Dispatch(1, 1, 1);
                          else c.cmd->Dispatch((main.view.width + 7) / 8, (main.view.height + 7) / 8, 1);
                      });
    }
}

GiLookupStats GiSystem::readLookupStats()
{
    GiLookupStats st;
    if (!m_lookupStats) return st;
    m_device.waitIdle();
    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = 512;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(m_device.d3d()->CreateCommittedResource3(&rb, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)),
          "GI lookup stats readback");
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(readback.Get(), 0, m_lookupStats.Get(), 0, 512);
    m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(cl));
    uint32_t h[128];
    void* mapped = nullptr;
    D3D12_RANGE all{ 0, 512 }, none{ 0, 0 };
    check(readback->Map(0, &all, &mapped), "map GI lookup stats");
    std::memcpy(h, mapped, 512);
    readback->Unmap(0, &none);
    st.pixels = h[0], st.levels = h[1], st.lookups = h[2], st.slots = h[3], st.found = h[4];
    st.tiles = h[5], st.tileKeys = h[6], st.tileEntries = h[7], st.maxTileKeys = h[8], st.maxTileEntries = h[9], st.multiLevelPixels = h[10];
    st.tileDiffers = h[11], st.tileMissed = h[13];
    std::memcpy(&st.tileMaxRel, &h[12], 4);
    st.evaluated = h[14];
    std::memcpy(&st.fillMaxRel, &h[80], 4);
    st.fillSumRel1e3 = h[81], st.fillCompared = h[82], st.fillOver1 = h[83], st.fillOver5 = h[84];
    st.screenFlagDiffers = h[85], st.screenOver = h[86], st.screenCompared = h[88];
    std::memcpy(&st.screenMaxRel, &h[87], 4);
    for (int i = 0; i < 64; ++i) st.entryHistogram[i] = h[16 + i];
    return st;
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
    st.epoch = h[18];  // GI_H_EPOCH
    st.hitLookups = h[38];  // GI_H_STAT_HIT_LOOKUPS
    st.hitMisses = h[39];
    st.gSamples = h[48];  // GI_H_STAT_G_SAMPLES
    st.gRatio = h[49];
    for (int i = 0; i < 9; ++i) st.gHistogram[i] = h[50 + i];  // GI_H_STAT_G_HIST
    st.gZero = h[59];                                            // GI_H_STAT_G_ZERO
    return st;
}
} // namespace unx::render::gi
