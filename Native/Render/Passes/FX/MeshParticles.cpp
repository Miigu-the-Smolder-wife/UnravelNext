// Mesh particles (A3, FEATURES_GAME 0.A 7b; render C). See include/unx/fx/MeshParticles.h and FxMeshInstances.hlsl.
// Cost [expected]: the writer is one thread per render thread of the tick (sprites return after two loads), 160 B written
// per live mesh particle and 32 B of drawn record; the instances then cost V and M what any N_m dynamic instances do.
#include "unx/fx/MeshParticles.h"
#include "unx/fx/Particles.h"

#include "unx/core/Log.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>

namespace unx::fx
{
using namespace unx::render;

namespace
{
// Mirror of FxMeshInstances.hlsl MeshConstants (its first 256 B are ParticleLayerPass.hlsli LayerConstants).
struct MeshConstants
{
    float offsetCur[3]; float w;
    float offsetPrev[3]; float dt;
    uint32_t threads, current, rangeCount, recordCapacity;
    uint32_t layerWidth, layerHeight, tilesX, tilesY;
    uint32_t posAgeCur, velocityCur, posAgePrev, velocityPrev;
    uint32_t dynamicCur, dynamicPrev, emitters, programs;
    uint32_t curveKeys, ranges, blocks, records;
    uint32_t layerUnused[32];  // the render pass's own fields (tiles, lighting, ribbons)
    uint32_t ribbonRows;
    float streamAxes[3];
    // mesh particles
    uint32_t orientationCur, orientationPrev, drawnIn, drawnOut;
    uint32_t instances, count, countOffset, first;
    uint32_t capacity, assets, assetCount, counters;
    uint32_t mode; float wHistory; float frameDt; uint32_t revision;
    float originDelta[3]; uint32_t pad0;
};
static_assert(offsetof(MeshConstants, ribbonRows) == 240 && offsetof(MeshConstants, orientationCur) == 256);
static_assert(sizeof(MeshConstants) == 336);
constexpr uint32_t kSlots = 16;           // upload ring slots (frames in flight stay far below)
constexpr uint32_t kHeadBytes = 512;      // constants, then 16 zero bytes (the counters' clear) at 448
constexpr uint32_t kZeroOffset = 448;
constexpr uint32_t kDrawnBytes = 32;      // MeshDrawn
constexpr uint32_t kNone = 0xFFFFFFFFu;

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "FX mesh particle buffer");
    r->SetName(name);
    return r;
}
} // namespace

struct MeshParticlePass::Impl
{
    Device& device;
    explicit Impl(Device& d) : device(d) {}
    ComPtr<ID3D12Resource> upload, readback, counters, drawn[2];
    uint8_t* uploadMapped = nullptr;
    uint32_t assetCapacity = 0, next = 0, drawnSlots = 0, parity = 0;
    uint64_t slotBytes = 0;
    // history of the previous recorded frame (the previous transform's source)
    bool drawnValid = false;
    uint64_t lastSerial = 0;
    float lastW = 0;
    double lastOrigin[3] = {};
    uint32_t lastReadback = kNone, lastMode = 0;

    void sizeUpload(uint32_t assets)
    {
        if (upload && assets <= assetCapacity) return;
        device.waitIdle();
        if (upload) upload->Unmap(0, nullptr);
        assetCapacity = std::max<uint32_t>(256, assetCapacity);
        while (assetCapacity < assets) assetCapacity *= 2;
        slotBytes = kHeadBytes + 16ull * assetCapacity;
        upload = makeBuffer(device, slotBytes * kSlots, D3D12_HEAP_TYPE_UPLOAD, L"FX mesh particle constants ring");
        D3D12_RANGE none{ 0, 0 };
        check(upload->Map(0, &none, reinterpret_cast<void**>(&uploadMapped)), "map FX mesh particle constants");
        std::memset(uploadMapped, 0, slotBytes * kSlots);
    }
};

MeshParticlePass::MeshParticlePass(Device& device) : m_impl(std::make_unique<Impl>(device))
{
    Impl& m = *m_impl;
    m.counters = makeBuffer(device, 16, D3D12_HEAP_TYPE_DEFAULT, L"FX mesh particle counters");
    m.readback = makeBuffer(device, 16ull * kSlots, D3D12_HEAP_TYPE_READBACK, L"FX mesh particle counters readback");
}

MeshParticlePass::~MeshParticlePass()
{
    m_impl->device.waitIdle();
    if (m_impl->upload && m_impl->uploadMapped) m_impl->upload->Unmap(0, nullptr);
}

void MeshParticlePass::record(ParticleSystem& particles, FramePassContext& fc, const std::vector<MeshAsset>& assetList)
{
    Impl& m = *m_impl;
    m.lastReadback = kNone;
    const GpuScene::GpuInstanceRange range = fc.scene.gpuInstanceRange();
    const ParticleRenderInputs in = range.capacity ? particles.renderInputs(fc.graph, fc.frame.frameIndex) : ParticleRenderInputs{};
    if (!in.valid || !in.orientation[0].valid() || !in.orientation[1].valid() || in.threads == 0)
    {
        m.drawnValid = false;  // the next frame has no drawn records of this one
        return;
    }

    // drawn records: one per slot of the particle capacity; a grown capacity keeps the previous frame's records (the
    // layout indices do not move), copied into the larger buffer
    const uint32_t slots = std::max<uint32_t>(particles.capacity(), 1);
    ComPtr<ID3D12Resource> grownFrom;
    const uint32_t oldSlots = m.drawnSlots;
    if (slots > m.drawnSlots)
    {
        grownFrom = m.drawn[m.parity];  // the previous frame's output: this frame's input
        for (auto& d : m.drawn)
            if (d) m.device.deferRelease(d);
        m.drawnSlots = std::max(slots, m.drawnSlots * 2);
        m.drawn[0] = makeBuffer(m.device, (uint64_t)m.drawnSlots * kDrawnBytes, D3D12_HEAP_TYPE_DEFAULT, L"FX mesh particle drawn 0");
        m.drawn[1] = makeBuffer(m.device, (uint64_t)m.drawnSlots * kDrawnBytes, D3D12_HEAP_TYPE_DEFAULT, L"FX mesh particle drawn 1");
        if (!grownFrom) m.drawnValid = false;
    }

    // mesh table sorted by (hi, lo), the lookup's order
    std::vector<std::array<uint32_t, 4>> table;
    table.reserve(assetList.size());
    for (const MeshAsset& a : assetList)
        if (a.mesh != kNone) table.push_back({ (uint32_t)a.asset, (uint32_t)(a.asset >> 32), a.mesh, 0u });
    std::sort(table.begin(), table.end(), [](const auto& a, const auto& b) { return a[1] != b[1] ? a[1] < b[1] : a[0] < b[0]; });
    table.erase(std::unique(table.begin(), table.end(), [](const auto& a, const auto& b) { return a[0] == b[0] && a[1] == b[1]; }), table.end());
    m.sizeUpload((uint32_t)table.size());

    // frame fraction between the previous tick's end (0) and the latest's (1), as the particle render pass
    const double w = in.dt > 0 ? std::clamp((fc.frame.time - (in.tickTime - in.dt)) / in.dt, 0.0, 1.0) : 1.0;
    uint32_t mode = 0;
    if (m.drawnValid && m.lastSerial == in.tickSerial) mode = 1;
    else if (m.drawnValid && m.lastSerial + 1 == in.tickSerial && !in.reset) mode = 2;

    MeshConstants mc{};
    for (int a = 0; a < 3; ++a)
    {
        const double axis = fc.frame.streamAxes[a];
        mc.offsetCur[a] = (float)(in.anchor[1][a] - fc.frame.worldOrigin[a] * axis);
        mc.offsetPrev[a] = (float)(in.anchor[0][a] - fc.frame.worldOrigin[a] * axis);
        mc.streamAxes[a] = fc.frame.streamAxes[a];
        mc.originDelta[a] = (float)(m.lastOrigin[a] - fc.frame.worldOrigin[a]);
    }
    mc.w = (float)w;
    mc.dt = in.dt;
    mc.threads = in.threads;
    mc.current = in.current;
    mc.rangeCount = in.rangeCount;
    mc.ribbonRows = kNone;
    mc.instances = range.instanceUav;
    mc.count = range.countUav;
    mc.countOffset = range.countByteOffset;
    mc.first = range.first;
    mc.capacity = range.capacity;
    mc.assetCount = (uint32_t)table.size();
    mc.mode = mode;
    mc.wHistory = m.lastW;
    mc.frameDt = fc.frame.deltaTime;
    mc.revision = (uint32_t)fc.frame.frameIndex;

    m.lastSerial = in.tickSerial;
    m.lastW = (float)w;
    std::memcpy(m.lastOrigin, fc.frame.worldOrigin, sizeof m.lastOrigin);
    m.drawnValid = true;
    m.lastMode = mode;
    const uint32_t out = m.parity ^ 1u;
    m.parity = out;

    RenderGraph& g = fc.graph;
    const BufferRef constants = g.createBuffer(BufferDesc{ "fx.mesh.constants", sizeof(MeshConstants), sizeof(MeshConstants) });
    const BufferRef assets = g.createBuffer(BufferDesc{ "fx.mesh.assets", 16ull * std::max<size_t>(table.size(), 1), 16 });
    const BufferRef counters = g.importBuffer(m.counters.Get(), BufferDesc{ "fx.mesh.counters", 16, 4 });
    const BufferRef drawnIn = g.importBuffer(m.drawn[out ^ 1u].Get(), BufferDesc{ "fx.mesh.drawnIn", (uint64_t)m.drawnSlots * kDrawnBytes, kDrawnBytes });
    const BufferRef drawnOut = g.importBuffer(m.drawn[out].Get(), BufferDesc{ "fx.mesh.drawnOut", (uint64_t)m.drawnSlots * kDrawnBytes, kDrawnBytes });

    if (grownFrom && mode != 0)
    {
        const BufferRef old = g.importBuffer(grownFrom.Get(), BufferDesc{ "fx.mesh.drawnGrown", (uint64_t)oldSlots * kDrawnBytes, kDrawnBytes });
        g.addPass("fx.mesh.grow", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(old, Use::CopySrc);
                      b.use(drawnIn, Use::CopyDst);
                  },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(drawnIn), 0, c.resource(old), 0, (uint64_t)oldSlots * kDrawnBytes); });
    }

    const uint32_t slot = m.next;
    m.next = (m.next + 1) % kSlots;
    ID3D12Resource* upload = m.upload.Get();
    uint8_t* mapped = m.uploadMapped + slot * m.slotBytes;
    const uint64_t base = slot * m.slotBytes;
    if (!table.empty()) std::memcpy(mapped + kHeadBytes, table.data(), table.size() * 16);
    const ParticleRenderInputs inputs = in;
    const uint64_t tableBytes = table.size() * 16;
    g.addPass("fx.mesh.constants", QueueType::Graphics,
              [=](PassBuilder& b) {
                  b.use(constants, Use::CopyDst);
                  b.use(assets, Use::CopyDst);
                  b.use(counters, Use::CopyDst);
              },
              [=](PassContext& c) mutable {
                  mc.posAgeCur = c.srv(inputs.posAge[1]);
                  mc.velocityCur = c.srv(inputs.velocity[1]);
                  mc.posAgePrev = c.srv(inputs.posAge[0]);
                  mc.velocityPrev = c.srv(inputs.velocity[0]);
                  mc.dynamicCur = c.srv(inputs.dynamic[1]);
                  mc.dynamicPrev = c.srv(inputs.dynamic[0]);
                  mc.emitters = c.srv(inputs.emitters);
                  mc.programs = c.srv(inputs.programs);
                  mc.curveKeys = c.srv(inputs.curveKeys);
                  mc.ranges = c.srv(inputs.renderRanges);
                  mc.blocks = c.srv(inputs.renderBlocks);
                  mc.orientationCur = c.srv(inputs.orientation[1]);
                  mc.orientationPrev = c.srv(inputs.orientation[0]);
                  mc.drawnIn = c.srv(drawnIn);
                  mc.drawnOut = c.uav(drawnOut);
                  mc.assets = c.srv(assets);
                  mc.counters = c.uav(counters);
                  std::memcpy(mapped, &mc, sizeof mc);
                  c.cmd->CopyBufferRegion(c.resource(constants), 0, upload, base, sizeof mc);
                  if (tableBytes) c.cmd->CopyBufferRegion(c.resource(assets), 0, upload, base + kHeadBytes, tableBytes);
                  c.cmd->CopyBufferRegion(c.resource(counters), 0, upload, base + kZeroOffset, 16);
              });

    ID3D12PipelineState* pso = fc.shaders.compute("Passes/FX/FxMeshInstances");
    const uint32_t groups = (in.threads + 255) / 256;
    ID3D12Resource* rangeBuffers[2] = { range.instanceBuffer, range.countBuffer };
    g.addPass("fx.mesh.instances", QueueType::Graphics,
              [=](PassBuilder& b) {
                  for (const BufferRef& x : { inputs.posAge[0], inputs.posAge[1], inputs.velocity[0], inputs.velocity[1], inputs.dynamic[0], inputs.dynamic[1],
                                              inputs.emitters, inputs.programs, inputs.curveKeys, inputs.renderRanges, inputs.renderBlocks,
                                              inputs.orientation[0], inputs.orientation[1], constants, assets, drawnIn })
                      b.use(x, Use::SrvCompute);
                  b.use(drawnOut, Use::UavCompute);
                  b.use(counters, Use::UavCompute);
                  b.keep();  // writes the scene's instance range (outside the graph)
              },
              [=](PassContext& c) {
                  // The range's buffers are the scene's (read as shader resources through the frame, after the scene update
                  // that zeroed the count): writable for this dispatch, readable again before V.
                  D3D12_BUFFER_BARRIER toWrite[2], toRead[2];
                  for (int k = 0; k < 2; ++k)
                  {
                      toWrite[k] = { D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
                                     D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, rangeBuffers[k], 0, UINT64_MAX };
                      toRead[k] = { D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                    D3D12_BARRIER_ACCESS_SHADER_RESOURCE, rangeBuffers[k], 0, UINT64_MAX };
                  }
                  D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_BUFFER, 2 };
                  group.pBufferBarriers = toWrite;
                  c.cmd->Barrier(1, &group);
                  const std::array<uint32_t, 8> p = { c.srv(constants), 0, 0, 0, 0, 0, 0, 0 };
                  c.cmd->SetPipelineState(pso);
                  c.computeConstants(p.data(), 8);
                  c.cmd->Dispatch(groups, 1, 1);
                  group.pBufferBarriers = toRead;
                  c.cmd->Barrier(1, &group);
              });

    ID3D12Resource* readback = m.readback.Get();
    g.addPass("fx.mesh.readback", QueueType::Graphics,
              [=](PassBuilder& b) {
                  b.use(counters, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& c) { c.cmd->CopyBufferRegion(readback, 16ull * slot, c.resource(counters), 0, 16); });
    m.lastReadback = slot;
}

MeshParticleStats MeshParticlePass::stats()
{
    Impl& m = *m_impl;
    MeshParticleStats s;
    if (m.lastReadback == kNone) return s;
    m.device.waitIdle();
    uint32_t words[4] = {};
    uint8_t* p = nullptr;
    const D3D12_RANGE range{ 16ull * m.lastReadback, 16ull * m.lastReadback + 16 };
    check(m.readback->Map(0, &range, reinterpret_cast<void**>(&p)), "map FX mesh particle counters");
    std::memcpy(words, p + range.Begin, 16);
    const D3D12_RANGE none{ 0, 0 };
    m.readback->Unmap(0, &none);
    s.instances = words[0];
    s.overflow = words[1];
    s.unmapped = words[2];
    s.status = words[3];
    s.mode = m.lastMode;
    s.recorded = true;
    return s;
}

std::vector<MeshAsset>& meshAssets(TrackState& state) { return state.get<std::vector<MeshAsset>>("fx.meshAssets"); }

MeshParticlePass* findMeshParticles(TrackState& state) { return state.get<std::unique_ptr<MeshParticlePass>>("fx.meshes").get(); }
} // namespace unx::fx

namespace unx::render::tracks
{
void particleMeshes(FramePassContext& fc)
{
    if (!fc.trackState) return;
    fx::ParticleSystem* particles = fx::findParticles(*fc.trackState);
    if (!particles) return;
    auto& pass = fc.trackState->get<std::unique_ptr<fx::MeshParticlePass>>("fx.meshes");
    if (!pass) pass = std::make_unique<fx::MeshParticlePass>(fc.device);
    pass->record(*particles, fc, fx::meshAssets(*fc.trackState));
}
} // namespace unx::render::tracks
