#include "unx/shading/ShadingSystem.h"
#include "unx/shading/Exposure.h"
#include "unx/shading/MotionBlur.h"
#include "unx/shading/Post.h"

#include "unx/core/Log.h"
#include "unx/material/MaterialSystem.h"
#include "unx/render/GpuScene.h"
#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::render::shading
{
namespace
{
namespace model = scene::model;

#include "LtcTable.inl"  // kLtcTable

// A table uploaded once as a StructuredBuffer (the f0-split specular albedo LUT, the LTC table).
struct StaticTable
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> buffer;
    uint32_t srv = gpu::kNone;
    ~StaticTable()
    {
        if (!device) return;
        device->deferRelease(buffer);
        DescriptorHeaps* h = &device->descriptors();
        const uint32_t s = srv;
        device->deferCall([h, s] { h->freeResource(s); });
    }
    void ensure(Device& d, const std::vector<float>& t, uint32_t floatsPerElement, const wchar_t* name)
    {
        if (buffer) return;
        device = &d;
        const uint64_t bytes = t.size() * sizeof(float);
        D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = bytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d.d3d()->CreateCommittedResource3(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&buffer)), "M table");
        buffer->SetName(name);
        ComPtr<ID3D12Resource> staging;
        check(d.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)), "M LUT staging");
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(staging->Map(0, &none, &p), "map M LUT staging");
        std::memcpy(p, t.data(), bytes);
        staging->Unmap(0, nullptr);
        CommandList cl = d.acquireCommandList(QueueType::Graphics);
        cl.list->CopyBufferRegion(buffer.Get(), 0, staging.Get(), 0, bytes);
        const uint64_t fence = d.submit(cl);
        d.queue(QueueType::Graphics).waitCpu(fence);
        srv = d.descriptors().allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_UNKNOWN;
        sd.Buffer.NumElements = (UINT)(t.size() / floatsPerElement);
        sd.Buffer.StructureByteStride = 4 * floatsPerElement;
        d.d3d()->CreateShaderResourceView(buffer.Get(), &sd, d.descriptors().resourceCpu(srv));
    }
};

// This frame's ShadowSrvs (ShadowVisibility.hlsli) for the fallback shading kernel, written at the pass's execution into
// an upload ring of kFrames frames x kViews views (frame f - kFrames has completed: frames in flight <= kFrames).
struct ShadowSrvRing
{
    static constexpr uint32_t kFrames = 4, kViews = 8, kSlots = kFrames * kViews;
    Device* device = nullptr;
    ComPtr<ID3D12Resource> buffer;
    uint8_t* mapped = nullptr;
    uint32_t srv[kSlots] = {};
    ~ShadowSrvRing()
    {
        if (!device) return;
        device->deferRelease(buffer);
        DescriptorHeaps* h = &device->descriptors();
        for (uint32_t s : srv) device->deferCall([h, s] { h->freeResource(s); });
    }
    void ensure(Device& d)
    {
        if (buffer) return;
        device = &d;
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kSlots * 32;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&buffer)), "M ShadowSrvs ring");
        buffer->SetName(L"M ShadowSrvs ring");
        D3D12_RANGE none{ 0, 0 };
        check(buffer->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map M ShadowSrvs ring");
        for (uint32_t k = 0; k < kSlots; ++k)
        {
            srv[k] = d.descriptors().allocateResource();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.Buffer.FirstElement = k * 8;
            sd.Buffer.NumElements = 8;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            d.d3d()->CreateShaderResourceView(buffer.Get(), &sd, d.descriptors().resourceCpu(srv[k]));
        }
    }
    // Slot of the frame's next view (views per frame <= kViews).
    uint32_t claim(uint64_t frame)
    {
        if (frame != lastFrame)
        {
            lastFrame = frame;
            views = 0;
        }
        if (views >= kViews) fail("M ShadowSrvs ring: more than %u views with shadow overflow fallback in one frame", kViews);
        return (uint32_t)(frame % kFrames) * kViews + views++;
    }
    uint32_t write(uint32_t slot, const uint32_t (&words)[8])
    {
        std::memcpy(mapped + slot * 32, words, sizeof words);
        return srv[slot];
    }
    uint64_t lastFrame = UINT64_MAX;
    uint32_t views = 0;
};

void checkQuality(const QualityConfig& q)
{
    if (q.string("shading.tonemap") != "pbr_neutral") fail("shading.tonemap: only \"pbr_neutral\" (INTERFACES 8.4) is implemented");
    if (q.string("shading.output_encoding") != "srgb") fail("shading.output_encoding: only \"srgb\" (INTERFACES 7.5) is implemented");
    (void)q.integer("shading.analytic_lights_max");  // S reads it for the froxel lists (INTERFACES 9); local lights come with them
}

// Edge (E) detection and composite (Edge.hlsli, EdgeComposite.hlsl), shading.edge_* keys.
struct EdgeConfig
{
    float cosAngle, footprintTolerance, distanceTolerance;
    uint32_t groupsMax;
};
EdgeConfig edgeConfig(const QualityConfig& q)
{
    EdgeConfig e;
    const double angle = q.number("shading.edge_normal_angle_deg");
    if (!(angle > 0 && angle < 90)) fail("shading.edge_normal_angle_deg must be in (0, 90)");
    e.cosAngle = (float)std::cos(angle * 3.14159265358979 / 180);
    e.footprintTolerance = (float)q.number("shading.edge_footprint_tolerance");
    e.distanceTolerance = (float)q.number("shading.edge_distance_tolerance");
    e.groupsMax = (uint32_t)q.integer("shading.edge_groups_max");
    if (e.groupsMax < 2 || e.groupsMax > 3) fail("shading.edge_groups_max must be 2 or 3 (EdgeComposite.hlsl)");
    return e;
}
uint32_t asUint(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

// Readback ring of the main view's tile counts (4 slots: never reused while the harness keeps <= 2 frames in flight).
struct StatsRing
{
    static constexpr uint32_t kSlots = 4, kBytes = 64;
    Device* device = nullptr;
    ComPtr<ID3D12Resource> buffer;
    uint8_t* mapped = nullptr;
    uint64_t frame[kSlots] = { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX };
    uint32_t tiles[kSlots] = {};
    bool coverage[kSlots] = {};  // the frame ran the coverage composite (its error word at byte 20 is this frame's)
    uint64_t last = UINT64_MAX;
    ~StatsRing()
    {
        if (buffer) buffer->Unmap(0, nullptr);
        if (device) device->deferRelease(buffer);
    }
    void ensure(Device& d)
    {
        if (buffer) return;
        device = &d;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kSlots * kBytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&buffer)),
              "M stats ring");
        D3D12_RANGE all{ 0, kSlots * kBytes };
        check(buffer->Map(0, &all, reinterpret_cast<void**>(&mapped)), "map M stats ring");
    }
};

uint32_t experimentMask(const QualityConfig& q)
{
    const int64_t m = q.integer("shading.experiment_disable");
    static bool logged = false;
    if (m != 0 && !logged)
    {
        logf("M shading: experiment mask %lld leaves terms out (cost attribution run, not an image)\n", (long long)m);
        logged = true;
    }
    return (uint32_t)m;
}
} // namespace

Stats latestStats(TrackState& state)
{
    StatsRing& ring = state.get<StatsRing>("M.statsRing");
    Stats st;
    if (ring.last == UINT64_MAX || !ring.mapped) return st;
    const uint32_t slot = (uint32_t)(ring.last % StatsRing::kSlots);
    uint32_t w[16];
    std::memcpy(w, ring.mapped + slot * StatsRing::kBytes, sizeof w);
    st.frameIndex = ring.frame[slot];
    for (uint32_t c = 0; c < 4; ++c) st.classTiles[c] = w[c];
    st.edgePixels = w[4];
    st.coverageErrors = ring.coverage[slot] ? w[5] : 0;
    st.tiles = ring.tiles[slot];
    return st;
}

const std::vector<float>& specularAlbedoTable() { return scene::model::specularAlbedoTable(); }

const std::vector<float>& ltcTable()
{
    static const std::vector<float> t(std::begin(kLtcTable), std::end(kLtcTable));
    return t;
}

namespace
{
// What the banded part creates for the composite part of the same view in this frame (keyed like the resolve's table).
struct ShadingResources
{
    TextureRef edgeRadiance, edgeTiles;
    BufferRef edgePixels, edgeArgs, fallbackArgs;
};
struct ShadingTable
{
    uint64_t frame = UINT64_MAX;
    std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, ShadingResources>> views;
};

enum class Part
{
    Banded,     // ShadeBegin (before the group) and the banded passes: edge detection, shading
    Composite,  // after the group: overflow fallback tiles, statistics, edge composite
};

std::vector<RenderGraph::BandedPass> record(FramePassContext& fc, ViewResources& view, Part part)
{
    if (!view.color.valid()) fail("M.shading: the view has no colour target");
    // Planar reflection views get S's own froxel lists and air volume (v1.22, FroxelSystem recordPlanarFroxels). Without
    // them while the frame's main view has lists, reflections would silently lose local lights and air.
    if (view.view.kind != gpu::ViewKind::Main && fc.resources.froxelLights.valid() && !view.froxelLights.valid())
        fail("M.shading: planar view without S's froxel lists while the main view has them (INTERFACES v1.22)");
    if (view.view.kind != gpu::ViewKind::Main && view.froxelLights.valid() && fc.resources.transmittanceLut.valid() && !view.airVolume.valid())
        fail("M.shading: planar view with S's froxel lists but without its air volume (INTERFACES v1.22)");
    // The shading kernel reads R's screen probes through the tile cache, whose K-path radiance comes from the atlas.
    if (view.screenProbes.valid() && !view.screenProbeMaps.valid()) fail("M.shading: R's screen probes without their K-path atlas (screenProbeMaps, v1.13)");
    checkQuality(fc.quality);
    const material::ResolveOutputs& o = material::resolveOutputs(fc, view);
    StaticTable& ltc = fc.state<StaticTable>("M.ltcTable");
    ltc.ensure(fc.device, ltcTable(), 4, L"M LTC table");
    ID3D12CommandSignature* signature = material::dispatchSignature(fc);

    // Linear writers (exposed radiance in a float target): secondary views, validation frames, and the main view shaded
    // into the post chain's HDR target (Post.cpp).
    const bool linear = view.view.kind != gpu::ViewKind::Main || fc.frame.outputLinearHdr || postActive(fc, view) || motionBlurActive(fc, view) || distortionActive(fc, view);
    // Area-light code only in scenes with area lights (ShadeOpaque AREA variant: exact either way, fewer registers without).
    bool areaLights = false;
    if (const scene::Scene* src = fc.scene.source())
        for (const scene::Light& l : src->lights) areaLights = areaLights || l.type > scene::LightType::Spot;
    auto opaqueKernel = [&](bool fallbackVariant) {
        const std::string name = std::string("Passes/Shading/ShadeOpaque.OUTPUT") + (linear ? "1" : "0") + ".FALLBACK" + (fallbackVariant ? "1" : "0") +
                                 ".AREA" + (areaLights ? "1" : "0") + ".PLANAR" + (view.view.kind != gpu::ViewKind::Main ? "1" : "0");
        return fc.shaders.compute(name.c_str());
    };
    ID3D12PipelineState* opaque = opaqueKernel(false);
    ID3D12PipelineState* sky = fc.shaders.compute(linear ? "Passes/Shading/ShadeSky.OUTPUT1" : "Passes/Shading/ShadeSky.OUTPUT0");
    const FrameResources r = fc.resources;
    const bool atmosphere = r.transmittanceLut.valid() && r.multiScatterLut.valid() && r.skyViewLut.valid();
    // This view's S products (v1.22): froxel light lists and air volume (main view: the frame's; planar views: S's own,
    // the air integrated from the mirror plane). Without them a view has no local lights and only the sun's transmittance.
    const bool air = atmosphere && view.airVolume.valid();
    // The froxel grid is the main view's: planar reflection views read neither its lists nor its air volume.
    const bool froxelLists = view.froxelLights.valid();
    const ViewResources v = view;
    // S's coverage fragment visibility (INTERFACES 7.3 v1.41): with it the fragment kernels shade fragments with S's sun
    // profile and local slots (CoverageShade.hlsli covFragmentShadow); without it they stay unshadowed.
    const bool fragmentShadows = v.shadowFragmentVisibility.valid() && v.coverageDepthRange.valid();
    // FX's particle layer of this view (tracks::particles, before shading): every output writer composites it before its
    // tone map (ShadingCommon.hlsli shParticles); two indices, UNX_NONE when the view has none.
    const bool particles = v.particleLayer.valid() && v.particleEdges.valid();
    // Automatic exposure: the main view's shading kernels fill M's luminance histogram (Exposure.cpp); read back below.
    const bool meter = v.view.kind == gpu::ViewKind::Main && part != Part::Composite;
    const ExposureHistogram histogram = meter ? exposureHistogram(fc) : ExposureHistogram{};
    auto useParticles = [=](PassBuilder& b) {
        if (!particles) return;
        b.use(v.particleLayer, Use::SrvCompute);
        b.use(v.particleEdges, Use::SrvCompute);
    };
    auto particleConstants = [=](PassContext& c, uint32_t* dst) {
        dst[0] = particles ? c.srv(v.particleLayer) : gpu::kNone;
        dst[1] = particles ? c.srv(v.particleEdges) : gpu::kNone;
    };
    const uint32_t tileCount = o.tilesX * o.tilesY, ltcSrv = ltc.srv, experiment = experimentMask(fc.quality);
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const EdgeConfig ec = edgeConfig(fc.quality);
    const bool planar = view.view.kind != gpu::ViewKind::Main;
    // V's coverage layer (INTERFACES 7.1 v1.41; main view): its fragments are composited over band A
    // (CoverageComposite.hlsl, design COVERAGE_REDESIGN 4.5). The fragments' shadows come from S's fragment visibility
    // (4.3, fragmentShadows): a frame with S's shadows but without it would leave fragments unshadowed, so it fails
    // (shading bit 8192 allows it for cost attribution, never an image).
    const bool coverage = !planar && v.coverageTiles.valid() && v.coverageRecords.valid() && v.coverageTileList.valid() && v.coverageTilePixels.valid();
    if (coverage && v.shadowVisibility.valid() && !fragmentShadows && (experiment & 8192) == 0)
        fail("M.shading: V's coverage layer with S's shadows but without S's fragment visibility (COVERAGE_REDESIGN 4.3): its fragments would be unshadowed");
    // S's shadow overflow (INTERFACES 7.3, v1.20; main view): the list the main kernel loads, and the fallback tiles over
    // its capacity, shaded by the fallback kernel with S's VSM.
    const bool overflow = v.shadowOverflowTiles.valid() && v.shadowOverflow.valid();
    const bool fallback = v.shadowOverflowFallbackTiles.valid();
    const bool vsm = r.vsmPageTable.valid() && (r.vsmAtlas.valid() || r.vsmPool.valid()) && r.vsmBlocks.valid() && r.vsmSearchBound.valid() && r.vsmConstants != gpu::kNone &&
                     r.vsmLocalLights != gpu::kNone && r.vsmSlotOfLight != gpu::kNone;
    // Edge pixels' exposed linear radiance, the edge tile masks, the edge pixel list and its dispatch arguments, and the
    // fallback kernel's dispatch arguments (M internal, this view): made by the banded part, used by both.
    ShadingTable& table = fc.state<ShadingTable>("M.shadingViews");
    if (table.frame != fc.frame.frameIndex)
    {
        table.frame = fc.frame.frameIndex;
        table.views.clear();
    }
    ShadingResources res;
    if (part == Part::Banded)
    {
        res.fallbackArgs = fallback ? fc.graph.createBuffer({ "m.shade fallback args", 12, 0 }) : BufferRef{};
        res.edgeRadiance = fc.graph.createTexture({ "m.edge radiance", v.view.width, v.view.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        res.edgePixels = fc.graph.createBuffer({ "m.edge pixels", ((uint64_t)v.view.width * v.view.height + 1) * 4, 0 });
        res.edgeArgs = fc.graph.createBuffer({ "m.edge args", 24, 0 });  // dispatch args + pixel count (Edge.hlsli)
        res.edgeTiles = fc.graph.createTexture({ "m.edge tile mask", o.tilesX, o.tilesY, 1, 1, DXGI_FORMAT_R32G32_UINT });  // 64 bits per tile
        table.views.push_back({ view.frameConstants, res });
    }
    else
    {
        bool found = false;
        for (const auto& [key, rs] : table.views)
            if (key == view.frameConstants)
            {
                res = rs;
                found = true;
            }
        if (!found) fail("M.shading: composite part without the banded part for this view in frame %llu", (unsigned long long)fc.frame.frameIndex);
    }
    const BufferRef fallbackArgs = res.fallbackArgs, edgePixels = res.edgePixels, edgeArgs = res.edgeArgs;
    const TextureRef edgeRadiance = res.edgeRadiance, edgeTiles = res.edgeTiles;
    // Planar views: tiles without mirror pixels are never shaded; edge detection and the composite treat their pixels as
    // outside the view (R always gives the tile mask with the pixel mask, v1.22).
    const TextureRef planarTiles = v.view.planarTileMask;
    if (v.view.planarMask.valid() && !planarTiles.valid()) fail("M.shading: a planar view has a pixel mask without its tile mask");
    if (part == Part::Banded)
    {
        ID3D12PipelineState* begin = fc.shaders.compute("Passes/Shading/ShadeBegin");
        fc.graph.addPass(planar ? "m.shade.begin.planar" : "m.shade.begin", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(edgeArgs, Use::UavCompute);
                             b.use(o.tileArgs, Use::UavCompute);
                             if (fallback)
                             {
                                 b.use(v.shadowOverflowFallbackTiles, Use::SrvCompute);
                                 b.use(fallbackArgs, Use::UavCompute);
                             }
                         },
                         [begin, edgeArgs, tileArgs = o.tileArgs, bands = o.bands, tilesX = o.tilesX, height = o.height, fallback, fallbackList = v.shadowOverflowFallbackTiles,
                          fallbackArgs](PassContext& c) {
                             const uint32_t k[8] = { c.uav(edgeArgs), c.uav(tileArgs), (uint32_t)material::ShadeClass::Count, bands,
                                                     fallback ? c.srv(fallbackList) : gpu::kNone, fallback ? c.uav(fallbackArgs) : gpu::kNone, tilesX, height };
                             c.cmd->SetPipelineState(begin);
                             c.computeConstants(k, 8);
                             c.cmd->Dispatch(1, 1, 1);
                         });

        // Edge detection and shading are banded passes (INTERFACES v1.29, design revision 1 4.8): in a band the detection
        // kernel reads the band's material words, depth and G-buffer and the shading kernels read them again while they are
        // in L2. Neither needs rows below its band (the detection's neighbours are the resolve's, complete before the group;
        // S's visibility has no screen-space filter), so no band is lagged. A pass band covers whole list bands of the
        // resolve's split (o.bands, passBandCount of the view): the shading pass dispatches every list band inside its rows,
        // the detection pass its tile rows. One pass band over the view dispatches all of them.
        ID3D12PipelineState* detect = fc.shaders.compute("Passes/Shading/EdgeDetect");
        const uint32_t bands = o.bands, height = o.height;
        auto passTileRows = [height, tilesY = o.tilesY](const PassContext& c) {
            const uint32_t y1 = std::min(c.band.y1, height);
            return std::pair<uint32_t, uint32_t>{ c.band.y0 / 8, y1 >= height ? tilesY : y1 / 8 };
        };
        // The list bands inside the pass band's rows (fails when one straddles its edge).
        auto listBands = [bands, height](const PassContext& c) {
            const uint32_t y0 = c.band.y0, y1 = std::min(c.band.y1, height);
            uint32_t first = UINT32_MAX, last = 0;
            for (uint32_t b = 0; b < bands; ++b)
            {
                const uint32_t r0 = material::bandRow(height, bands, b), r1 = material::bandRow(height, bands, b + 1);
                if (r1 <= y0 || r0 >= y1) continue;
                if (r0 < y0 || r1 > y1)
                    fail("M.shading: pass band %u of %u, rows [%u, %u), cuts the resolve's list band %u of %u [%u, %u)", c.band.index, c.band.count, y0, y1, b, bands,
                         r0, r1);
                first = std::min(first, b);
                last = b + 1;
            }
            return std::pair<uint32_t, uint32_t>{ first == UINT32_MAX ? 0 : first, last };
        };
        RenderGraph::BandedPass detectPass;
        detectPass.name = "edge.detect";
        detectPass.setup = [=](PassBuilder& b) {
            b.use(v.visId, Use::SrvCompute);
            b.use(o.materialWord, Use::SrvCompute);
            b.use(v.depth, Use::SrvCompute);
            b.use(v.gbuffer, Use::SrvCompute);
            if (planarTiles.valid()) b.use(planarTiles, Use::SrvCompute);
            b.use(edgeTiles, Use::UavCompute);
            b.use(edgePixels, Use::UavCompute);
            b.use(edgeArgs, Use::UavCompute);
        };
        detectPass.execute = [=](PassContext& c) {
            const auto [row0, row1] = passTileRows(c);
            if (row1 <= row0) return;
            const uint32_t k[16] = { c.srv(v.visId), c.srv(o.materialWord), c.srv(v.depth), c.srv(v.gbuffer),
                                     c.uav(edgeTiles), c.uav(edgePixels), c.uav(edgeArgs), planarTiles.valid() ? c.srv(planarTiles) : gpu::kNone,
                                     asUint(ec.cosAngle), asUint(ec.footprintTolerance), asUint(ec.distanceTolerance), experiment,
                                     row0, 0, 0, 0 };
            c.cmd->SetPipelineState(detect);
            c.bindFrameConstants(cb);
            c.computeConstants(k, 16);
            c.cmd->Dispatch(o.tilesX, row1 - row0, 1);
        };

        // Planar reflection views are timed apart (their cost is R's reflection budget, ARCHITECTURE 2.6 C_planar).
        RenderGraph::BandedPass shadePass;
        shadePass.name = "shade";
        shadePass.setup = [=](PassBuilder& b) {
            b.use(v.gbuffer, Use::SrvCompute);
            b.use(v.depth, Use::SrvCompute);
            b.use(o.materialWord, Use::SrvCompute);
            if (o.emissive.valid()) b.use(o.emissive, Use::SrvCompute);
            b.use(o.tiles, Use::SrvCompute);
            b.use(o.tileArgs, Use::IndirectArgs);
            b.use(v.color, Use::UavComputeDisjoint);
            if (v.shadowVisibility.valid()) b.use(v.shadowVisibility, Use::SrvCompute);
            if (v.screenProbes.valid()) b.use(v.screenProbes, Use::SrvCompute);
            if (v.screenProbeMaps.valid()) b.use(v.screenProbeMaps, Use::SrvCompute);
            if (v.reflection.valid()) b.use(v.reflection, Use::SrvCompute);
            if (r.giCache.valid()) b.use(r.giCache, Use::SrvCompute);  // planar: direct lookups; main: ProbeSrvs.pad1
            if (atmosphere)
                for (TextureRef t : { r.transmittanceLut, r.multiScatterLut, r.skyViewLut }) b.use(t, Use::SrvCompute);
            if (air) b.use(v.airVolume, Use::SrvCompute);
            if (froxelLists) b.use(v.froxelLights, Use::SrvCompute);
            if (overflow)
            {
                b.use(v.shadowOverflowTiles, Use::SrvCompute);
                b.use(v.shadowOverflow, Use::SrvCompute);
            }
            b.use(edgeRadiance, Use::UavCompute);
            b.use(edgeTiles, Use::SrvCompute);
            if (coverage) b.use(v.coverageTiles, Use::SrvCompute);
            useParticles(b);
            if (meter) b.use(histogram.buffer, Use::UavCompute);
        };
        shadePass.execute = [=](PassContext& c) {
            const auto [firstBand, lastBand] = listBands(c);
            const uint32_t none = gpu::kNone;
            const uint32_t atm[4] = { atmosphere ? c.srv(r.transmittanceLut) : none, atmosphere ? c.srv(r.multiScatterLut) : none,
                                      atmosphere ? c.srv(r.skyViewLut) : none, air ? c.srv(v.airVolume) : none };
            const uint32_t fx[2] = { froxelLists ? c.srv(v.froxelLights) : none, ltcSrv };
            c.bindFrameConstants(cb);
            ID3D12Resource* args = c.resource(o.tileArgs);
            const uint32_t edge[8] = { c.srv(edgeTiles), coverage ? c.srv(v.coverageTiles) : none, 0, 0, c.uav(edgeRadiance),
                                       v.screenProbeMaps.valid() ? c.srv(v.screenProbeMaps) : none, 0, 0 };
            // Sky tiles of the list bands in this pass band.
            c.cmd->SetPipelineState(sky);
            for (uint32_t band = firstBand; band < lastBand; ++band)
            {
                const uint32_t cls = (uint32_t)material::ShadeClass::Sky;
                uint32_t k[32] = { c.srv(o.materialWord), c.uav(v.color), c.srv(o.tiles), o.firstTile(cls, band),
                                   atm[0], atm[1], atm[2], atm[3], experiment, r.celestial, 0, 0, 0, 0, c.srv(edgeTiles), 0 };  // P[2].y: S's celestial record (v1.49)
                std::memcpy(k + 24, edge, sizeof edge);
                particleConstants(c, k + 22);  // P[5].zw
                k[18] = asUint(histogram.centreSigma);                        // P[4].z
                k[19] = meter ? c.uav(histogram.buffer) : gpu::kNone;          // P[4].w
                c.computeConstants(k, 32);
                c.cmd->ExecuteIndirect(signature, 1, args, o.argsOffset(cls, band), nullptr, 0);
            }
            // Surface classes (Subsurface and Water use the opaque model until theirs are defined).
            c.cmd->SetPipelineState(opaque);
            for (material::ShadeClass shadeClass : { material::ShadeClass::Opaque, material::ShadeClass::Subsurface, material::ShadeClass::Water })
            for (uint32_t band = firstBand; band < lastBand; ++band)
            {
                const uint32_t cls = (uint32_t)shadeClass;
                const uint32_t k[22] = { c.srv(v.gbuffer), c.srv(v.depth), c.srv(o.materialWord), c.uav(v.color),
                                         c.srv(o.tiles), o.firstTile(cls, band), cls, o.emissive.valid() ? c.srv(o.emissive) : none,
                                         v.shadowVisibility.valid() ? c.srv(v.shadowVisibility) : none, v.screenProbes.valid() ? c.srv(v.screenProbes) : none,
                                         v.reflection.valid() ? c.srv(v.reflection) : none,
                                         r.giCache.valid() ? c.srv(r.giCache) : none,
                                         atm[0], atm[1], overflow ? c.srv(v.shadowOverflowTiles) : none, atm[3], 0, o.textureTableSrv, experiment,
                                         0, fx[0], fx[1] };
                uint32_t k32[32] = {};
                std::memcpy(k32, k, sizeof k);
                std::memcpy(k32 + 24, edge, sizeof edge);
                k32[30] = overflow ? c.srv(v.shadowOverflow) : none;  // P[7].z
                k32[16] = r.areaLightStable;     // P[4].x (B2)
                k32[19] = meter ? c.uav(histogram.buffer) : gpu::kNone;  // P[4].w exposure histogram
                k32[26] = asUint(histogram.centreSigma);                 // P[6].z
                particleConstants(c, k32 + 22);  // P[5].zw
                c.computeConstants(k32, 32);
                c.cmd->ExecuteIndirect(signature, 1, args, o.argsOffset(cls, band), nullptr, 0);
            }
        };
        return { detectPass, shadePass };
    }

    // Tiles over S's overflow capacity (INTERFACES 7.3): every non-sky class of the tile, lights past the third from S's
    // VSM directly (FALLBACK=1; its extra registers stay out of the main kernel). An empty list costs one argument read.
    if (fallback)
    {
        ID3D12PipelineState* fallbackKernel = opaqueKernel(true);
        ShadowSrvRing& ring = fc.state<ShadowSrvRing>("M.shadowSrvRing");
        if (vsm) ring.ensure(fc.device);
        ShadowSrvRing* ringPtr = vsm ? &ring : nullptr;
        const uint32_t ringSlot = vsm ? ring.claim(fc.frame.frameIndex) : 0;
        fc.graph.addPass(planar ? "m.shade.fallback.planar" : "m.shade.fallback", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(v.gbuffer, Use::SrvCompute);
                             b.use(v.depth, Use::SrvCompute);
                             b.use(o.materialWord, Use::SrvCompute);
                             if (o.emissive.valid()) b.use(o.emissive, Use::SrvCompute);
                             b.use(v.shadowOverflowFallbackTiles, Use::SrvCompute);
                             b.use(fallbackArgs, Use::IndirectArgs);
                             b.use(v.color, Use::UavComputeDisjoint);
                             if (v.shadowVisibility.valid()) b.use(v.shadowVisibility, Use::SrvCompute);
                             if (v.screenProbes.valid()) b.use(v.screenProbes, Use::SrvCompute);
                             if (v.screenProbeMaps.valid()) b.use(v.screenProbeMaps, Use::SrvCompute);
                             if (v.reflection.valid()) b.use(v.reflection, Use::SrvCompute);
                             if (atmosphere)
                                 for (TextureRef t : { r.transmittanceLut, r.multiScatterLut }) b.use(t, Use::SrvCompute);
                             if (air) b.use(v.airVolume, Use::SrvCompute);
                             if (froxelLists) b.use(v.froxelLights, Use::SrvCompute);
                             if (vsm)
                                 for (BufferRef vb : { r.vsmPageTable, r.vsmBlocks, r.vsmSearchBound }) b.use(vb, Use::SrvCompute);
                             // ShadowSrvs.pool: the one-path page atlas (v1.43) once S publishes it, else the page pool
                             if (vsm && r.vsmAtlas.valid()) b.use(r.vsmAtlas, Use::SrvCompute);
                             else if (vsm) b.use(r.vsmPool, Use::SrvCompute);
                             if (vsm && r.vsmLayers.valid()) b.use(r.vsmLayers, Use::SrvCompute);
                             b.use(edgeRadiance, Use::UavCompute);
                             b.use(edgeTiles, Use::SrvCompute);
                             if (coverage) b.use(v.coverageTiles, Use::SrvCompute);
                             useParticles(b);
                         },
                         [=](PassContext& c) {
                             const uint32_t none = gpu::kNone;
                             uint32_t shadowSrvs = none;
                             if (ringPtr)
                             {
                                 const uint32_t words[8] = { c.srv(r.vsmPageTable), r.vsmAtlas.valid() ? c.srv(r.vsmAtlas) : c.srv(r.vsmPool), c.srv(r.vsmBlocks), c.srv(r.vsmSearchBound),
                                                             r.vsmConstants, r.vsmLocalLights, r.vsmSlotOfLight,
                                                             r.vsmLayers.valid() ? c.srv(r.vsmLayers) : none };  // S's transmittance layer (v1.26)
                                 shadowSrvs = ringPtr->write(ringSlot, words);
                             }
                             const uint32_t k[22] = { c.srv(v.gbuffer), c.srv(v.depth), c.srv(o.materialWord), c.uav(v.color),
                                                      c.srv(v.shadowOverflowFallbackTiles), 4, 0xFFFFFFFFu, o.emissive.valid() ? c.srv(o.emissive) : none,
                                                      v.shadowVisibility.valid() ? c.srv(v.shadowVisibility) : none, v.screenProbes.valid() ? c.srv(v.screenProbes) : none,
                                                      v.reflection.valid() ? c.srv(v.reflection) : none, r.giCache.valid() ? c.srv(r.giCache) : none,
                                                      atmosphere ? c.srv(r.transmittanceLut) : none, atmosphere ? c.srv(r.multiScatterLut) : none, none,
                                                      air ? c.srv(v.airVolume) : none, 0, o.textureTableSrv, experiment, 0,
                                                      froxelLists ? c.srv(v.froxelLights) : none, ltcSrv };
                             const uint32_t edge[8] = { c.srv(edgeTiles), coverage ? c.srv(v.coverageTiles) : none, 0, 0, c.uav(edgeRadiance),
                                                        v.screenProbeMaps.valid() ? c.srv(v.screenProbeMaps) : none, shadowSrvs, 0 };
                             uint32_t k32[32] = {};
                             std::memcpy(k32, k, sizeof k);
                             std::memcpy(k32 + 24, edge, sizeof edge);
                             particleConstants(c, k32 + 22);  // P[5].zw
                             k32[16] = r.areaLightStable;     // P[4].x (B2)
                             k32[19] = gpu::kNone;            // P[4].w: overflow tiles are shaded twice; the main kernel metered them
                             c.cmd->SetPipelineState(fallbackKernel);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k32, 32);
                             c.cmd->ExecuteIndirect(signature, 1, c.resource(fallbackArgs), 0, nullptr, 0);
                         });
    }

    // Tile counts of the main view into the readback ring (statistics for gates).
    if (!planar && fc.trackState)
    {
        StatsRing& ring = fc.state<StatsRing>("M.statsRing");
        ring.ensure(fc.device);
        const uint32_t slot = (uint32_t)(fc.frame.frameIndex % StatsRing::kSlots);
        ring.frame[slot] = fc.frame.frameIndex;
        ring.tiles[slot] = tileCount;
        ring.coverage[slot] = coverage;
        ring.last = fc.frame.frameIndex;
        ID3D12Resource* dst = ring.buffer.Get();
        const BufferRef classArgs = o.tileArgs;
        fc.graph.addPass("m.stats", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(classArgs, Use::CopySrc);
                             b.use(edgeArgs, Use::CopySrc);
                             b.keep();
                         },
                         [dst, slot, classArgs, edgeArgs, totals = o.totalsOffset()](PassContext& c) {
                             // Per-class tile totals (ShadeBegin sums the bands) and the edge pixel count.
                             c.cmd->CopyBufferRegion(dst, slot * StatsRing::kBytes, c.resource(classArgs), totals, 16);
                             c.cmd->CopyBufferRegion(dst, slot * StatsRing::kBytes + 16, c.resource(edgeArgs), 12, 4);
                         });
    }

    // Composite dispatch size from the edge pixel count.
    ID3D12PipelineState* edgeArgsKernel = fc.shaders.compute("Passes/Shading/EdgeArgs");
    fc.graph.addPass(planar ? "m.edge.args.planar" : "m.edge.args", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(edgeArgs, Use::UavCompute);
                         b.use(edgePixels, Use::UavCompute);
                     },
                     [edgeArgsKernel, edgeArgs, edgePixels, capacity = v.view.width * v.view.height](PassContext& c) {
                         const uint32_t k[4] = { c.uav(edgeArgs), c.uav(edgePixels), capacity, 0 };
                         c.cmd->SetPipelineState(edgeArgsKernel);
                         c.computeConstants(k, 4);
                         c.cmd->Dispatch(1, 1, 1);
                     });

    // Edge composite over the edge pixels (analytic coverage of the pixel square by the neighbourhood's surfaces). With the
    // coverage layer it also keeps its linear sum, the band A layer under the fragments of edge pixels.
    const TextureRef edgeResolved =
        coverage ? fc.graph.createTexture({ "m.edge resolved", v.view.width, v.view.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT }) : TextureRef{};
    ID3D12PipelineState* composite = fc.shaders.compute(linear ? "Passes/Shading/EdgeComposite.OUTPUT1" : "Passes/Shading/EdgeComposite.OUTPUT0");
    fc.graph.addPass(planar ? "m.edge.planar" : "m.edge", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(v.visId, Use::SrvCompute);
                         b.use(v.visibleClusters, Use::SrvCompute);
                         b.use(o.materialWord, Use::SrvCompute);
                         b.use(v.depth, Use::SrvCompute);
                         b.use(v.gbuffer, Use::SrvCompute);
                         b.use(edgeRadiance, Use::SrvCompute);
                         b.use(v.color, Use::UavCompute);
                         b.use(edgePixels, Use::SrvCompute);
                         b.use(edgeArgs, Use::IndirectArgs);
                         if (planarTiles.valid()) b.use(planarTiles, Use::SrvCompute);
                         if (coverage) b.use(edgeResolved, Use::UavCompute);
                         useParticles(b);
                     },
                     [=](PassContext& c) {
                         uint32_t k[24] = { c.srv(v.visId), c.srv(v.visibleClusters), c.srv(o.materialWord), c.srv(v.depth),
                                                  c.srv(v.gbuffer), c.srv(edgeRadiance), c.uav(v.color), c.srv(edgePixels),
                                                  asUint(ec.cosAngle), asUint(ec.footprintTolerance), asUint(ec.distanceTolerance), ec.groupsMax,
                                                  experiment, o.textureTableSrv, planarTiles.valid() ? c.srv(planarTiles) : gpu::kNone,
                                                  coverage ? c.uav(edgeResolved) : gpu::kNone };
                         particleConstants(c, k + 22);  // P[5].zw
                         c.cmd->SetPipelineState(composite);
                         c.bindFrameConstants(cb);
                         c.computeConstants(k, 24);
                         c.cmd->ExecuteIndirect(signature, 1, c.resource(edgeArgs), 0, nullptr, 0);
                     });

    // Coverage composite (design COVERAGE_REDESIGN 4.5; stages in CoverageShade.hlsli): V's fragments (pixel-major ranges,
    // v1.41) composited over the band A radiance the shading kernels kept for coverage tiles. Every group takes a bounded
    // piece of work (a light pixel, a run of the heavy sort, a round of a heavy pixel), dispatched indirectly with sizes
    // made on the GPU: no tile, pixel or fragment distribution makes one group's work grow.
    if (coverage)
    {
        constexpr uint64_t kBlock = 1024, kLight = 16, kRounds = 8, kTilePx = 8;
        const uint64_t capacity = fc.graph.desc(v.coverageRecords).size / 16;  // records of V's pool
        const uint64_t tiles = ((uint64_t)v.view.width + kTilePx - 1) / kTilePx * (((uint64_t)v.view.height + kTilePx - 1) / kTilePx);
        const uint64_t heavyCap = std::max<uint64_t>(1, std::min(capacity / (kLight + 1), tiles * 64));  // > COV_LIGHT records each
        const uint64_t cursorCap = heavyCap + capacity / kBlock;                                   // >= sum of ceil(count / COV_BLOCK)
        RenderGraph& g = fc.graph;
        const BufferRef state = g.createBuffer({ "m.coverage state", 8 * 4, 0 });
        const BufferRef args = g.createBuffer({ "m.coverage args", 16 * 4, 0 });
        const BufferRef pairs = g.createBuffer({ "m.coverage pairs", std::max<uint64_t>(capacity, 1) * 8, 0 });
        const BufferRef heavy = g.createBuffer({ "m.coverage heavy", heavyCap * 16 * 4, 0 });
        const BufferRef cursors = g.createBuffer({ "m.coverage cursors", cursorCap * 4, 0 });
        const BufferRef lists = g.createBuffer({ "m.coverage open", heavyCap * 2 * 4, 0 });
        const uint32_t hcap = (uint32_t)heavyCap, ccap = (uint32_t)cursorCap;
        const std::string area = areaLights ? "1" : "0", output = linear ? "1" : "0";
        ID3D12PipelineState* begin[3] = { fc.shaders.compute("Passes/Shading/CoverageBegin.MODE0"), fc.shaders.compute("Passes/Shading/CoverageBegin.MODE1"),
                                          fc.shaders.compute("Passes/Shading/CoverageBegin.MODE2") };
        ID3D12PipelineState* light = fc.shaders.compute(("Passes/Shading/CoverageComposite.OUTPUT" + output + ".AREA" + area).c_str());
        ID3D12PipelineState* sort = fc.shaders.compute("Passes/Shading/CoverageHeavySort");
        ID3D12PipelineState* heavyRound = fc.shaders.compute(("Passes/Shading/CoverageHeavyRound.AREA" + area).c_str());
        ID3D12PipelineState* finish = fc.shaders.compute(("Passes/Shading/CoverageHeavyFinish.OUTPUT" + output).c_str());

        // CoverageBegin: MODE 0 reset, 1 heavy arguments, 2 round r's arguments.
        auto addBegin = [&](const char* name, uint32_t mode, uint32_t roundIndex) {
            g.addPass(name, QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(state, Use::UavCompute);
                          b.use(args, Use::UavCompute);
                      },
                      [k = begin[mode], state, args, hcap, roundIndex](PassContext& c) {
                          const uint32_t p[4] = { c.uav(state), hcap, roundIndex, c.uav(args) };
                          c.cmd->SetPipelineState(k);
                          c.computeConstants(p, 4);
                          c.cmd->Dispatch(1, 1, 1);
                      });
        };
        // The fragment shading kernels' resources (CoverageShade.hlsli: P[1], P[3], P[4], P[5].x).
        auto useShading = [=](PassBuilder& b) {
            b.use(v.coverageRecords, Use::SrvCompute);
            b.use(v.visibleClusters, Use::SrvCompute);
            b.use(v.depth, Use::SrvCompute);
            if (froxelLists) b.use(v.froxelLights, Use::SrvCompute);
            if (atmosphere)
                for (TextureRef t : { r.transmittanceLut, r.multiScatterLut }) b.use(t, Use::SrvCompute);
            if (air) b.use(v.airVolume, Use::SrvCompute);
            if (v.screenProbes.valid()) b.use(v.screenProbes, Use::SrvCompute);
            if (v.screenProbeMaps.valid()) b.use(v.screenProbeMaps, Use::SrvCompute);
            if (r.giCache.valid()) b.use(r.giCache, Use::SrvCompute);
            if (fragmentShadows)
            {
                b.use(v.shadowFragmentVisibility, Use::SrvCompute);
                b.use(v.coverageDepthRange, Use::SrvCompute);
                if (v.shadowFragmentSun.valid()) b.use(v.shadowFragmentSun, Use::SrvCompute);
            }
        };
        // P[6].zw, P[7].x of the fragment kernels (CoverageShade.hlsli): R's GI cache, S's per-record sun, V's depth range.
        auto fragmentConstants = [=](PassContext& c, uint32_t* k) {
            k[26] = r.giCache.valid() ? c.srv(r.giCache) : gpu::kNone;
            k[27] = fragmentShadows && v.shadowFragmentSun.valid() ? c.srv(v.shadowFragmentSun) : gpu::kNone;
            k[28] = fragmentShadows ? c.srv(v.coverageDepthRange) : gpu::kNone;
            k[29] = r.areaLightStable;  // P[7].y (B2)
        };
        auto shadingConstants = [=](PassContext& c, uint32_t (&k)[24], uint32_t colour) {
            const uint32_t none = gpu::kNone;
            const uint32_t s[16] = { c.srv(v.visibleClusters), o.textureTableSrv, c.srv(v.depth), colour,
                                     0, 0, 0, 0,
                                     froxelLists ? c.srv(v.froxelLights) : none, ltcSrv, experiment, fragmentShadows ? c.srv(v.shadowFragmentVisibility) : none,
                                     atmosphere ? c.srv(r.transmittanceLut) : none, atmosphere ? c.srv(r.multiScatterLut) : none,
                                     air ? c.srv(v.airVolume) : none, v.screenProbes.valid() ? c.srv(v.screenProbes) : none };
            for (uint32_t i = 0; i < 16; ++i)
                if (i < 4 || i >= 8) k[4 + i] = s[i];
            k[20] = v.screenProbeMaps.valid() ? c.srv(v.screenProbeMaps) : none;
        };
        auto useBandA = [=](PassBuilder& b) {
            b.use(edgeRadiance, Use::SrvCompute);
            b.use(edgeResolved, Use::SrvCompute);
            b.use(edgeTiles, Use::SrvCompute);
        };

        addBegin("m.coverage.begin", 0, 0);
        // E: per listed tile, light pixels composited, heavy pixels recorded.
        g.addPass("m.coverage", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      useShading(b);
                      useBandA(b);
                      b.use(v.coverageTilePixels, Use::SrvCompute);
                      b.use(v.coverageTileList, Use::SrvCompute);
                      b.use(v.coverageTileList, Use::IndirectArgs);
                      b.use(state, Use::UavCompute);
                      b.use(heavy, Use::UavCompute);
                      b.use(v.color, Use::UavCompute);
                      useParticles(b);
                  },
                  [=](PassContext& c) {
                      uint32_t k[24] = { c.srv(v.coverageRecords), c.srv(v.coverageTilePixels), c.srv(v.coverageTileList), c.uav(state) };
                      shadingConstants(c, k, c.uav(v.color));
                      k[8] = c.srv(edgeRadiance);
                      k[9] = c.srv(edgeResolved);
                      k[10] = c.srv(edgeTiles);
                      k[11] = c.uav(heavy);
                      k[21] = hcap;
                      k[22] = ccap;
                      uint32_t k32[32] = {};
                      std::memcpy(k32, k, sizeof k);
                      particleConstants(c, k32 + 24);  // P[6].xy
                      fragmentConstants(c, k32);      // P[6].zw, P[7].x
                      c.cmd->SetPipelineState(light);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k32, 32);
                      c.cmd->ExecuteIndirect(signature, 1, c.resource(v.coverageTileList), 0, nullptr, 0);
                  });
        addBegin("m.coverage.heavy args", 1, 0);
        // F1: heavy pixels' runs of COV_BLOCK records sorted into the pair buffer.
        g.addPass("m.coverage.sort", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(state, Use::SrvCompute);
                      b.use(heavy, Use::SrvCompute);
                      b.use(pairs, Use::UavCompute);
                      b.use(cursors, Use::UavCompute);
                      b.use(v.coverageRecords, Use::SrvCompute);
                      b.use(args, Use::IndirectArgs);
                  },
                  [=](PassContext& c) {
                      const uint32_t p[8] = { c.srv(state), c.srv(heavy), c.uav(pairs), c.uav(cursors), hcap, c.srv(v.coverageRecords), 0, 0 };
                      c.cmd->SetPipelineState(sort);
                      c.computeConstants(p, 8);
                      c.cmd->ExecuteIndirect(signature, 1, c.resource(args), 16, nullptr, 0);
                  });
        // F2: COV_ROUNDS rounds, each over the heavy pixels the previous one left open.
        for (uint32_t rd = 0; rd < kRounds; ++rd)
        {
            if (rd > 0) addBegin("m.coverage.round args", 2, rd);
            g.addPass("m.coverage.round", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          useShading(b);
                          b.use(state, Use::UavCompute);
                          b.use(heavy, Use::UavCompute);
                          b.use(cursors, Use::UavCompute);
                          b.use(pairs, Use::SrvCompute);
                          b.use(lists, Use::UavCompute);
                          b.use(args, Use::IndirectArgs);
                      },
                      [=](PassContext& c) {
                          uint32_t k[24] = { c.srv(v.coverageRecords), c.uav(state), c.uav(heavy), c.uav(cursors) };
                          shadingConstants(c, k, gpu::kNone);
                          k[11] = c.srv(pairs);
                          k[21] = rd;
                          k[22] = hcap;
                          k[23] = c.uav(lists);
                          uint32_t k32[32] = {};
                          std::memcpy(k32, k, sizeof k);
                          k32[24] = k32[25] = gpu::kNone;  // (no particle layer in the rounds)
                          fragmentConstants(c, k32);      // P[6].zw, P[7].x
                          c.cmd->SetPipelineState(heavyRound);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k32, 32);
                          c.cmd->ExecuteIndirect(signature, 1, c.resource(args), 32, nullptr, 0);
                      });
        }
        // F3: heavy pixels' band A remainder and output; a pixel the rounds left open sets the error bit.
        g.addPass("m.coverage.finish", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      useBandA(b);
                      b.use(state, Use::UavCompute);
                      b.use(heavy, Use::SrvCompute);
                      b.use(v.color, Use::UavCompute);
                      b.use(args, Use::IndirectArgs);
                      useParticles(b);
                  },
                  [=](PassContext& c) {
                      uint32_t p[12] = { c.uav(state), c.srv(heavy), hcap, 0, 0, 0, 0, c.uav(v.color), c.srv(edgeRadiance), c.srv(edgeResolved), c.srv(edgeTiles), 0 };
                      particleConstants(c, p + 4);  // P[1].xy
                      c.cmd->SetPipelineState(finish);
                      c.bindFrameConstants(cb);
                      c.computeConstants(p, 12);
                      c.cmd->ExecuteIndirect(signature, 1, c.resource(args), 48, nullptr, 0);
                  });
        // Its error bits (INTERFACES 3.6: a data-dependent loop reached its bound) into the statistics: a gate failure.
        if (fc.trackState)
        {
            StatsRing& ring = fc.state<StatsRing>("M.statsRing");
            ring.ensure(fc.device);
            ID3D12Resource* dst = ring.buffer.Get();
            const uint32_t slot = (uint32_t)(fc.frame.frameIndex % StatsRing::kSlots);
            g.addPass("m.coverage.stats", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(state, Use::CopySrc);
                          b.keep();
                      },
                      [dst, slot, state](PassContext& c) { c.cmd->CopyBufferRegion(dst, slot * StatsRing::kBytes + 20, c.resource(state), 12, 4); });
        }
    }
    return {};
}
} // namespace

std::vector<RenderGraph::BandedPass> shadingPasses(FramePassContext& fc, ViewResources& view) { return record(fc, view, Part::Banded); }

void shadingComposite(FramePassContext& fc, ViewResources& view)
{
    record(fc, view, Part::Composite);
    // The main view's luminance histogram, filled by its shading kernels, into the readback ring (automatic exposure).
    if (view.view.kind == gpu::ViewKind::Main) exposureReadback(fc, exposureHistogram(fc));
}

void shade(FramePassContext& fc, ViewResources& view)
{
    // Views the frame does not record as the lighting group (planar reflection views through renderView, tests): M's own
    // group over the frame's bands (output.band_pixels; one band by default, v1.31). The banded part checks the view first.
    // With a post term on, the main view is shaded into the chain's HDR target and the chain writes the display output.
    // Motion blur (the shutter's time integral) reads the shaded image around each pixel: with it the view is shaded into
    // a float target first, then blurred into the chain's input (display) or into the linear capture itself.
    // Heat haze re-reads it at displaced points too (before the exposure integral: the haze bends what the lens sees).
    const bool post = postActive(fc, view);
    const bool blur = motionBlurActive(fc, view), haze = distortionActive(fc, view);
    const DXGI_FORMAT floatFormat = post ? DXGI_FORMAT_R16G16B16A16_FLOAT : fc.graph.desc(view.color).format;
    auto intermediate = [&](const char* name) { return fc.graph.createTexture(TextureDesc{ name, view.view.width, view.view.height, 1, 1, floatFormat }); };
    ViewResources target = view;
    if (post) target.color = postTarget(fc, view);
    else if (blur || haze) target.color = intermediate("m.shaded");
    const std::vector<RenderGraph::BandedPass> passes = shadingPasses(fc, target);
    const material::ResolveOutputs& o = material::resolveOutputs(fc, target);
    fc.graph.addBandedGroup(view.view.kind != gpu::ViewKind::Main ? "m.lit.planar" : "m.lit", o.height, o.bands, passes);
    shadingComposite(fc, target);
    TextureRef image = target.color;
    if (haze)
    {
        const TextureRef displaced = (blur || post) ? intermediate("m.distorted") : view.color;
        distortion(fc, view, image, displaced);
        image = displaced;
    }
    if (blur)
    {
        const TextureRef blurred = post ? postTarget(fc, view) : view.color;
        motionBlur(fc, view, image, blurred);
        image = blurred;
    }
    if (post) postChain(fc, view, image);
}
} // namespace unx::render::shading
