// Water surface shading, stage 1 (track W). See include/unx/water/WaterSurface.h and WaterSurface.hlsli.
#include "unx/water/WaterSurface.h"
#include "unx/water/LinearDispatch.h"

#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace unx::water
{
using namespace unx::render;
namespace
{
constexpr uint32_t kStatCount = 11, kStatBytes = 64, kRing = 4, kSlots = 64, kRayJobs = 1u << 20;
// After the slot rows: the refraction source's pyramid, count + SRV per level (WaterSurface.hlsli WATER_LEVELS_OFFSET).
constexpr uint32_t kMaxLevels = 15, kTableBytes = kSlots * 16 + 4 * (1 + kMaxLevels + 0) + 0;

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
            table[i] = hostBuffer(d, kTableBytes, D3D12_HEAP_TYPE_UPLOAD, L"water slot table", &tableMapped[i]);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.NumElements = kTableBytes / 4;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            tableSrv[i] = d.descriptors().allocateResource();
            d.d3d()->CreateShaderResourceView(table[i].Get(), &sd, d.descriptors().resourceCpu(tableSrv[i]));
        }
        readback = hostBuffer(d, kRing * kStatBytes, D3D12_HEAP_TYPE_READBACK, L"water surface statistics", &readbackMapped);
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
    // Its box pyramid (WaterSourceMips.hlsl; WaterFootprint.hlsli integrates the refracted pixel footprint over it): level
    // k + 1 = the 2 x 2 means of level k, down to 1 x 1 (at most kMaxLevels levels, the source being level 0).
    std::vector<TextureRef> levels{ source };
    {
        ID3D12PipelineState* mips = fc.shaders.compute("Passes/Water/WaterSourceMips");
        uint32_t lw = fromDesc.width, lh = fromDesc.height;
        while ((lw > 1 || lh > 1) && levels.size() < kMaxLevels)
        {
            lw = std::max(1u, (lw + 1) / 2);
            lh = std::max(1u, (lh + 1) / 2);
            const TextureRef prev = levels.back(), next = g.createTexture(TextureDesc{ "w.surface.source level", lw, lh, 1, 1, fromDesc.format });
            const uint32_t w2 = lw, h2 = lh;
            g.addPass("w.surface.source level", QueueType::Graphics, [&](PassBuilder& b) { b.use(prev, Use::SrvCompute); b.use(next, Use::UavCompute); },
                      [=](PassContext& c) {
                          const uint32_t k[4] = { c.srv(prev), c.uav(next), w2, h2 };
                          c.cmd->SetPipelineState(mips);
                          c.computeConstants(k, 4);
                          c.cmd->Dispatch((w2 + 7) / 8, (h2 + 7) / 8, 1);
                      });
            levels.push_back(next);
        }
    }
    const BufferRef stats = g.createBuffer({ "w.surface.stats", kStatBytes, 0 });
    ID3D12PipelineState* clear = fc.shaders.compute("Passes/Water/ViewGridClear");
    g.addPass("w.surface.stats clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(stats, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[4] = { 0, 0, c.uav(stats), kStatBytes / 4 };  // ViewGridClear: the counter words only
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
    auto shadingUses = [&, vertices, levels](PassBuilder& b) {
        for (const TextureRef& l : levels) b.use(l, Use::SrvCompute);
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
            uint32_t pyramid[1 + kMaxLevels] = { uint32_t(levels.size()) };
            for (size_t l = 0; l < levels.size(); ++l) pyramid[1 + l] = c.srv(levels[l]);
            std::memcpy(tableMapped + kSlots * 16, pyramid, sizeof pyramid);
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

    // Stage 3: R's ray service. Lists sized for a band of rows (2 jobs per pixel: a reflection and, for a fallback, a
    // refraction) and reused by the record rounds; without the service nothing is written and each pass runs once.
    ID3D12CommandSignature* signature = st.signature.Get();
    const bool rays = static_cast<bool>(fc.services.traceRefractions) && (!interiorPass || bandARadiance.valid());
    const uint32_t jobCapacity = std::max(debug.rayJobCapacity ? debug.rayJobCapacity : kRayJobs, 2 * W);
    const uint32_t bandRows = rays ? std::min(H, jobCapacity / (2 * W)) : H, sampleCapacity = W * bandRows, jobs = 2 * sampleCapacity;
    BufferRef jobList, results, samples, applyArgs;
    ID3D12PipelineState* heads = rays ? fc.shaders.compute("Passes/Water/ViewGridClear") : nullptr;
    ID3D12PipelineState* argsKernel = rays ? fc.shaders.compute("Passes/Water/WaterRayArgs") : nullptr;
    ID3D12PipelineState* applyKernel = rays ? fc.shaders.compute("Passes/Water/WaterRayApply") : nullptr;
    if (rays)
    {
        jobList = g.createBuffer({ "w.surface.ray jobs", 16 + 48ull * jobs, 0 });
        results = g.createBuffer({ "w.surface.ray results", 8ull * jobs, 0 });
        samples = g.createBuffer({ "w.surface.ray samples", 16 + 80ull * sampleCapacity, 0 });  // (WATER_RAY_SAMPLE_BYTES)
        applyArgs = g.createBuffer({ "w.surface.ray apply args", 16, 0 });
    }
    auto clearLists = [&] {
        g.addPass("w.surface.ray clear", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(jobList, Use::UavCompute);
                      b.use(samples, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(heads);
                      const uint32_t a[4] = { 0, 0, c.uav(jobList), 4 }, s[4] = { 0, 0, c.uav(samples), 4 };  // the heads only
                      c.computeConstants(a, 4);
                      c.cmd->Dispatch(1, 1, 1);
                      c.computeConstants(s, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    };
    // After a band's or round's shading pass: the apply dispatch's size, R's rays, the apply pass (pixels: band A radiance
    // and colour; records: coverageRecordRadiance).
    auto traceAndApply = [&](BufferRef recordRadiance) {
        g.addPass("w.surface.ray args", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(samples, Use::UavCompute);
                      b.use(applyArgs, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(samples), c.uav(applyArgs), sampleCapacity, 0 };
                      c.cmd->SetPipelineState(argsKernel);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
        fc.services.traceRefractions(fc, jobList, results, jobs);
        const bool pixels = !recordRadiance.valid();
        g.addPass("w.surface.ray apply", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(samples, Use::SrvCompute);
                      b.use(applyArgs, Use::IndirectArgs);
                      b.use(results, Use::SrvCompute);
                      b.use(stats, Use::UavCompute);
                      if (pixels)
                      {
                          b.use(bandARadiance, Use::UavCompute);
                          b.use(colour, Use::UavCompute);
                          if (particleLayer.valid()) b.use(particleLayer, Use::SrvCompute);
                          if (particleEdges.valid()) b.use(particleEdges, Use::SrvCompute);
                      }
                      else b.use(recordRadiance, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[8] = { c.srv(samples), c.srv(results), pixels ? c.uav(bandARadiance) : none, pixels ? c.uav(colour) : none,
                                              pixels && particleLayer.valid() ? c.srv(particleLayer) : none,
                                              pixels && particleLayer.valid() && particleEdges.valid() ? c.srv(particleEdges) : none, c.uav(stats),
                                              pixels ? none : c.uav(recordRadiance) };
                      c.cmd->SetPipelineState(applyKernel);
                      c.computeConstants(k, 8);
                      c.cmd->ExecuteIndirect(signature, 1, c.resource(applyArgs), 0, nullptr, 0);
                  });
    };
    uint32_t rounds = 0;

    if (interiorPass)
    {
        ID3D12PipelineState* kernel = fc.shaders.compute("Passes/Water/WaterInterior");
        uint32_t bands = 0;
        for (uint32_t row0 = 0; row0 < H; row0 += bandRows, ++bands)
        {
            const uint32_t rows = std::min(bandRows, H - row0), first = row0 == 0 ? 1 : 0;
            if (rays) clearLists();
            g.addPass("w.surface.interior", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          shadingUses(b);
                          b.use(colour, Use::UavCompute);
                          if (bandARadiance.valid()) b.use(bandARadiance, Use::UavCompute);
                          if (status.valid()) b.use(status, Use::UavCompute);
                          if (marchImage.valid()) b.use(marchImage, Use::UavCompute);
                          if (particleLayer.valid()) b.use(particleLayer, Use::SrvCompute);
                          if (particleEdges.valid()) b.use(particleEdges, Use::SrvCompute);
                          if (rays)
                              for (const BufferRef& x : { jobList, results, samples }) b.use(x, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k[32];
                          shadingConstants(c, k, first);
                          k[0] = c.uav(colour);
                          k[11] = status.valid() ? c.uav(status) : none;
                          k[17] = marchImage.valid() ? c.uav(marchImage) : none;
                          k[20] = bandARadiance.valid() ? c.uav(bandARadiance) : none;
                          k[21] = particleLayer.valid() ? c.srv(particleLayer) : none;
                          k[22] = particleLayer.valid() && particleEdges.valid() ? c.srv(particleEdges) : none;
                          k[24] = rays ? c.uav(jobList) : none;
                          k[25] = rays ? c.uav(results) : none;
                          k[26] = rays ? c.uav(samples) : none;
                          k[27] = jobs;
                          k[28] = row0, k[29] = rows, k[30] = sampleCapacity, k[31] = 0;
                          c.cmd->SetPipelineState(kernel);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 32);
                          c.cmd->Dispatch((W + 7) / 8, (rows + 7) / 8, 1);
                      });
            if (rays) traceAndApply(BufferRef{});
        }
        debug.rayBands = rays ? bands : 0;
    }
    if (recordPass)
    {
        ID3D12PipelineState* kernel = fc.shaders.compute("Passes/Water/WaterRecords");
        const BufferRef special = view.coverageSpecial, records = view.coverageRecords, tileList = view.coverageTileList, radiance = view.coverageRecordRadiance;
        // Without rays one indirect pass over the list's header; with them rounds of sampleCapacity entries over the list's
        // capacity (its count is known only on the GPU: rounds past it do nothing).
        const uint32_t specialEntries = uint32_t((g.desc(special).size / 4 - 4) / 2);  // (CoverageLayer.hlsli COV_SPECIAL_HEADER = 4)
        const uint32_t roundSize = rays ? std::max(1u, std::min(sampleCapacity, specialEntries)) : UINT32_MAX;
        for (uint32_t base = 0; base == 0 || (rays && base < specialEntries); base += roundSize, ++rounds)
        {
            const uint32_t fillsTable = !interiorPass && base == 0 ? 1 : 0;
            if (rays) clearLists();
            g.addPass("w.surface.records", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          shadingUses(b);
                          b.use(special, Use::SrvCompute);
                          if (!rays) b.use(special, Use::IndirectArgs);
                          b.use(records, Use::SrvCompute);
                          b.use(tileList, Use::SrvCompute);
                          b.use(radiance, Use::UavCompute);
                          if (rays)
                              for (const BufferRef& x : { jobList, results, samples }) b.use(x, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k[32];
                          shadingConstants(c, k, fillsTable);
                          k[0] = 0;
                          k[17] = c.srv(special);
                          k[18] = c.srv(records);
                          k[19] = c.srv(tileList);
                          k[20] = c.uav(radiance);
                          k[24] = rays ? c.uav(jobList) : none;
                          k[25] = rays ? c.uav(results) : none;
                          k[26] = rays ? c.uav(samples) : none;
                          k[27] = jobs;
                          k[28] = base, k[29] = roundSize, k[30] = sampleCapacity, k[31] = 0;
                          c.cmd->SetPipelineState(kernel);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 32);
                          if (rays) dispatchLinear(c.cmd, (roundSize + 63) / 64);
                          else c.cmd->ExecuteIndirect(signature, 1, c.resource(special), 4, nullptr, 0);  // header words 1..3
                      });
            if (rays) traceAndApply(radiance);
        }
    }
    ID3D12Resource* readback = st.readback.Get();
    g.addPass("w.surface.stats read", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(stats, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& c) { c.cmd->CopyBufferRegion(readback, ring * kStatBytes, c.resource(stats), 0, 4 * kStatCount); });
    debug.rayRounds = rounds;
    if (debug.copyRadiance && bandARadiance.valid())
    {
        // (M's edge composite, after tracks::water, rewrites band A radiance at outline pixels)
        const TextureDesc rd = g.desc(bandARadiance);
        const TextureRef copy = g.createTexture(TextureDesc{ "w.surface.radiance copy", rd.width, rd.height, 1, 1, rd.format });
        g.addPass("w.surface.radiance copy", QueueType::Graphics, [&](PassBuilder& b) { b.use(bandARadiance, Use::CopySrc); b.use(copy, Use::CopyDst); },
                  [=](PassContext& c) { c.cmd->CopyResource(c.resource(copy), c.resource(bandARadiance)); });
        debug.radiance = copy;
        debug.copyRadiance = false;
    }
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
    std::memcpy(w, st.readbackMapped + ring * kStatBytes, sizeof w);
    out.frameIndex = st.frame[ring];
    out.shaded = w[0], out.offscreen = w[1], out.exited = w[2], out.occluded = w[3], out.steps = w[4], out.inside = w[5], out.unlit = w[6];
    out.rayOverflow = w[7], out.reflectJobs = w[8], out.refractJobs = w[9], out.traced = w[10];
    return out;
}
} // namespace unx::water
