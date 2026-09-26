// A14 full auxiliary views in V (render C; Requests/20260926_C_per_view_history.md), real device with the debug layer:
//   aux_views_keep_their_own_history
//     - a main view (id 0) and a render-texture view (id 7, another size and camera) drawn every frame keep separate
//       histories: both run two-phase occlusion (what lies behind a wall is deferred in phase 1 - chunks, instances,
//       nodes or clusters: statistics "main" and "view7");
//     - when the auxiliary view changes size its HiZ is new and holds no history: that frame defers nothing (the earlier
//       defect: the history flag survived the new texture, and phase 1 read an uninitialised HiZ), while the main view's
//       frame still defers;
//     - the auxiliary view's depth after the resize equals a main view's depth drawn from a fresh renderer state at the
//       same camera and size (bit for bit).
//   unx_test_visibility_auxviewtests
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"
#include "unx/visibility/Visibility.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <limits>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
#define CHECK(cond) \
    do { if (!(cond)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #cond); } while (0)

Device& device()
{
    static Device d([] {
        DeviceOptions o;
        o.debugLayer = true;
        return o;
    }());
    return d;
}
ShaderLibrary& shaders()
{
    static ShaderLibrary lib(device(), executableDirectory() / "shaders");
    return lib;
}

uint32_t rowPitch(uint32_t width) { return (width * 4 + 255) / 256 * 256; }

ComPtr<ID3D12Resource> buffer(uint64_t bytes, D3D12_HEAP_TYPE heap, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{ heap };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(bytes, 256);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "buffer");
    r->SetName(name);
    return r;
}

ComPtr<ID3D12Resource> filled(const void* data, uint64_t bytes, uint64_t capacity, const wchar_t* name)
{
    ComPtr<ID3D12Resource> dst = buffer(capacity, D3D12_HEAP_TYPE_DEFAULT, name), up = buffer(capacity, D3D12_HEAP_TYPE_UPLOAD, L"test upload");
    uint8_t* m = nullptr;
    check(up->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map");
    std::memset(m, 0, capacity);
    std::memcpy(m, data, bytes);
    up->Unmap(0, nullptr);
    CommandList cl = device().acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(dst.Get(), 0, up.Get(), 0, capacity);
    device().queue(QueueType::Graphics).waitCpu(device().submit(cl));
    return dst;
}

void copyTexture(PassContext& c, TextureRef t, ID3D12Resource* dst, uint32_t width, uint32_t height)
{
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = dst;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint = { c.resource(t)->GetDesc().Format, width, height, 1, rowPitch(width) };
    from.pResource = c.resource(t);
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
}

template <typename T>
std::vector<T> readTexture(ID3D12Resource* rb, uint32_t width, uint32_t height)
{
    std::vector<T> out((size_t)width * height);
    uint8_t* p = nullptr;
    check(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
    for (uint32_t y = 0; y < height; ++y) std::memcpy(&out[(size_t)y * width], p + (size_t)y * rowPitch(width), (size_t)width * 4);
    rb->Unmap(0, nullptr);
    return out;
}

scene::Mesh box(float3 half)
{
    scene::Mesh m;
    m.name = "box";
    for (int k = 0; k < 8; ++k) m.positions.push_back({ (k & 1) ? half.x : -half.x, (k & 2) ? half.y : -half.y, (k & 4) ? half.z : -half.z });
    for (const float3& p : m.positions) m.normals.push_back(normalize(p));
    m.indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    return m;
}

struct Target
{
    uint32_t width, height;
};
} // namespace

int main()
{
    try
    {
        QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        q.applyOverride("visibility.occlusion_culling = true");
        scene::Scene s;
        s.name = "aux views";
        s.materials.resize(1);
        s.meshes = { box({ 30, 30, 0.5f }), box({ 4, 4, 0.2f }), box({ 0.3f, 0.3f, 0.3f }) };
        scene::Instance backdrop, wall;
        backdrop.mesh = 0;
        backdrop.transform.m[2][3] = 40;
        wall.mesh = 1;
        wall.transform.m[2][3] = 6;
        s.instances = { backdrop, wall };
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 8; ++j)
            {
                scene::Instance b;
                b.mesh = 2;
                b.transform.m[0][3] = -3.5f + i;
                b.transform.m[1][3] = -3.5f + j;
                b.transform.m[2][3] = 12;
                s.instances.push_back(b);
            }
        scene::Camera mainCam, auxCam;
        mainCam.name = "main";
        mainCam.forward = { 0, 0, 1 };
        auxCam.name = "scope";
        auxCam.position = { 0.4f, 0.2f, -1 };
        auxCam.forward = { 0, 0, 1 };
        auxCam.verticalFov = 0.5f;
        s.cameras = { mainCam, auxCam };
        scene::validate(s);
        GpuScene gs(device());
        gs.upload(s);
        gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));

        ComPtr<ID3D12Resource> constants = buffer(8192, D3D12_HEAP_TYPE_UPLOAD, L"test constants");
        uint8_t* mapped = nullptr;
        check(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map");
        // Draws the frame's views (main first) into 'graph' with 'state'; returns the last view's depth (read back).
        auto frameOf = [&](RenderGraph& graph, TrackState& state, uint64_t f, const std::vector<std::pair<scene::Camera, Target>>& views, const std::vector<uint32_t>& ids,
                           std::vector<float>* lastDepth) {
            FrameContext frame;
            frame.frameIndex = f;
            frame.mainView = ViewDesc::fromCamera(views[0].first, views[0].second.width, views[0].second.height, {});
            frame.mainView.prevViewProj = frame.mainView.viewProj;
            FrameResources resources;
            FrameServices services;
            services.rasterizeDepth = [](FramePassContext& c, const DepthRasterRequest& r) { tracks::rasterizeDepth(c, r); };
            std::vector<D3D12_GPU_VIRTUAL_ADDRESS> addresses;
            std::vector<ViewDesc> descs;
            for (size_t v = 0; v < views.size(); ++v)
            {
                ViewDesc d = v == 0 ? frame.mainView : ViewDesc::fromCamera(views[v].first, views[v].second.width, views[v].second.height, {});
                d.prevViewProj = d.viewProj;
                if (v > 0) d.kind = gpu::ViewKind::RenderTexture;
                const gpu::FrameConstants fcData = FrameRenderer::frameConstants(gs, frame, d);
                const uint64_t offset = ((f % 2) * 4 + v) * 1024;
                std::memcpy(mapped + offset, &fcData, sizeof fcData);
                addresses.push_back(constants->GetGPUVirtualAddress() + offset);
                descs.push_back(d);
            }
            const D3D12_GPU_VIRTUAL_ADDRESS first = addresses[0];
            FramePassContext fc{ device(), graph, shaders(), q, gs, frame, resources, services, [=](const ViewDesc&) { return first; }, &state, 2 };
            TextureRef depth;
            uint32_t w = 0, h = 0;
            for (size_t v = 0; v < views.size(); ++v)
            {
                ViewResources vr;
                vr.view = descs[v];
                vr.viewId = ids[v];
                vr.frameConstants = addresses[v];
                tracks::visibility(fc, vr);
                depth = vr.depth;
                w = descs[v].width;
                h = descs[v].height;
            }
            ComPtr<ID3D12Resource> rb = lastDepth ? buffer((uint64_t)rowPitch(w) * h, D3D12_HEAP_TYPE_READBACK, L"rb depth") : nullptr;
            if (lastDepth)
            {
                ID3D12Resource* r = rb.Get();
                graph.addPass("test.readback", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  b.use(depth, Use::CopySrc);
                                  b.keep();
                              },
                              [=](PassContext& c) { copyTexture(c, depth, r, w, h); });
            }
            graph.execute(nullptr);
            device().waitIdle();
            if (lastDepth) *lastDepth = readTexture<float>(rb.Get(), w, h);
        };

        const Target mainSize{ 320, 180 }, auxSize{ 256, 256 }, resized{ 200, 120 };
        RenderGraph graph(device());
        TrackState state;
        std::vector<float> auxDepth;
        uint32_t steadyMain = 0, steadyAux = 0;
        const uint64_t resizeFrame = 6;
        for (uint64_t f = 0; f <= resizeFrame + 2; ++f)
        {
            const Target aux = f >= resizeFrame ? resized : auxSize;
            frameOf(graph, state, f, { { mainCam, mainSize }, { auxCam, aux } }, { 0, 7 }, f == resizeFrame + 2 ? &auxDepth : nullptr);
            // Statistics of frame f - 2 are the latest after frame f (two frames in flight).
            const visibility::Stats m = visibility::latestStats(state, "main"), a = visibility::latestStats(state, "view7");
            // Everything phase 1 put off to phase 2 (chunks, instances, nodes, clusters).
            auto deferred = [](const visibility::Stats& st) { return st.deferredChunks + st.deferredInstances + st.deferredNodes + st.deferredClusters; };
            if (f == resizeFrame - 1)
            {
                steadyMain = deferred(m);
                steadyAux = deferred(a);
                logf("    frame %llu (steady): main deferred %u (stats of frame %llu), view7 deferred %u (frame %llu)\n", (unsigned long long)f, deferred(m),
                     (unsigned long long)m.frameIndex, deferred(a), (unsigned long long)a.frameIndex);
                CHECK(m.frameIndex == f - 2 && a.frameIndex == f - 2);
                CHECK(steadyMain > 0 && steadyAux > 0);
            }
            if (f == resizeFrame + 2)
            {
                logf("    resize frame %llu: main deferred %u, view7 deferred %u (stats frames %llu, %llu)\n", (unsigned long long)resizeFrame, deferred(m), deferred(a),
                     (unsigned long long)m.frameIndex, (unsigned long long)a.frameIndex);
                CHECK(m.frameIndex == resizeFrame && a.frameIndex == resizeFrame);
                CHECK(deferred(m) > 0);  // the main view kept its history
                CHECK(deferred(a) == 0);  // the resized view's HiZ is new: no phase-1 occlusion
            }
        }
        // Reference: a fresh state drawing the auxiliary camera as its main view at the resized size, for as many frames.
        RenderGraph refGraph(device());
        TrackState refState;
        std::vector<float> refDepth;
        for (uint64_t f = 0; f < 3; ++f) frameOf(refGraph, refState, f, { { auxCam, resized } }, { 0 }, f == 2 ? &refDepth : nullptr);
        uint64_t differ = 0;
        for (size_t i = 0; i < refDepth.size(); ++i) differ += std::memcmp(&refDepth[i], &auxDepth[i], 4) != 0;
        logf("    auxiliary depth vs a fresh main view: %llu of %zu pixels differ\n", (unsigned long long)differ, refDepth.size());
        CHECK(!refDepth.empty() && differ == 0);
        logf("PASS aux_views_keep_their_own_history\n1/1 passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL aux_views_keep_their_own_history: %s\n0/1 passed\n", e.what());
        return 1;
    }
}
