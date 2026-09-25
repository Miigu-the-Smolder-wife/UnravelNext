#include "unx/material/MaterialSystem.h"

#include "unx/core/Log.h"
#include "unx/material/TextureSystem.h"
#include "unx/render/GpuScene.h"

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
} // namespace

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
    o.textureTableSrv = textures.tableSrv();
    const uint32_t tileCount = o.tilesX * o.tilesY;
    view.gbuffer = fc.graph.createTexture({ "m.gbuffer", W, H, 1, 1, DXGI_FORMAT_R32G32_UINT });
    view.reflectionLobeTiles = fc.graph.createTexture({ "m.reflection lobe tiles", o.tilesX, o.tilesY, 1, 1, DXGI_FORMAT_R8_UNORM });
    o.materialWord = fc.graph.createTexture({ "m.material word", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
    if (textures.anyEmissiveTexture()) o.emissive = fc.graph.createTexture({ "m.emissive", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    o.tiles = fc.graph.createBuffer({ "m.tiles", (uint64_t)kShadeClassCount * tileCount * 4, 0 });
    o.tileArgs = fc.graph.createBuffer({ "m.tile args", (uint64_t)kShadeClassCount * sizeof(D3D12_DISPATCH_ARGUMENTS), 0 });

    ID3D12PipelineState* begin = fc.shaders.compute("Passes/Material/ResolveBegin");
    const ResolveDebug& debug = fc.state<ResolveDebug>("M.resolveDebug");
    ID3D12PipelineState* kernel = fc.shaders.compute(debug.buffer.valid() ? "Passes/Material/Resolve.DEBUG1" : "Passes/Material/Resolve.DEBUG0");
    const BufferRef debugBuffer = debug.buffer;
    const BufferRef args = o.tileArgs;
    // Planar reflection views (R, through FrameServices::renderView) are timed apart: their cost is the reflection
    // budget's (ARCHITECTURE 2.6 C_planar), not the main view's resolve.
    const bool planar = view.view.kind != gpu::ViewKind::Main;
    fc.graph.addPass(planar ? "m.resolve.begin.planar" : "m.resolve.begin", QueueType::Graphics, [&](PassBuilder& b) { b.use(args, Use::UavCompute); },
                     [begin, args](PassContext& c) {
                         const uint32_t k[4] = { c.uav(args), kShadeClassCount, 0, 0 };
                         c.cmd->SetPipelineState(begin);
                         c.computeConstants(k, 4);
                         c.cmd->Dispatch(1, 1, 1);
                     });

    const ViewResources v = view;
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    fc.graph.addPass(planar ? "m.resolve.planar" : "m.resolve", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(v.visId, Use::SrvCompute);
                         b.use(v.visibleClusters, Use::SrvCompute);
                         b.use(v.gbuffer, Use::UavCompute);
                         b.use(v.reflectionLobeTiles, Use::UavCompute);
                         b.use(o.materialWord, Use::UavCompute);
                         if (o.emissive.valid()) b.use(o.emissive, Use::UavCompute);
                         b.use(o.tiles, Use::UavCompute);
                         b.use(o.tileArgs, Use::UavCompute);
                         if (debugBuffer.valid()) b.use(debugBuffer, Use::UavCompute);
                     },
                     [kernel, v, o, cb, tileCount, debugBuffer](PassContext& c) {
                         const uint32_t k[13] = { c.srv(v.visId), c.srv(v.visibleClusters), c.uav(v.gbuffer), c.uav(o.materialWord),
                                                  o.emissive.valid() ? c.uav(o.emissive) : gpu::kNone, c.uav(v.reflectionLobeTiles), c.uav(o.tiles), c.uav(o.tileArgs),
                                                  o.textureTableSrv, o.tilesX, o.tilesY, tileCount, debugBuffer.valid() ? c.uav(debugBuffer) : gpu::kNone };
                         c.cmd->SetPipelineState(kernel);
                         c.bindFrameConstants(cb);
                         c.computeConstants(k, 13);
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
