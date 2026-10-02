#include "unx/material/MaterialSystem.h"

#include "unx/core/Log.h"
#include "unx/material/TextureSystem.h"
#include "unx/render/GpuScene.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace unx::render::material
{
namespace
{
// Resolve outputs of the views of the current frame, keyed by the view's frame-constant slot (unique per view and
// frame). Shading of the same view finds them here.
struct ViewTable
{
    uint64_t frame = UINT64_MAX;
    std::vector<std::pair<D3D12_GPU_VIRTUAL_ADDRESS, ResolveOutputs>> views;
};

struct Signature
{
    ComPtr<ID3D12CommandSignature> dispatch;
};

// Quality keys of the material resolve (Config/quality/material.toml). The anisotropy is the static sampler's (s3,
// Device.cpp); the key documents it and the check keeps the two equal.
void checkQuality(const QualityConfig& q)
{
    if (q.integer("material.max_anisotropy") != 16) fail("material.max_anisotropy must be 16 (the aniso sampler s3 of the root signature)");
    if (q.string("material.normal_filter") != "lean") fail("material.normal_filter: only \"lean\" (slope moments + footprint variance) is implemented");
}

uint32_t experimentMask(const QualityConfig& q)
{
    const int64_t m = q.integer("material.experiment_disable");
    static bool logged = false;
    if (m != 0 && !logged)
    {
        logf("M material: experiment mask %lld leaves resolve work out (cost attribution run, not an image)\n", (long long)m);
        logged = true;
    }
    return (uint32_t)m;
}
} // namespace

namespace
{
// COVERAGE 12.4 structure 2 (B2): per scene light, the frame its revision last changed; per frame a bit mask of the lights
// whose revision held for >= kStableFrames (the GI cache's reconvergence, design 2.5), in an upload ring read by M's
// shading and R's reflection paths through FrameResources::areaLightStable.
struct AreaLightStability
{
    static constexpr uint32_t kFrames = 4, kStableFrames = 8;
    Device* device = nullptr;
    ComPtr<ID3D12Resource> buffer;
    uint8_t* mapped = nullptr;
    uint32_t words = 0;
    uint32_t srv[kFrames] = { gpu::kNone, gpu::kNone, gpu::kNone, gpu::kNone };
    std::vector<uint32_t> revision;
    std::vector<uint64_t> since;
    void releaseViews()
    {
        if (!device) return;
        DescriptorHeaps* h = &device->descriptors();
        for (uint32_t& s : srv)
            if (s != gpu::kNone)
            {
                const uint32_t v = s;
                device->deferCall([h, v] { h->freeResource(v); });
                s = gpu::kNone;
            }
        if (buffer) device->deferRelease(buffer);
        buffer.Reset();
        mapped = nullptr;
    }
    ~AreaLightStability() { releaseViews(); }
    uint32_t publish(Device& d, const std::vector<gpu::Light>& lights, uint64_t frame)
    {
        device = &d;
        // Raw views start on 16-byte boundaries (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT): each slot is a whole number of 4 words.
        const uint32_t n = (uint32_t)lights.size(), need = std::max<uint32_t>(((n + 31) / 32 + 3) & ~3u, 4);
        if (revision.size() != n)
        {
            revision.assign(n, UINT32_MAX);
            since.assign(n, frame);
        }
        if (need > words)
        {
            releaseViews();
            words = need;
            D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC1 bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = (uint64_t)kFrames * words * 4;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(d.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&buffer)),
                  "M area light stability ring");
            buffer->SetName(L"M area light stability ring");
            D3D12_RANGE none{ 0, 0 };
            check(buffer->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map M area light stability ring");
            for (uint32_t k = 0; k < kFrames; ++k)
            {
                srv[k] = d.descriptors().allocateResource();
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Format = DXGI_FORMAT_R32_TYPELESS;
                sd.Buffer.FirstElement = (uint64_t)k * words;
                sd.Buffer.NumElements = words;
                sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                d.d3d()->CreateShaderResourceView(buffer.Get(), &sd, d.descriptors().resourceCpu(srv[k]));
            }
        }
        const uint32_t slot = (uint32_t)(frame % kFrames);
        uint32_t* out = reinterpret_cast<uint32_t*>(mapped + (size_t)slot * words * 4);
        std::memset(out, 0, (size_t)words * 4);
        for (uint32_t i = 0; i < n; ++i)
        {
            if (lights[i].revision != revision[i])
            {
                revision[i] = lights[i].revision;
                since[i] = frame;
            }
            if (frame - since[i] >= kStableFrames) out[i / 32] |= 1u << (i % 32);
        }
        return srv[slot];
    }
};
} // namespace

void prepareScene(FramePassContext& fc)
{
    TextureSystem& t = fc.state<TextureSystem>("M.textures");
    t.sync(fc.device, fc.scene);
    // A no-op when nothing changed (GpuScene keeps the buffer and the revision).
    fc.scene.setMaterialTextures(t.published());
    // the rect lights' source textures (scene::Light::sourceTexture): the same textures' SRVs into the light records
    // (and each image's mean colour, for the consumers that take the light as a point)
    if (const std::vector<uint32_t> sources = t.lightSourceTextures(); sources.size() == fc.scene.lights().size())
        fc.scene.setLightSourceTextures(sources, t.lightSourceMeans());
    // B2: which lights' specular R's reflection paths carry (only with R's emitters on; frames in flight use their own slot).
    if (fc.quality.has("raytracing.emitters") && fc.quality.boolean("raytracing.emitters"))
        fc.resources.areaLightStable = fc.state<AreaLightStability>("M.areaLightStability").publish(fc.device, fc.scene.lights(), fc.frame.frameIndex);
}

uint32_t textureTable(FramePassContext& fc)
{
    TextureSystem& t = fc.state<TextureSystem>("M.textures");
    t.sync(fc.device, fc.scene);
    return t.tableSrv();
}

ID3D12CommandSignature* dispatchSignature(FramePassContext& fc)
{
    Signature& s = fc.state<Signature>("M.dispatchSignature");
    if (!s.dispatch)
    {
        D3D12_INDIRECT_ARGUMENT_DESC a{};
        a.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC d{};
        d.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        d.NumArgumentDescs = 1;
        d.pArgumentDescs = &a;
        check(fc.device.d3d()->CreateCommandSignature(&d, nullptr, IID_PPV_ARGS(&s.dispatch)), "M dispatch signature");
    }
    return s.dispatch.Get();
}

void resolve(FramePassContext& fc, ViewResources& view)
{
    if (!view.visId.valid() || !view.visibleClusters.valid() || !view.depth.valid())
        fail("M.materialResolve: the view has no vis buffer (V's visibility must run first)");
    checkQuality(fc.quality);
    TextureSystem& textures = fc.state<TextureSystem>("M.textures");
    textures.sync(fc.device, fc.scene);

    const uint32_t W = view.view.width, H = view.view.height;
    ResolveOutputs o;
    o.tilesX = (W + kTile - 1) / kTile;
    o.tilesY = (H + kTile - 1) / kTile;
    o.bands = passBandCount(fc.quality, W, H);  // the shading passes' screen bands (INTERFACES v1.29)
    o.height = H;
    o.textureTableSrv = textures.tableSrv();
    const uint32_t tileCount = o.tilesX * o.tilesY, experiment = experimentMask(fc.quality);
    view.gbuffer = fc.graph.createTexture({ "m.gbuffer", W, H, 1, 1, DXGI_FORMAT_R32G32_UINT });
    view.reflectionLobeTiles = fc.graph.createTexture({ "m.reflection lobe tiles", o.tilesX, o.tilesY, 1, 1, DXGI_FORMAT_R8_UNORM });
    o.materialWord = fc.graph.createTexture({ "m.material word", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
    view.materialWord = o.materialWord;
    if (textures.anyEmissiveTexture()) o.emissive = fc.graph.createTexture({ "m.emissive", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    // (the class word: anisotropic pixels' frame word, an eye's pixels' eye word - MaterialEye.hlsli; with
    // shading.eye_model off the resolve writes an eye's pixels the word 0: no iris, the plain Subsurface model)
    if (fc.scene.anyAnisotropic() || fc.scene.anyEye()) o.anisoWord = fc.graph.createTexture({ "m.aniso word", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
    const bool eyeModel = !fc.quality.has("shading.eye_model") || fc.quality.boolean("shading.eye_model");  // (a quality set without M's shading file: on)
    o.tiles = fc.graph.createBuffer({ "m.tiles", (uint64_t)kShadeClassCount * tileCount * 4, 0 });
    o.tileArgs = fc.graph.createBuffer({ "m.tile args", (uint64_t)o.totalsOffset() + kShadeClassCount * 4, 0 });

    ID3D12PipelineState* begin = fc.shaders.compute("Passes/Material/ResolveBegin");
    const ResolveDebug& debug = fc.state<ResolveDebug>("M.resolveDebug");
    // Planar reflection views with R's mirror mask (v1.22) use the PLANAR_MASK variant; other views compile none of it.
    const bool planarMask = view.view.planarTileMask.valid() || view.view.planarMask.valid();
    const std::string kernelName = std::string("Passes/Material/Resolve.DEBUG") + (debug.buffer.valid() ? "1" : "0") + ".PLANAR_MASK" + (planarMask ? "1" : "0");
    ID3D12PipelineState* kernel = fc.shaders.compute(kernelName.c_str());
    const BufferRef debugBuffer = debug.buffer;
    const BufferRef args = o.tileArgs;
    // Planar reflection views (R, through FrameServices::renderView) are timed apart: their cost is the reflection
    // budget's (ARCHITECTURE 2.6 C_planar), not the main view's resolve.
    const bool planar = view.view.kind != gpu::ViewKind::Main;
    const uint32_t argEntries = kShadeClassCount * o.bands;
    fc.graph.addPass(planar ? "m.resolve.begin.planar" : "m.resolve.begin", QueueType::Graphics, [&](PassBuilder& b) { b.use(args, Use::UavCompute); },
                     [begin, args, argEntries](PassContext& c) {
                         const uint32_t k[4] = { c.uav(args), argEntries, 0, 0 };
                         c.cmd->SetPipelineState(begin);
                         c.computeConstants(k, 4);
                         c.cmd->Dispatch((argEntries + 31) / 32, 1, 1);
                     });

    const ViewResources v = view;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    // A7 surface layers (SurfaceLayers.hlsli): E's field and S's weather of the frame
    struct SurfaceInputs
    {
        BufferRef surfaceConstants, surfaceTable, surfacePool;
        uint32_t weather;
        TextureRef rainShadow;
    } surface{ fc.resources.surfaceConstants, fc.resources.surfaceTable, fc.resources.surfacePool, fc.resources.weather,
               fc.resources.rainShadow };
    if (surface.weather == UINT32_MAX) surface.weather = gpu::kNone;
    fc.graph.addPass(planar ? "m.resolve.planar" : "m.resolve", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(v.visId, Use::SrvCompute);
                         b.use(v.visibleClusters, Use::SrvCompute);
                         b.use(v.gbuffer, Use::UavCompute);
                         b.use(v.reflectionLobeTiles, Use::UavCompute);
                         b.use(o.materialWord, Use::UavCompute);
                         if (o.emissive.valid()) b.use(o.emissive, Use::UavCompute);
                         if (o.anisoWord.valid()) b.use(o.anisoWord, Use::UavCompute);
                         b.use(o.tiles, Use::UavCompute);
                         b.use(o.tileArgs, Use::UavCompute);
                         if (debugBuffer.valid()) b.use(debugBuffer, Use::UavCompute);
                         // Planar reflection views (v1.22): tiles without mirror pixels go to no class list.
                         if (v.view.planarTileMask.valid()) b.use(v.view.planarTileMask, Use::SrvCompute);
                         else if (v.view.planarMask.valid()) b.use(v.view.planarMask, Use::SrvCompute);
                         // E's decals of the view (A7; both invalid when none is live)
                         if (v.decalFrames.valid() && v.decalTiles.valid())
                         {
                             b.use(v.decalFrames, Use::SrvCompute);
                             b.use(v.decalTiles, Use::SrvCompute);
                         }
                         // E's surface state field and S's rain shadow map (A7 layers; invalid = none)
                         if (surface.surfaceConstants.valid())
                         {
                             b.use(surface.surfaceConstants, Use::SrvCompute);
                             b.use(surface.surfaceTable, Use::SrvCompute);
                             b.use(surface.surfacePool, Use::SrvCompute);
                         }
                         if (surface.rainShadow.valid()) b.use(surface.rainShadow, Use::SrvCompute);
                     },
                     [kernel, v, o, cb, tileCount, debugBuffer, experiment, surface, eyeModel](PassContext& c) {
                         const uint32_t tileMask = v.view.planarTileMask.valid() ? c.srv(v.view.planarTileMask) : gpu::kNone;
                         const uint32_t pixelMask = !v.view.planarTileMask.valid() && v.view.planarMask.valid() ? c.srv(v.view.planarMask) : gpu::kNone;
                         const bool decals = v.decalFrames.valid() && v.decalTiles.valid();
                         const bool field = surface.surfaceConstants.valid();
                         const uint32_t k[28] = { c.srv(v.visId), c.srv(v.visibleClusters), c.uav(v.gbuffer), c.uav(o.materialWord),
                                                  o.emissive.valid() ? c.uav(o.emissive) : gpu::kNone, c.uav(v.reflectionLobeTiles), c.uav(o.tiles), c.uav(o.tileArgs),
                                                  o.textureTableSrv, o.tilesX, o.tilesY, tileCount, debugBuffer.valid() ? c.uav(debugBuffer) : gpu::kNone, experiment,
                                                  tileMask, pixelMask, o.bands, o.height, decals ? c.srv(v.decalFrames) : gpu::kNone,
                                                  decals ? c.srv(v.decalTiles) : gpu::kNone,
                                                  field ? c.srv(surface.surfaceConstants) : gpu::kNone, field ? c.srv(surface.surfaceTable) : gpu::kNone,
                                                  field ? c.srv(surface.surfacePool) : gpu::kNone, surface.weather,
                                                  o.anisoWord.valid() ? c.uav(o.anisoWord) : gpu::kNone, eyeModel ? 1u : 0u, 0, 0 };
                         c.cmd->SetPipelineState(kernel);
                         c.bindFrameConstants(cb);
                         c.computeConstants(k, 28);
                         c.cmd->Dispatch(o.tilesX, o.tilesY, 1);
                     });

    ViewTable& table = fc.state<ViewTable>("M.views");
    if (table.frame != fc.frame.frameIndex)
    {
        table.frame = fc.frame.frameIndex;
        table.views.clear();
    }
    table.views.push_back({ view.frameConstants, o });
}

const ResolveOutputs& resolveOutputs(FramePassContext& fc, const ViewResources& view)
{
    ViewTable& table = fc.state<ViewTable>("M.views");
    if (table.frame == fc.frame.frameIndex)
        for (const auto& [key, o] : table.views)
            if (key == view.frameConstants) return o;
    fail("M: no material resolve recorded for this view in frame %llu", (unsigned long long)fc.frame.frameIndex);
}
} // namespace unx::render::material
