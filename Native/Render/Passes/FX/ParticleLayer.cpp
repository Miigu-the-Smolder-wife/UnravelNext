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
    uint32_t edgeBlocksSrv, pad0, pad1, pad2;
    // Stage 2 (lighting, request 3): S's ShadowSrvs, R's GI cache, S's froxel lights and air volume, the atmosphere LUTs
    // of the view; UNX_NONE where the frame has none.
    uint32_t shadow[8];
    uint32_t giCache, froxelLights, airVolume, transmittance;
    uint32_t multiScatter, pad3, pad4, pad5;
};
static_assert(sizeof(LayerConstants) == 240);
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
    // Tile entries: a sprite's square covers at least one tile; the buffer takes 4 per particle + one per tile, and more
    // sets FX_LAYER_STATUS_ENTRY_OVERFLOW (reported; the capacity is a cost term to redesign, never a silent cap).
    const uint32_t entryCapacity = std::max<uint32_t>(threads * 4u + tiles, 4096u);
    // Edge blocks: a quarter of the layer pixels (overflow reported the same way).
    const uint32_t edgeCapacity = std::max<uint32_t>(lw * lh / 4u, 4096u);
    const uint64_t edgeIndexBytes = align(16u + 4ull * lw * lh, 16);

    // frame time between the previous tick's end (w = 0) and the latest tick's end (w = 1)
    const double w = in.dt > 0 ? std::clamp((f.time - (in.tickTime - in.dt)) / in.dt, 0.0, 1.0) : 1.0;

    out.valid = true;
    out.threads = threads;
    out.tiles = tiles;
    out.layerWidth = lw;
    out.layerHeight = lh;
    out.entryCapacity = entryCapacity;
    out.edgeCapacity = edgeCapacity;
    out.w = (float)w;
    out.constants = g.createBuffer(BufferDesc{ "fx.layer.constants", kConstantSlotBytes, (uint32_t)sizeof(LayerConstants) });
    out.records = g.createBuffer(BufferDesc{ "fx.layer.records", (uint64_t)std::max<uint32_t>(threads, 1) * kRecordBytes, kRecordBytes });
    out.tileCounts = g.createBuffer(BufferDesc{ "fx.layer.tileCounts", (uint64_t)tiles * 4, 4 });
    const BufferRef tileFill = g.createBuffer(BufferDesc{ "fx.layer.tileFill", (uint64_t)tiles * 4, 4 });
    out.tileStarts = g.createBuffer(BufferDesc{ "fx.layer.tileStarts", (uint64_t)tiles * 4, 4 });
    out.entries = g.createBuffer(BufferDesc{ "fx.layer.entries", (uint64_t)entryCapacity * 8, 8 });
    out.counters = g.createBuffer(BufferDesc{ "fx.layer.counters", 16, 4 });
    out.layer = g.createTexture(TextureDesc{ "fx.layer", lw, lh, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    out.depthRange = g.createTexture(TextureDesc{ "fx.layer.depthRange", lw, lh, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
    out.edges = g.createBuffer(BufferDesc{ "fx.layer.edges", edgeIndexBytes + (uint64_t)edgeCapacity * kEdgeBlockBytes, 0 });

    // constants: stream anchors relative to the camera (double differences), then the views of this frame's resources
    LayerConstants lc{};
    for (int a = 0; a < 3; ++a)
    {
        lc.offsetCur[a] = (float)(in.anchor[1][a] - f.camera[a]);
        lc.offsetPrev[a] = (float)(in.anchor[0][a] - f.camera[a]);
    }
    lc.w = (float)w;
    lc.dt = in.dt;
    lc.threads = threads;
    lc.current = in.current;
    lc.rangeCount = in.rangeCount;
    lc.recordCapacity = std::max<uint32_t>(threads, 1);
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
    auto dispatch = [&](const char* name, const char* kernel, uint32_t groupCount, std::function<void(PassBuilder&)> uses) {
        if (groupCount == 0) return;
        ID3D12PipelineState* pso = shaders.compute(kernel);
        g.addPass(name, QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(o.constants, Use::SrvCompute);
                      uses(b);
                  },
                  [=](PassContext& c) {
                      const std::array<uint32_t, 8> p = { c.srv(o.constants), 0, 0, 0, 0, 0, 0, 0 };
                      c.cmd->SetPipelineState(pso);
                      c.bindFrameConstants(frameConstants);
                      c.computeConstants(p.data(), 8);
                      c.cmd->Dispatch(groupCount, 1, 1);
                  });
    };
    const BufferRef inputBuffers[] = { in.posAge[0], in.posAge[1], in.velocity[0], in.velocity[1], in.dynamic[0], in.dynamic[1], in.emitters, in.programs,
                                       in.curveKeys, in.renderRanges, in.renderBlocks };
    dispatch("fx.layer.clear", "Passes/FX/FxLayerScan.STEP0", groups(tiles, 1024), [=](PassBuilder& b) {
        b.use(o.tileCounts, Use::UavCompute);
        b.use(tileFill, Use::UavCompute);
        b.use(o.counters, Use::UavCompute);
        b.use(o.edges, Use::UavCompute);
    });
    const ParticleLighting lighting = f.lighting;
    dispatch("fx.layer.setup", "Passes/FX/FxLayerSetup.STEP0", groups(threads, 256), [=](PassBuilder& b) {
        for (const BufferRef& x : inputBuffers) b.use(x, Use::SrvCompute);
        for (const BufferRef& x : { lighting.vsmPageTable, lighting.vsmPool, lighting.vsmBlocks, lighting.vsmSearchBound, lighting.vsmLayers, lighting.giCache, lighting.froxelLights })
            if (x.valid()) b.use(x, Use::SrvCompute);
        for (const TextureRef& x : { lighting.vsmAtlas, lighting.airVolume, lighting.transmittanceLut, lighting.multiScatterLut })
            if (x.valid()) b.use(x, Use::SrvCompute);
        b.use(o.records, Use::UavCompute);
        b.use(o.tileCounts, Use::UavCompute);
        b.use(o.counters, Use::UavCompute);
    });
    dispatch("fx.layer.scan", "Passes/FX/FxLayerScan.STEP1", 1, [=](PassBuilder& b) {
        b.use(o.tileCounts, Use::UavCompute);
        b.use(o.tileStarts, Use::UavCompute);
        b.use(o.counters, Use::UavCompute);
    });
    dispatch("fx.layer.scatter", "Passes/FX/FxLayerSetup.STEP1", groups(threads, 256), [=](PassBuilder& b) {
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
    });
    return out;
}
} // namespace unx::fx
