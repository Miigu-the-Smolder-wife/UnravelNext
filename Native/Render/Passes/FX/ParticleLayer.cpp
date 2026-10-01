// Particle render pass (G7; request 20260926_FX_particle_render_pass.md). See include/unx/fx/ParticleLayer.h and
// ParticleLayerPass.hlsli.
#include "unx/fx/ParticleLayer.h"

#include "unx/core/Log.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace unx::fx
{
using namespace unx::render;

namespace
{
// Mirror of ParticleLayerPass.hlsli LayerConstants.
struct LayerConstants
{
    float offsetCur[3]; float w;
    float offsetPrev[3]; float dt;
    uint32_t threads, current, rangeCount, recordCapacity;
    uint32_t layerWidth, layerHeight, tilesX, tilesY;
    uint32_t posAgeCur, velocityCur, posAgePrev, velocityPrev;
    uint32_t dynamicCur, dynamicPrev, emitters, programs;
    uint32_t curveKeys, ranges, blocks, records;
    uint32_t tileCounts, tileStarts, tileFill, entries;
    uint32_t depth, layer, depthRange, edgeBlocks;
    uint32_t counters, entryCapacity, edgeCapacity, layerSrv;
    uint32_t edgeBlocksSrv, ribbonPoints, ribbonLinks, ribbonVertices;
    // Stage 2 (lighting, request 3): S's ShadowSrvs, R's GI cache, S's froxel lights and air volume, the atmosphere LUTs
    // of the view; UNX_NONE where the frame has none.
    uint32_t shadow[8];
    uint32_t giCache, froxelLights, airVolume, transmittance;
    uint32_t multiScatter, ribbonAppearance, ribbonCapacity, stripBase;
    uint32_t ribbonRows;
    float streamAxes[3];  // stream space -> renderer axis signs: fxParticleAt maps each camera-relative position
};
static_assert(sizeof(LayerConstants) == 256);
constexpr uint32_t kConstantSlots = 64, kConstantSlotBytes = 256;
constexpr uint32_t kLayerScale = 4, kTilePixels = 32;  // FX_LAYER_SCALE, FX_LAYER_TILE x FX_LAYER_SCALE
constexpr uint32_t kRecordBytes = 32, kEdgeBlockBytes = 128;

uint32_t groups(uint64_t n, uint32_t size) { return (uint32_t)((n + size - 1) / size); }
uint64_t align(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
} // namespace

ParticleLayerPass::ParticleLayerPass(Device& device) : m_device(device)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = (uint64_t)kConstantSlots * kConstantSlotBytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_upload)),
          "FX layer constants");
    m_upload->SetName(L"FX layer constants ring");
    D3D12_RANGE none{ 0, 0 };
    check(m_upload->Map(0, &none, reinterpret_cast<void**>(&m_mapped)), "map FX layer constants");
}

ParticleLayerPass::~ParticleLayerPass()
{
    m_device.waitIdle();
    if (m_upload && m_mapped) m_upload->Unmap(0, nullptr);
}

ParticleLayerOutput ParticleLayerPass::record(ParticleSystem& particles, RenderGraph& g, ShaderLibrary& shaders, uint64_t importIndex, const ParticleLayerFrame& f)
{
    ParticleLayerOutput out;
    const ParticleRenderInputs in = particles.renderInputs(g, importIndex);
    if (!in.valid || !f.view || !f.depth.valid()) return out;
    const uint32_t width = f.view->width, height = f.view->height;
    const uint32_t lw = (width + kLayerScale - 1) / kLayerScale, lh = (height + kLayerScale - 1) / kLayerScale;
    const uint32_t tilesX = (width + kTilePixels - 1) / kTilePixels, tilesY = (height + kTilePixels - 1) / kTilePixels, tiles = tilesX * tilesY;
    if (tiles > 1024u * 64u) fail("FX layer: %u tiles exceed the one-group scan (%u)", tiles, 1024u * 64u);
    const uint32_t threads = in.threads;
    // Ribbon points of this frame (their strips' segments are records after the sprites'): the latest tick's layout.
    const uint32_t ribbons = in.ribbonRangeCount ? in.ribbonCapacity : 0u;
    const uint32_t recordCount = threads + ribbons;
    // Tile entries: a sprite's square covers at least one tile; the buffer takes 4 per record + one per tile, and more
    // sets FX_LAYER_STATUS_ENTRY_OVERFLOW (reported; the capacity is a cost term to redesign, never a silent cap).
    const uint32_t entryCapacity = std::max<uint32_t>(recordCount * 4u + tiles, 4096u);
    // Edge blocks: a quarter of the layer pixels (overflow reported the same way).
    const uint32_t edgeCapacity = std::max<uint32_t>(lw * lh / 4u, 4096u);
    const uint64_t edgeIndexBytes = align(16u + 4ull * lw * lh, 16);

    // frame time between the previous tick's end (w = 0) and the latest tick's end (w = 1)
    const double w = in.dt > 0 ? std::clamp((f.time - (in.tickTime - in.dt)) / in.dt, 0.0, 1.0) : 1.0;

    out.valid = true;
    out.threads = threads;
    out.recordCount = recordCount;
    out.tiles = tiles;
    out.layerWidth = lw;
    out.layerHeight = lh;
    out.entryCapacity = entryCapacity;
    out.edgeCapacity = edgeCapacity;
    out.w = (float)w;
    out.constants = g.createBuffer(BufferDesc{ "fx.layer.constants", kConstantSlotBytes, (uint32_t)sizeof(LayerConstants) });
    out.records = g.createBuffer(BufferDesc{ "fx.layer.records", (uint64_t)std::max<uint32_t>(recordCount, 1) * kRecordBytes, kRecordBytes });
    // ribbons: this frame's points, their half4 appearance, FxRibbon's links, vertices (2 per point), run starts, tangents
    const uint32_t rb = std::max<uint32_t>(ribbons, 1);
    const BufferRef ribbonPoints = g.createBuffer(BufferDesc{ "fx.layer.ribbonPoints", (uint64_t)rb * 32, 32 });
    out.ribbonAppearance = g.createBuffer(BufferDesc{ "fx.layer.ribbonAppearance", (uint64_t)rb * 8, 8 });
    const BufferRef ribbonLinks = g.createBuffer(BufferDesc{ "fx.layer.ribbonLinks", (uint64_t)rb * 4, 4 });
    out.ribbonVertices = g.createBuffer(BufferDesc{ "fx.layer.ribbonVertices", (uint64_t)rb * 64, 32 });
    const BufferRef ribbonRunStart = g.createBuffer(BufferDesc{ "fx.layer.ribbonRunStart", (uint64_t)rb * 4, 4 });
    const BufferRef ribbonTangents = g.createBuffer(BufferDesc{ "fx.layer.ribbonTangents", (uint64_t)rb * 16, 16 });
    out.tileCounts = g.createBuffer(BufferDesc{ "fx.layer.tileCounts", (uint64_t)tiles * 4, 4 });
    const BufferRef tileFill = g.createBuffer(BufferDesc{ "fx.layer.tileFill", (uint64_t)tiles * 4, 4 });
    out.tileStarts = g.createBuffer(BufferDesc{ "fx.layer.tileStarts", (uint64_t)tiles * 4, 4 });
    out.entries = g.createBuffer(BufferDesc{ "fx.layer.entries", (uint64_t)entryCapacity * 8, 8 });
    out.counters = g.createBuffer(BufferDesc{ "fx.layer.counters", 16, 4 });
    out.layer = g.createTexture(TextureDesc{ "fx.layer", lw, lh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    out.depthRange = g.createTexture(TextureDesc{ "fx.layer.depthRange", lw, lh, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
    out.edges = g.createBuffer(BufferDesc{ "fx.layer.edges", edgeIndexBytes + (uint64_t)edgeCapacity * kEdgeBlockBytes, 0 });

    // constants: stream anchors relative to the camera in stream space (double differences; the axis signs are their own
    // inverse), then the views of this frame's resources
    LayerConstants lc{};
    for (int a = 0; a < 3; ++a)
    {
        const double camera = f.camera[a] * f.streamAxes[a];
        lc.offsetCur[a] = (float)(in.anchor[1][a] - camera);
        lc.offsetPrev[a] = (float)(in.anchor[0][a] - camera);
        lc.streamAxes[a] = f.streamAxes[a];
    }
    lc.w = (float)w;
    lc.dt = in.dt;
    lc.threads = threads;
    lc.current = in.current;
    lc.rangeCount = in.rangeCount;
    lc.recordCapacity = recordCount;
    lc.ribbonCapacity = ribbons;
    lc.stripBase = threads;
    lc.layerWidth = lw;
    lc.layerHeight = lh;
    lc.tilesX = tilesX;
    lc.tilesY = tilesY;
    lc.entryCapacity = entryCapacity;
    lc.edgeCapacity = edgeCapacity;
    const uint32_t slot = m_next;
    m_next = (m_next + 1) % kConstantSlots;
    uint8_t* mapped = m_mapped + (uint64_t)slot * kConstantSlotBytes;
    ID3D12Resource* upload = m_upload.Get();
    const uint64_t uploadOffset = (uint64_t)slot * kConstantSlotBytes;
    const ParticleRenderInputs inputs = in;
    const BufferRef ribbonRowsRef = ribbons ? in.ribbonRows : BufferRef{};
    const ParticleLayerOutput o = out;
    g.addPass("fx.layer.constants", QueueType::Graphics, [=](PassBuilder& b) { b.use(o.constants, Use::CopyDst); },
              [=](PassContext& c) mutable {
                  lc.posAgeCur = c.srv(inputs.posAge[1]);
                  lc.velocityCur = c.srv(inputs.velocity[1]);
                  lc.posAgePrev = c.srv(inputs.posAge[0]);
                  lc.velocityPrev = c.srv(inputs.velocity[0]);
                  lc.dynamicCur = c.srv(inputs.dynamic[1]);
                  lc.dynamicPrev = c.srv(inputs.dynamic[0]);
                  lc.emitters = c.srv(inputs.emitters);
                  lc.programs = c.srv(inputs.programs);
                  lc.curveKeys = c.srv(inputs.curveKeys);
                  lc.ranges = c.srv(inputs.renderRanges);
                  lc.blocks = c.srv(inputs.renderBlocks);
                  lc.records = c.uav(o.records);
                  lc.tileCounts = c.uav(o.tileCounts);
                  lc.tileStarts = c.uav(o.tileStarts);
                  lc.tileFill = c.uav(tileFill);
                  lc.entries = c.uav(o.entries);
                  lc.depth = c.srv(f.depth);
                  lc.layer = c.uav(o.layer);
                  lc.depthRange = c.uav(o.depthRange);
                  lc.edgeBlocks = c.uav(o.edges);
                  lc.counters = c.uav(o.counters);
                  lc.layerSrv = c.srv(o.layer);
                  lc.edgeBlocksSrv = c.srv(o.edges);
                  lc.ribbonPoints = c.uav(ribbonPoints);
                  lc.ribbonLinks = c.uav(ribbonLinks);
                  lc.ribbonVertices = c.uav(o.ribbonVertices);
                  lc.ribbonAppearance = c.uav(o.ribbonAppearance);
                  lc.ribbonRows = ribbonRowsRef.valid() ? c.srv(ribbonRowsRef) : 0xFFFFFFFFu;
                  const ParticleLighting& L = f.lighting;
                  const uint32_t none = 0xFFFFFFFFu;
                  const bool sun = L.vsmPageTable.valid() && (L.vsmAtlas.valid() || L.vsmPool.valid()) && L.vsmBlocks.valid() && L.vsmSearchBound.valid();
                  lc.shadow[0] = sun ? c.srv(L.vsmPageTable) : none;
                  lc.shadow[1] = sun ? (L.vsmAtlas.valid() ? c.srv(L.vsmAtlas) : c.srv(L.vsmPool)) : none;
                  lc.shadow[2] = sun ? c.srv(L.vsmBlocks) : none;
                  lc.shadow[3] = sun ? c.srv(L.vsmSearchBound) : none;
                  lc.shadow[4] = sun ? L.vsmConstants : none;
                  lc.shadow[5] = L.vsmLocalLights;
                  lc.shadow[6] = L.vsmSlotOfLight;
                  lc.shadow[7] = L.vsmLayers.valid() ? c.srv(L.vsmLayers) : none;
                  lc.giCache = L.giCache.valid() ? c.srv(L.giCache) : none;
                  lc.froxelLights = L.froxelLights.valid() ? c.srv(L.froxelLights) : none;
                  lc.airVolume = L.airVolume.valid() ? c.srv(L.airVolume) : none;
                  lc.transmittance = L.transmittanceLut.valid() ? c.srv(L.transmittanceLut) : none;
                  lc.multiScatter = L.multiScatterLut.valid() ? c.srv(L.multiScatterLut) : none;
                  std::memcpy(mapped, &lc, sizeof lc);
                  c.cmd->CopyBufferRegion(c.resource(o.constants), 0, upload, uploadOffset, sizeof lc);
              });

    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = f.frameConstants;
    // (words 4, 5 = P[1].xy: the setup kernel's sampled local light volumes - shading.mega_lights; UNX_NONE elsewhere)
    auto dispatch = [&](const char* name, const char* kernel, uint32_t groupCount, std::function<void(PassBuilder&)> uses, TextureRef volume0 = {}, TextureRef volume1 = {}) {
        if (groupCount == 0) return;
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
                      const std::array<uint32_t, 8> p = { c.srv(o.constants), 0, 0, 0, volumes ? c.srv(volume0) : 0xFFFFFFFFu, volumes ? c.srv(volume1) : 0xFFFFFFFFu, 0, 0 };
                      c.cmd->SetPipelineState(pso);
                      c.bindFrameConstants(frameConstants);
                      c.computeConstants(p.data(), 8);
                      c.cmd->Dispatch(groupCount, 1, 1);
                  });
    };
    const BufferRef inputBuffers[] = { in.posAge[0], in.posAge[1], in.velocity[0], in.velocity[1], in.dynamic[0], in.dynamic[1], in.emitters, in.programs,
                                       in.curveKeys, in.renderRanges, in.renderBlocks, ribbons ? in.ribbonRows : BufferRef{} };
    dispatch("fx.layer.clear", "Passes/FX/FxLayerScan.STEP0", groups(tiles, 1024), [=](PassBuilder& b) {
        b.use(o.tileCounts, Use::UavCompute);
        b.use(tileFill, Use::UavCompute);
        b.use(o.counters, Use::UavCompute);
        b.use(o.edges, Use::UavCompute);
    });
    const ParticleLighting lighting = f.lighting;
    // (ML1: the local lights from shading.mega_lights' sampled volumes, P[1].xy; ML0: the loop over the froxel list)
    const bool sampledLocal = lighting.localFluence.valid() && lighting.localMoment.valid();
    dispatch("fx.layer.setup", sampledLocal ? "Passes/FX/FxLayerSetup.STEP0.ML1" : "Passes/FX/FxLayerSetup.STEP0.ML0", groups(threads, 256), [=](PassBuilder& b) {
        for (const BufferRef& x : inputBuffers)
            if (x.valid()) b.use(x, Use::SrvCompute);
        for (const BufferRef& x : { lighting.vsmPageTable, lighting.vsmPool, lighting.vsmBlocks, lighting.vsmSearchBound, lighting.vsmLayers, lighting.giCache, lighting.froxelLights, lighting.fxLights })
            if (x.valid()) b.use(x, Use::SrvCompute);
        for (const TextureRef& x : { lighting.vsmAtlas, lighting.airVolume, lighting.transmittanceLut, lighting.multiScatterLut })
            if (x.valid()) b.use(x, Use::SrvCompute);
        b.use(o.records, Use::UavCompute);
        b.use(o.tileCounts, Use::UavCompute);
        b.use(o.counters, Use::UavCompute);
        b.use(ribbonPoints, Use::UavCompute);
        b.use(o.ribbonAppearance, Use::UavCompute);
    }, lighting.localFluence, lighting.localMoment);
    if (ribbons)
    {
        // the strips of this frame's points (FxRibbon, the simulation's kernel: same geometry rules), then their segments
        dispatch("fx.layer.ribbon.clear", "Passes/FX/FxLayerStrips.STEP0", groups(ribbons, 256), [=](PassBuilder& b) { b.use(ribbonLinks, Use::UavCompute); });
        ID3D12PipelineState* ribbonPso = shaders.compute("Passes/FX/FxRibbon");
        const BufferRef ranges = in.ribbonRanges, programs = in.programs;  // (the rows table is the setup's input)
        const uint32_t rangeCount = in.ribbonRangeCount;
        const std::array<float, 3> streamAxes = { f.streamAxes[0], f.streamAxes[1], f.streamAxes[2] };
        g.addPass("fx.layer.ribbon", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      for (const BufferRef& x : { ribbonPoints, ribbonLinks, o.ribbonVertices, ribbonRunStart, ribbonTangents }) b.use(x, Use::UavCompute);
                      b.use(ranges, Use::SrvCompute);
                      b.use(programs, Use::SrvCompute);
                  },
                  [=](PassContext& c) {
                      uint32_t axes[3];
                      std::memcpy(axes, streamAxes.data(), sizeof axes);  // the render pass's points are in renderer axes
                      const std::array<uint32_t, 12> p = { c.uav(ribbonPoints), c.uav(ribbonLinks), c.uav(o.ribbonVertices), c.srv(ranges),
                                                           c.uav(ribbonRunStart), c.uav(ribbonTangents), rangeCount, c.srv(programs), axes[0], axes[1], axes[2], 0 };
                      c.cmd->SetPipelineState(ribbonPso);
                      c.bindFrameConstants(frameConstants);
                      c.computeConstants(p.data(), 12);
                      c.cmd->Dispatch(rangeCount, 1, 1);
                  });
        dispatch("fx.layer.strips", "Passes/FX/FxLayerStrips.STEP1", groups(ribbons, 256), [=](PassBuilder& b) {
            b.use(ribbonLinks, Use::UavCompute);
            b.use(o.ribbonVertices, Use::UavCompute);
            b.use(o.records, Use::UavCompute);
            b.use(o.tileCounts, Use::UavCompute);
            b.use(o.counters, Use::UavCompute);
        });
    }
    dispatch("fx.layer.scan", "Passes/FX/FxLayerScan.STEP1", 1, [=](PassBuilder& b) {
        b.use(o.tileCounts, Use::UavCompute);
        b.use(o.tileStarts, Use::UavCompute);
        b.use(o.counters, Use::UavCompute);
    });
    dispatch("fx.layer.scatter", "Passes/FX/FxLayerSetup.STEP1.ML0", groups(recordCount, 256), [=](PassBuilder& b) {
        b.use(o.records, Use::UavCompute);
        b.use(tileFill, Use::UavCompute);
        b.use(o.tileStarts, Use::UavCompute);
        b.use(o.entries, Use::UavCompute);
        b.use(o.counters, Use::UavCompute);
    });
    dispatch("fx.layer.tiles", "Passes/FX/FxLayerTile", tiles, [=](PassBuilder& b) {
        b.use(o.records, Use::UavCompute);
        b.use(o.tileCounts, Use::UavCompute);
        b.use(o.tileStarts, Use::UavCompute);
        b.use(o.entries, Use::UavCompute);
        b.use(o.counters, Use::UavCompute);
        b.use(f.depth, Use::SrvCompute);
        b.use(o.layer, Use::UavCompute);
        b.use(o.depthRange, Use::UavCompute);
        b.use(o.edges, Use::UavCompute);
        b.use(o.ribbonVertices, Use::UavCompute);
        b.use(o.ribbonAppearance, Use::UavCompute);
        for (const TextureRef& x : { lighting.airVolume, lighting.transmittanceLut, lighting.multiScatterLut })
            if (x.valid()) b.use(x, Use::SrvCompute);
    });
    return out;
}
} // namespace unx::fx
