// Particle media and heat haze (track E). See include/unx/volume/VolumePass.h and VolumeCommon.hlsli.
#include "unx/volume/VolumePass.h"

#include "unx/core/Log.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>

namespace unx::volume
{
using namespace unx::render;

namespace
{
// Mirror of VolumeCommon.hlsli VolumeConstants.
struct VolumeConstants
{
    float offsetCur[3]; float w;
    float offsetPrev[3]; float dt;
    uint32_t threads, current, rangeCount, mode;
    uint32_t posAgeCur, velocityCur, posAgePrev, velocityPrev;
    uint32_t dynamicCur, dynamicPrev, emitters, programs;
    uint32_t curveKeys, ranges, blocks, records;
    uint32_t hazeWidth, hazeHeight, hazeTilesX, hazeTilesY;
    uint32_t hazeCounts, hazeStarts, hazeFill, hazeEntries;
    uint32_t hazeEntryCapacity, distortionOffset, distortionDepth, counters;
    uint32_t froxelLights, mediaCounts, mediaStarts, mediaFill;
    uint32_t mediaEntries, mediaEntryCapacity, volumeSlices, mediaTiles;
    uint32_t shadow[8];
    uint32_t giCache, airVolume, transmittance, multiScatter;
    float streamAxes[3]; uint32_t hazeCells;
};
static_assert(sizeof(VolumeConstants) == 240);
constexpr uint32_t kConstantSlots = 64, kConstantSlotBytes = 256;
constexpr uint32_t kHazeScale = 4, kHazeTile = 8;  // VOLUME_HAZE_SCALE, VOLUME_HAZE_TILE
constexpr uint32_t kRecordBytes = 48;

uint32_t groups(uint64_t n, uint32_t size) { return (uint32_t)((n + size - 1) / size); }
constexpr uint32_t kLooseSpan = 4;  // VolumeCommon.hlsli VOLUME_LOOSE_SPAN: a record's list cells span at most K x K
// Cells of the loose-quadtree tile lists over a tilesX x tilesY grid: every level from the tiles to the first 1 x 1
// (VolumeCommon.hlsli volumeLevelBase(tiles, volumeLevelCount(tiles))).
uint32_t listCells(uint32_t tilesX, uint32_t tilesY)
{
    uint32_t cells = 0;
    for (uint32_t L = 0;; ++L)
    {
        const uint32_t x = (tilesX + (1u << L) - 1) >> L, y = (tilesY + (1u << L) - 1) >> L;
        cells += x * y;
        if (x <= 1 && y <= 1) return cells;
    }
}
} // namespace

void froxelGridSize(const QualityConfig& q, uint32_t width, uint32_t height, uint32_t& gridX, uint32_t& gridY, uint32_t& slices, uint32_t& tilePx,
                    uint32_t mainHeight)
{
    tilePx = render::froxelTilePx(q, mainHeight ? mainHeight : height);
    slices = (uint32_t)q.integer("atmosphere.froxels.depth_slices");
    if (tilePx == 0 || slices == 0 || slices > 64) fail("volume: atmosphere.froxels tile_px > 0 and 1 <= depth_slices <= 64");
    gridX = (width + tilePx - 1) / tilePx;
    gridY = (height + tilePx - 1) / tilePx;
}

VolumePass::VolumePass(Device& device) : m_device(device)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = (uint64_t)kConstantSlots * kConstantSlotBytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_upload)),
          "volume constants");
    m_upload->SetName(L"Volume constants ring");
    D3D12_RANGE none{ 0, 0 };
    check(m_upload->Map(0, &none, reinterpret_cast<void**>(&m_mapped)), "map volume constants");
}

VolumePass::~VolumePass()
{
    m_device.waitIdle();
    if (m_upload && m_mapped) m_upload->Unmap(0, nullptr);
}

VolumeOutput VolumePass::record(fx::ParticleSystem& particles, RenderGraph& g, ShaderLibrary& shaders, const QualityConfig& quality, uint64_t importIndex,
                                const VolumeFrame& f)
{
    if (!f.view || (!(f.media && f.froxelLights.valid()) && !f.haze)) return {};
    const fx::ParticleRenderInputs in = particles.renderInputs(g, importIndex);
    if (!in.valid) return {};
    return recordImpl(&in, {}, 0, g, shaders, quality, f);
}

VolumeOutput VolumePass::recordRecords(BufferRef records, uint32_t count, RenderGraph& g, ShaderLibrary& shaders, const QualityConfig& quality, const VolumeFrame& f)
{
    return recordImpl(nullptr, records, count, g, shaders, quality, f);
}

VolumeOutput VolumePass::recordImpl(const fx::ParticleRenderInputs* particlesIn, BufferRef external, uint32_t externalCount, RenderGraph& g, ShaderLibrary& shaders,
                                    const QualityConfig& quality, const VolumeFrame& f)
{
    VolumeOutput out;
    const bool media = f.media && f.froxelLights.valid();
    if (!f.view || (!media && !f.haze)) return out;
    fx::ParticleRenderInputs in;
    if (particlesIn) in = *particlesIn;
    else in.threads = externalCount;
    const uint32_t width = f.view->width, height = f.view->height;
    froxelGridSize(quality, width, height, out.gridX, out.gridY, out.slices, out.tilePx, f.mainHeight);
    const uint32_t mediaTiles = listCells(out.gridX, out.gridY);  // media list cells (loose quadtree, VolumeCommon.hlsli)
    out.hazeWidth = (width + kHazeScale - 1) / kHazeScale;
    out.hazeHeight = (height + kHazeScale - 1) / kHazeScale;
    const uint32_t hazeTilesX = (out.hazeWidth + kHazeTile - 1) / kHazeTile, hazeTilesY = (out.hazeHeight + kHazeTile - 1) / kHazeTile;
    const uint32_t hazeTiles = listCells(hazeTilesX, hazeTilesY);  // haze list cells
    const uint32_t threads = in.threads;
    // Entry buffers: a record has at most K^2 entries (its loose-quadtree cells), so K^2 per render thread is exact - the
    // overflow status can only report a defect, never a load.
    out.mediaEntryCapacity = std::max<uint32_t>(threads * kLooseSpan * kLooseSpan, 1u);
    out.hazeEntryCapacity = std::max<uint32_t>(threads * kLooseSpan * kLooseSpan, 1u);
    const double w = in.dt > 0 ? std::clamp((f.time - (in.tickTime - in.dt)) / in.dt, 0.0, 1.0) : 1.0;
    out.valid = true;
    out.threads = threads;
    out.w = (float)w;
    out.constants = g.createBuffer(BufferDesc{ "volume.constants", kConstantSlotBytes, (uint32_t)sizeof(VolumeConstants) });
    out.records = external.valid() ? external : g.createBuffer(BufferDesc{ "volume.records", (uint64_t)std::max<uint32_t>(threads, 1) * kRecordBytes, kRecordBytes });
    out.counters = g.createBuffer(BufferDesc{ "volume.counters", 32, 4 });
    out.mediaCounts = g.createBuffer(BufferDesc{ "volume.media.counts", (uint64_t)mediaTiles * 4, 4 });
    out.mediaStarts = g.createBuffer(BufferDesc{ "volume.media.starts", (uint64_t)mediaTiles * 4, 4 });
    const BufferRef mediaFill = g.createBuffer(BufferDesc{ "volume.media.fill", (uint64_t)mediaTiles * 4, 4 });
    out.mediaEntries = g.createBuffer(BufferDesc{ "volume.media.entries", (uint64_t)out.mediaEntryCapacity * 16, 16 });
    out.hazeCounts = g.createBuffer(BufferDesc{ "volume.haze.counts", (uint64_t)hazeTiles * 4, 4 });
    out.hazeStarts = g.createBuffer(BufferDesc{ "volume.haze.starts", (uint64_t)hazeTiles * 4, 4 });
    const BufferRef hazeFill = g.createBuffer(BufferDesc{ "volume.haze.fill", (uint64_t)hazeTiles * 4, 4 });
    out.hazeEntries = g.createBuffer(BufferDesc{ "volume.haze.entries", (uint64_t)out.hazeEntryCapacity * 16, 16 });
    if (media) out.volumeSlices = g.createTexture(TextureDesc{ "volume.slices", out.gridX, out.gridY, (uint16_t)(2 * out.slices), 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D });
    if (f.haze)
    {
        out.distortionOffset = g.createTexture(TextureDesc{ "volume.distortionOffset", out.hazeWidth, out.hazeHeight, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
        out.distortionDepth = g.createTexture(TextureDesc{ "volume.distortionDepth", out.hazeWidth, out.hazeHeight, 1, 1, DXGI_FORMAT_R16_FLOAT });
    }

    VolumeConstants vc{};
    for (int a = 0; a < 3; ++a)
    {
        // stream-space camera (the axis signs are their own inverse): anchor - camera, a stream-space double difference
        const double camera = f.camera[a] * f.streamAxes[a];
        vc.offsetCur[a] = (float)(in.anchor[1][a] - camera);
        vc.offsetPrev[a] = (float)(in.anchor[0][a] - camera);
        vc.streamAxes[a] = f.streamAxes[a];
    }
    vc.w = (float)w;
    vc.dt = in.dt;
    vc.threads = threads;
    vc.current = in.current;
    vc.rangeCount = in.rangeCount;
    vc.mode = (f.haze ? 1u : 0u) | (media ? 2u : 0u) | (external.valid() ? 4u : 0u);
    vc.hazeWidth = out.hazeWidth;
    vc.hazeHeight = out.hazeHeight;
    vc.hazeTilesX = hazeTilesX;
    vc.hazeTilesY = hazeTilesY;
    vc.hazeEntryCapacity = out.hazeEntryCapacity;
    vc.mediaEntryCapacity = out.mediaEntryCapacity;
    vc.mediaTiles = mediaTiles;
    vc.hazeCells = hazeTiles;
    const uint32_t slot = m_next;
    m_next = (m_next + 1) % kConstantSlots;
    uint8_t* mapped = m_mapped + (uint64_t)slot * kConstantSlotBytes;
    ID3D12Resource* upload = m_upload.Get();
    const uint64_t uploadOffset = (uint64_t)slot * kConstantSlotBytes;
    const fx::ParticleRenderInputs inputs = in;
    const VolumeOutput o = out;
    const fx::ParticleLighting L = f.lighting;
    const BufferRef froxelLights = f.froxelLights;
    g.addPass("volume.constants", QueueType::Graphics, [=](PassBuilder& b) { b.use(o.constants, Use::CopyDst); },
              [=](PassContext& c) mutable {
                  const uint32_t none = 0xFFFFFFFFu;
                  auto srvOr = [&](BufferRef b) { return b.valid() ? c.srv(b) : none; };
                  vc.posAgeCur = srvOr(inputs.posAge[1]);
                  vc.velocityCur = srvOr(inputs.velocity[1]);
                  vc.posAgePrev = srvOr(inputs.posAge[0]);
                  vc.velocityPrev = srvOr(inputs.velocity[0]);
                  vc.dynamicCur = srvOr(inputs.dynamic[1]);
                  vc.dynamicPrev = srvOr(inputs.dynamic[0]);
                  vc.emitters = srvOr(inputs.emitters);
                  vc.programs = srvOr(inputs.programs);
                  vc.curveKeys = srvOr(inputs.curveKeys);
                  vc.ranges = srvOr(inputs.renderRanges);
                  vc.blocks = srvOr(inputs.renderBlocks);
                  vc.records = c.uav(o.records);
                  vc.counters = c.uav(o.counters);
                  vc.mediaCounts = c.uav(o.mediaCounts);
                  vc.mediaStarts = c.uav(o.mediaStarts);
                  vc.mediaFill = c.uav(mediaFill);
                  vc.mediaEntries = c.uav(o.mediaEntries);
                  vc.hazeCounts = c.uav(o.hazeCounts);
                  vc.hazeStarts = c.uav(o.hazeStarts);
                  vc.hazeFill = c.uav(hazeFill);
                  vc.hazeEntries = c.uav(o.hazeEntries);
                  vc.volumeSlices = o.volumeSlices.valid() ? c.uav(o.volumeSlices) : none;
                  vc.distortionOffset = o.distortionOffset.valid() ? c.uav(o.distortionOffset) : none;
                  vc.distortionDepth = o.distortionDepth.valid() ? c.uav(o.distortionDepth) : none;
                  vc.froxelLights = froxelLights.valid() ? c.srv(froxelLights) : none;
                  const bool sun = L.vsmPageTable.valid() && (L.vsmAtlas.valid() || L.vsmPool.valid()) && L.vsmBlocks.valid() && L.vsmSearchBound.valid();
                  vc.shadow[0] = sun ? c.srv(L.vsmPageTable) : none;
                  vc.shadow[1] = sun ? (L.vsmAtlas.valid() ? c.srv(L.vsmAtlas) : c.srv(L.vsmPool)) : none;
                  vc.shadow[2] = sun ? c.srv(L.vsmBlocks) : none;
                  vc.shadow[3] = sun ? c.srv(L.vsmSearchBound) : none;
                  vc.shadow[4] = sun ? L.vsmConstants : none;
                  vc.shadow[5] = L.vsmLocalLights;
                  vc.shadow[6] = L.vsmSlotOfLight;
                  vc.shadow[7] = L.vsmLayers.valid() ? c.srv(L.vsmLayers) : none;
                  vc.giCache = giSourceWord(c, L.gi);
                  vc.airVolume = L.airVolume.valid() ? c.srv(L.airVolume) : none;
                  vc.transmittance = L.transmittanceLut.valid() ? c.srv(L.transmittanceLut) : none;
                  vc.multiScatter = L.multiScatterLut.valid() ? c.srv(L.multiScatterLut) : none;
                  std::memcpy(mapped, &vc, sizeof vc);
                  c.cmd->CopyBufferRegion(c.resource(o.constants), 0, upload, uploadOffset, sizeof vc);
              });

    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = f.frameConstants;
    // (words 4, 5 = P[1].xy: shading.mega_lights' sampled local light volumes for the setup kernel; UNX_NONE elsewhere)
    auto dispatch = [&](const char* name, const char* kernel, uint32_t gx, uint32_t gy, uint32_t list, std::function<void(PassBuilder&)> uses,
                        TextureRef volume0 = {}, TextureRef volume1 = {}) {
        if (gx == 0 || gy == 0) return;
        ID3D12PipelineState* pso = shaders.compute(kernel);
        g.addPass(name, QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(o.constants, Use::SrvCompute);
                      if (volume0.valid()) b.use(volume0, Use::SrvCompute);
                      if (volume1.valid()) b.use(volume1, Use::SrvCompute);
                      uses(b);
                  },
                  [=](PassContext& c) {
                      const bool volumes = volume0.valid() && volume1.valid();
                      const std::array<uint32_t, 8> p = { c.srv(o.constants), list, 0, 0, volumes ? c.srv(volume0) : 0xFFFFFFFFu, volumes ? c.srv(volume1) : 0xFFFFFFFFu, 0, 0 };
                      c.cmd->SetPipelineState(pso);
                      c.bindFrameConstants(frameConstants);
                      c.computeConstants(p.data(), 8);
                      gpuDispatch(c.cmd, gx, gy, 1);
                  });
    };
    const BufferRef inputBuffers[] = { in.posAge[0], in.posAge[1], in.velocity[0], in.velocity[1], in.dynamic[0], in.dynamic[1], in.emitters, in.programs,
                                       in.curveKeys, in.renderRanges, in.renderBlocks };
    const BufferRef lists[] = { o.mediaCounts, mediaFill, o.mediaStarts, o.mediaEntries, o.hazeCounts, hazeFill, o.hazeStarts, o.hazeEntries, o.counters };
    auto useLists = [=](PassBuilder& b) { for (const BufferRef& x : lists) b.use(x, Use::UavCompute); };
    dispatch("volume.clear", "Passes/Volume/VolumeSetup.STEP0", groups(std::max(mediaTiles, hazeTiles), 256), 1, 0, useLists);
    dispatch("volume.setup", "Passes/Volume/VolumeSetup.STEP1", groups(threads, 256), 1, 0, [=](PassBuilder& b) {
        for (const BufferRef& x : inputBuffers)
            if (x.valid()) b.use(x, Use::SrvCompute);
        for (const BufferRef& x : { L.vsmPageTable, L.vsmPool, L.vsmBlocks, L.vsmSearchBound, L.vsmLayers, froxelLights })
            if (x.valid()) b.use(x, Use::SrvCompute);
        declareGiSource(b, L.gi, Use::SrvCompute);
        for (const TextureRef& x : { L.vsmAtlas, L.airVolume, L.transmittanceLut, L.multiScatterLut, L.fogVolume })
            if (x.valid()) b.use(x, Use::SrvCompute);
        b.use(o.records, Use::UavCompute);
        useLists(b);
    }, L.localFluence, L.localMoment);
    // binning: one thread per (record, cell row) - at most K cell rows per record (VolumeCommon.hlsli)
    const uint32_t binRows = kLooseSpan;
    dispatch("volume.count", "Passes/Volume/VolumeSetup.STEP4", groups(threads, 256), binRows, 0, [=](PassBuilder& b) {
        b.use(o.records, Use::UavCompute);
        if (froxelLights.valid()) b.use(froxelLights, Use::SrvCompute);
        useLists(b);
    });
    if (media) dispatch("volume.media.scan", "Passes/Volume/VolumeSetup.STEP2", 1, 1, 0, useLists);
    if (f.haze) dispatch("volume.haze.scan", "Passes/Volume/VolumeSetup.STEP2", 1, 1, 1, useLists);
    dispatch("volume.scatter", "Passes/Volume/VolumeSetup.STEP3", groups(threads, 256), binRows, 0, [=](PassBuilder& b) {
        b.use(o.records, Use::UavCompute);
        if (froxelLights.valid()) b.use(froxelLights, Use::SrvCompute);
        useLists(b);
    });
    if (media)
        dispatch("volume.media.slices", "Passes/Volume/VolumeSlices", out.gridX * out.gridY, 1, 0, [=](PassBuilder& b) {
            b.use(o.records, Use::SrvCompute);
            b.use(froxelLights, Use::SrvCompute);
            for (const BufferRef& x : { o.mediaCounts, o.mediaStarts, o.mediaEntries }) b.use(x, Use::SrvCompute);
            b.use(o.counters, Use::UavCompute);
            b.use(o.volumeSlices, Use::UavCompute);
        });
    if (f.haze)
        dispatch("volume.haze.field", "Passes/Volume/VolumeDistortion", hazeTilesX, hazeTilesY, 0, [=](PassBuilder& b) {
            b.use(o.records, Use::SrvCompute);
            for (const BufferRef& x : { o.hazeCounts, o.hazeStarts, o.hazeEntries }) b.use(x, Use::SrvCompute);
            b.use(o.counters, Use::UavCompute);
            b.use(o.distortionOffset, Use::UavCompute);
            b.use(o.distortionDepth, Use::UavCompute);
        });
    return out;
}
} // namespace unx::volume
