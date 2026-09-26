// Water surface shading, stage 1 (track W). See include/unx/water/WaterSurface.h and WaterSurface.hlsli.
#include "unx/water/WaterSurface.h"

#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/Shaders.h"

#include <cstring>
#include <vector>

namespace unx::water
{
using namespace unx::render;
namespace
{
constexpr uint32_t kStatCount = 7, kRing = 4, kSlots = 64;

ComPtr<ID3D12Resource> hostBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name, uint8_t** mapped)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "water surface buffer");
    r->SetName(name);
    D3D12_RANGE none{ 0, 0 };
    check(r->Map(0, type == D3D12_HEAP_TYPE_READBACK ? nullptr : &none, reinterpret_cast<void**>(mapped)), "map water surface buffer");
    return r;
}
// The frame's slot table (upload ring: its SRV indices are known only when the pass executes) and the statistics
// readback ring.
struct SurfaceState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> table[kRing], readback;
    ComPtr<ID3D12CommandSignature> signature;  // DispatchIndirect (the record pass over coverageSpecial's header)
    uint8_t* tableMapped[kRing] = {};
    uint8_t* readbackMapped = nullptr;
    uint32_t tableSrv[kRing] = {};
    uint64_t frame[kRing] = { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX }, last = UINT64_MAX;
    ~SurfaceState()
    {
        if (!device) return;
        for (uint32_t i = 0; i < kRing; ++i)
        {
            table[i]->Unmap(0, nullptr);
            device->deferRelease(table[i]);
            device->descriptors().freeResource(tableSrv[i]);
        }
        readback->Unmap(0, nullptr);
        device->deferRelease(readback);
    }
    void ensure(Device& d)
    {
        if (device) return;
        device = &d;
        for (uint32_t i = 0; i < kRing; ++i)
        {
            table[i] = hostBuffer(d, kSlots * 16, D3D12_HEAP_TYPE_UPLOAD, L"water slot table", &tableMapped[i]);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.NumElements = kSlots * 4;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            tableSrv[i] = d.descriptors().allocateResource();
            d.d3d()->CreateShaderResourceView(table[i].Get(), &sd, d.descriptors().resourceCpu(tableSrv[i]));
        }
        readback = hostBuffer(d, kRing * 32, D3D12_HEAP_TYPE_READBACK, L"water surface statistics", &readbackMapped);
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sig{};
        sig.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        sig.NumArgumentDescs = 1;
        sig.pArgumentDescs = &arg;
        check(d.d3d()->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&signature)), "water surface dispatch signature");
    }
};
} // namespace

void waterSurface(FramePassContext& fc, ViewResources& view)
{
    if (view.view.kind != gpu::ViewKind::Main) return;
    RenderGraph& g = fc.graph;
    const FrameResources& r = fc.resources;
    // W's streams (fluids: triangle streams with vertices; slot = index in the frame's list, 63 is the sea's).
    std::vector<uint32_t> slots;
    for (uint32_t i = 0; i < (uint32_t)r.triangleStreams.size() && i < kSlots - 1; ++i)
        if (r.triangleStreams[i].vertices.valid()) slots.push_back(i);
    const bool interiorPass = view.waterVis.valid() && view.waterDepth.valid();
    const bool recordPass = view.coverageSpecial.valid() && view.coverageRecordRadiance.valid() && view.coverageRecords.valid() && view.coverageTileList.valid();
    if (slots.empty() || (!interiorPass && !recordPass)) return;
    if (!fc.trackState) fail("W: water surface shading needs the renderer's track state");
    SurfaceState& st = fc.state<SurfaceState>("W.surface");
    st.ensure(fc.device);
    WaterSurfaceDebug& debug = fc.state<WaterSurfaceDebug>("W.surface.debug");
    const uint32_t ring = uint32_t(fc.frame.frameIndex % kRing);
    const uint32_t W = view.view.width, H = view.view.height;
    const TextureRef colour = view.color, bandARadiance = view.bandARadiance;
    if (interiorPass)
    {
        const DXGI_FORMAT format = g.desc(colour).format;
        if (format != DXGI_FORMAT_R16G16B16A16_FLOAT && format != DXGI_FORMAT_R32G32B32A32_FLOAT)
            fail("W: the water layer needs the shaded colour as exposed linear float (M postActive), got format %u", (unsigned)format);
    }

    // The refraction source: band A's shaded radiance without the particle layer (M keeps it in bandARadiance), copied
    // (the interior pass overwrites the water pixels of the texture it reads).
    const TextureRef from = bandARadiance.valid() ? bandARadiance : colour;
    const TextureDesc fromDesc = g.desc(from);
    const TextureRef source = g.createTexture(TextureDesc{ "w.surface.source", fromDesc.width, fromDesc.height, 1, 1, fromDesc.format });
    g.addPass("w.surface.source", QueueType::Graphics, [&](PassBuilder& b) { b.use(from, Use::CopySrc); b.use(source, Use::CopyDst); },
              [=](PassContext& c) { c.cmd->CopyResource(c.resource(source), c.resource(from)); });
    const BufferRef stats = g.createBuffer({ "w.surface.stats", 32, 0 });
    ID3D12PipelineState* clear = fc.shaders.compute("Passes/Water/ViewGridClear");
    g.addPass("w.surface.stats clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(stats, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[4] = { 0, 0, c.uav(stats), 8 };  // ViewGridClear: the counter words only
                  c.cmd->SetPipelineState(clear);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(1, 1, 1);
              });
    TextureRef status, marchImage;
    if (debug.status && interiorPass)
    {
        status = g.createTexture(TextureDesc{ "w.surface.status", W, H, 1, 1, DXGI_FORMAT_R8_UINT });
        debug.image = status;
        marchImage = g.createTexture(TextureDesc{ "w.surface.march", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        debug.march = marchImage;
        ID3D12PipelineState* zero = fc.shaders.compute("Passes/Water/WaterStatusClear");
        g.addPass("w.surface.status clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(status, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(status), W, H, 0 };
                      c.cmd->SetPipelineState(zero);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                  });
        debug.status = false;
    }
    std::vector<BufferRef> vertices;
    std::vector<uint32_t> materials;
    for (uint32_t s : slots)
    {
        vertices.push_back(r.triangleStreams[s].vertices);
        materials.push_back(r.triangleStreams[s].material);
    }
    const TextureRef vis = view.waterVis, waterDepth = view.waterDepth, depth = view.depth;
    const TextureRef particleLayer = view.particleLayer;
    const BufferRef particleEdges = view.particleEdges;
    uint8_t* tableMapped = st.tableMapped[ring];
    const uint32_t tableSrv = st.tableSrv[ring];
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const uint32_t none = gpu::kNone;
    // Inputs both passes read (the shading function's), declared and bound alike.
    auto shadingUses = [&, vertices](PassBuilder& b) {
        b.use(source, Use::SrvCompute);
        b.use(depth, Use::SrvCompute);
        if (vis.valid()) b.use(vis, Use::SrvCompute);
        if (waterDepth.valid()) b.use(waterDepth, Use::SrvCompute);
        b.use(stats, Use::UavCompute);
        for (const BufferRef& v : vertices) b.use(v, Use::SrvCompute);
        if (r.giCache.valid()) b.use(r.giCache, Use::SrvCompute);
        for (const TextureRef& t : { r.transmittanceLut, r.multiScatterLut, view.airVolume })
            if (t.valid()) b.use(t, Use::SrvCompute);
        for (const BufferRef& x : { r.vsmPageTable, r.vsmBlocks, r.vsmSearchBound, r.vsmLayers })
            if (x.valid()) b.use(x, Use::SrvCompute);
        if (r.vsmAtlas.valid()) b.use(r.vsmAtlas, Use::SrvCompute);
    };
    struct Frame
    {
        BufferRef giCache, pageTable, blocks, searchBound, layers;
        TextureRef transmittance, multiScatter, airVolume;
        uint32_t vsmConstants;
    };
    const Frame fr{ r.giCache, r.vsmPageTable, r.vsmBlocks, r.vsmSearchBound, r.vsmLayers, r.transmittanceLut, r.multiScatterLut, view.airVolume, r.vsmConstants };
    // P[0..4] of both kernels (WaterInterior.hlsl, WaterRecords.hlsl); the first execute also fills the slot table.
    auto shadingConstants = [=](PassContext& c, uint32_t k[24], uint32_t first) {
        if (first)
        {
            std::memset(tableMapped, 0xFF, kSlots * 16);  // UNX_NONE: not a W stream
            for (size_t i = 0; i < slots.size(); ++i)
            {
                const uint32_t row[2] = { c.srv(vertices[i]), materials[i] };
                std::memcpy(tableMapped + 16 * slots[i], row, 8);
            }
        }
        const bool shadows = fr.pageTable.valid() && fr.vsmConstants != UINT32_MAX;
        const uint32_t values[20] = { first, c.srv(source), c.srv(depth), c.uav(stats),
                                      vis.valid() ? c.srv(vis) : none, waterDepth.valid() ? c.srv(waterDepth) : none, tableSrv,
                                      fr.giCache.valid() ? c.srv(fr.giCache) : none,
                                      fr.transmittance.valid() ? c.srv(fr.transmittance) : none, fr.multiScatter.valid() ? c.srv(fr.multiScatter) : none,
                                      fr.airVolume.valid() ? c.srv(fr.airVolume) : none, none,
                                      shadows ? c.srv(fr.pageTable) : none, shadows ? c.srv(fr.blocks) : none, shadows && fr.searchBound.valid() ? c.srv(fr.searchBound) : none,
                                      shadows ? fr.vsmConstants : none, shadows && fr.layers.valid() ? c.srv(fr.layers) : none, none, none, none };
        std::memcpy(k, values, sizeof values);
        k[20] = k[21] = k[22] = k[23] = none;
    };

    if (interiorPass)
    {
        ID3D12PipelineState* kernel = fc.shaders.compute("Passes/Water/WaterInterior");
        g.addPass("w.surface.interior", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      shadingUses(b);
                      b.use(colour, Use::UavCompute);
                      if (bandARadiance.valid()) b.use(bandARadiance, Use::UavCompute);
                      if (status.valid()) b.use(status, Use::UavCompute);
                      if (marchImage.valid()) b.use(marchImage, Use::UavCompute);
                      if (particleLayer.valid()) b.use(particleLayer, Use::SrvCompute);
                      if (particleEdges.valid()) b.use(particleEdges, Use::SrvCompute);
                  },
                  [=](PassContext& c) {
                      uint32_t k[24];
                      shadingConstants(c, k, 1);
                      k[0] = c.uav(colour);
                      k[11] = status.valid() ? c.uav(status) : none;
                      k[17] = marchImage.valid() ? c.uav(marchImage) : none;
                      k[20] = bandARadiance.valid() ? c.uav(bandARadiance) : none;
                      k[21] = particleLayer.valid() ? c.srv(particleLayer) : none;
                      k[22] = particleLayer.valid() && particleEdges.valid() ? c.srv(particleEdges) : none;
                      c.cmd->SetPipelineState(kernel);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 24);
                      c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                  });
    }
    if (recordPass)
    {
        ID3D12PipelineState* kernel = fc.shaders.compute("Passes/Water/WaterRecords");
        ID3D12CommandSignature* signature = st.signature.Get();
        const BufferRef special = view.coverageSpecial, records = view.coverageRecords, tileList = view.coverageTileList, radiance = view.coverageRecordRadiance;
        const bool fillsTable = !interiorPass;
        g.addPass("w.surface.records", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      shadingUses(b);
                      b.use(special, Use::SrvCompute);
                      b.use(special, Use::IndirectArgs);
                      b.use(records, Use::SrvCompute);
                      b.use(tileList, Use::SrvCompute);
                      b.use(radiance, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      uint32_t k[24];
                      shadingConstants(c, k, fillsTable ? 1 : 0);
                      k[0] = 0;
                      k[17] = c.srv(special);
                      k[18] = c.srv(records);
                      k[19] = c.srv(tileList);
                      k[20] = c.uav(radiance);
                      c.cmd->SetPipelineState(kernel);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 24);
                      c.cmd->ExecuteIndirect(signature, 1, c.resource(special), 4, nullptr, 0);  // header words 1..3
                  });
    }
    ID3D12Resource* readback = st.readback.Get();
    g.addPass("w.surface.stats read", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(stats, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& c) { c.cmd->CopyBufferRegion(readback, ring * 32, c.resource(stats), 0, 4 * kStatCount); });
    st.frame[ring] = fc.frame.frameIndex;
    st.last = fc.frame.frameIndex;
}

WaterSurfaceStats latestWaterSurfaceStats(TrackState& state)
{
    SurfaceState& st = state.get<SurfaceState>("W.surface");
    WaterSurfaceStats out;
    if (st.last == UINT64_MAX || !st.readbackMapped) return out;
    const uint32_t ring = uint32_t(st.last % kRing);
    uint32_t w[kStatCount];
    std::memcpy(w, st.readbackMapped + ring * 32, sizeof w);
    out.frameIndex = st.frame[ring];
    out.shaded = w[0], out.offscreen = w[1], out.exited = w[2], out.occluded = w[3], out.steps = w[4], out.inside = w[5], out.unlit = w[6];
    return out;
}
} // namespace unx::water
