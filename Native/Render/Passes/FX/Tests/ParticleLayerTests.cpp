// FX particle render pass correctness (request 20260926_FX_particle_render_pass.md 7, stage 1: emissive sprites):
//   1. the RPP stream (reduced root population by default) through the particle module for --ticks ticks;
//   2. a camera looking at the particles, a test opaque depth (a wall at the particles' median depth over the left half of
//      the view, sky elsewhere), the frame time half way between the last two ticks (w = 0.5);
//   3. the pass (records -> tile lists -> per tile sort, 1/4 composite, full-resolution edge blocks), then the reference
//      (per pixel: every covering record in front of the pixel's surface, sorted per pixel, composited at full resolution)
//      and the pass's output through fxParticleLayerAt (the function M's shading composite calls);
//   4. checks: pass status 0 (no entry, tile or edge overflow), records drawn > 0, edge pixels equal to the reference within
//      half precision (|dT| <= 2e-3, |dL| <= 2e-3 max(L, 1)), the other pixels within the layer's band bound (|dT| <= 1/256
//      + half precision), the pass's layer, edge pixels and composited pixels identical in two runs (determinism; edge
//      block indices come from an atomic counter, so their storage order is not compared).
// Options: --ticks N (60) --particles P (8192) --width W --height H (1920 x 1080) --warp --no-debug-layer --out DIR (PFM
//          images of the reference and the composite).
#include "RppStream.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/fx/ParticleLayer.h"
#include "unx/fx/Particles.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/render/Tracks.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#define FX_LOG(fmt, ...) unx::logf(fmt "\n", ##__VA_ARGS__)
using namespace unx;
using namespace unx::render;
namespace fs = std::filesystem;

namespace
{
#define FX_CHECK(cond, ...)                                                                                           \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            logf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            logf(__VA_ARGS__);                                                                                        \
            logf("\n");                                                                                               \
            throw std::runtime_error("check failed");                                                                 \
        }                                                                                                             \
    } while (0)

struct Options
{
    uint32_t ticks = 60, width = 1920, height = 1080;
    bool warp = false, debugLayer = true;
    std::string out;
    fx::test::RppConfig rpp;
};

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d,
                                                type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                IID_PPV_ARGS(&r)),
          "test buffer");
    return r;
}

// Copies of graph resources into readback memory, filled after the graph ran (finish()).
struct Readbacks
{
    Device& device;
    std::vector<std::function<void()>> after;

    std::shared_ptr<std::vector<uint8_t>> texture(RenderGraph& g, TextureRef t, uint32_t& rowPitch)
    {
        auto out = std::make_shared<std::vector<uint8_t>>();
        auto pitch = std::make_shared<uint32_t>(0);
        Device* dev = &device;
        auto* list = &after;
        g.addPass("fx.test.readback", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(t, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& c) {
                      ID3D12Resource* src = c.resource(t);
                      D3D12_RESOURCE_DESC d = src->GetDesc();
                      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                      UINT rows;
                      UINT64 rowBytes, total;
                      dev->d3d()->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
                      ComPtr<ID3D12Resource> buffer = makeBuffer(*dev, total, D3D12_HEAP_TYPE_READBACK);
                      D3D12_TEXTURE_COPY_LOCATION dst{ buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                      dst.PlacedFootprint = fp;
                      D3D12_TEXTURE_COPY_LOCATION s{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                      s.SubresourceIndex = 0;
                      c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
                      *pitch = fp.Footprint.RowPitch;
                      list->push_back([buffer, out, total]() {
                          void* p = nullptr;
                          D3D12_RANGE r{ 0, (SIZE_T)total };
                          check(buffer->Map(0, &r, &p), "map readback");
                          out->assign((uint8_t*)p, (uint8_t*)p + total);
                          D3D12_RANGE none{ 0, 0 };
                          buffer->Unmap(0, &none);
                      });
                  });
        pitches.push_back({ pitch, &rowPitch });
        return out;
    }
    std::shared_ptr<std::vector<uint8_t>> buffer(RenderGraph& g, BufferRef b, uint64_t bytes)
    {
        auto out = std::make_shared<std::vector<uint8_t>>();
        Device* dev = &device;
        auto* list = &after;
        g.addPass("fx.test.readback.buffer", QueueType::Graphics,
                  [=](PassBuilder& pb) {
                      pb.use(b, Use::CopySrc);
                      pb.keep();
                  },
                  [=](PassContext& c) {
                      ComPtr<ID3D12Resource> buffer = makeBuffer(*dev, bytes, D3D12_HEAP_TYPE_READBACK);
                      c.cmd->CopyBufferRegion(buffer.Get(), 0, c.resource(b), 0, bytes);
                      list->push_back([buffer, out, bytes]() {
                          void* p = nullptr;
                          D3D12_RANGE r{ 0, (SIZE_T)bytes };
                          check(buffer->Map(0, &r, &p), "map readback");
                          out->assign((uint8_t*)p, (uint8_t*)p + bytes);
                          D3D12_RANGE none{ 0, 0 };
                          buffer->Unmap(0, &none);
                      });
                  });
        return out;
    }
    void finish()
    {
        device.waitIdle();
        for (auto& f : after) f();
        after.clear();
        for (auto& p : pitches) *p.second = *p.first;
        pitches.clear();
    }
    std::vector<std::pair<std::shared_ptr<uint32_t>, uint32_t*>> pitches;
};

void writePfm(const fs::path& path, const std::vector<uint8_t>& rgba32f, uint32_t pitch, uint32_t w, uint32_t h, bool alpha)
{
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.wstring().c_str(), L"wb") != 0 || !f) fail("cannot write %s", path.string().c_str());
    std::fprintf(f, "PF\n%u %u\n-1.0\n", w, h);
    std::vector<float> row(3 * (size_t)w);
    for (uint32_t y = 0; y < h; ++y)
    {
        const float* src = reinterpret_cast<const float*>(rgba32f.data() + (size_t)(h - 1 - y) * pitch);
        for (uint32_t x = 0; x < w; ++x)
            for (int k = 0; k < 3; ++k) row[3 * x + k] = alpha ? src[4 * x + 3] : src[4 * x + k];
        std::fwrite(row.data(), 4, row.size(), f);
    }
    std::fclose(f);
}

// What a run draws: the layer, which layer pixels are edge blocks, and every full-resolution pixel's (L, T) through
// fxParticleLayerAt. Edge block indices come from an atomic counter, so the blocks' storage order is not part of it.
struct RunResult
{
    std::vector<uint8_t> layer, edgeMask, composite;
    std::vector<uint8_t> records;  // the pass's particle records (fx::kLayerRecordBytes each)
    ViewDesc view;
    uint32_t status = 0, drawn = 0;  // the pass's status bits and records drawn
};
// Stage 2 check: the sun of the lit run (frame constants; no shadow pages, GI, local lights or air in this test).
const float3 kTestSunDirection = normalize(float3{ 0.3f, 0.8f, -0.5f }), kTestSunColor{ 1.0f, 0.9f, 0.8f };
constexpr float kTestSunIlluminance = 1000.0f, kTestPhase = 0.6f;

// material >= 0: every program's material (the refusal check uses 2, outside the contract).
// mirror: the renderer sees the stream through FrameContext::streamAxes (1, 1, -1) (the Unity World's mapping): the camera
// and the sun are the identity run's mirrored in z and the wall is left out, so every sprite lands mirrored across the
// view's vertical centre line (wall = false: the identity run without the wall, its counterpart).
RunResult run(Device& device, const Options& o, bool verify, bool lit = false, int material = -1, bool mirror = false, bool wall = true)
{
    ShaderLibrary shaders(device, executableDirectory() / "shaders");
    const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    GpuScene scene(device);
    RenderGraph graph(device);
    TrackState state;
    fx::ParticleSystem& ps = fx::particles(state, device, quality);
    fx::test::RppConfig rpp = o.rpp;
    if (lit)
    {
        rpp.material = 1;
        rpp.phase = kTestPhase;
    }
    if (material >= 0) rpp.material = (uint32_t)material;
    fx::test::RppStream stream(rpp);
    FrameContext frame;
    FrameServices services;
    std::vector<NV_StreamEvent> previous;
    std::vector<NV_StreamEmitter> table;
    NV_StreamHeader last{};
    for (uint32_t t = 1; t <= o.ticks; ++t)
    {
        const std::vector<uint8_t> packet = stream.next(t == 1 ? nullptr : &previous);
        const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet.data());
        nv_stream::apply_emitter_table(table, h, packet.data(), packet.size());
        ps.submit(packet.data(), packet.size());
        FrameResources resources;
        FramePassContext fc{ device, graph, shaders, quality, scene, frame, resources, services, [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state };
        tracks::simulation(fc);
        graph.execute(nullptr);
        ++frame.frameIndex;
        const fx::TickReadback rb = ps.readback(h.stream, h.generation, h.tick);
        FX_CHECK((rb.counters.status & ~1u) == 0, "tick %u: particle status 0x%x", t, rb.counters.status);
        previous = rb.events;
        last = h;
    }

    // camera: the particles' centre (world = anchor + origin_anchor + position) seen from 1.2 x their extent
    const std::vector<NV_StreamParticle> particles = ps.checkpoint(shaders);
    FX_CHECK(!particles.empty(), "no particles after %u ticks", o.ticks);
    double lo[3] = { 1e300, 1e300, 1e300 }, hi[3] = { -1e300, -1e300, -1e300 };
    for (const NV_StreamParticle& p : particles)
        for (int a = 0; a < 3; ++a)
        {
            const double x = last.anchor[a] + table[p.emitter].origin_anchor[a] + p.position[a];
            lo[a] = std::min(lo[a], x);
            hi[a] = std::max(hi[a], x);
        }
    double centre[3], extent = 0;
    for (int a = 0; a < 3; ++a) { centre[a] = 0.5 * (lo[a] + hi[a]); extent = std::max(extent, hi[a] - lo[a]); }
    const double eye[3] = { centre[0], centre[1] + 0.35 * extent, centre[2] - 1.1 * extent };
    const float zs = mirror ? -1.0f : 1.0f;  // stream -> renderer z sign
    scene::Camera cam;
    cam.position = { (float)eye[0], (float)eye[1], zs * (float)eye[2] };
    cam.forward = normalize(float3{ (float)(centre[0] - eye[0]), (float)(centre[1] - eye[1]), zs * (float)(centre[2] - eye[2]) });
    const ViewDesc view = ViewDesc::fromCamera(cam, o.width, o.height, float4x4{});
    const double wallDistance = std::sqrt((centre[0] - eye[0]) * (centre[0] - eye[0]) + (centre[1] - eye[1]) * (centre[1] - eye[1]) +
                                          (centre[2] - eye[2]) * (centre[2] - eye[2]));

    // frame constants of the view
    ComPtr<ID3D12Resource> cbuffer = makeBuffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD);
    {
        gpu::FrameConstants c{};
        c.viewProj = view.viewProj;
        c.prevViewProj = view.prevViewProj;
        c.invViewProj = view.invViewProj;
        c.view = view.view;
        c.proj = view.proj;
        c.cameraPosition = view.position;
        c.nearPlane = view.nearPlane;
        c.viewWidth = view.width;
        c.viewHeight = view.height;
        c.exposure = 1.0f;
        c.tanHalfFovY = std::tan(view.verticalFov * 0.5f);
        c.sunDirection = { kTestSunDirection.x, kTestSunDirection.y, zs * kTestSunDirection.z };
        c.sunIlluminance = kTestSunIlluminance;
        c.sunColor = kTestSunColor;
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(cbuffer->Map(0, &none, &p), "map frame constants");
        std::memcpy(p, &c, sizeof c);
        cbuffer->Unmap(0, nullptr);
    }
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = cbuffer->GetGPUVirtualAddress();

    // test depth (wall at the particles' centre distance over the left half), the pass, the reference, the composite
    Readbacks rb{ device };
    const TextureRef depth = graph.createTexture(TextureDesc{ "fx.test.depth", o.width, o.height, 1, 1, DXGI_FORMAT_R32_FLOAT });
    const float wallDepth = (float)(view.nearPlane / wallDistance);
    const uint32_t wallWidth = wall && !mirror ? o.width / 2 : 0;
    {
        ID3D12PipelineState* pso = shaders.compute("Passes/FX/Tests/FxLayerTestDepth");
        const uint32_t gx = (o.width + 7) / 8, gy = (o.height + 7) / 8;
        graph.addPass("fx.test.depth", QueueType::Graphics, [=](PassBuilder& b) { b.use(depth, Use::UavCompute); },
                      [=](PassContext& c) {
                          uint32_t bits;
                          std::memcpy(&bits, &wallDepth, 4);
                          const std::array<uint32_t, 8> p = { 0, c.uav(depth), bits, wallWidth, 0, 0, 0, 0 };
                          c.cmd->SetPipelineState(pso);
                          c.bindFrameConstants(frameConstants);
                          c.computeConstants(p.data(), 8);
                          c.cmd->Dispatch(gx, gy, 1);
                      });
    }
    fx::ParticleLayerPass pass(device);
    fx::ParticleLayerFrame lf;
    lf.view = &view;
    lf.frameConstants = frameConstants;
    lf.depth = depth;
    for (int a = 0; a < 3; ++a) lf.camera[a] = a == 2 ? zs * eye[a] : eye[a];
    lf.streamAxes[2] = zs;
    lf.time = last.time - 0.5 * last.dt;
    const fx::ParticleLayerOutput out = pass.record(ps, graph, shaders, frame.frameIndex, lf);
    FX_CHECK(out.valid, "the pass has no input");
    const TextureRef reference = graph.createTexture(TextureDesc{ "fx.test.reference", o.width, o.height, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
    const TextureRef composite = graph.createTexture(TextureDesc{ "fx.test.composite", o.width, o.height, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
    const BufferRef flags = graph.createBuffer(BufferDesc{ "fx.test.flags", 16, 4 });
    graph.addPass("fx.test.flags.clear", QueueType::Graphics, [=](PassBuilder& b) { b.use(flags, Use::CopyDst); },
                  [=](PassContext& c) {
                      D3D12_WRITEBUFFERIMMEDIATE_PARAMETER w{ c.address(flags), 0 };
                      c.cmd->WriteBufferImmediate(1, &w, nullptr);
                  });
    for (int step = 0; step < 2; ++step)
    {
        ID3D12PipelineState* pso = shaders.compute(step == 0 ? "Passes/FX/Tests/FxLayerReference.STEP0" : "Passes/FX/Tests/FxLayerReference.STEP1");
        const TextureRef target = step == 0 ? reference : composite;
        const uint32_t gx = (o.width + 7) / 8, gy = (o.height + 7) / 8;
        graph.addPass(step == 0 ? "fx.test.reference" : "fx.test.composite", QueueType::Graphics,
                      [=](PassBuilder& b) {
                          b.use(out.constants, Use::SrvCompute);
                          b.use(target, Use::UavCompute);
                          b.use(flags, Use::UavCompute);
                          b.use(depth, Use::SrvCompute);
                          if (step == 0)
                          {
                              b.use(out.tileCounts, Use::UavCompute);
                              b.use(out.tileStarts, Use::UavCompute);
                              b.use(out.entries, Use::UavCompute);
                              b.use(out.records, Use::UavCompute);
                              b.use(out.ribbonVertices, Use::UavCompute);
                              b.use(out.ribbonAppearance, Use::UavCompute);
                          }
                          else
                          {
                              b.use(out.layer, Use::SrvCompute);
                              b.use(out.edges, Use::SrvCompute);
                          }
                      },
                      [=](PassContext& c) {
                          const std::array<uint32_t, 8> p = { c.srv(out.constants), c.uav(target), c.uav(flags), 0, 0, 0, 0, 0 };
                          c.cmd->SetPipelineState(pso);
                          c.bindFrameConstants(frameConstants);
                          c.computeConstants(p.data(), 8);
                          c.cmd->Dispatch(gx, gy, 1);
                      });
    }
    uint32_t refPitch = 0, cmpPitch = 0, layerPitch = 0;
    auto refBytes = rb.texture(graph, reference, refPitch);
    auto cmpBytes = rb.texture(graph, composite, cmpPitch);
    auto layerBytes = rb.texture(graph, out.layer, layerPitch);
    const uint64_t edgeBytes = ((16ull + 4ull * out.layerWidth * out.layerHeight + 15) & ~15ull) + (uint64_t)out.edgeCapacity * 128;
    auto edgeData = rb.buffer(graph, out.edges, edgeBytes);
    auto recordData = rb.buffer(graph, out.records, (uint64_t)std::max<uint32_t>(out.threads, 1) * fx::kLayerRecordBytes);
    // the strip records after the sprite records (ribbon segments, FxLayerStrips)
    const uint32_t stripRecords = out.recordCount > out.threads ? out.recordCount - out.threads : 0u;
    auto stripData = stripRecords ? rb.buffer(graph, out.records, (uint64_t)out.recordCount * fx::kLayerRecordBytes) : nullptr;
    auto counterData = rb.buffer(graph, out.counters, 16);
    auto flagData = rb.buffer(graph, flags, 16);
    graph.execute(nullptr);
    rb.finish();
    FX_CHECK(device.drainDebugMessages() == 0, "D3D12 debug layer errors");

    uint32_t counters[4], refFlags;
    std::memcpy(counters, counterData->data(), 16);
    std::memcpy(&refFlags, flagData->data(), 4);
    const uint32_t edgeCount = [&] { uint32_t n; std::memcpy(&n, edgeData->data(), 4); return n; }();
    FX_LOG("frame w %.3f: %u render threads, %u records drawn, %u tile entries (capacity %u), %u edge blocks (capacity %u) of %u layer pixels, status 0x%x",
           out.w, out.threads, counters[3], counters[0], out.entryCapacity, edgeCount, out.edgeCapacity, out.layerWidth * out.layerHeight, counters[2]);
    RunResult result;
    result.layer = *layerBytes;
    result.composite = *cmpBytes;
    result.records = *recordData;
    result.view = view;
    result.status = counters[2];
    result.drawn = counters[3];
    result.edgeMask.resize((size_t)out.layerWidth * out.layerHeight);
    for (size_t i = 0; i < result.edgeMask.size(); ++i)
    {
        uint32_t index;
        std::memcpy(&index, edgeData->data() + 16 + 4 * i, 4);
        result.edgeMask[i] = index != 0xFFFFFFFFu ? 1 : 0;
    }
    if (!verify) return result;
    FX_CHECK(counters[2] == 0, "pass status 0x%x (1 entry overflow, 2 tile overflow, 4 edge overflow, 8 range)", counters[2]);
    FX_CHECK(refFlags == 0, "reference: more than 128 covering records at a pixel");
    FX_CHECK(counters[3] > 0, "no record drawn");
    if (stripData)
    {
        uint32_t strips = 0;
        for (uint32_t i = out.threads; i < out.recordCount; ++i)
        {
            float radius;
            uint32_t recordFlags;
            std::memcpy(&radius, stripData->data() + (size_t)i * fx::kLayerRecordBytes + 8, 4);
            std::memcpy(&recordFlags, stripData->data() + (size_t)i * fx::kLayerRecordBytes + 24, 4);
            strips += radius > 0 && (recordFlags & 2u) != 0;  // FX_LAYER_RECORD_STRIP
        }
        FX_LOG("ribbons: %u points, %u segments drawn as strips", stripRecords, strips);
        FX_CHECK(strips > 0, "the stream's ribbons drew no strip");
    }

    // pixel comparison: edge pixels (their layer pixel is an edge block) and the others
    const uint32_t lw = out.layerWidth;
    const uint8_t* table0 = edgeData->data() + 16;
    struct Worst { double dT = 0, dL = 0; uint64_t n = 0, drawn = 0; std::vector<double> errs; } edge, band;
    double maxL = 0;
    for (uint32_t y = 0; y < o.height; ++y)
        for (uint32_t x = 0; x < o.width; ++x)
        {
            const float* r = reinterpret_cast<const float*>(refBytes->data() + (size_t)y * refPitch) + 4 * x;
            const float* c = reinterpret_cast<const float*>(cmpBytes->data() + (size_t)y * cmpPitch) + 4 * x;
            uint32_t index;
            std::memcpy(&index, table0 + 4 * ((size_t)(y / 4) * lw + x / 4), 4);
            Worst& wst = index != 0xFFFFFFFFu ? edge : band;
            const double dT = std::abs((double)r[3] - c[3]);
            double dL = 0;
            for (int k = 0; k < 3; ++k)
            {
                dL = std::max(dL, std::abs((double)r[k] - c[k]) / std::max(1.0, (double)r[k]));
                maxL = std::max(maxL, (double)r[k]);
            }
            wst.dT = std::max(wst.dT, dT);
            wst.dL = std::max(wst.dL, dL);
            ++wst.n;
            if (r[3] < 1.0f) { ++wst.drawn; wst.errs.push_back(dT); }
        }
    auto p99 = [](std::vector<double>& v) { if (v.empty()) return 0.0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(v.size() * 0.99))]; };
    FX_LOG("edge pixels %llu (%llu covered): |dT| max %.3g p99 %.3g, |dL|/max(L,1) max %.3g", (unsigned long long)edge.n, (unsigned long long)edge.drawn, edge.dT,
           p99(edge.errs), edge.dL);
    FX_LOG("layer pixels %llu (%llu covered): |dT| max %.3g p99 %.3g, |dL|/max(L,1) max %.3g; max reference radiance %.3g", (unsigned long long)band.n,
           (unsigned long long)band.drawn, band.dT, p99(band.errs), band.dL, maxL);
    if (!o.out.empty())
    {
        fs::create_directories(o.out);
        writePfm(fs::path(o.out) / "reference.pfm", *refBytes, refPitch, o.width, o.height, false);
        writePfm(fs::path(o.out) / "composite.pfm", *cmpBytes, cmpPitch, o.width, o.height, false);
        writePfm(fs::path(o.out) / "reference_T.pfm", *refBytes, refPitch, o.width, o.height, true);
    }
    FX_CHECK(edge.drawn + band.drawn > 0, "no covered pixel");
    FX_CHECK(edge.dT <= 2e-3 && edge.dL <= 2e-3, "edge pixels differ from the reference: |dT| %.3g |dL| %.3g", edge.dT, edge.dL);
    FX_CHECK(band.dT <= 1.0 / 256 + 2e-3 && band.dL <= 1.0 / 256 + 2e-3, "layer pixels outside the band bound: |dT| %.3g |dL| %.3g", band.dT, band.dL);
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        Options o;
        o.rpp.particles = 8192;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--ticks") o.ticks = (uint32_t)std::stoul(next());
            else if (a == "--particles") o.rpp.particles = (uint32_t)std::stoul(next());
            else if (a == "--width") o.width = (uint32_t)std::stoul(next());
            else if (a == "--height") o.height = (uint32_t)std::stoul(next());
            else if (a == "--warp") o.warp = true;
            else if (a == "--no-debug-layer") o.debugLayer = false;
            else if (a == "--out") o.out = next();
            else fail("unknown option %s", a.c_str());
        }
        if (o.rpp.particles < 524288u)
        {
            const double f = o.rpp.particles / 524288.0;
            o.rpp.ribbonPoints = std::max<uint32_t>(8u, (uint32_t)(o.rpp.ribbonPoints * f));
            o.rpp.volumeParticles = std::max<uint32_t>(8u, (uint32_t)(o.rpp.volumeParticles * f));
        }
        DeviceOptions opts;
        ComPtr<ID3D12Device> warpDevice;
        if (o.warp)
        {
            ComPtr<IDXGIFactory6> factory;
            check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&warpDevice))))
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&warpDevice)), "WARP device");
            opts.externalDevice = warpDevice.Get();
            opts.debugLayer = false;
        }
        else opts.debugLayer = o.debugLayer;
        Device device(opts);
        FX_LOG("device: %s (%s), view %u x %u, %u ticks, %u root particles", device.caps().adapter.c_str(), o.warp ? "WARP" : "hardware", o.width, o.height, o.ticks,
               o.rpp.particles);
        const RunResult a = run(device, o, true);
        const RunResult b = run(device, o, false);
        FX_CHECK(a.layer == b.layer && a.edgeMask == b.edgeMask && a.composite == b.composite,
                 "determinism: the layer, the edge pixels or the composited pixels differ between two runs");
        FX_LOG("determinism: layer, edge pixels and composited pixels bit identical between two runs");

        // Stage 2: the same particles lit by the sun alone (albedo = the emissive run's colour): each record's radiance
        // is the emissive one x E_sun x HG(dot(l, D), g), D from the camera to the particle centre (its record).
        const RunResult lit = run(device, o, false, true);
        FX_CHECK(lit.records.size() == a.records.size(), "lit run: %zu record bytes vs %zu", lit.records.size(), a.records.size());
        const ViewDesc& v = a.view;
        double worst = 0;
        uint32_t checked = 0;
        for (size_t i = 0; i + 32 <= a.records.size(); i += 32)
        {
            float e[4], l[4], centre[2], radius, depth;
            auto half4 = [](const uint8_t* p, float* out) {
                uint32_t w[2];
                std::memcpy(w, p, 8);
                const uint16_t h[4] = { (uint16_t)(w[0] & 0xFFFF), (uint16_t)(w[0] >> 16), (uint16_t)(w[1] & 0xFFFF), (uint16_t)(w[1] >> 16) };
                for (int k = 0; k < 4; ++k)
                {
                    const uint32_t s = (h[k] >> 15) & 1, ex = (h[k] >> 10) & 31, m = h[k] & 1023;
                    const float f = ex == 0 ? std::ldexp((float)m, -24) : std::ldexp(1.0f + m / 1024.0f, (int)ex - 15);
                    out[k] = s ? -f : f;
                }
            };
            std::memcpy(centre, a.records.data() + i, 8);
            std::memcpy(&radius, a.records.data() + i + 8, 4);
            std::memcpy(&depth, a.records.data() + i + 12, 4);
            if (!(radius > 0)) continue;
            half4(a.records.data() + i + 16, e);
            half4(lit.records.data() + i + 16, l);
            if (e[0] < 1e-2f) continue;
            // the centre's view ray: NDC -> view space direction (the view's inverse projection), then world
            const float x = centre[0] / v.width * 2 - 1, y = 1 - centre[1] / v.height * 2;
            const float tanY = std::tan(v.verticalFov * 0.5f), tanX = tanY * v.width / v.height;
            const float3 dv = normalize(float3{ x * tanX, y * tanY, -1.0f });
            // view -> world: the view matrix's rotation rows (orthonormal)
            const float3 D = normalize(float3{ v.view.m[0][0] * dv.x + v.view.m[1][0] * dv.y + v.view.m[2][0] * dv.z,
                                               v.view.m[0][1] * dv.x + v.view.m[1][1] * dv.y + v.view.m[2][1] * dv.z,
                                               v.view.m[0][2] * dv.x + v.view.m[1][2] * dv.y + v.view.m[2][2] * dv.z });
            const float c = kTestSunDirection.x * D.x + kTestSunDirection.y * D.y + kTestSunDirection.z * D.z;
            const float g = kTestPhase, g2 = g * g;
            const float hg = (1 - g2) / (4 * 3.14159265f * std::pow(std::max(1 + g2 - 2 * g * c, 1e-6f), 1.5f));
            const float expected[3] = { e[0] * kTestSunIlluminance * kTestSunColor.x * hg, e[1] * kTestSunIlluminance * kTestSunColor.y * hg,
                                        e[2] * kTestSunIlluminance * kTestSunColor.z * hg };
            for (int k = 0; k < 3; ++k) worst = std::max(worst, (double)std::abs(l[k] - expected[k]) / std::max(expected[k], 1e-3f));
            FX_CHECK(l[3] == e[3], "lit record %zu: opacity %g vs %g (lighting must not change it)", i / 32, l[3], e[3]);
            ++checked;
        }
        FX_LOG("stage 2: %u lit records vs albedo x E_sun x HG(g %.1f): worst relative error %.2e (half precision + the centre's pixel ray)", checked, kTestPhase, worst);
        FX_CHECK(checked > 100 && worst < 1e-2, "stage 2: lit records differ from the analytic sun term (worst %.3e over %u)", worst, checked);

        // The stream seen through the Unity World's axes (FrameContext::streamAxes (1, 1, -1), engine 2's N2 finding): the
        // same particles under a z-mirrored camera and sun land mirrored across the view's vertical centre line - each sprite
        // record's centre x -> width - x with the same y, radius and depth - and the composite is the plain one flipped.
        {
            const RunResult plain = run(device, o, false, false, -1, false, false);
            const RunResult mirrored = run(device, o, false, false, -1, true, false);
            FX_CHECK(plain.records.size() == mirrored.records.size() && plain.drawn == mirrored.drawn && plain.drawn > 0,
                     "mirror: %zu vs %zu record bytes, %u vs %u drawn", plain.records.size(), mirrored.records.size(), plain.drawn, mirrored.drawn);
            double worstCentre = 0, worstOther = 0;
            uint32_t compared = 0;
            for (size_t i = 0; i + 32 <= plain.records.size(); i += 32)
            {
                float ra[4], rm[4];
                std::memcpy(ra, plain.records.data() + i, 16);
                std::memcpy(rm, mirrored.records.data() + i, 16);
                if (!(ra[2] > 0) && !(rm[2] > 0)) continue;
                worstCentre = std::max({ worstCentre, (double)std::abs(rm[0] - ((float)o.width - ra[0])), (double)std::abs(rm[1] - ra[1]) });
                worstOther = std::max({ worstOther, (double)std::abs(rm[2] - ra[2]) / std::max(ra[2], 1e-3f), (double)std::abs(rm[3] - ra[3]) / std::max(std::abs(ra[3]), 1e-9f) });
                ++compared;
            }
            double worstPixel = 0;
            uint64_t over = 0, covered = 0;
            uint32_t wx = 0, wy = 0;
            const size_t rowBytes = plain.composite.size() / o.height;
            for (uint32_t y = 0; y < o.height; ++y)
                for (uint32_t x = 0; x < o.width; ++x)
                {
                    const float* pa = reinterpret_cast<const float*>(plain.composite.data() + y * rowBytes) + 4 * x;
                    const float* pb = reinterpret_cast<const float*>(mirrored.composite.data() + y * rowBytes) + 4 * (o.width - 1 - x);
                    double d = 0;
                    for (int k = 0; k < 4; ++k) d = std::max(d, (double)std::abs(pa[k] - pb[k]) / std::max(1.0, (double)std::abs(pa[k])));
                    covered += pa[3] < 1.0f;
                    over += d > 2e-3;
                    if (d > worstPixel) { worstPixel = d; wx = x; wy = y; }
                }
            {
                const float* pa = reinterpret_cast<const float*>(plain.composite.data() + wy * rowBytes) + 4 * wx;
                const float* pb = reinterpret_cast<const float*>(mirrored.composite.data() + wy * rowBytes) + 4 * (o.width - 1 - wx);
                FX_LOG("mirror: %llu of %llu covered pixels differ by more than 2e-3; worst at (%u, %u): (%.4f %.4f %.4f %.4f) vs (%.4f %.4f %.4f %.4f)", (unsigned long long)over,
                       (unsigned long long)covered, wx, wy, pa[0], pa[1], pa[2], pa[3], pb[0], pb[1], pb[2], pb[3]);
                for (int dy = -2; dy <= 2 && over > 0; ++dy)  // (the neighbourhood of the worst pixel when any differ)
                {
                    std::string lp, lm;
                    for (int dx = -8; dx <= 8; ++dx)
                    {
                        const int x = (int)wx + dx, y = (int)wy + dy;
                        if (x < 0 || y < 0 || x >= (int)o.width || y >= (int)o.height) continue;
                        const float* qa = reinterpret_cast<const float*>(plain.composite.data() + y * rowBytes) + 4 * x;
                        const float* qb = reinterpret_cast<const float*>(mirrored.composite.data() + y * rowBytes) + 4 * (o.width - 1 - x);
                        lp += format(" %.2f", qa[3]);
                        lm += format(" %.2f", qb[3]);
                    }
                    FX_LOG("  T plain  %s", lp.c_str());
                    FX_LOG("  T mirror %s", lm.c_str());
                }
            }
            FX_LOG("mirror (stream axes 1, 1, -1): %u records, centre |dx|, |dy| max %.3g px, radius / depth relative max %.3g; composite flipped max %.3g",
                   compared, worstCentre, worstOther, worstPixel);
            FX_CHECK(compared > 100 && worstCentre <= 2e-3 && worstOther <= 1e-5, "mirror: records do not land mirrored (centre %.3g px, other %.3g)", worstCentre, worstOther);
            FX_CHECK(worstPixel <= 2e-3, "mirror: the composite is not the plain one flipped (%.3g)", worstPixel);
        }

        // A material outside the contract (0 emissive nit, 1 lit albedo) is refused: nothing drawn, status bit 16.
        const RunResult refused = run(device, o, false, false, 2);
        FX_LOG("material 2: %u records drawn, status 0x%x", refused.drawn, refused.status);
        FX_CHECK(refused.drawn == 0 && (refused.status & 16u) != 0, "material 2 was drawn (%u records) or not reported (status 0x%x)", refused.drawn, refused.status);
        FX_LOG("FX particle layer tests PASS");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FX particle layer tests FAILED: %s\n", e.what());
        return 1;
    }
}
