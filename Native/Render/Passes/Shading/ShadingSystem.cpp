#include "unx/shading/ShadingSystem.h"
#include "unx/shading/MegaLights.h"
#include "unx/shading/DepthOfField.h"
#include "unx/shading/Exposure.h"
#include "unx/shading/MotionBlur.h"
#include "unx/shading/Post.h"
#include "unx/shading/Upscale.h"

#include "unx/core/Log.h"
#include "unx/material/MaterialSystem.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"
#include "unx/lights/EmissiveLights.h"
#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <array>
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
    bool glass[kSlots] = {};     // the frame ran the glass composite (its counts at bytes 24..35 are this frame's)
    bool dof[kSlots] = {};       // the frame ran the depth of field (its counts at bytes 36..43 are this frame's)
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
    if (ring.coverage[slot])
        st.coverageEntries = w[11], st.coverageWalked = w[12], st.coverageShaded = w[13], st.coverageLightPixels = w[14], st.coverageHeavyPixels = w[15];
    if (ring.glass[slot]) st.glassPanePixels = w[6], st.glassSolidPixels = w[7], st.glassUnlitPixels = w[8];
    if (ring.dof[slot]) st.dofPixels = w[9], st.dofClampedPixels = w[10];
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
    TextureRef areaLobes;  // A9 (AreaLobes.hlsl): area-light lobe radiance of the layered classes (area-lit scenes only)
    TextureRef direct;     // ShadeOpaque part 1 -> ShadeIndirect (part 2): the direct radiance (RGBA32F, linear before exposure)
    TextureRef megaLighting;  // shading.mega_lights: the local lights' direct light (m.ml.spatial; invalid = off)
    // shading.subsurface_scatter (SubsurfaceScatter.hlsli): the Subsurface class's diffuse light per unit f_d (RGBA16F:
    // rgb x exposure, a = view depth; invalid = off), and with shading.mega_lights the local lights' specular of the
    // class's pixels (m.ml.spatial.sss; their diffuse is in the first)
    TextureRef scatterDiffuse, scatterMlSpecular;
    bool scattered = false;  // m.sss.scatter is recorded for this view (shadingScatter, or the composite part itself)
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
    Scatter,    // after the group, before anything reads the lit image: the Subsurface class's scatter pass
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
    // A9: clearcoat materials shade in their own class with the LAYERED=1 variant, sheen materials in theirs with LAYERED=2;
    // the fallback kernel runs once per variant the scene needs, each over its classes (P[1].z mask).
    // Subsurface materials shade their part 1 with the LAYERED=3 variant (the class's two specular lobes and its light
    // through thin parts; ShadeOpaque.hlsl header), in the class's own tile list and in a fallback run of its own.
    bool layeredMaterials = false, sheenMaterials = false, subsurfaceMaterials = false;
    if (const scene::Scene* src = fc.scene.source())
        for (const scene::Material& mt : src->materials)
        {
            layeredMaterials = layeredMaterials || mt.clearcoat > 0 || mt.anisotropy > 0 || mt.thinFilmThickness > 0;  // (A9 anisotropy and thin films shade in the layered variants)
            sheenMaterials = sheenMaterials || mt.sheenColor.x > 0 || mt.sheenColor.y > 0 || mt.sheenColor.z > 0;
            subsurfaceMaterials = subsurfaceMaterials || mt.cls == scene::MaterialClass::Subsurface;
        }
    // Two kernels per class (ShadeOpaque.hlsl header): part 1 (direct light -> the direct radiance texture) and part 2
    // (ShadeIndirect: indirect light, air, output), bit-identical to the one kernel; one UAV barrier between them.
    auto opaqueKernel = [&](bool fallbackVariant, uint32_t layered) {
        const std::string name = std::string("Passes/Shading/ShadeOpaque.FALLBACK") + (fallbackVariant ? "1" : "0") +
                                 ".AREA" + (areaLights ? "1" : "0") + ".PLANAR" + (view.view.kind != gpu::ViewKind::Main ? "1" : "0") + ".LAYERED" +
                                 std::to_string(layered);
        return fc.shaders.compute(name.c_str());
    };
    auto indirectKernel = [&](bool fallbackVariant, uint32_t layered) {
        const std::string name = std::string("Passes/Shading/ShadeIndirect.OUTPUT") + (linear ? "1" : "0") + ".FALLBACK" + (fallbackVariant ? "1" : "0") +
                                 ".PLANAR" + (view.view.kind != gpu::ViewKind::Main ? "1" : "0") + ".LAYERED" + std::to_string(layered);
        return fc.shaders.compute(name.c_str());
    };
    // Subsurface class, stage B (shading.subsurface_scatter; SubsurfaceScatter.hlsli states the passes): the class's
    // kernels keep its diffuse light apart, per unit f_d, in a texture of its own (SubsurfaceDirect, SubsurfaceIndirect:
    // ShadeOpaque.hlsl with SSS_SPLIT), and m.sss.scatter scatters it, puts f_d back and writes the output. Scenes without
    // Subsurface materials, and the switch off, run the kernels and passes of stage A.
    const bool scatter = subsurfaceMaterials && fc.quality.boolean("shading.subsurface_scatter");
    const uint32_t scatterSamples = scatter ? (uint32_t)fc.quality.integer("shading.subsurface_scatter_samples") : 0u;
    const float scatterMinPixels = scatter ? (float)fc.quality.number("shading.subsurface_scatter_min_px") : 1.0f;
    if (scatter && (scatterSamples < 2 || scatterSamples > 64 || (scatterSamples & 1u) != 0))
        fail("shading.subsurface_scatter_samples must be even and in [2, 64] (pairs across the pixel)");
    if (scatter && !(scatterMinPixels > 0)) fail("shading.subsurface_scatter_min_px must be positive");
    auto subsurfaceKernel = [&](uint32_t shadePart, bool fallbackVariant) {
        const std::string planarTag = view.view.kind != gpu::ViewKind::Main ? "1" : "0", fallbackTag = fallbackVariant ? "1" : "0";
        const std::string name = shadePart == 1 ? "Passes/Shading/SubsurfaceDirect.FALLBACK" + fallbackTag + ".AREA" + (areaLights ? "1" : "0") + ".PLANAR" + planarTag
                                                : "Passes/Shading/SubsurfaceIndirect.FALLBACK" + fallbackTag + ".PLANAR" + planarTag;
        return fc.shaders.compute(name.c_str());
    };
    ID3D12PipelineState* opaque = opaqueKernel(false, 0);
    ID3D12PipelineState* opaqueLayered = layeredMaterials ? opaqueKernel(false, 1) : nullptr;
    ID3D12PipelineState* opaqueSheen = sheenMaterials ? opaqueKernel(false, 2) : nullptr;
    ID3D12PipelineState* opaqueSubsurface = subsurfaceMaterials ? (scatter ? subsurfaceKernel(1, false) : opaqueKernel(false, 3)) : opaque;  // (part 2: the plain kernel's)
    ID3D12PipelineState* indirect = indirectKernel(false, 0);
    ID3D12PipelineState* indirectSubsurface = scatter ? subsurfaceKernel(2, false) : indirect;
    ID3D12PipelineState* indirectLayered = layeredMaterials ? indirectKernel(false, 1) : nullptr;
    ID3D12PipelineState* indirectSheen = sheenMaterials ? indirectKernel(false, 2) : nullptr;
    // A9 area-light lobes (AreaLobes.hlsl: ShadeOpaque's light loop with the sheen and anisotropic lobes over each area
    // light, dispatched on the same tiles right before the layered kernels, which add its texture)
    bool anisoMaterials = false;
    if (const scene::Scene* src = fc.scene.source())
        for (const scene::Material& mt : src->materials) anisoMaterials = anisoMaterials || mt.anisotropy > 0;
    const bool lobesOn = areaLights && (anisoMaterials || sheenMaterials);
    auto lobeKernel = [&](bool fallbackVariant, uint32_t layered) {
        const std::string name = std::string("Passes/Shading/AreaLobes.FALLBACK") + (fallbackVariant ? "1" : "0") + ".PLANAR" +
                                 (view.view.kind != gpu::ViewKind::Main ? "1" : "0") + ".LAYERED" + std::to_string(layered);
        return fc.shaders.compute(name.c_str());
    };
    ID3D12PipelineState* lobesLayered = lobesOn && anisoMaterials ? lobeKernel(false, 1) : nullptr;
    ID3D12PipelineState* lobesSheen = lobesOn && sheenMaterials ? lobeKernel(false, 2) : nullptr;
    // the lobe kernel's writes complete before the layered kernel reads them (the same pass: one global UAV barrier)
    auto lobeBarrier = [](PassContext& c) {
        D3D12_GLOBAL_BARRIER gb{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                 D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
        D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
        group.pGlobalBarriers = &gb;
        c.cmd->Barrier(1, &group);
    };
    ID3D12PipelineState* sky = fc.shaders.compute(linear ? "Passes/Shading/ShadeSky.OUTPUT1" : "Passes/Shading/ShadeSky.OUTPUT0");
    const FrameResources r = fc.resources;
    const bool atmosphere = r.transmittanceLut.valid() && r.multiScatterLut.valid() && r.skyViewLut.valid();
    // This view's S products (v1.22): froxel light lists and air volume (main view: the frame's; planar views: S's own,
    // the air integrated from the mirror plane). Without them a view has no local lights and only the sun's transmittance.
    const bool air = atmosphere && view.airVolume.valid();
    // The froxel grid is the main view's: planar reflection views read neither its lists nor its air volume.
    const bool froxelLists = view.froxelLights.valid();
    const ViewResources v = view;
    // The indirect light's source beside R's screen probes (unx/render/Frame.h GiSource, Passes/GI/GiSource.hlsli): with
    // the probes, the world cache they fall back to; without them (gi.lumen_only, planar views) the translucency volume
    // when the frame has published one, else the cache.
    GiSource giSrc;
    if (view.screenProbes.valid()) giSrc.cache = r.giCache;
    else giSrc = giSource(r);
    // gi.lumen_only's composite (ShadeOpaque.hlsl part 2): the gather's rough specular and the short-range AO
    const bool lumenComposite = !view.screenProbes.valid() && view.giIrradiance.valid();
    const TextureRef roughSpecular = lumenComposite ? view.giRoughSpecular : TextureRef{}, shortRangeAO = lumenComposite ? view.shortRangeAO : TextureRef{};
    const TextureRef backfaceIrradiance = lumenComposite ? view.giBackfaceIrradiance : TextureRef{};
    // S's coverage fragment visibility (INTERFACES 7.3 v1.41): with it the fragment kernels shade fragments with S's sun
    // profile and local slots (CoverageShade.hlsli covFragmentShadow); without it they stay unshadowed.
    const bool fragmentShadows = v.shadowFragmentVisibility.valid() && v.coverageDepthRange.valid();
    // FX's particle layer of this view (tracks::particles, before shading): every output writer composites it before its
    // tone map (ShadingCommon.hlsli shParticles); two indices, UNX_NONE when the view has none.
    const bool particles = v.particleLayer.valid() && v.particleEdges.valid();
    // Automatic exposure: the main view's shading kernels fill M's luminance histogram (Exposure.cpp); read back below.
    const bool meter = v.view.kind == gpu::ViewKind::Main && part != Part::Composite;
    // (the main view's histogram in both parts: the composite part's fallback kernel meters its tiles too)
    const ExposureHistogram histogram = v.view.kind == gpu::ViewKind::Main ? exposureHistogram(fc) : ExposureHistogram{};
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
    // 14.1b (L2b): the converted emissive surfaces as quadtree area lights (shading.emissive_area_lights; invalid = off).
    // (the scatter part reads neither them nor the tile records)
    const BufferRef emissiveLights = part == Part::Scatter ? BufferRef{} : lights::emissiveLights(fc);
    TextureRef emissiveIrradiance;  // per-pixel diffuse irradiance from them (EmissiveDirect.hlsl, exposed RGBA16F)
    // 14.1/14.2 (L2): the tile lights' records (TileLights.hlsl; shading.tile_lights, main view with S's lists).
    // shading.mega_lights (MegaLights.cpp; main view with S's lists and R's ray scene): the local lights' direct light comes
    // from light samples (m.ml.*) and the kernels skip their loop over the lists, so the tile records are not built.
#if UNX_M_HAS_RAYTRACING
    // Every view runs it (as Unreal's MegaLights does per view): the main view, the full auxiliary views (their own history
    // under viewId) and planar reflection views (no history: the spatial filter alone).
    const bool megaWanted = fc.quality.boolean("shading.mega_lights") && froxelLists && r.tlasStatic.valid() && fc.trackState != nullptr && (experiment & 32) == 0;
#else
    const bool megaWanted = false;
#endif
    const bool tileLights = fc.quality.boolean("shading.tile_lights") && froxelLists && view.view.kind == gpu::ViewKind::Main && !megaWanted;
    // Lighting channels in the kernels' own light loop (the froxel lists without shading.mega_lights, whose sampling tests
    // them): part 1 reads the pixel's instance through the vis buffer (ShadeOpaque.hlsl P[11].zw).
    const bool channelVis = froxelLists && !megaWanted && view.visId.valid() && view.visibleClusters.valid();
    const BufferRef tileRecords = tileLights && part != Part::Scatter ? fc.graph.createBuffer({ "M tile lights", (uint64_t)tileCount * 96, 0 }) : BufferRef{};
    if (emissiveLights.valid())
    {
        RenderGraph& g = fc.graph;
        const uint32_t W = view.view.width, H = view.view.height;
        emissiveIrradiance = g.createTexture({ "M emissive irradiance", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        ID3D12PipelineState* kernel = fc.shaders.compute("Passes/Lights/EmissiveDirect");
        const TextureRef depth = view.depth, gbuffer = view.gbuffer, outTex = emissiveIrradiance;
        const BufferRef lightsBuf = emissiveLights;
        const D3D12_GPU_VIRTUAL_ADDRESS cbAddr = view.frameConstants;
        g.addPass("m.emissive.direct", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(depth, Use::SrvCompute);
                      b.use(gbuffer, Use::SrvCompute);
                      b.use(lightsBuf, Use::SrvCompute);
                      b.use(outTex, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.srv(depth), c.srv(gbuffer), c.srv(lightsBuf), c.uav(outTex) };
                      c.cmd->SetPipelineState(kernel);
                      c.bindFrameConstants(cbAddr);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                  });
    }
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const EdgeConfig ec = edgeConfig(fc.quality);
    const bool planar = view.view.kind != gpu::ViewKind::Main;
    // V's coverage layer (INTERFACES 7.1 v1.41; main view): its fragments are composited over band A
    // (CoverageComposite.hlsl, design COVERAGE_REDESIGN 4.5). The fragments' shadows come from S's fragment visibility
    // (4.3, fragmentShadows): a frame with S's shadows but without it would leave fragments unshadowed, so it fails
    // (shading bit 8192 allows it for cost attribution, never an image).
    const bool coverage = !planar && v.coverageTiles.valid() && v.coverageRecords.valid() && v.coverageTileList.valid() && v.coverageTilePixels.valid();
    // v1.75: band A radiance is kept under V's water layer too (W's refraction source in tracks::water reads bandARadiance).
    const bool keepWater = v.waterVis.valid();
    // v1.77 W stage 2: the sun-space water map (all four valid or none)
    const bool waterSun = r.waterSunDepth.valid() && r.waterSunNormal.valid() && r.waterSunMedium.valid() && r.waterSunConstants.valid();
    const bool caustics = waterSun && r.waterSunCaustics.valid();
    // k[0..3] the map, k[causticsAt] its caustics
    auto waterSunConstants = [=](PassContext& c, uint32_t* k, uint32_t causticsAt) {
        const uint32_t none = gpu::kNone;
        k[0] = waterSun ? c.srv(r.waterSunDepth) : none;
        k[1] = waterSun ? c.srv(r.waterSunNormal) : none;
        k[2] = waterSun ? c.srv(r.waterSunMedium) : none;
        k[3] = waterSun ? c.srv(r.waterSunConstants) : none;
        k[causticsAt] = caustics ? c.srv(r.waterSunCaustics) : none;
    };
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
        // (shading.mega_lights: the lobes are in its kernel, on the light samples - no lobe texture, no lobe pass)
        if (lobesOn && !megaWanted) res.areaLobes = fc.graph.createTexture({ "m.area lobes", v.view.width, v.view.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        res.direct = fc.graph.createTexture({ "m.direct radiance", v.view.width, v.view.height, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        if (scatter)
        {
            res.scatterDiffuse = fc.graph.createTexture({ "m.sss diffuse", v.view.width, v.view.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            if (megaWanted)
                res.scatterMlSpecular = fc.graph.createTexture({ "m.sss ml specular", v.view.width, v.view.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        }
        res.edgePixels = fc.graph.createBuffer({ "m.edge pixels", ((uint64_t)v.view.width * v.view.height + 1) * 4, 0 });
        res.edgeArgs = fc.graph.createBuffer({ "m.edge args", 24, 0 });  // dispatch args + pixel count (Edge.hlsli)
        res.edgeTiles = fc.graph.createTexture({ "m.edge tile mask", o.tilesX, o.tilesY, 1, 1, DXGI_FORMAT_R32G32_UINT });  // 64 bits per tile
        table.views.push_back({ view.frameConstants, res });
        // v1.75: the band A radiance the composites put behind fragments (W writes its interior water here too), and the
        // special records' radiance (CoverageSpecial.hlsli): kinds 1 and 2 get 0 before tracks::water, their owners
        // overwrite them (W: kind 2 in tracks::water), M shades kind 5 before the composite.
        view.bandARadiance = res.edgeRadiance;
        if (coverage && v.coverageSpecial.valid())
        {
            const uint64_t elements = fc.graph.desc(v.coverageRecords).size / 16;
            const BufferRef shaded = fc.graph.createBuffer({ "m.coverage record radiance", std::max<uint64_t>(elements, 1) * 8, 0 });
            view.coverageRecordRadiance = shaded;
            ID3D12PipelineState* defaults = fc.shaders.compute("Passes/Shading/CoverageSpecial.MODE0.AREA0");
            const BufferRef special = v.coverageSpecial;
            fc.graph.addPass("m.coverage.special default", QueueType::Graphics,
                             [&](PassBuilder& b) {
                                 b.use(special, Use::SrvCompute);
                                 b.use(special, Use::IndirectArgs);
                                 b.use(shaded, Use::UavCompute);
                             },
                             [=](PassContext& c) {
                                 const uint32_t k[4] = { gpu::kNone, c.srv(special), gpu::kNone, c.uav(shaded) };
                                 c.cmd->SetPipelineState(defaults);
                                 c.computeConstants(k, 4);
                                 c.cmd->ExecuteIndirect(signature, 1, c.resource(special), 4, nullptr, 0);  // header words 1..3
                             });
        }
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
    const TextureRef edgeRadiance = res.edgeRadiance, edgeTiles = res.edgeTiles, areaLobes = res.areaLobes, directRadiance = res.direct;
    const TextureRef scatterDiffuse = res.scatterDiffuse, scatterMlSpecular = res.scatterMlSpecular;
    TextureRef megaLighting = res.megaLighting;  // (the banded part sets it below)
    // Planar views: tiles without mirror pixels are never shaded; edge detection and the composite treat their pixels as
    // outside the view (R always gives the tile mask with the pixel mask, v1.22).
    const TextureRef planarTiles = v.view.planarTileMask;
    if (v.view.planarMask.valid() && !planarTiles.valid()) fail("M.shading: a planar view has a pixel mask without its tile mask");
    // m.sss.scatter (SubsurfaceScatter.hlsl; shading.subsurface_scatter): the Subsurface class's tiles - every list band
    // of the resolve, or (fallbackTiles) the tiles over S's overflow capacity with the class as a mask, which the first run
    // leaves out - scattered, f_d put back, and written as part 2 of the plain classes writes its pixels (air, exposure
    // histogram, particles, output, edge / coverage radiance). It reads the class's diffuse texture around each pixel, so
    // it follows the whole group (and the fallback kernels for their tiles).
    const uint32_t subsurfaceBit = 1u << (uint32_t)material::ShadeClass::Subsurface;
    // A first-person view model drawn through viewmodel.fov_override_degrees (the main view's clip.xy remap) covers more
    // pixels than its geometry at the pixel's ray would: the scatter kernel then reads the vis buffer and takes a
    // view-model pixel's true ray, so its scatter radius in pixels is the drawn size's (ShadeOpaque.hlsl SHADE_PART 3).
    const bool scatterViewModels = scatter && view.view.kind == gpu::ViewKind::Main && fc.scene.viewModelInstances() > 0 &&
                                   fc.quality.number("viewmodel.fov_override_degrees") > 0 && v.visId.valid() && v.visibleClusters.valid();
    auto addScatter = [&](bool fallbackTiles) {
        ID3D12PipelineState* kernel = fc.shaders.compute(linear ? "Passes/Shading/SubsurfaceScatter.OUTPUT1" : "Passes/Shading/SubsurfaceScatter.OUTPUT0");
        const uint32_t listBandCount = o.bands;
        const char* name = fallbackTiles ? (planar ? "m.sss.scatter.fallback.planar" : "m.sss.scatter.fallback") : (planar ? "m.sss.scatter.planar" : "m.sss.scatter");
        fc.graph.addPass(name, QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(v.gbuffer, Use::SrvCompute);
                             b.use(v.depth, Use::SrvCompute);
                             b.use(o.materialWord, Use::SrvCompute);
                             b.use(scatterDiffuse, Use::SrvCompute);
                             b.use(directRadiance, Use::SrvCompute);
                             if (scatterViewModels)
                             {
                                 b.use(v.visId, Use::SrvCompute);
                                 b.use(v.visibleClusters, Use::SrvCompute);
                             }
                             if (o.anisoWord.valid()) b.use(o.anisoWord, Use::SrvCompute);  // (an eye's pixels: the iris mask)
                             if (fallbackTiles)
                             {
                                 b.use(v.shadowOverflowFallbackTiles, Use::SrvCompute);
                                 b.use(fallbackArgs, Use::IndirectArgs);
                             }
                             else
                             {
                                 b.use(o.tiles, Use::SrvCompute);
                                 b.use(o.tileArgs, Use::IndirectArgs);
                                 if (overflow) b.use(v.shadowOverflowTiles, Use::SrvCompute);
                             }
                             b.use(v.color, Use::UavCompute);
                             if (atmosphere)
                                 for (TextureRef t : { r.transmittanceLut, r.multiScatterLut }) b.use(t, Use::SrvCompute);
                             if (air) b.use(v.airVolume, Use::SrvCompute);
                             declareFog(b, r, Use::SrvCompute);
                             b.use(edgeRadiance, Use::UavCompute);
                             b.use(edgeTiles, Use::SrvCompute);
                             if (coverage) b.use(v.coverageTiles, Use::SrvCompute);
                             if (keepWater) b.use(v.waterVis, Use::SrvCompute);
                             useParticles(b);
                             if (histogram.buffer.valid()) b.use(histogram.buffer, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             const uint32_t none = gpu::kNone;
                             uint32_t k32[48];
                             for (uint32_t& w : k32) w = none;
                             k32[0] = c.srv(v.gbuffer);
                             k32[1] = c.srv(v.depth);
                             k32[2] = c.srv(o.materialWord);
                             k32[3] = c.uav(v.color);
                             k32[12] = atmosphere ? c.srv(r.transmittanceLut) : none;
                             k32[13] = atmosphere ? c.srv(r.multiScatterLut) : none;
                             k32[14] = !fallbackTiles && overflow ? c.srv(v.shadowOverflowTiles) : none;  // P[3].z: tiles the fallback run takes
                             k32[15] = air ? c.srv(v.airVolume) : none;
                             k32[18] = experiment;                                                 // P[4].z
                             k32[19] = histogram.buffer.valid() ? c.uav(histogram.buffer) : none;  // P[4].w (the main view's histogram)
                             particleConstants(c, k32 + 22);                                       // P[5].zw
                             k32[24] = c.srv(edgeTiles);                                           // P[6].x
                             k32[25] = coverage ? c.srv(v.coverageTiles) : none;                   // P[6].y
                             k32[26] = asUint(histogram.centreSigma);                              // P[6].z
                             k32[28] = c.uav(edgeRadiance);                                        // P[7].x
                             k32[31] = keepWater ? c.srv(v.waterVis) : none;                       // P[7].w
                             k32[32] = scatterViewModels ? c.srv(v.visId) : none;                  // P[8].x: view models under the projection remap
                             k32[33] = scatterViewModels ? c.srv(v.visibleClusters) : none;        // P[8].y
                             k32[37] = o.anisoWord.valid() ? c.srv(o.anisoWord) : none;            // P[9].y: the class word (an eye's mask)
                             k32[38] = c.srv(scatterDiffuse);                                      // P[9].z: the class's diffuse light per unit f_d
                             k32[40] = c.srv(directRadiance);                                      // P[10].x: its specular light and emission
                             k32[45] = scatterSamples;                                             // P[11].y
                             k32[46] = asUint(scatterMinPixels);                                   // P[11].z
                             c.cmd->SetPipelineState(kernel);
                             c.bindFrameConstants(cb);
                             if (fallbackTiles)
                             {
                                 k32[4] = c.srv(v.shadowOverflowFallbackTiles);
                                 k32[5] = 4;
                                 k32[6] = 0x80000000u | subsurfaceBit;  // P[1].z: the class as a mask
                                 c.computeConstants(k32, 48);
                                 c.cmd->ExecuteIndirect(signature, 1, c.resource(fallbackArgs), 0, nullptr, 0);
                                 return;
                             }
                             const uint32_t cls = (uint32_t)material::ShadeClass::Subsurface;
                             ID3D12Resource* args = c.resource(o.tileArgs);
                             k32[4] = c.srv(o.tiles);
                             k32[6] = cls;
                             for (uint32_t band = 0; band < listBandCount; ++band)
                             {
                                 k32[5] = o.firstTile(cls, band);
                                 c.computeConstants(k32, 48);
                                 c.cmd->ExecuteIndirect(signature, 1, args, o.argsOffset(cls, band), nullptr, 0);
                             }
                         });
    };
    auto markScattered = [&] {
        for (auto& [key, rs] : table.views)
            if (key == view.frameConstants) rs.scattered = true;
    };
    if (part == Part::Scatter)
    {
        if (scatter)
        {
            addScatter(false);
            markScattered();
        }
        return {};
    }
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

        if (scatter)
        {
            // m.sss.clear: the class's diffuse texture to 0 before its kernels add to it (alpha 0: not the class's pixel)
            ID3D12PipelineState* clear = fc.shaders.compute("Passes/Shading/SubsurfaceClear");
            fc.graph.addPass(planar ? "m.sss.clear.planar" : "m.sss.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(scatterDiffuse, Use::UavCompute); },
                             [clear, scatterDiffuse, w = v.view.width, h = v.view.height](PassContext& c) {
                                 const uint32_t k[4] = { c.uav(scatterDiffuse), w, h, 0 };
                                 c.cmd->SetPipelineState(clear);
                                 c.computeConstants(k, 4);
                                 c.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                             });
        }

        if (megaWanted)
        {
            // m.ml.sample, m.ml.trace; then m.ml.shade here (ShadeOpaque.hlsl with MEGA_LIGHTS = 1 on the class tile lists
            // of every band, the LAYERED variant of each class); then m.ml.sets, m.ml.temporal, m.ml.spatial
            MegaLightsOptions mlOptions;
            mlOptions.classWord = o.anisoWord;  // (an eye's iris pixels are weighed on the iris plane: their eye word)
            MegaLightsFrame ml = megaLightsSample(fc, view, o.materialWord, areaLights, ltcSrv, signature, nullptr, mlOptions);
            if (!ml.on) fail("M.shading: shading.mega_lights could not start on a view (its inputs were present)");
            const bool mlMainView = view.view.kind == gpu::ViewKind::Main;
            auto mlKernel = [&](uint32_t layered) {
                const std::string name = std::string("Passes/Shading/MegaLightsShade.AREA") + (areaLights ? "1" : "0") + ".LAYERED" + std::to_string(layered) + ".FULL0";
                return fc.shaders.compute(name.c_str());
            };
            ID3D12PipelineState* mlPlain = mlKernel(0);
            ID3D12PipelineState* mlLayered = layeredMaterials ? mlKernel(1) : nullptr;
            ID3D12PipelineState* mlSheen = sheenMaterials ? mlKernel(2) : nullptr;
            ID3D12PipelineState* mlSubsurface = subsurfaceMaterials ? mlKernel(3) : mlPlain;
            const TextureRef samples = ml.samples, keys = ml.keys, outDiffuse = ml.resolvedDiffuse, outSpecular = ml.resolvedSpecular;
            auto half = [](float f) {  // positive, in the half range (the weight caps)
                uint32_t u;
                std::memcpy(&u, &f, 4);
                const int32_t e = (int32_t)((u >> 23) & 0xFF) - 127 + 15;
                if (e <= 0) return 0u;
                if (e >= 31) return 0x7BFFu;
                return ((uint32_t)e << 10) | ((u >> 13) & 0x3FFu);
            };
            const uint32_t caps = half(ml.maxWeight) | (half(ml.maxWeightHidden) << 16), mlMode = ml.factor | (ml.count << 8);
            const float minWeight = ml.minSampleWeight;
            const uint32_t listBandCount = o.bands;
            fc.graph.addPass("m.ml.shade", QueueType::Graphics,
                             [&](PassBuilder& b) {
                                 b.use(v.gbuffer, Use::SrvCompute);
                                 b.use(v.depth, Use::SrvCompute);
                                 b.use(o.materialWord, Use::SrvCompute);
                                 if (o.anisoWord.valid()) b.use(o.anisoWord, Use::SrvCompute);
                                 b.use(o.tiles, Use::SrvCompute);
                                 b.use(o.tileArgs, Use::IndirectArgs);
                                 if (r.lightFunctions.valid()) b.use(r.lightFunctions, Use::SrvCompute);
                                 b.use(v.froxelLights, Use::SrvCompute);
                                 if (r.fxLights.valid()) b.use(r.fxLights, Use::SrvCompute);
                                 b.use(samples, Use::SrvCompute);
                                 b.use(keys, Use::SrvCompute);
                                 b.use(outDiffuse, Use::UavCompute);
                                 b.use(outSpecular, Use::UavCompute);
                             },
                             [=](PassContext& c) {
                                 const uint32_t none = gpu::kNone;
                                 c.bindFrameConstants(cb);
                                 ID3D12Resource* args = c.resource(o.tileArgs);
                                 for (material::ShadeClass shadeClass : { material::ShadeClass::Opaque, material::ShadeClass::Subsurface, material::ShadeClass::Water,
                                                                          material::ShadeClass::Layered, material::ShadeClass::Sheen })
                                 {
                                     ID3D12PipelineState* kernel = shadeClass == material::ShadeClass::Layered ? mlLayered : (shadeClass == material::ShadeClass::Sheen ? mlSheen : mlPlain);
                                     if (shadeClass == material::ShadeClass::Subsurface) kernel = mlSubsurface;
                                     if (!kernel) continue;
                                     c.cmd->SetPipelineState(kernel);
                                     const uint32_t cls = (uint32_t)shadeClass;
                                     for (uint32_t band = 0; band < listBandCount; ++band)
                                     {
                                         uint32_t k32[48];
                                         for (uint32_t& w : k32) w = none;
                                         k32[0] = c.srv(v.gbuffer);
                                         k32[1] = c.srv(v.depth);
                                         k32[2] = c.srv(o.materialWord);
                                         k32[4] = c.srv(o.tiles);
                                         k32[5] = o.firstTile(cls, band);
                                         k32[6] = cls;
                                         k32[16] = mlMainView ? r.areaLightStable : gpu::kNone;  // P[4].x (B2; planar views: none, as PLANAR = 1)
                                         k32[17] = o.textureTableSrv;
                                         k32[18] = experiment;
                                         k32[20] = c.srv(v.froxelLights);
                                         k32[21] = ltcSrv;
                                         k32[27] = r.lightFunctions.valid() ? c.srv(r.lightFunctions) : none;  // P[6].w (A8)
                                         k32[37] = o.anisoWord.valid() ? c.srv(o.anisoWord) : none;            // P[9].y
                                         k32[41] = c.srv(samples);     // P[10].y
                                         k32[42] = c.srv(keys);        // P[10].z
                                         k32[43] = c.uav(outDiffuse);  // P[10].w
                                         k32[44] = c.uav(outSpecular); // P[11].x
                                         k32[45] = caps;               // P[11].y
                                         std::memcpy(&k32[46], &minWeight, 4);  // P[11].z
                                         k32[47] = mlMode;             // P[11].w
                                         c.computeConstants(k32, 48);
                                         c.cmd->ExecuteIndirect(signature, 1, args, o.argsOffset(cls, band), nullptr, 0);
                                     }
                                 }
                             });
            megaLightsDenoise(fc, view, o.materialWord, ml);
            megaLighting = ml.lighting;
            view.localDirect = megaLighting;
            table.views.back().second.megaLighting = megaLighting;
            if (scatter)
            {
                // m.ml.spatial.sss: the Subsurface class's pixels with the lights' diffuse and specular apart - the diffuse
                // into the class's diffuse texture, the specular for the class's part 2 in place of m.ml.spatial's sum
                std::vector<MegaLightsClassBand> classBands;
                for (uint32_t band = 0; band < o.bands; ++band)
                    classBands.push_back({ o.firstTile((uint32_t)material::ShadeClass::Subsurface, band), o.argsOffset((uint32_t)material::ShadeClass::Subsurface, band) });
                megaLightsSubsurface(fc, view, o.materialWord, ml, o.tiles, o.tileArgs, classBands, (uint32_t)material::ShadeClass::Subsurface, signature, scatterDiffuse,
                                     scatterMlSpecular);
            }
        }

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
        // 14.1/14.2 (L2): the tile lights' classification and corner irradiance, after the edge tiles, before shading.
        RenderGraph::BandedPass tilePass;
        tilePass.name = "tile.lights";
        if (tileLights)
        {
            ID3D12PipelineState* tileKernel = fc.shaders.compute("Passes/Lights/TileLights");
            const uint32_t channelsShared = fc.scene.lightingChannelsShared();  // (a FAR light lights every instance)
            tilePass.setup = [=](PassBuilder& b) {
                b.use(v.depth, Use::SrvCompute);
                b.use(v.gbuffer, Use::SrvCompute);
                b.use(v.froxelLights, Use::SrvCompute);
                if (r.fxLights.valid()) b.use(r.fxLights, Use::SrvCompute);
                b.use(edgeTiles, Use::SrvCompute);
                b.use(tileRecords, Use::UavCompute);
                if (r.vsmTileLit.valid()) b.use(r.vsmTileLit, Use::SrvCompute);  // L3: lit casters may be FAR
            };
            tilePass.execute = [=](PassContext& c) {
                const auto [row0, row1] = passTileRows(c);
                if (row1 <= row0) return;
                const uint32_t k[12] = { c.srv(v.depth), c.srv(v.gbuffer), c.srv(v.froxelLights), c.srv(edgeTiles), c.uav(tileRecords), o.tilesX, row0,
                                         r.vsmTileLit.valid() ? c.srv(r.vsmTileLit) : gpu::kNone, channelsShared, 0, 0, 0 };
                c.cmd->SetPipelineState(tileKernel);
                c.bindFrameConstants(cb);
                c.computeConstants(k, 12);
                c.cmd->Dispatch(o.tilesX, row1 - row0, 1);
            };
        }
        RenderGraph::BandedPass shadePass;
        shadePass.name = "shade";
        shadePass.setup = [=](PassBuilder& b) {
            b.use(v.gbuffer, Use::SrvCompute);
            b.use(v.depth, Use::SrvCompute);
            b.use(o.materialWord, Use::SrvCompute);
            if (o.emissive.valid()) b.use(o.emissive, Use::SrvCompute);
            if (o.anisoWord.valid()) b.use(o.anisoWord, Use::SrvCompute);
            if (areaLobes.valid()) b.use(areaLobes, Use::UavComputeDisjoint);
            b.use(directRadiance, Use::UavComputeDisjoint);
            b.use(o.tiles, Use::SrvCompute);
            b.use(o.tileArgs, Use::IndirectArgs);
            b.use(v.color, Use::UavComputeDisjoint);
            if (v.shadowVisibility.valid()) b.use(v.shadowVisibility, Use::SrvCompute);
            if (r.lightFunctions.valid()) b.use(r.lightFunctions, Use::SrvCompute);  // A8 light functions (E)
            if (v.screenProbes.valid()) b.use(v.screenProbes, Use::SrvCompute);
            if (v.screenProbeMaps.valid()) b.use(v.screenProbeMaps, Use::SrvCompute);
            if (v.reflection.valid()) b.use(v.reflection, Use::SrvCompute);
            declareGiSource(b, giSrc, Use::SrvCompute);  // planar: direct lookups; main: ProbeSrvs.pad1, or the translucency volume
            if (v.giIrradiance.valid()) b.use(v.giIrradiance, Use::SrvCompute);  // R's per-pixel front irradiance (P[9].w)
            if (roughSpecular.valid()) b.use(roughSpecular, Use::SrvCompute);  // P[11].y
            if (shortRangeAO.valid()) b.use(shortRangeAO, Use::SrvCompute);    // P[11].z
            if (backfaceIrradiance.valid()) b.use(backfaceIrradiance, Use::SrvCompute);  // P[11].w
            if (atmosphere)
                for (TextureRef t : { r.transmittanceLut, r.multiScatterLut, r.skyViewLut }) b.use(t, Use::SrvCompute);
            if (air) b.use(v.airVolume, Use::SrvCompute);
            declareFog(b, r, Use::SrvCompute);
            if (froxelLists) b.use(v.froxelLights, Use::SrvCompute);
            if (froxelLists && r.fxLights.valid()) b.use(r.fxLights, Use::SrvCompute);  // v1.81: the lists index the FX light tail
            if (overflow)
            {
                b.use(v.shadowOverflowTiles, Use::SrvCompute);
                b.use(v.shadowOverflow, Use::SrvCompute);
            }
            b.use(edgeRadiance, Use::UavCompute);
            b.use(edgeTiles, Use::SrvCompute);
            if (coverage) b.use(v.coverageTiles, Use::SrvCompute);
            if (keepWater) b.use(v.waterVis, Use::SrvCompute);
            if (waterSun)
            {
                for (TextureRef t : { r.waterSunDepth, r.waterSunNormal, r.waterSunMedium }) b.use(t, Use::SrvCompute);
                b.use(r.waterSunConstants, Use::SrvCompute);
                if (caustics) b.use(r.waterSunCaustics, Use::SrvCompute);
            }
            useParticles(b);
            if (emissiveIrradiance.valid()) b.use(emissiveIrradiance, Use::SrvCompute);  // 14.1b
            if (tileLights) b.use(tileRecords, Use::SrvCompute);  // L2
            if (megaLighting.valid()) b.use(megaLighting, Use::SrvCompute);  // shading.mega_lights (P[10].y)
            if (channelVis)
            {
                b.use(v.visId, Use::SrvCompute);
                b.use(v.visibleClusters, Use::SrvCompute);
            }
            if (r.vsmTileLit.valid()) b.use(r.vsmTileLit, Use::SrvCompute);  // L3 (P[10].w)
            if (meter) b.use(histogram.buffer, Use::UavCompute);
            if (scatter)
            {
                b.use(scatterDiffuse, Use::UavComputeDisjoint);  // shading.subsurface_scatter: the Subsurface class's P[9].z
                if (scatterMlSpecular.valid()) b.use(scatterMlSpecular, Use::SrvCompute);  // ... and its P[10].y
            }
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
                                       v.screenProbeMaps.valid() ? c.srv(v.screenProbeMaps) : none, 0,
                                       keepWater ? c.srv(v.waterVis) : none };  // P[7].w (v1.75)
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
            // Surface classes (Water uses the opaque model until its own is defined; A9 layered materials with the LAYERED
            // variant, Subsurface with its part 1): every class's part 1 (and the A9 lobe kernel before it), one UAV barrier, then
            // every class's part 2 (ShadeOpaque.hlsl header).
            for (uint32_t part = 1; part <= 2; ++part)
            for (material::ShadeClass shadeClass : { material::ShadeClass::Opaque, material::ShadeClass::Subsurface, material::ShadeClass::Water,
                                                     material::ShadeClass::Layered, material::ShadeClass::Sheen })
            for (uint32_t band = firstBand; band < lastBand; ++band)
            {
                if (shadeClass == material::ShadeClass::Layered && !opaqueLayered) break;
                if (shadeClass == material::ShadeClass::Sheen && !opaqueSheen) break;
                if (part == 2 && shadeClass == material::ShadeClass::Opaque && band == firstBand) lobeBarrier(c);  // part 1's writes before part 2's reads
                c.cmd->SetPipelineState(part == 1 ? (shadeClass == material::ShadeClass::Layered ? opaqueLayered : (shadeClass == material::ShadeClass::Sheen ? opaqueSheen : opaque))
                                                  : (shadeClass == material::ShadeClass::Layered ? indirectLayered : (shadeClass == material::ShadeClass::Sheen ? indirectSheen : indirect)));
                if (shadeClass == material::ShadeClass::Subsurface) c.cmd->SetPipelineState(part == 1 ? opaqueSubsurface : indirectSubsurface);
                const uint32_t cls = (uint32_t)shadeClass;
                const uint32_t k[22] = { c.srv(v.gbuffer), c.srv(v.depth), c.srv(o.materialWord), c.uav(v.color),
                                         c.srv(o.tiles), o.firstTile(cls, band), cls, o.emissive.valid() ? c.srv(o.emissive) : none,
                                         v.shadowVisibility.valid() ? c.srv(v.shadowVisibility) : none, v.screenProbes.valid() ? c.srv(v.screenProbes) : none,
                                         v.reflection.valid() ? c.srv(v.reflection) : none,
                                         giSourceWord(c, giSrc),
                                         atm[0], atm[1], overflow ? c.srv(v.shadowOverflowTiles) : none, atm[3], none, o.textureTableSrv, experiment,
                                         none, fx[0], fx[1] };
                uint32_t k32[48] = {};
                std::memcpy(k32, k, sizeof k);
                std::memcpy(k32 + 24, edge, sizeof edge);
                waterSunConstants(c, k32 + 32, 4);                    // P[8], P[9].x (v1.77)
                k32[37] = o.anisoWord.valid() ? c.srv(o.anisoWord) : none;  // P[9].y (A9 anisotropy word)
                k32[30] = overflow ? c.srv(v.shadowOverflow) : none;  // P[7].z
                k32[16] = r.areaLightStable;     // P[4].x (B2)
                k32[19] = meter ? c.uav(histogram.buffer) : gpu::kNone;  // P[4].w exposure histogram
                k32[26] = asUint(histogram.centreSigma);                 // P[6].z
                k32[27] = r.lightFunctions.valid() ? c.srv(r.lightFunctions) : none;  // P[6].w (A8)
                particleConstants(c, k32 + 22);  // P[5].zw
                k32[38] = areaLobes.valid() ? c.uav(areaLobes) : none;  // P[9].z (A9 area-light lobes)
                k32[39] = v.giIrradiance.valid() ? c.srv(v.giIrradiance) : none;  // P[9].w (R's per-pixel front irradiance)
                k32[40] = c.uav(directRadiance);                                 // P[10].x: part 1 -> part 2 direct radiance
                k32[41] = megaLighting.valid() ? c.srv(megaLighting) : none;      // P[10].y: shading.mega_lights' result
                k32[42] = emissiveIrradiance.valid() ? c.srv(emissiveIrradiance) : none;  // P[10].z: 14.1b (P[4].x is B2's mask)
                k32[43] = r.vsmTileLit.valid() ? c.srv(r.vsmTileLit) : none;             // P[10].w: L3 (P[6].z is the histogram's centre weight)
                k32[44] = tileLights ? c.srv(tileRecords) : none;                        // P[11].x: L2 (P[4].w is the histogram)
                k32[45] = roughSpecular.valid() ? c.srv(roughSpecular) : none;           // P[11].y: gi.lumen_only's rough specular
                k32[46] = shortRangeAO.valid() ? c.srv(shortRangeAO) : none;             // P[11].z: ... and short-range AO
                k32[47] = backfaceIrradiance.valid() ? c.srv(backfaceIrradiance) : none;  // P[11].w: ... and Foliage's back side
                if (part == 1)
                {
                    // part 1's own light loop (the froxel lists, without shading.mega_lights): the vis buffer for the pixel's
                    // lighting channels (ShadeOpaque.hlsl P[11].zw; part 2 reads the gather's textures there)
                    k32[46] = channelVis ? c.srv(v.visId) : none;
                    k32[47] = channelVis ? c.srv(v.visibleClusters) : none;
                }
                if (scatter && shadeClass == material::ShadeClass::Subsurface)
                {
                    // shading.subsurface_scatter (SSS_SPLIT kernels): the class's diffuse texture, and the local lights'
                    // specular alone in place of m.ml.spatial's sum (their diffuse is in the diffuse texture)
                    k32[38] = c.uav(scatterDiffuse);                                        // P[9].z
                    k32[41] = scatterMlSpecular.valid() ? c.srv(scatterMlSpecular) : none;  // P[10].y
                }
                ID3D12PipelineState* lobes = shadeClass == material::ShadeClass::Layered ? lobesLayered : (shadeClass == material::ShadeClass::Sheen ? lobesSheen : nullptr);
                if (part == 1 && lobes && areaLobes.valid())
                {
                    // the lobe texture is read by part 2 (after the barrier below)
                    c.cmd->SetPipelineState(lobes);
                    c.computeConstants(k32, 48);
                    c.cmd->ExecuteIndirect(signature, 1, args, o.argsOffset(cls, band), nullptr, 0);
                    c.cmd->SetPipelineState(shadeClass == material::ShadeClass::Layered ? opaqueLayered : opaqueSheen);
                }
                c.computeConstants(k32, 48);
                c.cmd->ExecuteIndirect(signature, 1, args, o.argsOffset(cls, band), nullptr, 0);
            }
        };
        if (tileLights) return { detectPass, tilePass, shadePass };
        return { detectPass, shadePass };
    }

    // Tiles over S's overflow capacity (INTERFACES 7.3): every non-sky class of the tile, lights past the third from S's
    // VSM directly (FALLBACK=1; its extra registers stay out of the main kernel). An empty list costs one argument read.
    // (a caller that records the two parts without shadingScatter: the scatter pass here, before the composites read the
    // edge / coverage radiance it keeps)
    if (scatter && !res.scattered)
    {
        addScatter(false);
        markScattered();
    }
    if (fallback)
    {
        // one run per LAYERED variant over its classes (P[1].z mask, bit 31): the clearcoat or plain variant over every
        // class but Sheen and Subsurface, then the sheen variant over Sheen and the Subsurface variant over Subsurface
        // (shading.subsurface_scatter: its SSS_SPLIT kernels; m.sss.scatter.fallback follows the pass)
        const uint32_t sheenBit = 1u << (uint32_t)material::ShadeClass::Sheen;
        std::vector<std::pair<ID3D12PipelineState*, uint32_t>> fallbackRuns = {
            { opaqueKernel(true, layeredMaterials ? 1 : 0), 0xFFFFFFFFu & ~(sheenMaterials ? sheenBit : 0u) & ~(subsurfaceMaterials ? subsurfaceBit : 0u) } };
        if (sheenMaterials) fallbackRuns.push_back({ opaqueKernel(true, 2), 0x80000000u | sheenBit });
        const size_t subsurfaceRun = scatter ? fallbackRuns.size() : SIZE_MAX;  // the run whose kernels take the class's diffuse texture
        if (subsurfaceMaterials) fallbackRuns.push_back({ scatter ? subsurfaceKernel(1, true) : opaqueKernel(true, 3), 0x80000000u | subsurfaceBit });
        std::vector<ID3D12PipelineState*> fallbackIndirect = { indirectKernel(true, layeredMaterials ? 1 : 0) };
        if (sheenMaterials) fallbackIndirect.push_back(indirectKernel(true, 2));
        if (subsurfaceMaterials) fallbackIndirect.push_back(scatter ? subsurfaceKernel(2, true) : indirectKernel(true, 0));
        std::vector<ID3D12PipelineState*> fallbackLobes = { lobesOn && anisoMaterials ? lobeKernel(true, 1) : nullptr };
        if (sheenMaterials) fallbackLobes.push_back(lobesOn ? lobeKernel(true, 2) : nullptr);
        if (subsurfaceMaterials) fallbackLobes.push_back(nullptr);
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
                             if (o.anisoWord.valid()) b.use(o.anisoWord, Use::SrvCompute);
                             if (areaLobes.valid()) b.use(areaLobes, Use::UavCompute);
                             b.use(directRadiance, Use::UavComputeDisjoint);
                             b.use(v.shadowOverflowFallbackTiles, Use::SrvCompute);
                             b.use(fallbackArgs, Use::IndirectArgs);
                             b.use(v.color, Use::UavComputeDisjoint);
                             if (histogram.buffer.valid()) b.use(histogram.buffer, Use::UavCompute);
                             if (v.shadowVisibility.valid()) b.use(v.shadowVisibility, Use::SrvCompute);
                             if (r.lightFunctions.valid()) b.use(r.lightFunctions, Use::SrvCompute);  // A8 light functions (E)
                             if (v.screenProbes.valid()) b.use(v.screenProbes, Use::SrvCompute);
                             if (v.screenProbeMaps.valid()) b.use(v.screenProbeMaps, Use::SrvCompute);
                             if (v.reflection.valid()) b.use(v.reflection, Use::SrvCompute);
                             if (v.giIrradiance.valid()) b.use(v.giIrradiance, Use::SrvCompute);  // P[9].w
                             declareGiSource(b, giSrc, Use::SrvCompute);
                             if (roughSpecular.valid()) b.use(roughSpecular, Use::SrvCompute);  // P[11].y
                             if (shortRangeAO.valid()) b.use(shortRangeAO, Use::SrvCompute);    // P[11].z
                             if (backfaceIrradiance.valid()) b.use(backfaceIrradiance, Use::SrvCompute);  // P[11].w
                             if (atmosphere)
                                 for (TextureRef t : { r.transmittanceLut, r.multiScatterLut }) b.use(t, Use::SrvCompute);
                             if (air) b.use(v.airVolume, Use::SrvCompute);
                             declareFog(b, r, Use::SrvCompute);
                             if (froxelLists) b.use(v.froxelLights, Use::SrvCompute);
            if (froxelLists && r.fxLights.valid()) b.use(r.fxLights, Use::SrvCompute);  // v1.81: the lists index the FX light tail
                             if (vsm)
                                 for (BufferRef vb : { r.vsmPageTable, r.vsmBlocks, r.vsmSearchBound }) b.use(vb, Use::SrvCompute);
                             // ShadowSrvs.pool: the one-path page atlas (v1.43) once S publishes it, else the page pool
                             if (vsm && r.vsmAtlas.valid()) b.use(r.vsmAtlas, Use::SrvCompute);
                             else if (vsm) b.use(r.vsmPool, Use::SrvCompute);
                             if (vsm && r.vsmLayers.valid()) b.use(r.vsmLayers, Use::SrvCompute);
                             b.use(edgeRadiance, Use::UavCompute);
                             b.use(edgeTiles, Use::SrvCompute);
                             if (coverage) b.use(v.coverageTiles, Use::SrvCompute);
                             if (keepWater) b.use(v.waterVis, Use::SrvCompute);
                             if (waterSun)
                             {
                                 for (TextureRef t : { r.waterSunDepth, r.waterSunNormal, r.waterSunMedium }) b.use(t, Use::SrvCompute);
                                 b.use(r.waterSunConstants, Use::SrvCompute);
                                 if (caustics) b.use(r.waterSunCaustics, Use::SrvCompute);
                             }
                             useParticles(b);
                             if (emissiveIrradiance.valid()) b.use(emissiveIrradiance, Use::SrvCompute);  // 14.1b
                             if (tileLights) b.use(tileRecords, Use::SrvCompute);  // L2
                             if (megaLighting.valid()) b.use(megaLighting, Use::SrvCompute);  // shading.mega_lights (P[10].y)
                             if (channelVis)
                             {
                                 b.use(v.visId, Use::SrvCompute);
                                 b.use(v.visibleClusters, Use::SrvCompute);
                             }
                             if (scatter)
                             {
                                 b.use(scatterDiffuse, Use::UavComputeDisjoint);  // shading.subsurface_scatter: the Subsurface run's P[9].z
                                 if (scatterMlSpecular.valid()) b.use(scatterMlSpecular, Use::SrvCompute);  // ... and its P[10].y
                             }
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
                                                      v.reflection.valid() ? c.srv(v.reflection) : none, giSourceWord(c, giSrc),
                                                      atmosphere ? c.srv(r.transmittanceLut) : none, atmosphere ? c.srv(r.multiScatterLut) : none, none,
                                                      air ? c.srv(v.airVolume) : none, none, o.textureTableSrv, experiment,
                                                      none,
                                                      froxelLists ? c.srv(v.froxelLights) : none, ltcSrv };
                             const uint32_t edge[8] = { c.srv(edgeTiles), coverage ? c.srv(v.coverageTiles) : none, 0, 0, c.uav(edgeRadiance),
                                                        v.screenProbeMaps.valid() ? c.srv(v.screenProbeMaps) : none, shadowSrvs,
                                                        keepWater ? c.srv(v.waterVis) : none };  // P[7].w (v1.75)
                             uint32_t k32[48] = {};
                             std::memcpy(k32, k, sizeof k);
                             std::memcpy(k32 + 24, edge, sizeof edge);
                             waterSunConstants(c, k32 + 32, 4);  // P[8], P[9].x (v1.77)
                             k32[40] = c.uav(directRadiance);           // P[10].x: part 1 -> part 2 direct radiance
                             k32[41] = megaLighting.valid() ? c.srv(megaLighting) : none;  // P[10].y
                             k32[42] = emissiveIrradiance.valid() ? c.srv(emissiveIrradiance) : none;  // P[10].z: 14.1b
                             k32[43] = none;                            // P[10].w (the fallback kernel reads no classification)
                             k32[44] = none;                            // P[11].x (the fallback kernel keeps every light per pixel)
                             k32[45] = roughSpecular.valid() ? c.srv(roughSpecular) : none;  // P[11].y
                             k32[46] = shortRangeAO.valid() ? c.srv(shortRangeAO) : none;    // P[11].z
                             k32[47] = backfaceIrradiance.valid() ? c.srv(backfaceIrradiance) : none;  // P[11].w
                             k32[37] = o.anisoWord.valid() ? c.srv(o.anisoWord) : gpu::kNone;  // P[9].y (A9 anisotropy word)
                             particleConstants(c, k32 + 22);  // P[5].zw
                             k32[16] = r.areaLightStable;     // P[4].x (B2)
                             // P[4].w, P[6].z: the exposure histogram and its centre weight. The main kernel leaves the tiles over S's
                             // overflow capacity before shading or metering them, so the fallback meters its tiles itself (they
                             // were not metered at all: the first frames' and steady overflow frames' EV left those pixels out).
                             k32[19] = histogram.buffer.valid() ? c.uav(histogram.buffer) : gpu::kNone;
                             k32[26] = asUint(histogram.centreSigma);
                             k32[27] = r.lightFunctions.valid() ? c.srv(r.lightFunctions) : none;  // P[6].w (A8)
                             k32[38] = areaLobes.valid() ? c.uav(areaLobes) : none;  // P[9].z (A9 area-light lobes)
                             k32[39] = v.giIrradiance.valid() ? c.srv(v.giIrradiance) : none;  // P[9].w
                             // (the Subsurface run with shading.subsurface_scatter: the class's diffuse texture in P[9].z and the
                             // local lights' specular alone in P[10].y, as in the banded pass)
                             const uint32_t lobeWord = k32[38], localWord = k32[41];
                             auto runWords = [&](size_t run) {
                                 const bool split = run == subsurfaceRun;
                                 k32[38] = split ? c.uav(scatterDiffuse) : lobeWord;
                                 k32[41] = split ? (scatterMlSpecular.valid() ? c.srv(scatterMlSpecular) : none) : localWord;
                             };
                             // every run's part 1 (with its lobe kernel), one barrier, every run's part 2
                             // (part 1's P[11].zw: the vis buffer for the pixel's lighting channels; part 2's: the gather's textures)
                             const uint32_t gatherZ = k32[46], gatherW = k32[47];
                             k32[46] = channelVis ? c.srv(v.visId) : none;
                             k32[47] = channelVis ? c.srv(v.visibleClusters) : none;
                             for (size_t run = 0; run < fallbackRuns.size(); ++run)
                             {
                                 const auto& [kernel, classes] = fallbackRuns[run];
                                 k32[6] = classes;  // P[1].z
                                 runWords(run);
                                 c.bindFrameConstants(cb);
                                 if (fallbackLobes[run] && areaLobes.valid())
                                 {
                                     c.cmd->SetPipelineState(fallbackLobes[run]);
                                     c.computeConstants(k32, 48);
                                     c.cmd->ExecuteIndirect(signature, 1, c.resource(fallbackArgs), 0, nullptr, 0);
                                 }
                                 c.cmd->SetPipelineState(kernel);
                                 c.computeConstants(k32, 48);
                                 c.cmd->ExecuteIndirect(signature, 1, c.resource(fallbackArgs), 0, nullptr, 0);
                             }
                             lobeBarrier(c);
                             k32[46] = gatherZ, k32[47] = gatherW;
                             for (size_t run = 0; run < fallbackRuns.size(); ++run)
                             {
                                 k32[6] = fallbackRuns[run].second;  // P[1].z
                                 runWords(run);
                                 c.cmd->SetPipelineState(fallbackIndirect[run]);
                                 c.computeConstants(k32, 48);
                                 c.cmd->ExecuteIndirect(signature, 1, c.resource(fallbackArgs), 0, nullptr, 0);
                             }
                         });
        if (scatter) addScatter(true);
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
        ring.glass[slot] = false;  // (set again by the glass composite when it runs)
        ring.dof[slot] = false;    // (and by the depth of field)
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

        // shading.mega_lights: the coverage layer's own MegaLights instance (MegaLightsCoverage.hlsl; as Unreal's second
        // instance on the hair visibility samples). The surface = each coverage pixel's nearest opaque cluster fragment in
        // front of band A; then the sample, trace, shade (full-screen kernel), temporal and spatial passes on it, kept
        // divided by the modulation factors. The fragment kernels add it times each fragment's factors (P[11].xy) in place
        // of their loop over the froxel list.
        TextureRef covMlDiffuse, covMlSpecular;
#if UNX_M_HAS_RAYTRACING
        if (megaLighting.valid())
        {
            const uint32_t W = v.view.width, H = v.view.height;
            const TextureRef nearest = g.createTexture({ "m.ml.cov nearest depth", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
            const TextureRef element = g.createTexture({ "m.ml.cov element", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
            const TextureRef covGbuffer = g.createTexture({ "m.ml.cov gbuffer", W, H, 1, 1, DXGI_FORMAT_R32G32_UINT });
            const TextureRef covWord = g.createTexture({ "m.ml.cov material word", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
            const TextureRef covDepth = g.createTexture({ "m.ml.cov depth", W, H, 1, 1, DXGI_FORMAT_R32_FLOAT });
            // (the nearest fragment's lighting channels: the instance's samples take no light its instance is not lit by)
            const TextureRef covChannels = g.createTexture({ "m.ml.cov lighting channels", W, H, 1, 1, DXGI_FORMAT_R8_UINT });
            std::array<ID3D12PipelineState*, 4> covKernel{};
            for (int mode = 0; mode < 4; ++mode) covKernel[mode] = fc.shaders.compute(("Passes/Shading/MegaLightsCoverage.MODE" + std::to_string(mode)).c_str());
            g.addPass("m.ml.cov.nearest", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(v.coverageRecords, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::IndirectArgs);
                          b.use(v.depth, Use::SrvCompute);
                          b.use(nearest, Use::UavCompute);
                          b.use(element, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[8] = { c.srv(v.coverageRecords), c.srv(v.coverageTileList), c.uav(nearest), c.uav(element), c.srv(v.depth), 0, 0, 0 };
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 8);
                          c.cmd->SetPipelineState(covKernel[0]);
                          c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                          // the per-record passes: V's arguments over the record blocks (tile list header words 8..10);
                          // the depths are complete before the elements are chosen (one global UAV barrier between them)
                          D3D12_GLOBAL_BARRIER gb{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                                   D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
                          D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
                          group.pGlobalBarriers = &gb;
                          for (int mode = 1; mode <= 2; ++mode)
                          {
                              c.cmd->Barrier(1, &group);
                              c.cmd->SetPipelineState(covKernel[mode]);
                              c.cmd->ExecuteIndirect(signature, 1, c.resource(v.coverageTileList), 32, nullptr, 0);
                          }
                      });
            g.addPass("m.ml.cov.surface", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(v.coverageRecords, Use::SrvCompute);
                          b.use(v.visibleClusters, Use::SrvCompute);
                          b.use(nearest, Use::SrvCompute);
                          b.use(element, Use::SrvCompute);
                          for (TextureRef t : { covGbuffer, covWord, covDepth, covChannels }) b.use(t, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[12] = { c.srv(v.coverageRecords), gpu::kNone, c.srv(nearest), c.srv(element), gpu::kNone, c.srv(v.visibleClusters), 0, 0,
                                                   c.uav(covGbuffer), c.uav(covWord), c.uav(covDepth), c.uav(covChannels) };
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 12);
                          c.cmd->SetPipelineState(covKernel[3]);
                          c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                      });
            // the instance: the view with the coverage surface as its depth and G-buffer (no vis buffer: its history is
            // reprojected as static)
            ViewResources cview = view;
            cview.depth = covDepth;
            cview.gbuffer = covGbuffer;
            cview.visId = {};
            MegaLightsOptions covOptions;
            covOptions.channels = covChannels;
            MegaLightsFrame cml = megaLightsSample(fc, cview, covWord, areaLights, ltcSrv, signature, "coverage", covOptions);
            if (!cml.on) fail("M.shading: the coverage layer's shading.mega_lights instance could not start");
            ID3D12PipelineState* covShade = fc.shaders.compute((std::string("Passes/Shading/MegaLightsShade.AREA") + (areaLights ? "1" : "0") + ".LAYERED0.FULL1").c_str());
            // Subsurface fragments take that class's variant in a dispatch of its own - its two specular lobes and the light
            // through thin parts, as the fragment kernels' own light loop shades them (CoverageShade.hlsli) - and the plain
            // kernel leaves them out. Scenes without such materials run the one dispatch as before.
            ID3D12PipelineState* covShadeSubsurface =
                subsurfaceMaterials ? fc.shaders.compute((std::string("Passes/Shading/MegaLightsShade.AREA") + (areaLights ? "1" : "0") + ".LAYERED3.FULL1").c_str()) : nullptr;
            auto half = [](float f) {  // positive, in the half range (the weight caps)
                uint32_t u;
                std::memcpy(&u, &f, 4);
                const int32_t e = (int32_t)((u >> 23) & 0xFF) - 127 + 15;
                if (e <= 0) return 0u;
                if (e >= 31) return 0x7BFFu;
                return ((uint32_t)e << 10) | ((u >> 13) & 0x3FFu);
            };
            const uint32_t caps = half(cml.maxWeight) | (half(cml.maxWeightHidden) << 16), mlMode = cml.factor | (cml.count << 8);
            const float minWeight = cml.minSampleWeight;
            const TextureRef samples = cml.samples, keys = cml.keys, outDiffuse = cml.resolvedDiffuse, outSpecular = cml.resolvedSpecular;
            const bool covMainView = view.view.kind == gpu::ViewKind::Main;
            g.addPass("m.ml.cov.shade", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          for (TextureRef t : { covGbuffer, covDepth, covWord, samples, keys }) b.use(t, Use::SrvCompute);
                          if (r.lightFunctions.valid()) b.use(r.lightFunctions, Use::SrvCompute);
                          b.use(v.froxelLights, Use::SrvCompute);
                          if (r.fxLights.valid()) b.use(r.fxLights, Use::SrvCompute);
                          b.use(outDiffuse, Use::UavCompute);
                          b.use(outSpecular, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k32[48];
                          for (uint32_t& w : k32) w = gpu::kNone;
                          k32[0] = c.srv(covGbuffer);
                          k32[1] = c.srv(covDepth);
                          k32[2] = c.srv(covWord);
                          k32[5] = 0;
                          // P[1].z: every shade class but the sky (the plain kernel: a layered material's base), less the
                          // Subsurface class when its own dispatch follows
                          k32[6] = covShadeSubsurface ? 0xFFFFFFFEu & ~subsurfaceBit : 0xFFFFFFFEu;
                          k32[16] = covMainView ? r.areaLightStable : gpu::kNone;  // P[4].x (B2)
                          k32[17] = o.textureTableSrv;
                          k32[18] = experiment;
                          k32[20] = c.srv(v.froxelLights);
                          k32[21] = ltcSrv;
                          k32[27] = r.lightFunctions.valid() ? c.srv(r.lightFunctions) : gpu::kNone;  // P[6].w (A8)
                          k32[41] = c.srv(samples);     // P[10].y
                          k32[42] = c.srv(keys);        // P[10].z
                          k32[43] = c.uav(outDiffuse);  // P[10].w
                          k32[44] = c.uav(outSpecular); // P[11].x
                          k32[45] = caps;               // P[11].y
                          std::memcpy(&k32[46], &minWeight, 4);  // P[11].z
                          k32[47] = mlMode;             // P[11].w
                          c.bindFrameConstants(cb);
                          c.cmd->SetPipelineState(covShade);
                          c.computeConstants(k32, 48);
                          c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                          if (covShadeSubsurface)
                          {
                              k32[6] = 0x80000000u | subsurfaceBit;  // (the class as a mask; its pixels are the first dispatch's gaps)
                              c.cmd->SetPipelineState(covShadeSubsurface);
                              c.computeConstants(k32, 48);
                              c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                          }
                      });
            megaLightsDenoise(fc, cview, covWord, cml, true);
            covMlDiffuse = cml.lighting;
            covMlSpecular = cml.lightingSpecular;
        }
#endif
        const BufferRef state = g.createBuffer({ "m.coverage state", 16 * 4, 0 });  // COVS_WORDS
        const BufferRef args = g.createBuffer({ "m.coverage args", 16 * 4, 0 });
        const BufferRef pairs = g.createBuffer({ "m.coverage pairs", std::max<uint64_t>(capacity, 1) * 8, 0 });
        const BufferRef heavy = g.createBuffer({ "m.coverage heavy", heavyCap * 16 * 4, 0 });
        const BufferRef cursors = g.createBuffer({ "m.coverage cursors", cursorCap * 4, 0 });
        const BufferRef lists = g.createBuffer({ "m.coverage open", heavyCap * 2 * 4, 0 });
        const uint32_t hcap = (uint32_t)heavyCap, ccap = (uint32_t)cursorCap;
        const std::string area = areaLights ? "1" : "0", output = linear ? "1" : "0";
        ID3D12PipelineState* begin[3] = { fc.shaders.compute("Passes/Shading/CoverageBegin.MODE0"), fc.shaders.compute("Passes/Shading/CoverageBegin.MODE1"),
                                          fc.shaders.compute("Passes/Shading/CoverageBegin.MODE2") };
        // The fragment shading is split in two parts (the DXIL limit, CoverageComposite.hlsl): part 1 the direct light into
        // a per-pixel sum, part 2 the indirect light and the output; the heavy rounds run once per part.
        // shading.coverage_compact (CoverageShade.hlsli; written 2026-10-03, built, not run): the light pixels in the
        // compact form instead - the walk weights the fragments and lists those with weight per tile (CoverageWalk), two
        // kernels shade the list one fragment per lane (CoverageShadeList: the same two parts) and the gather sums each
        // pixel's entries with the band A remainder (CoverageGather). The same weights and shading calls; the sums stay
        // in float. Entry buffers: one entry per record at most (V's record capacity).
        const bool compact = fc.quality.has("shading.coverage_compact") && fc.quality.boolean("shading.coverage_compact");
        ID3D12PipelineState* light1 = compact ? nullptr : fc.shaders.compute(("Passes/Shading/CoverageComposite.PART1.OUTPUT" + output + ".AREA" + area).c_str());
        ID3D12PipelineState* light2 = compact ? nullptr : fc.shaders.compute(("Passes/Shading/CoverageComposite.PART2.OUTPUT" + output + ".AREA" + area).c_str());
        ID3D12PipelineState* walk = compact ? fc.shaders.compute("Passes/Shading/CoverageWalk") : nullptr;
        // The list's shading in one kernel (direct and indirect light together: the surface, material and air once per
        // fragment) where the fragments do not run the area lights' loop - no area light in the scene, or the local
        // lights come from the coverage MegaLights instance; else in the composite's two parts (the one-kernel form with
        // that loop is over the DXIL limit).
        const bool compactWhole = compact && (!areaLights || covMlDiffuse.valid());
        ID3D12PipelineState* shadeList[2] = {
            !compact ? nullptr : fc.shaders.compute(compactWhole ? "Passes/Shading/CoverageShadeWhole" : ("Passes/Shading/CoverageShadeList.PART1.AREA" + area).c_str()),
            !compact || compactWhole ? nullptr : fc.shaders.compute(("Passes/Shading/CoverageShadeList.PART2.AREA" + area).c_str())
        };
        ID3D12PipelineState* gather = compact ? fc.shaders.compute(("Passes/Shading/CoverageGather.OUTPUT" + output).c_str()) : nullptr;
        const BufferRef covEntries = compact ? g.createBuffer({ "m.coverage visible", std::max<uint64_t>(capacity, 1) * 8, 0 }) : BufferRef{};
        const BufferRef covRadiance = compact ? g.createBuffer({ "m.coverage radiance", std::max<uint64_t>(capacity, 1) * 12, 0 }) : BufferRef{};
        const BufferRef tileSpans = compact ? g.createBuffer({ "m.coverage tile spans", tiles * 8, 0 }) : BufferRef{};
        const BufferRef pixelSpans = compact ? g.createBuffer({ "m.coverage pixel spans", tiles * 64 * 8, 0 }) : BufferRef{};
        ID3D12PipelineState* heavyReset = fc.shaders.compute("Passes/Shading/CoverageHeavyReset");
        const TextureRef directSum = compact ? TextureRef{} : g.createTexture({ "m.coverage direct", v.view.width, v.view.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        ID3D12PipelineState* sort[2] = { fc.shaders.compute("Passes/Shading/CoverageHeavySort.SIZE0"), fc.shaders.compute("Passes/Shading/CoverageHeavySort.SIZE1") };
        ID3D12PipelineState* heavyRounds[2] = { fc.shaders.compute(("Passes/Shading/CoverageHeavyRound.PART1.AREA" + area).c_str()),
                                                fc.shaders.compute(("Passes/Shading/CoverageHeavyRound.PART2.AREA" + area).c_str()) };
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
        // E's grooms between the layer's fragments and the sun (shading.hair_shadows; Passes/Hair/HairShadow.hlsl MODE 2):
        // per pixel with records the hair's transmittance at the 4 depths of S's sun profile. The cluster fragments
        // multiply their sun visibility by it (CoverageShade.hlsli covFragmentShadow, P[11].z); the hair records count
        // the hair themselves. Frames without a density volume record nothing.
        TextureRef hairSun;
        if (fragmentShadows && (!fc.quality.has("shading.hair_shadows") || fc.quality.boolean("shading.hair_shadows")) && r.hairDensityParams.valid() &&
            r.hairDensity.valid() && r.hairDensityCoarse.valid())
        {
            const uint32_t hairSteps = fc.quality.has("shading.hair_shadow_steps") ? (uint32_t)fc.quality.integer("shading.hair_shadow_steps") : 32u;
            if (hairSteps < 2 || hairSteps > 128) fail("shading.hair_shadow_steps must be in [2, 128]");
            const uint32_t hairJitter = !fc.quality.has("shading.hair_march_jitter") || fc.quality.boolean("shading.hair_march_jitter") ? 1u : 0u;
            const float3 originOffset = v.view.position - r.hairOrigin;
            const BufferRef hairParams = r.hairDensityParams;
            const TextureRef hairFine = r.hairDensity, hairCoarse = r.hairDensityCoarse, ranges = v.coverageDepthRange;
            const uint32_t hairW = v.view.width, hairH = v.view.height;
            hairSun = g.createTexture({ "m.coverage hair sun", hairW, hairH, 1, 1, DXGI_FORMAT_R32_UINT });
            ID3D12PipelineState* hairProfile = fc.shaders.compute("Passes/Hair/HairShadow.MODE2");
            g.addPass("m.coverage.hairsun", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(ranges, Use::SrvCompute);
                          b.use(hairParams, Use::SrvCompute);
                          b.use(hairFine, Use::SrvCompute);
                          b.use(hairCoarse, Use::SrvCompute);
                          b.use(hairSun, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k[8] = { c.srv(hairParams), c.srv(ranges), c.uav(hairSun), hairSteps, 0, 0, 0, hairJitter };
                          const float o3[3] = { originOffset.x, originOffset.y, originOffset.z };
                          std::memcpy(&k[4], o3, 12);
                          c.cmd->SetPipelineState(hairProfile);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch((hairW + 7) / 8, (hairH + 7) / 8, 1);
                      });
        }
        // The fragment shading kernels' resources (CoverageShade.hlsli: P[1], P[3], P[4], P[5].x).
        auto useShading = [=](PassBuilder& b) {
            b.use(v.coverageRecords, Use::SrvCompute);
            if (hairSun.valid()) b.use(hairSun, Use::SrvCompute);  // (P[11].z)
            b.use(v.visibleClusters, Use::SrvCompute);
            b.use(v.depth, Use::SrvCompute);
            if (froxelLists) b.use(v.froxelLights, Use::SrvCompute);
            if (froxelLists && r.fxLights.valid()) b.use(r.fxLights, Use::SrvCompute);  // v1.81: the lists index the FX light tail
            if (atmosphere)
                for (TextureRef t : { r.transmittanceLut, r.multiScatterLut }) b.use(t, Use::SrvCompute);
            if (air) b.use(v.airVolume, Use::SrvCompute);
            declareFog(b, r, Use::SrvCompute);
            if (v.screenProbes.valid()) b.use(v.screenProbes, Use::SrvCompute);
            if (v.screenProbeMaps.valid()) b.use(v.screenProbeMaps, Use::SrvCompute);
            declareGiSource(b, giSrc, Use::SrvCompute);
            if (r.surfaceConstants.valid()) b.use(r.surfaceConstants, Use::SrvCompute);  // A7 surface layers (one buffer)
            if (r.lightFunctions.valid()) b.use(r.lightFunctions, Use::SrvCompute);    // A8 light functions
            if (r.rainShadow.valid()) b.use(r.rainShadow, Use::SrvCompute);
            if (covMlDiffuse.valid())
            {
                b.use(covMlDiffuse, Use::SrvCompute);  // shading.mega_lights: the coverage instance (P[11].xy)
                b.use(covMlSpecular, Use::SrvCompute);
            }
            if (fragmentShadows)
            {
                b.use(v.shadowFragmentVisibility, Use::SrvCompute);
                b.use(v.coverageDepthRange, Use::SrvCompute);
                if (v.shadowFragmentSun.valid()) b.use(v.shadowFragmentSun, Use::SrvCompute);
            }
            if (waterSun)
            {
                for (TextureRef t : { r.waterSunDepth, r.waterSunNormal, r.waterSunMedium }) b.use(t, Use::SrvCompute);
                b.use(r.waterSunConstants, Use::SrvCompute);
                if (caustics) b.use(r.waterSunCaustics, Use::SrvCompute);
            }
        };
        // P[6].zw, P[7].x of the fragment kernels (CoverageShade.hlsli): R's GI cache, S's per-record sun, V's depth range.
        auto fragmentConstants = [=](PassContext& c, uint32_t* k) {
            k[26] = giSourceWord(c, giSrc);
            k[27] = fragmentShadows && v.shadowFragmentSun.valid() ? c.srv(v.shadowFragmentSun) : gpu::kNone;
            k[28] = fragmentShadows ? c.srv(v.coverageDepthRange) : gpu::kNone;
            k[29] = r.areaLightStable;  // P[7].y (B2)
            k[30] = r.surfaceConstants.valid() ? c.srv(r.surfaceConstants) : gpu::kNone;  // P[7].z (A7 surface layers)
            k[31] = r.weather != UINT32_MAX ? r.weather : gpu::kNone;                  // P[7].w
            k[32] = r.lightFunctions.valid() ? c.srv(r.lightFunctions) : gpu::kNone;  // P[8].x (A8; the arrays hold 48)
            k[33] = v.coverageRecordRadiance.valid() ? c.srv(v.coverageRecordRadiance) : gpu::kNone;  // P[8].y (v1.75)
            waterSunConstants(c, k + 36, 4);  // P[9], P[10].x (v1.77)
            k[44] = covMlDiffuse.valid() ? c.srv(covMlDiffuse) : gpu::kNone;   // P[11].x: shading.mega_lights' coverage instance
            k[45] = covMlSpecular.valid() ? c.srv(covMlSpecular) : gpu::kNone;  // P[11].y
            k[46] = hairSun.valid() ? c.srv(hairSun) : gpu::kNone;              // P[11].z: the grooms towards the sun
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
        // v1.75: the special records of M's pre-shaded classes (kind 5: Cut, Terrain), shaded once each before the composite
        // reads them (CoverageSpecial.hlsl): the material of each class into M's scratch (MODE 1 Cut, 2 Terrain), then the
        // lighting of every kind 5 entry (MODE 5 direct, MODE 6 indirect) - apart, as together they exceed the 200 KB DXIL limit.
        const BufferRef shaded = v.coverageRecordRadiance, special = v.coverageSpecial;
        if (shaded.valid())
        {
            const uint64_t entries = std::max<uint64_t>((fc.graph.desc(special).size / 4 - 4) / 2, 1);  // header 4 words, 2 per entry
            const BufferRef scratch = g.createBuffer({ "m.coverage special materials", entries * 48, 0 });
            for (const char* mode : { "1", "2", "4" })  // Cut, Terrain, A9 layered Standard
            {
                ID3D12PipelineState* kernel = fc.shaders.compute((std::string("Passes/Shading/CoverageSpecial.MODE") + mode + ".AREA0").c_str());
                g.addPass("m.coverage.special material", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(v.coverageRecords, Use::SrvCompute);
                              b.use(v.visibleClusters, Use::SrvCompute);
                              b.use(special, Use::SrvCompute);
                              b.use(special, Use::IndirectArgs);
                              b.use(v.coverageTileList, Use::SrvCompute);
                              b.use(scratch, Use::UavComputeDisjoint);  // (each entry is one class: the kernels write disjoint slots)
                          },
                          [=](PassContext& c) {
                              uint32_t k[8] = { c.srv(v.coverageRecords), c.srv(special), c.srv(v.coverageTileList), c.uav(scratch),
                                                c.srv(v.visibleClusters), o.textureTableSrv, 0, 0 };
                              c.cmd->SetPipelineState(kernel);
                              c.bindFrameConstants(cb);
                              c.computeConstants(k, 8);
                              c.cmd->ExecuteIndirect(signature, 1, c.resource(special), 4, nullptr, 0);  // header words 1..3
                          });
            }
            // the lighting of every kind 5 entry in two kernels (DXIL limit):
            // MODE 5 the direct part into a second scratch, MODE 6 the indirect part plus it into the record radiance
            const BufferRef direct = g.createBuffer({ "m.coverage special direct", entries * 16, 0 });
            auto addLight = [&](const char* mode) {
                ID3D12PipelineState* lit = fc.shaders.compute((std::string("Passes/Shading/CoverageSpecial.MODE") + mode + ".AREA" + area).c_str());
                const bool toDirect = mode[0] == '5', fromDirect = mode[0] == '6';
                g.addPass("m.coverage.special", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              useShading(b);
                              b.use(special, Use::SrvCompute);
                              b.use(special, Use::IndirectArgs);
                              b.use(v.coverageTileList, Use::SrvCompute);
                              b.use(scratch, Use::SrvCompute);
                              if (fromDirect) b.use(direct, Use::SrvCompute);
                              b.use(toDirect ? direct : shaded, Use::UavCompute);
                          },
                          [=](PassContext& c) {
                              uint32_t k[24] = { c.srv(v.coverageRecords), c.srv(special), c.srv(v.coverageTileList), toDirect ? c.uav(direct) : c.uav(shaded) };
                              shadingConstants(c, k, gpu::kNone);
                              k[21] = c.srv(scratch);                         // P[5].y
                              k[22] = fromDirect ? c.srv(direct) : gpu::kNone;  // P[5].z
                              uint32_t k32[48] = {};
                              std::memcpy(k32, k, sizeof k);
                              k32[24] = k32[25] = gpu::kNone;
                              fragmentConstants(c, k32);  // P[6].zw, P[7], P[8].x
                              k32[33] = gpu::kNone;       // (the kernels write the record radiance)
                              c.cmd->SetPipelineState(lit);
                              c.bindFrameConstants(cb);
                              c.computeConstants(k32, 48);
                              c.cmd->ExecuteIndirect(signature, 1, c.resource(special), 4, nullptr, 0);  // header words 1..3
                          });
            };
            addLight("5");  // every kind 5 entry (MODE 3, one lighting kernel, retired at the DXIL limit)
            addLight("6");

            // Strand hair records (kind 1; CoverageHair.hlsl): the strand model of Passes/Hair/HairScattering.hlsli with E's
            // density volume. The sun and the indirect light per record - the strand's part of them once per segment that
            // has a record, at three points along it (shading.hair_segment_shading) -; the local lights from a MegaLights
            // instance of its own on each pixel's nearest hair record under shading.mega_lights (as the coverage layer's
            // instance above; its samples shaded at the pixel's nearest and farthest hair record, a record between them
            // by its depth; its shadow rays from where the pixel's ray first meets the groom,
            // shading.hair_lights_ray_depths), else from the froxel list per record with S's fragment slots.
            const bool hairRecords = r.hairSegments.valid() && r.hairBodies.valid() && fc.quality.boolean("visibility.coverage_hair");
            if (hairRecords)
            {
                const uint32_t W = v.view.width, H = v.view.height;
                const BufferRef hairSegments = r.hairSegments, hairBodies = r.hairBodies, densityParams = r.hairDensityParams;
                const TextureRef densityFine = r.hairDensity, densityCoarse = r.hairDensityCoarse;
                const bool density = densityParams.valid() && densityFine.valid() && densityCoarse.valid();
                const uint32_t steps = (uint32_t)fc.quality.integer("shading.hair_density_steps");
                const float fibresBehind = (float)fc.quality.number("shading.hair_fibres_behind");
                if (steps < 2 || steps > 64 || !(fibresBehind >= 0)) fail("shading.hair_density_steps must be in [2, 64], shading.hair_fibres_behind >= 0");
                const GiSource hairGi = giSource(r);  // (the translucency volume whenever the frame has one)
                std::array<ID3D12PipelineState*, 12> hairKernel{};
                for (int mode = 0; mode < 12; ++mode) hairKernel[mode] = fc.shaders.compute(("Passes/Shading/CoverageHair.MODE" + std::to_string(mode)).c_str());
                const bool segmentShading = !fc.quality.has("shading.hair_segment_shading") || fc.quality.boolean("shading.hair_segment_shading");
                const bool rayDepths = density && (!fc.quality.has("shading.hair_lights_ray_depths") || fc.quality.boolean("shading.hair_lights_ray_depths"));
                const bool marchJitter = !fc.quality.has("shading.hair_march_jitter") || fc.quality.boolean("shading.hair_march_jitter");
                // (shading.hair_shadows: a strand also counts the other bodies' hair between it and a light)
                const bool otherBodies = density && (!fc.quality.has("shading.hair_shadows") || fc.quality.boolean("shading.hair_shadows"));
                auto useHair = [=](PassBuilder& b) {
                    b.use(v.coverageRecords, Use::SrvCompute);
                    b.use(hairSegments, Use::SrvCompute);
                    b.use(hairBodies, Use::SrvCompute);
                    if (density)
                    {
                        b.use(densityParams, Use::SrvCompute);
                        b.use(densityFine, Use::SrvCompute);
                        b.use(densityCoarse, Use::SrvCompute);
                    }
                    if (r.fxLights.valid()) b.use(r.fxLights, Use::SrvCompute);
                    if (r.lightFunctions.valid()) b.use(r.lightFunctions, Use::SrvCompute);
                };
                // P[0].x, P[1].xyz, P[5] of every mode
                auto hairConstants = [=](PassContext& c, uint32_t* k) {
                    k[0] = c.srv(v.coverageRecords);
                    k[4] = c.srv(hairSegments);
                    k[5] = c.srv(hairBodies);
                    k[6] = density ? c.srv(densityParams) : gpu::kNone;
                    // (CoverageHair.hlsl HAIR_STEPS, hairCounts' other bodies, hairJitter)
                    k[20] = steps | (otherBodies ? 0x40000000u : 0u) | (marchJitter ? 0x80000000u : 0u);
                    std::memcpy(&k[21], &fibresBehind, 4);
                    k[22] = experiment;
                    k[23] = r.lightFunctions.valid() ? c.srv(r.lightFunctions) : gpu::kNone;
                };
                TextureRef hairLighting, hairLightingFar, hairNearest, hairFarthest;
#if UNX_M_HAS_RAYTRACING
                if (megaLighting.valid())
                {
                    const TextureRef nearest = g.createTexture({ "m.hair nearest depth", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
                    const TextureRef element = g.createTexture({ "m.hair element", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
                    const TextureRef farthest = g.createTexture({ "m.hair farthest depth", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
                    const TextureRef farElement = g.createTexture({ "m.hair farthest element", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
                    const TextureRef hairGbuffer = g.createTexture({ "m.hair gbuffer", W, H, 1, 1, DXGI_FORMAT_R32G32_UINT });
                    const TextureRef hairWord = g.createTexture({ "m.hair material word", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
                    const TextureRef hairDepth = g.createTexture({ "m.hair depth", W, H, 1, 1, DXGI_FORMAT_R32_FLOAT });
                    g.addPass("m.hair.nearest", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  b.use(v.coverageRecords, Use::SrvCompute);
                                  b.use(v.coverageTileList, Use::SrvCompute);
                                  b.use(v.coverageTileList, Use::IndirectArgs);
                                  b.use(v.depth, Use::SrvCompute);
                                  for (TextureRef t : { nearest, element, farthest, farElement }) b.use(t, Use::UavCompute);
                              },
                              [=](PassContext& c) {
                                  uint32_t k[36] = { c.srv(v.coverageRecords), 0, c.srv(v.coverageTileList), 0, 0, 0, 0, c.srv(v.depth), c.uav(nearest), c.uav(element), 0, 0 };
                                  k[32] = c.uav(farthest);    // P[8].xy
                                  k[33] = c.uav(farElement);
                                  c.bindFrameConstants(cb);
                                  c.computeConstants(k, 36);
                                  c.cmd->SetPipelineState(hairKernel[0]);
                                  c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                                  // the depths are complete before the elements are chosen (one global UAV barrier between the steps)
                                  D3D12_GLOBAL_BARRIER gb{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                                           D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
                                  D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
                                  group.pGlobalBarriers = &gb;
                                  for (int mode = 1; mode <= 2; ++mode)
                                  {
                                      c.cmd->Barrier(1, &group);
                                      c.cmd->SetPipelineState(hairKernel[mode]);
                                      c.cmd->ExecuteIndirect(signature, 1, c.resource(v.coverageTileList), 32, nullptr, 0);  // V's arguments over the record blocks
                                  }
                              });
                    g.addPass("m.hair.surface", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  useHair(b);
                                  b.use(element, Use::SrvCompute);
                                  for (TextureRef t : { hairGbuffer, hairWord, hairDepth }) b.use(t, Use::UavCompute);
                              },
                              [=](PassContext& c) {
                                  uint32_t k[24] = {};
                                  hairConstants(c, k);
                                  k[9] = c.srv(element);
                                  k[10] = c.uav(hairGbuffer);
                                  k[11] = c.uav(hairWord);
                                  k[12] = c.uav(hairDepth);
                                  c.bindFrameConstants(cb);
                                  c.computeConstants(k, 24);
                                  c.cmd->SetPipelineState(hairKernel[3]);
                                  c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                              });
                    // the instance: the view with the hair surface as its depth and G-buffer (no vis buffer: its history
                    // is reprojected as static); a view that cannot run it keeps the per-record loop
                    ViewResources hview = view;
                    hview.depth = hairDepth;
                    hview.gbuffer = hairGbuffer;
                    hview.visId = {};
                    // (the instance's samples are not shadowed by the density volume - m.hair.lights counts the hair in front
                    // of each strand -, and their shadow rays start at the first fibre on the pixel's ray: CoverageHair MODE 6)
                    MegaLightsOptions hairOptions;
                    hairOptions.hairShadow = false;
                    if (rayDepths)
                        hairOptions.traceKeys = [&](TextureRef sampleKeys, uint32_t dsW, uint32_t dsH, uint32_t sampling) {
                            const TextureRef rayKeys = g.createTexture({ "m.hair ray keys", dsW, dsH, 1, 1, DXGI_FORMAT_R32G32_UINT });
                            g.addPass("m.hair.raydepth", QueueType::Graphics,
                                      [&](PassBuilder& b) {
                                          useHair(b);
                                          b.use(element, Use::SrvCompute);
                                          b.use(sampleKeys, Use::SrvCompute);
                                          b.use(v.depth, Use::SrvCompute);
                                          b.use(rayKeys, Use::UavCompute);
                                      },
                                      [=](PassContext& c) {
                                          uint32_t k[32] = {};
                                          hairConstants(c, k);
                                          k[9] = c.srv(element);
                                          k[11] = c.srv(sampleKeys);
                                          k[12] = c.uav(rayKeys);
                                          k[15] = sampling;         // P[3].w: factor | N << 8
                                          k[29] = c.srv(v.depth);   // P[7].y
                                          c.bindFrameConstants(cb);
                                          c.computeConstants(k, 32);
                                          c.cmd->SetPipelineState(hairKernel[6]);
                                          c.cmd->Dispatch((dsW + 7) / 8, (dsH + 7) / 8, 1);
                                      });
                            return rayKeys;
                        };
                    MegaLightsFrame hml = megaLightsSample(fc, hview, hairWord, areaLights, ltcSrv, signature, "hair", hairOptions);
                    if (hml.on)
                    {
                        auto half = [](float f) {  // positive, in the half range (the weight caps)
                            uint32_t u;
                            std::memcpy(&u, &f, 4);
                            const int32_t e = (int32_t)((u >> 23) & 0xFF) - 127 + 15;
                            if (e <= 0) return 0u;
                            if (e >= 31) return 0x7BFFu;
                            return ((uint32_t)e << 10) | ((u >> 13) & 0x3FFu);
                        };
                        const uint32_t caps = half(hml.maxWeight) | (half(hml.maxWeightHidden) << 16), mlMode = hml.factor | (hml.count << 8);
                        const TextureRef samples = hml.samples, keys = hml.keys, outDiffuse = hml.resolvedDiffuse, outSpecular = hml.resolvedSpecular;
                        g.addPass("m.hair.lights", QueueType::Graphics,
                                  [&](PassBuilder& b) {
                                      useHair(b);
                                      for (TextureRef t : { element, farElement, samples, keys }) b.use(t, Use::SrvCompute);
                                      b.use(outDiffuse, Use::UavCompute);
                                      b.use(outSpecular, Use::UavCompute);
                                  },
                                  [=](PassContext& c) {
                                      uint32_t k[36] = {};
                                      hairConstants(c, k);
                                      k[33] = c.srv(farElement);  // P[8].y
                                      k[9] = c.srv(element);
                                      k[10] = c.srv(samples);
                                      k[11] = c.srv(keys);
                                      k[12] = c.uav(outDiffuse);
                                      k[13] = c.uav(outSpecular);
                                      k[14] = caps;
                                      k[15] = mlMode;
                                      c.bindFrameConstants(cb);
                                      c.computeConstants(k, 36);
                                      c.cmd->SetPipelineState(hairKernel[4]);
                                      c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                                  });
                        // (shading.hair_lights_spatial: the instance's spatial filter; off as the reference's hair input)
                        megaLightsDenoise(fc, hview, hairWord, hml, true, fc.quality.boolean("shading.hair_lights_spatial"));
                        hairLighting = hml.lighting;                // at the pixel's nearest hair record
                        hairLightingFar = hml.lightingSpecular;     // at its farthest
                        hairNearest = nearest;
                        hairFarthest = farthest;
                    }
                }
#endif
                // shading.hair_segment_shading: the hair records in front of the band A surface (a list) and their
                // segments (a bit each, then a list), and the strand's part of the sun's and the indirect light at
                // kHairSegmentPoints points along each of those segments; m.hair shades the listed records with their
                // segment's values (CoverageHair.hlsl MODE 7..11). The segment buffers are sized for every segment of the
                // frame (48 B each for the values), the record list for V's pool (8 B a record: its element and its tile).
                constexpr uint64_t kHairSegmentPoints = 3;  // CoverageHair.hlsl HAIR_SEGMENT_POINTS
                const uint32_t segmentCount = (uint32_t)(fc.graph.desc(hairSegments).size / 32);
                BufferRef segmentLight, visibleRecords;
                if (segmentShading)
                {
                    const uint64_t recordCapacity = fc.graph.desc(v.coverageRecords).size / 16;
                    visibleRecords = g.createBuffer({ "m.hair visible records", (4 + 2 * recordCapacity) * 4, 0 });
                    const uint32_t bitWords = (segmentCount + 31) / 32;
                    const BufferRef segmentBits = g.createBuffer({ "m.hair segment bits", ((uint64_t)bitWords + 3) / 4 * 16, 0 });
                    const BufferRef segmentList = g.createBuffer({ "m.hair segment list", (4 + (uint64_t)segmentCount) * 4, 0 });
                    segmentLight = g.createBuffer({ "m.hair segment light", (uint64_t)segmentCount * kHairSegmentPoints * 16, 0 });
                    g.addPass("m.hair.visible", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  b.use(v.coverageRecords, Use::SrvCompute);
                                  b.use(v.coverageTileList, Use::SrvCompute);
                                  b.use(v.coverageTileList, Use::IndirectArgs);
                                  b.use(v.depth, Use::SrvCompute);
                                  b.use(segmentBits, Use::UavCompute);
                                  b.use(segmentList, Use::UavCompute);
                                  b.use(visibleRecords, Use::UavCompute);
                                  b.use(shaded, Use::UavCompute);
                              },
                              [=](PassContext& c) {
                                  uint32_t k[44] = {};
                                  k[40] = c.uav(visibleRecords);  // P[10]
                                  k[41] = (uint32_t)recordCapacity;
                                  k[42] = c.uav(shaded);
                                  k[0] = c.srv(v.coverageRecords);
                                  k[2] = c.srv(v.coverageTileList);
                                  k[29] = c.srv(v.depth);      // P[7].y
                                  k[36] = c.uav(segmentBits);  // P[9]
                                  k[37] = c.uav(segmentList);
                                  k[38] = gpu::kNone;
                                  k[39] = segmentCount;
                                  c.bindFrameConstants(cb);
                                  c.computeConstants(k, 44);
                                  // (each step reads what the one before wrote: a global UAV barrier between them)
                                  D3D12_GLOBAL_BARRIER gb{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                                           D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
                                  D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
                                  group.pGlobalBarriers = &gb;
                                  c.cmd->SetPipelineState(hairKernel[7]);
                                  c.cmd->Dispatch((bitWords + 255) / 256, 1, 1);
                                  c.cmd->Barrier(1, &group);
                                  c.cmd->SetPipelineState(hairKernel[8]);
                                  c.cmd->ExecuteIndirect(signature, 1, c.resource(v.coverageTileList), 32, nullptr, 0);  // V's arguments over the record blocks
                                  c.cmd->Barrier(1, &group);
                                  c.cmd->SetPipelineState(hairKernel[9]);
                                  c.cmd->Dispatch((bitWords + 63) / 64, 1, 1);
                                  c.cmd->Barrier(1, &group);
                                  c.cmd->SetPipelineState(hairKernel[10]);
                                  c.cmd->Dispatch(1, 1, 1);
                              });
                    g.addPass("m.hair.strands", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  useHair(b);
                                  b.use(segmentList, Use::SrvCompute);
                                  b.use(segmentList, Use::IndirectArgs);
                                  b.use(segmentLight, Use::UavCompute);
                                  declareGiSource(b, hairGi, Use::SrvCompute);
                              },
                              [=](PassContext& c) {
                                  uint32_t k[40] = {};
                                  hairConstants(c, k);
                                  k[27] = giSourceWord(c, hairGi);  // P[6].w
                                  k[36] = gpu::kNone;               // P[9]
                                  k[37] = c.srv(segmentList);
                                  k[38] = c.uav(segmentLight);
                                  k[39] = segmentCount;
                                  c.bindFrameConstants(cb);
                                  c.computeConstants(k, 40);
                                  c.cmd->SetPipelineState(hairKernel[11]);
                                  c.cmd->ExecuteIndirect(signature, 1, c.resource(segmentList), 4, nullptr, 0);  // header words 1..3
                              });
                }
                g.addPass("m.hair", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              useHair(b);
                              // (its entries: the records m.hair.visible listed, else V's special list)
                              b.use(visibleRecords.valid() ? visibleRecords : special, Use::SrvCompute);
                              b.use(visibleRecords.valid() ? visibleRecords : special, Use::IndirectArgs);
                              b.use(v.coverageTileList, Use::SrvCompute);
                              b.use(shaded, Use::UavCompute);
                              b.use(v.depth, Use::SrvCompute);
                              if (segmentLight.valid()) b.use(segmentLight, Use::SrvCompute);
                              if (fragmentShadows)
                              {
                                  b.use(v.shadowFragmentVisibility, Use::SrvCompute);
                                  b.use(v.coverageDepthRange, Use::SrvCompute);
                                  if (v.shadowFragmentSun.valid()) b.use(v.shadowFragmentSun, Use::SrvCompute);
                              }
                              if (hairLighting.valid())
                                  for (TextureRef t : { hairLighting, hairLightingFar, hairNearest, hairFarthest }) b.use(t, Use::SrvCompute);
                              else if (froxelLists) b.use(v.froxelLights, Use::SrvCompute);
                              if (atmosphere)
                                  for (TextureRef t : { r.transmittanceLut, r.multiScatterLut }) b.use(t, Use::SrvCompute);
                              if (air) b.use(v.airVolume, Use::SrvCompute);
                              declareFog(b, r, Use::SrvCompute);
                              declareGiSource(b, hairGi, Use::SrvCompute);
                          },
                          [=](PassContext& c) {
                              const uint32_t none = gpu::kNone;
                              uint32_t k[40] = {};
                              hairConstants(c, k);
                              k[1] = c.srv(special);
                              k[2] = c.srv(v.coverageTileList);
                              k[3] = c.uav(shaded);
                              k[37] = visibleRecords.valid() ? c.srv(visibleRecords) : none;  // P[9].yzw
                              k[38] = segmentLight.valid() ? c.srv(segmentLight) : none;
                              k[39] = segmentCount;
                              k[16] = fragmentShadows ? c.srv(v.shadowFragmentVisibility) : none;
                              k[17] = fragmentShadows ? c.srv(v.coverageDepthRange) : none;
                              k[18] = fragmentShadows && v.shadowFragmentSun.valid() ? c.srv(v.shadowFragmentSun) : none;
                              k[19] = !hairLighting.valid() && froxelLists ? c.srv(v.froxelLights) : none;
                              k[24] = atmosphere ? c.srv(r.transmittanceLut) : none;
                              k[25] = atmosphere ? c.srv(r.multiScatterLut) : none;
                              k[26] = air ? c.srv(v.airVolume) : none;
                              k[27] = giSourceWord(c, hairGi);
                              k[28] = hairLighting.valid() ? c.srv(hairLighting) : none;
                              k[29] = c.srv(v.depth);
                              if (hairLighting.valid())
                              {
                                  k[32] = c.srv(hairFarthest);      // P[8]
                                  k[34] = c.srv(hairNearest);
                                  k[35] = c.srv(hairLightingFar);
                              }
                              c.bindFrameConstants(cb);
                              c.computeConstants(k, 40);
                              c.cmd->SetPipelineState(hairKernel[5]);
                              c.cmd->ExecuteIndirect(signature, 1, c.resource(visibleRecords.valid() ? visibleRecords : special), 4, nullptr, 0);  // header words 1..3
                          });
            }
        }
        // L2c (14.1c): the coverage tiles' FAR field (CoverageTileLights.hlsl), before the composite reads it.
        const bool covTileLights = froxelLists && fc.quality.boolean("shading.coverage_tile_lights");
        const uint32_t covTlCapacity = (uint32_t)fc.quality.integer("shading.coverage_tile_lights_capacity");
        const BufferRef covTlField = covTileLights ? g.createBuffer({ "m.coverage tile lights", (uint64_t)std::max(covTlCapacity, 1u) * 6176, 0 }) : BufferRef{};
        if (covTileLights)
        {
            if (covTlCapacity == 0 || covTlCapacity > 65535) fail("shading.coverage_tile_lights_capacity must be in [1, 65535]");
            ID3D12PipelineState* tlKernel = fc.shaders.compute("Passes/Lights/CoverageTileLights");
            const uint32_t channelsShared = fc.scene.lightingChannelsShared();  // (a FAR light lights every instance)
            g.addPass("m.coverage.tilelights", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(v.coverageRecords, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::SrvCompute);
                          b.use(v.froxelLights, Use::SrvCompute);
                          if (r.fxLights.valid()) b.use(r.fxLights, Use::SrvCompute);
                          b.use(covTlField, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          const uint32_t k[8] = { c.srv(v.coverageRecords), c.srv(v.coverageTileList), c.srv(v.froxelLights), c.uav(covTlField), covTlCapacity,
                                                  channelsShared, 0, 0 };
                          c.cmd->SetPipelineState(tlKernel);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch(covTlCapacity, 1, 1);  // groups past the list's count return at once
                      });
        }
        // E: per listed tile and part, the light pixels' fragments walked and weighted (part 1 records the heavy pixels).
        auto addComposite = [&](uint32_t stage) {
            g.addPass(stage == 1 ? "m.coverage direct" : "m.coverage", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          useShading(b);
                          if (covTileLights) b.use(covTlField, Use::SrvCompute);  // L2c
                          if (shaded.valid()) b.use(shaded, Use::SrvCompute);
                          b.use(v.coverageTilePixels, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::IndirectArgs);
                          b.use(state, Use::UavCompute);
                          if (stage == 1)
                          {
                              b.use(heavy, Use::UavCompute);
                              b.use(directSum, Use::UavCompute);
                          }
                          else
                          {
                              useBandA(b);
                              b.use(directSum, Use::SrvCompute);
                              b.use(v.color, Use::UavCompute);
                              useParticles(b);
                          }
                      },
                      [=](PassContext& c) {
                          uint32_t k[24] = { c.srv(v.coverageRecords), c.srv(v.coverageTilePixels), c.srv(v.coverageTileList), c.uav(state) };
                          shadingConstants(c, k, stage == 2 ? c.uav(v.color) : gpu::kNone);
                          k[8] = stage == 2 ? c.srv(edgeRadiance) : gpu::kNone;
                          k[9] = stage == 2 ? c.srv(edgeResolved) : gpu::kNone;
                          k[10] = stage == 2 ? c.srv(edgeTiles) : gpu::kNone;
                          k[11] = stage == 1 ? c.uav(heavy) : gpu::kNone;
                          k[21] = hcap;
                          k[22] = ccap;
                          k[23] = covTileLights ? c.srv(covTlField) : gpu::kNone;  // P[5].w (L2c)
                          uint32_t k32[48] = {};
                          std::memcpy(k32, k, sizeof k);
                          k32[41] = covTileLights ? covTlCapacity : 0;  // P[10].y (L2c)
                          if (stage == 2) particleConstants(c, k32 + 24);  // P[6].xy
                          else k32[24] = k32[25] = gpu::kNone;
                          fragmentConstants(c, k32);      // P[6].zw, P[7], P[8].xy
                          k32[34] = stage == 1 ? c.uav(directSum) : c.srv(directSum);  // P[8].z
                          c.cmd->SetPipelineState(stage == 1 ? light1 : light2);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k32, 48);
                          c.cmd->ExecuteIndirect(signature, 1, c.resource(v.coverageTileList), 0, nullptr, 0);
                      });
        };
        // The compact form's stages (shading.coverage_compact): W the walk, S the list's shading (part 1, part 2), G the gather.
        auto addWalk = [&] {
            g.addPass("m.coverage.walk", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(v.coverageRecords, Use::SrvCompute);
                          b.use(v.visibleClusters, Use::SrvCompute);
                          b.use(v.depth, Use::SrvCompute);
                          b.use(v.coverageTilePixels, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::IndirectArgs);
                          b.use(state, Use::UavCompute);
                          b.use(heavy, Use::UavCompute);
                          for (BufferRef x : { covEntries, pixelSpans, tileSpans }) b.use(x, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k[24] = {};
                          k[0] = c.srv(v.coverageRecords);
                          k[1] = c.srv(v.coverageTilePixels);
                          k[2] = c.srv(v.coverageTileList);
                          k[3] = c.uav(state);
                          k[4] = c.srv(v.visibleClusters);  // P[1].x (the sort's tie order)
                          k[6] = c.srv(v.depth);            // P[1].z
                          k[8] = c.uav(covEntries);         // P[2]
                          k[9] = c.uav(pixelSpans);
                          k[10] = c.uav(tileSpans);
                          k[11] = c.uav(heavy);
                          k[21] = hcap;                     // P[5].yzw
                          k[22] = ccap;
                          k[23] = (uint32_t)capacity;
                          c.cmd->SetPipelineState(walk);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 24);
                          c.cmd->ExecuteIndirect(signature, 1, c.resource(v.coverageTileList), 0, nullptr, 0);
                      });
        };
        auto addShadeList = [&](uint32_t stage) {  // 0: the one kernel, 1 and 2: the two parts
            g.addPass(stage == 1 ? "m.coverage direct" : "m.coverage", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          useShading(b);
                          if (covTileLights) b.use(covTlField, Use::SrvCompute);  // L2c
                          if (shaded.valid()) b.use(shaded, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::IndirectArgs);
                          b.use(covEntries, Use::SrvCompute);
                          b.use(tileSpans, Use::SrvCompute);
                          b.use(covRadiance, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k[24] = { c.srv(v.coverageRecords), c.srv(tileSpans), c.srv(v.coverageTileList), c.srv(covEntries) };
                          shadingConstants(c, k, gpu::kNone);
                          k[8] = c.uav(covRadiance);  // P[2].x
                          k[9] = k[10] = k[11] = gpu::kNone;
                          k[23] = covTileLights ? c.srv(covTlField) : gpu::kNone;  // P[5].w (L2c)
                          uint32_t k32[48] = {};
                          std::memcpy(k32, k, sizeof k);
                          k32[41] = covTileLights ? covTlCapacity : 0;  // P[10].y (L2c)
                          k32[24] = k32[25] = gpu::kNone;
                          fragmentConstants(c, k32);  // P[6].zw, P[7], P[8].xy
                          k32[34] = gpu::kNone;       // P[8].z: no per-pixel direct sum in this form
                          c.cmd->SetPipelineState(shadeList[stage == 2 ? 1 : 0]);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k32, 48);
                          c.cmd->ExecuteIndirect(signature, 1, c.resource(v.coverageTileList), 0, nullptr, 0);
                      });
        };
        auto addGather = [&] {
            g.addPass("m.coverage.gather", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          useBandA(b);
                          b.use(v.coverageTileList, Use::SrvCompute);
                          b.use(v.coverageTileList, Use::IndirectArgs);
                          b.use(covRadiance, Use::SrvCompute);
                          b.use(pixelSpans, Use::SrvCompute);
                          b.use(tileSpans, Use::SrvCompute);
                          b.use(v.color, Use::UavCompute);
                          useParticles(b);
                      },
                      [=](PassContext& c) {
                          uint32_t k[28] = {};
                          k[0] = c.srv(covRadiance);
                          k[1] = c.srv(pixelSpans);
                          k[2] = c.srv(v.coverageTileList);
                          k[3] = c.srv(tileSpans);
                          k[7] = c.uav(v.color);         // P[1].w
                          k[8] = c.srv(edgeRadiance);    // P[2]: the band A radiance under the fragments
                          k[9] = c.srv(edgeResolved);
                          k[10] = c.srv(edgeTiles);
                          particleConstants(c, k + 24);  // P[6].xy
                          c.cmd->SetPipelineState(gather);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 28);
                          c.cmd->ExecuteIndirect(signature, 1, c.resource(v.coverageTileList), 0, nullptr, 0);
                      });
        };
        if (compact) addWalk();
        else addComposite(1);
        addBegin("m.coverage.heavy args", 1, 0);
        // F1: heavy pixels' runs of COV_BLOCK records sorted into the pair buffer.
        g.addPass("m.coverage.sort", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(state, Use::SrvCompute);
                      b.use(heavy, Use::SrvCompute);
                      b.use(pairs, Use::UavCompute);
                      b.use(cursors, Use::UavCompute);
                      b.use(v.coverageRecords, Use::SrvCompute);
                      b.use(v.visibleClusters, Use::SrvCompute);
                      b.use(args, Use::IndirectArgs);
                  },
                  [=](PassContext& c) {
                      const uint32_t p[8] = { c.srv(state), c.srv(heavy), c.uav(pairs), c.uav(cursors), hcap, c.srv(v.coverageRecords), c.srv(v.visibleClusters), 0 };
                      // (each run is one of the two kernels': the short runs' groups of 64 threads, the long runs' of 256)
                      c.computeConstants(p, 8);
                      for (ID3D12PipelineState* kernel : sort)
                      {
                          c.cmd->SetPipelineState(kernel);
                          c.cmd->ExecuteIndirect(signature, 1, c.resource(args), 16, nullptr, 0);
                      }
                  });
        // F2: COV_ROUNDS rounds per stage, each over the heavy pixels the previous one left open; between the parts every
        // heavy pixel's merge goes back to its start (CoverageHeavyReset), keeping stage 1's sum.
        for (uint32_t stage = 1; stage <= 2; ++stage)
        {
            if (stage == 2)
            {
                addBegin("m.coverage.heavy args", 1, 0);  // round 0's arguments again (every heavy pixel)
                g.addPass("m.coverage.heavy reset", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(state, Use::UavCompute);
                              b.use(heavy, Use::UavCompute);
                              b.use(cursors, Use::UavCompute);
                              b.use(args, Use::IndirectArgs);
                          },
                          [=](PassContext& c) {
                              const uint32_t p[4] = { c.uav(state), c.uav(heavy), c.uav(cursors), hcap };
                              c.cmd->SetPipelineState(heavyReset);
                              c.computeConstants(p, 4);
                              c.cmd->ExecuteIndirect(signature, 1, c.resource(args), 48, nullptr, 0);  // the finish grid: 64 per group
                          });
            }
            for (uint32_t rd = 0; rd < kRounds; ++rd)
            {
                if (rd > 0) addBegin("m.coverage.round args", 2, rd);
                g.addPass("m.coverage.round", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              useShading(b);
                              if (shaded.valid()) b.use(shaded, Use::SrvCompute);
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
                              uint32_t k32[48] = {};
                              std::memcpy(k32, k, sizeof k);
                              k32[24] = k32[25] = gpu::kNone;  // (no particle layer in the rounds)
                              fragmentConstants(c, k32);      // P[6].zw, P[7], P[8].x
                              c.cmd->SetPipelineState(heavyRounds[stage - 1]);
                              c.bindFrameConstants(cb);
                              c.computeConstants(k32, 48);
                              c.cmd->ExecuteIndirect(signature, 1, c.resource(args), 32, nullptr, 0);
                          });
            }
        }
        if (compact)
        {
            if (compactWhole) addShadeList(0);
            else
            {
                addShadeList(1);
                addShadeList(2);
            }
            addGather();
        }
        else addComposite(2);
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
                      [dst, slot, state](PassContext& c) {
                          c.cmd->CopyBufferRegion(dst, slot * StatsRing::kBytes + 20, c.resource(state), 12, 4);
                          // (the composite's counts: COVS_ENTRIES, COVS_WALKED, COVS_SHADED, COVS_LIGHT_PIXELS, then COVS_HEAVY)
                          c.cmd->CopyBufferRegion(dst, slot * StatsRing::kBytes + 44, c.resource(state), 32, 16);
                          c.cmd->CopyBufferRegion(dst, slot * StatsRing::kBytes + 60, c.resource(state), 8, 4);
                      });
        }
    }
    return {};
}
} // namespace

std::vector<RenderGraph::BandedPass> shadingPasses(FramePassContext& fc, ViewResources& view) { return record(fc, view, Part::Banded); }

void shadingScatter(FramePassContext& fc, ViewResources& view) { record(fc, view, Part::Scatter); }

void shadingComposite(FramePassContext& fc, ViewResources& view)
{
    record(fc, view, Part::Composite);
    // The main view's luminance histogram, filled by its shading kernels, into the readback ring (automatic exposure);
    // on a snap frame first metered on the GPU for this frame's own output (Exposure.cpp exposureMeter).
    if (view.view.kind == gpu::ViewKind::Main)
    {
        const ExposureHistogram histogram = exposureHistogram(fc);
        view.exposureCorrection = exposureMeter(fc, histogram);
        exposureReadback(fc, histogram);
    }
}

// A10 glass over V's translucent layer (FEATURES_GAME 14.1): class 1 pixels composited in place on the float image.
void translucentComposite(FramePassContext& fc, const ViewResources& view, TextureRef colour)
{
    RenderGraph& g = fc.graph;
    const FrameResources& r = fc.resources;
    const TextureRef vis = view.translucentVis, classes = view.translucentClass;
    const BufferRef stats = g.createBuffer({ "m.glass.stats", 16, 0 });  // raw
    ID3D12PipelineState* clear = fc.shaders.compute("Passes/Shading/ExposureClear");  // (zeroes a raw buffer: P[0].x, count P[0].y)
    // R-1 / R-2 (A10): with R's refraction service, the view in bands of kGlassBandRows rows (at most kGlassMaxRecords
    // pixels, so a band's jobs - two per pixel at most - never exceed kGlassMaxJobs): jobs and records, their arguments, R's
    // rays, then the records applied. Without it, one dispatch over the view (JOBS=0).
    const bool jobs = static_cast<bool>(fc.services.traceRefractions);
    ID3D12PipelineState* kernel = fc.shaders.compute(jobs ? "Passes/Shading/TranslucentComposite.JOBS1" : "Passes/Shading/TranslucentComposite.JOBS0");
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const uint32_t w = view.view.width, h = view.view.height, none = gpu::kNone;
    const uint32_t textureTable = material::resolveOutputs(fc, view).textureTableSrv;
    g.addPass("m.glass.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(stats, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.uav(stats), 4, 0, 0 };
                  c.cmd->SetPipelineState(clear);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(1, 1, 1);
              });
    constexpr uint32_t kGlassMaxJobs = 1u << 20, kGlassMaxRecords = kGlassMaxJobs / 2;
    const uint32_t bandRows = jobs ? std::max(8u, (kGlassMaxRecords / std::max(w, 1u)) & ~7u) : h;
    const uint32_t bands = (h + bandRows - 1) / bandRows;
    const float mirrorMax = jobs ? (float)fc.quality.number("reflection.mirror_roughness_max") : 0.0f;
    BufferRef glassJobs, glassRecords, glassResults;
    if (jobs)
    {
        glassJobs = g.createBuffer({ "m.glass.jobs", 16 + 48ull * kGlassMaxJobs, 0 });        // raw
        glassRecords = g.createBuffer({ "m.glass.records", 32 + 48ull * kGlassMaxRecords, 0 });  // raw
        glassResults = g.createBuffer({ "m.glass.results", 8ull * kGlassMaxJobs, 0 });          // raw
    }
    ID3D12PipelineState* argsKernel = jobs ? fc.shaders.compute("Passes/Shading/TranslucentArgs") : nullptr;
    ID3D12PipelineState* applyKernel = jobs ? fc.shaders.compute("Passes/Shading/TranslucentApply") : nullptr;
    ID3D12CommandSignature* signature = jobs ? material::dispatchSignature(fc) : nullptr;
    for (uint32_t band = 0; band < bands; ++band)
    {
    const uint32_t row0 = band * bandRows, row1 = std::min(h, row0 + bandRows);
    if (jobs)
        g.addPass("m.glass.reset", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(glassJobs, Use::UavCompute);
                      b.use(glassRecords, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(clear);
                      const uint32_t a[4] = { c.uav(glassJobs), 4, 0, 0 }, b[4] = { c.uav(glassRecords), 8, 0, 0 };
                      c.computeConstants(a, 4);
                      c.cmd->Dispatch(1, 1, 1);
                      c.computeConstants(b, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    g.addPass("m.glass", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(vis, Use::SrvCompute);
                  b.use(classes, Use::SrvCompute);
                  b.use(colour, Use::UavCompute);
                  b.use(stats, Use::UavCompute);
                  if (jobs)
                  {
                      b.use(glassJobs, Use::UavCompute);
                      b.use(glassRecords, Use::UavCompute);
                      b.use(glassResults, Use::UavCompute);
                  }
                  if (view.visibleClusters.valid()) b.use(view.visibleClusters, Use::SrvCompute);
                  declareGiSource(b, giSource(r), Use::SrvCompute);
                  for (const TextureRef& t : { r.transmittanceLut, r.multiScatterLut, view.airVolume })
                      if (t.valid()) b.use(t, Use::SrvCompute);
                  declareFog(b, r, Use::SrvCompute);
                  for (const BufferRef& x : { r.vsmPageTable, r.vsmBlocks, r.vsmSearchBound, r.vsmLayers })
                      if (x.valid()) b.use(x, Use::SrvCompute);
                  if (r.vsmAtlas.valid()) b.use(r.vsmAtlas, Use::SrvCompute);
              },
              // (by value: the frame context and its resources do not outlive the graph's build; a reference read at
              // execute time handed the pass other graphs' handles - render C's coverage_layer_is_exact crash)
              [=, gi = giSource(r), transmittance = r.transmittanceLut, multiScatter = r.multiScatterLut, pageTable = r.vsmPageTable,
               blocks = r.vsmBlocks, searchBound = r.vsmSearchBound, layers = r.vsmLayers, vsmConstants = r.vsmConstants,
               visibleClusters = view.visibleClusters, airVolume = view.airVolume](PassContext& c) {
                  const bool shadows = pageTable.valid() && vsmConstants != UINT32_MAX;
                  const uint32_t k[24] = { c.srv(vis), c.srv(classes), c.uav(colour), c.uav(stats),
                                           c.srv(visibleClusters), textureTable, giSourceWord(c, gi),
                                           jobs ? c.uav(glassResults) : 0,
                                           transmittance.valid() ? c.srv(transmittance) : none, multiScatter.valid() ? c.srv(multiScatter) : none,
                                           airVolume.valid() ? c.srv(airVolume) : none, 0,
                                           shadows ? c.srv(pageTable) : none, shadows ? c.srv(blocks) : none,
                                           shadows && searchBound.valid() ? c.srv(searchBound) : none, shadows ? vsmConstants : none,
                                           shadows && layers.valid() ? c.srv(layers) : none, row0, row1, asUint(mirrorMax),
                                           jobs ? c.uav(glassJobs) : 0, jobs ? c.uav(glassRecords) : 0, kGlassMaxJobs, kGlassMaxRecords };
                  c.cmd->SetPipelineState(kernel);
                  c.bindFrameConstants(cb);
                  c.computeConstants(k, 24);
                  c.cmd->Dispatch((w + 7) / 8, (row1 - row0 + 7) / 8, 1);
              });
    if (!jobs) continue;
    g.addPass("m.glass.args", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(glassJobs, Use::UavCompute);
                  b.use(glassRecords, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.uav(glassJobs), c.uav(glassRecords), kGlassMaxJobs, kGlassMaxRecords };
                  c.cmd->SetPipelineState(argsKernel);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(1, 1, 1);
              });
    fc.services.traceRefractions(fc, glassJobs, glassResults, kGlassMaxJobs);
    g.addPass("m.glass.apply", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(glassRecords, Use::SrvCompute);
                  b.use(glassRecords, Use::IndirectArgs);
                  b.use(glassResults, Use::SrvCompute);
                  b.use(colour, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.srv(glassRecords), c.srv(glassResults), c.uav(colour), kGlassMaxRecords };
                  c.cmd->SetPipelineState(applyKernel);
                  c.computeConstants(k, 4);
                  c.cmd->ExecuteIndirect(signature, 1, c.resource(glassRecords), 4, nullptr, 0);
              });
    }
    if (fc.trackState)
    {
        StatsRing& ring = fc.state<StatsRing>("M.statsRing");
        ring.ensure(fc.device);
        ID3D12Resource* dst = ring.buffer.Get();
        const uint32_t slot = (uint32_t)(fc.frame.frameIndex % StatsRing::kSlots);
        ring.glass[slot] = true;
        g.addPass("m.glass.stats", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(stats, Use::CopySrc);
                      b.keep();
                  },
                  [dst, slot, stats](PassContext& c) { c.cmd->CopyBufferRegion(dst, slot * StatsRing::kBytes + 24, c.resource(stats), 0, 12); });
    }
}

void shade(FramePassContext& fc, ViewResources& view)
{
    // Views the frame does not record as the lighting group (planar reflection views through renderView, tests): M's own
    // group over the frame's bands (output.band_pixels; one band by default, v1.31). The banded part checks the view first.
    // With a post term on, the main view is shaded into the chain's HDR target and the chain writes the display output.
    // Motion blur (the shutter's time integral) reads the shaded image around each pixel: with it the view is shaded into
    // a float target first, then blurred into the chain's input (display) or into the linear capture itself.
    // Heat haze re-reads it at displaced points too (before the exposure integral: the haze bends what the lens sees).
    // Depth of field (the lens's aperture integral) follows the time integral (FEATURES_GAME 4: time, then lens).
    // Temporal upscale (output.render_height_max): all of the above at the internal resolution into the chain's HDR
    // target (postTarget: the view's size), then the upscale to the output resolution, then the chain into view.color.
    // With shading.motion_blur_after_upscale the time integral of an upscaled view follows the upscale instead, at the
    // output resolution (the reference's order: lens, upscale, motion blur; motionBlurUpscaled).
    const bool post = postActive(fc, view);
    const bool blurAfter = motionBlurActive(fc, view) && upscaleActive(fc, view) && motionBlurAfterUpscale(fc);
    const bool blur = motionBlurActive(fc, view) && !blurAfter, haze = distortionActive(fc, view), dof = depthOfFieldActive(fc, view);
    const DXGI_FORMAT floatFormat = post ? DXGI_FORMAT_R16G16B16A16_FLOAT : fc.graph.desc(view.color).format;
    auto intermediate = [&](const char* name) { return fc.graph.createTexture(TextureDesc{ name, view.view.width, view.view.height, 1, 1, floatFormat }); };
    ViewResources target = view;
    if (post) target.color = postTarget(fc, view);
    else if (blur || haze || dof) target.color = intermediate("m.shaded");
    const std::vector<RenderGraph::BandedPass> passes = shadingPasses(fc, target);
    const material::ResolveOutputs& o = material::resolveOutputs(fc, target);
    fc.graph.addBandedGroup(view.view.kind != gpu::ViewKind::Main ? "m.lit.planar" : "m.lit", o.height, o.bands, passes);
    shadingScatter(fc, target);  // (shading.subsurface_scatter: the Subsurface class's pixels, before the lit image is read)
    keepSceneColor(fc, target, target.color);  // (main view with the upscale: the next frame's screen-trace source)
    tracks::water(fc, target);  // W (engine 1): the water surfaces' refraction targets are the shaded opaque scene and its depth
    shadingComposite(fc, target);
    view.exposureCorrection = target.exposureCorrection;  // a snap frame's own metering (Exposure.cpp): the chain and the caller
    view.localDirect = target.localDirect;                 // (shading.mega_lights' result, for the gate's captures)
    view.coverageRecordRadiance = target.coverageRecordRadiance;  // (the special records' radiance, for the tests' readback)
    if (translucentActive(fc, view)) translucentComposite(fc, view, target.color);  // A10 glass over the composited image
    TextureRef image = target.color;
    if (haze)
    {
        const TextureRef displaced = (blur || dof || post) ? intermediate("m.distorted") : view.color;
        distortion(fc, view, image, displaced);
        image = displaced;
    }
    if (blur)
    {
        const TextureRef blurred = dof ? intermediate("m.blurred") : post ? postTarget(fc, view) : view.color;
        motionBlur(fc, view, image, blurred);
        image = blurred;
    }
    if (dof)
    {
        const TextureRef focused = post ? postTarget(fc, view) : view.color;
        const BufferRef stats = depthOfField(fc, view, image, focused);
        image = focused;
        if (fc.trackState)
        {
            StatsRing& ring = fc.state<StatsRing>("M.statsRing");
            ring.ensure(fc.device);
            ID3D12Resource* dst = ring.buffer.Get();
            const uint32_t slot = (uint32_t)(fc.frame.frameIndex % StatsRing::kSlots);
            ring.dof[slot] = true;
            fc.graph.addPass("m.dof.stats", QueueType::Graphics,
                             [&](PassBuilder& b) {
                                 b.use(stats, Use::CopySrc);
                                 b.keep();
                             },
                             [dst, slot, stats](PassContext& c) { c.cmd->CopyBufferRegion(dst, slot * StatsRing::kBytes + 36, c.resource(stats), 0, 8); });
        }
    }
    if (upscaleActive(fc, view))
    {
        // the output resolution from the internal image and the history (Upscale.cpp); the chain encodes it (postActive)
        UpscaleProducts products;
        image = temporalUpscale(fc, view, image, &products);
        view.upscaled = image;  // (captures of the upscaled image: renderergate --capture-output)
        if (blurAfter)
        {
            const TextureDesc upscaled = fc.graph.desc(image);
            const TextureRef blurred = fc.graph.createTexture(TextureDesc{ "m.motion.blurred", upscaled.width, upscaled.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            motionBlurUpscaled(fc, view, image, blurred, products.motion, products.depth);
            image = blurred;
        }
        view.chainInput = image;  // (captures of what the chain encodes: renderergate --capture-layers chain)
        postChain(fc, upscaleOutputView(fc, view), image);
        return;
    }
    if (post) postChain(fc, view, image);
}
} // namespace unx::render::shading
