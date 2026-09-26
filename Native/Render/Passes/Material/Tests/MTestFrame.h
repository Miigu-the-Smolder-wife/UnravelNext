#pragma once
// M track test frame: a FramePassContext without the other tracks, frame constants like FrameRenderer's, GPU readback,
// and a stand-in for V's band-A visibility (FakeVis.*.hlsl) that writes the vis buffer in V's format (INTERFACES 7.1)
// until V's output lands (5.5.1). Correctness only (no GPU lock). Shared by Passes/Material and Passes/Shading tests.
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/material/MaterialSystem.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuProfiler.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace unx::mtest
{
using namespace unx::render;

#define M_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

inline ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "M test buffer");
    return r;
}

// Default-heap buffer with the given contents (blocking upload).
inline ComPtr<ID3D12Resource> uploadStatic(Device& device, const void* data, uint64_t bytes, const wchar_t* name)
{
    ComPtr<ID3D12Resource> b = makeBuffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT);
    b->SetName(name);
    ComPtr<ID3D12Resource> staging = makeBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD);
    void* p = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &p), "map M staging");
    std::memcpy(p, data, bytes);
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(b.Get(), 0, staging.Get(), 0, bytes);
    const uint64_t fence = device.submit(cl);
    device.queue(QueueType::Graphics).waitCpu(fence);
    return b;
}

// Clusters for the stand-in raster: each submesh's triangles in index order, <= 64 per cluster (one submesh each),
// one LOD level (the source) per mesh. Same records as V's builder output (INTERFACES 6.5), without hierarchy.
inline ClusterData chunkClusters(const scene::Scene& s)
{
    ClusterData d;
    for (const scene::Mesh& m : s.meshes)
    {
        ClusterData::MeshRange range;
        range.clusterOffset = (uint32_t)d.clusters.size();
        for (uint32_t si = 0; si < (uint32_t)m.submeshes.size(); ++si)
        {
            const scene::Submesh& sm = m.submeshes[si];
            const uint32_t triCount = sm.indexCount / 3;
            for (uint32_t t0 = 0; t0 < triCount; t0 += 64)
            {
                gpu::Cluster c{};
                c.vertexOffset = (uint32_t)d.clusterVertexIndices.size();
                c.triangleOffset = (uint32_t)d.clusterTriangles.size();
                std::unordered_map<uint32_t, uint32_t> local;
                const uint32_t tn = std::min(64u, triCount - t0);
                float3 lo{ FLT_MAX, FLT_MAX, FLT_MAX }, hi{ -FLT_MAX, -FLT_MAX, -FLT_MAX };
                for (uint32_t t = 0; t < tn; ++t)
                {
                    uint32_t packed = 0;
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        const uint32_t mv = m.indices[sm.indexOffset + 3 * (t0 + t) + k];
                        auto it = local.find(mv);
                        if (it == local.end())
                        {
                            it = local.emplace(mv, (uint32_t)local.size()).first;
                            d.clusterVertexIndices.push_back(mv);
                            const float3 p = m.positions[mv];
                            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
                            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
                        }
                        packed |= it->second << (8 * k);
                    }
                    d.clusterTriangles.push_back(packed);
                }
                const float3 centre = (lo + hi) * 0.5f;
                float radius = 0;
                for (const auto& [mv, li] : local) radius = std::max(radius, length(m.positions[mv] - centre));
                c.boundsSphere = { centre.x, centre.y, centre.z, radius };
                c.normalCone = { 0, 0, 1, 2 };  // w >= 1: no cone
                c.counts = (uint32_t)local.size() | (tn << 8) | (si << 16);
                c.material = sm.material;
                c.lodError = 0;
                c.parentLodError = FLT_MAX;
                c.minFeatureWidth = 1e30f;
                c.brick = gpu::kNone;
                d.clusters.push_back(c);
            }
        }
        range.clusterCount = (uint32_t)d.clusters.size() - range.clusterOffset;
        range.lodLevelOffset = (uint32_t)d.lodLevels.size();
        range.lodLevelCount = 1;
        gpu::LodLevel level{};
        level.clusterOffset = (uint32_t)d.lodLevelClusters.size();
        level.clusterCount = range.clusterCount;
        level.error = 0;
        level.triangleCount = (uint32_t)(m.indices.size() / 3);
        for (uint32_t i = 0; i < range.clusterCount; ++i) d.lodLevelClusters.push_back(range.clusterOffset + i);
        d.lodLevels.push_back(level);
        d.meshes.push_back(range);
    }
    return d;
}

// Stand-in for V's band-A visibility: every cluster of every instance, hardware raster, V's output formats.
class FakeVisibility
{
public:
    // Installs the clusters into the GPU scene and builds the visible cluster list (all instances x clusters). Instances
    // in 'notRasterised' stay in the list (their vis ids decode, e.g. for coverage fragments) but come after the others
    // and the band-A raster skips them.
    void install(Device& device, GpuScene& gpuScene, const scene::Scene& s, const std::vector<uint32_t>& notRasterised = {})
    {
        ClusterData d = chunkClusters(s);
        const ClusterData::MeshRange* ranges = d.meshes.data();
        visible.clear();
        for (int pass = 0; pass < 2; ++pass)
        {
            for (uint32_t i = 0; i < (uint32_t)s.instances.size(); ++i)
            {
                const bool skipped = std::find(notRasterised.begin(), notRasterised.end(), i) != notRasterised.end();
                if (skipped != (pass == 1)) continue;
                const ClusterData::MeshRange& r = ranges[s.instances[i].mesh];
                for (uint32_t c = 0; c < r.clusterCount; ++c) visible.push_back({ i, r.clusterOffset + c });
            }
            if (pass == 0) rasterised = (uint32_t)visible.size();
        }
        clusters = d;
        gpuScene.setClusters(std::move(d));
        const std::vector<gpu::VisibleCluster>& v = visible.empty() ? std::vector<gpu::VisibleCluster>{ { 0, 0 } } : visible;
        list = uploadStatic(device, v.data(), v.size() * sizeof(gpu::VisibleCluster), L"M fake visible clusters");
    }

    // Creates view.depth / visId / visibleClusters and records the raster pass (targets cleared to VIS_NONE = 0 and
    // reversed-Z far = 0, INTERFACES 7.1 v1.5).
    void record(FramePassContext& fc, ViewResources& view)
    {
        const uint32_t W = view.view.width, H = view.view.height;
        view.depth = fc.graph.createTexture({ "m.fake depth", W, H, 1, 1, DXGI_FORMAT_D32_FLOAT });
        view.visId = fc.graph.createTexture({ "m.fake vis id", W, H, 1, 1, DXGI_FORMAT_R32_UINT });
        view.visibleClusters = fc.graph.importBuffer(list.Get(), { "m.fake visible clusters", (uint64_t)std::max<size_t>(visible.size(), 1) * 8, 8 });
        const uint32_t table = material::textureTable(fc);
        MeshPipelineDesc d;
        d.meshShader = "Passes/Material/Tests/FakeVis.ms";
        d.pixelShader = "Passes/Material/Tests/FakeVis.ps";
        d.renderTargets = { DXGI_FORMAT_R32_UINT };
        d.depthFormat = DXGI_FORMAT_D32_FLOAT;
        d.cull = D3D12_CULL_MODE_NONE;
        ID3D12PipelineState* pso = fc.shaders.mesh("m.test.fakevis", d);
        const TextureRef vis = view.visId, depth = view.depth;
        const BufferRef vcl = view.visibleClusters;
        const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
        const uint32_t groups = rasterised;
        fc.graph.addPass("m.test.fakevis.raster", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(vis, Use::RenderTarget);
                             b.use(depth, Use::DepthWrite);
                             b.use(vcl, Use::SrvGraphics);
                         },
                         [=](PassContext& c) {
                             const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(vis), dsv = c.dsv(depth);
                             const float zero[4] = {};
                             c.cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
                             c.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                             c.cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
                             D3D12_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 };
                             D3D12_RECT sc{ 0, 0, (LONG)W, (LONG)H };
                             c.cmd->RSSetViewports(1, &vp);
                             c.cmd->RSSetScissorRects(1, &sc);
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(cb);
                             const uint32_t k[4] = { c.srv(vcl), groups, table, 0 };
                             c.graphicsConstants(k, 4);
                             if (groups) c.cmd->DispatchMesh(std::min(65535u, groups), (groups + 65534) / 65535, 1);
                         });
    }

    std::vector<gpu::VisibleCluster> visible;
    uint32_t rasterised = 0;  // the first 'rasterised' entries of 'visible' are drawn
    ClusterData clusters;
    ComPtr<ID3D12Resource> list;
};

class TestFrame
{
public:
    // gpuValidation: GPU-based validation (implies the debug layer; out-of-range and state errors reported per access).
    explicit TestFrame(bool debugLayer = true, bool gpuValidation = false)
        : device([&] { DeviceOptions o; o.debugLayer = debugLayer || gpuValidation; o.gpuValidation = gpuValidation; return o; }()),
          shaders(device, executableDirectory() / "shaders"),
          quality(QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality")),
          gpuScene(device)
    {
        constants = makeBuffer(device, kSlots * 1024, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE none{ 0, 0 };
        check(constants->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map M test constants");
    }

    // Uploads the scene and installs the stand-in visibility's clusters ('notRasterised': FakeVisibility::install).
    void setScene(const scene::Scene& s, const std::vector<uint32_t>& notRasterised = {})
    {
        sceneData = s;
        gpuScene.upload(sceneData);
        vis.install(device, gpuScene, sceneData, notRasterised);
    }

    D3D12_GPU_VIRTUAL_ADDRESS frameConstantsFor(const ViewDesc& view)
    {
        if (slot >= kSlots) fail("M test frame: more than %u views", kSlots);
        gpu::FrameConstants c{};
        c.viewProj = view.viewProj;
        c.prevViewProj = view.prevViewProj;
        c.invViewProj = view.invViewProj;
        c.view = view.view;
        c.proj = view.proj;
        c.cameraPosition = view.position;
        c.nearPlane = view.nearPlane;
        c.clipPlane = view.clipPlane;
        c.viewWidth = view.width;
        c.viewHeight = view.height;
        c.viewKind = (uint32_t)view.kind;
        c.frameIndex = (uint32_t)frame.frameIndex;
        c.time = (float)frame.time;
        c.deltaTime = frame.deltaTime;
        c.exposure = 1.0f / (1.2f * std::exp2(view.ev100));
        c.tanHalfFovY = std::tan(view.verticalFov * 0.5f);
        c.sunDirection = sceneData.sun.direction;
        c.sunIlluminance = sceneData.sun.illuminance;
        c.sunColor = sceneData.sun.color;
        c.sunAngularRadius = sceneData.sun.angularRadius;
        c.windDirection = sceneData.windDirection;
        c.windSpeed = sceneData.windSpeed;
        gpuScene.fill(c);
        std::memcpy(mapped + slot * 1024, &c, sizeof c);
        return constants->GetGPUVirtualAddress() + (slot++) * 1024;
    }

    // Records one frame ('build' declares passes with the context), executes it and waits.
    void run(const std::function<void(FramePassContext&)>& build)
    {
        slot = 0;
        FrameResources resources;
        FrameServices services;
        FramePassContext fc{ device, graph, shaders, quality, gpuScene, frame, resources, services, [this](const ViewDesc& v) { return frameConstantsFor(v); }, &trackState };
        material::prepareScene(fc);  // before any frame constants, as FrameRenderer::record does (INTERFACES 5.2 v1.10)
        build(fc);
        if (profiler) profiler->beginFrame(frame.frameIndex);
        graph.execute(profiler);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) device.queue((QueueType)q).waitCpu(graph.lastFence((QueueType)q));
        if (profiler)
        {
            profiler->beginFrame(frame.frameIndex);  // one slot: reads this frame's timestamps
            lastTiming = profiler->lastCompleted() ? *profiler->lastCompleted() : FrameTiming{};
        }
        for (auto& f : afterFrame) f();
        afterFrame.clear();
        keepAlive.clear();
        ++frame.frameIndex;
        const uint32_t errors = device.drainDebugMessages();
        if (errors) fail("M test frame: %u debug layer errors", errors);
    }

    // Main view of the scene's first camera.
    ViewResources mainView(FramePassContext& fc, uint32_t width, uint32_t height, uint32_t camera = 0)
    {
        ViewResources v;
        v.view = ViewDesc::fromCamera(sceneData.cameras.at(camera), width, height, float4x4{});
        v.view.prevViewProj = v.view.viewProj;
        v.frameConstants = fc.frameConstantsFor(v.view);
        return v;
    }

    // Keeps a resource alive until the current frame has finished on the GPU (staging buffers of test passes).
    void keep(ComPtr<ID3D12Resource> r) { keepAlive.push_back(std::move(r)); }

    static uint32_t rowPitch(uint32_t width, uint32_t bytesPerTexel) { return (width * bytesPerTexel + 255) & ~255u; }

    // Copies mip 0 of 'texture' into a vector filled after run() (rows at rowPitch()).
    std::shared_ptr<std::vector<uint8_t>> readback(FramePassContext& fc, TextureRef texture)
    {
        auto out = std::make_shared<std::vector<uint8_t>>();
        fc.graph.addPass("m.test.readback", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(texture, Use::CopySrc);
                             b.keep();
                         },
                         [this, texture, out](PassContext& c) {
                             ID3D12Resource* src = c.resource(texture);
                             D3D12_RESOURCE_DESC d = src->GetDesc();
                             D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                             UINT rows;
                             UINT64 rowBytes, total;
                             device.d3d()->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
                             ComPtr<ID3D12Resource> buffer = makeBuffer(device, total, D3D12_HEAP_TYPE_READBACK);
                             D3D12_TEXTURE_COPY_LOCATION dst{ buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                             dst.PlacedFootprint = fp;
                             D3D12_TEXTURE_COPY_LOCATION s{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                             s.SubresourceIndex = 0;
                             c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
                             afterFrame.push_back([buffer, out, total]() {
                                 void* p = nullptr;
                                 D3D12_RANGE r{ 0, (SIZE_T)total };
                                 check(buffer->Map(0, &r, &p), "map readback");
                                 out->assign((uint8_t*)p, (uint8_t*)p + total);
                                 D3D12_RANGE none{ 0, 0 };
                                 buffer->Unmap(0, &none);
                             });
                         });
        return out;
    }

    std::shared_ptr<std::vector<uint8_t>> readbackBuffer(FramePassContext& fc, BufferRef buffer, uint64_t bytes)
    {
        auto out = std::make_shared<std::vector<uint8_t>>();
        fc.graph.addPass("m.test.readback.buffer", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(buffer, Use::CopySrc);
                             b.keep();
                         },
                         [this, buffer, bytes, out](PassContext& c) {
                             ComPtr<ID3D12Resource> rb = makeBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK);
                             c.cmd->CopyBufferRegion(rb.Get(), 0, c.resource(buffer), 0, bytes);
                             afterFrame.push_back([rb, out, bytes]() {
                                 void* p = nullptr;
                                 D3D12_RANGE r{ 0, (SIZE_T)bytes };
                                 check(rb->Map(0, &r, &p), "map readback");
                                 out->assign((uint8_t*)p, (uint8_t*)p + bytes);
                                 D3D12_RANGE none{ 0, 0 };
                                 rb->Unmap(0, &none);
                             });
                         });
        return out;
    }

    Device device;
    ShaderLibrary shaders;
    QualityConfig quality;
    GpuScene gpuScene;
    scene::Scene sceneData;
    RenderGraph graph{ device };
    FrameContext frame;
    TrackState trackState;
    FakeVisibility vis;
    GpuProfiler* profiler = nullptr;  // optional (one frame in flight): per-pass GPU times of each run in lastTiming
    FrameTiming lastTiming;

private:
    static constexpr uint32_t kSlots = 16;
    ComPtr<ID3D12Resource> constants;
    uint8_t* mapped = nullptr;
    uint32_t slot = 0;
    std::vector<std::function<void()>> afterFrame;
    std::vector<ComPtr<ID3D12Resource>> keepAlive;
};

// Typed texel of a readback (rows at TestFrame::rowPitch).
template <typename T>
T texelOf(const std::vector<uint8_t>& data, uint32_t width, uint32_t x, uint32_t y)
{
    T v;
    std::memcpy(&v, data.data() + (size_t)y * TestFrame::rowPitch(width, sizeof(T)) + (size_t)x * sizeof(T), sizeof(T));
    return v;
}
} // namespace unx::mtest
