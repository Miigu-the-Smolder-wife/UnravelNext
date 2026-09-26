// V gate for GPU triangle streams (INTERFACES v1.60; W's request 20260926_W_ocean_patch_stream asks for the per-triangle and
// per-group unit costs): V called directly (tracks::visibility, coverage layer on) with one stored triangle stream of an
// ocean-like surface seen from 2 m above it - a world-fixed polar grid on the plane y = 0 whose cells grow with the
// distance (spacing r x d, r = 0.005 / sqrt(scale): about 10 px wide at 4K, W's spacing rule with c_LOD 2), from 1 m to
// 5 km over a 1.6 rad horizontal fan - and reports the stream raster pass (v.coverage.stream), the rest of the coverage
// layer (count, scan, offsets, scatter, blocks, heavy), the stream records and the unit costs. Variants:
//   --scale S       spacing x 1/sqrt(S): S = 1 about 2.2 M triangles, S = 0.25 about 0.55 M over the same screen
//   --empty         capacity for the same count, draw arguments 0 (the cost of groups that emit nothing)
// Performance runs only under the GPU lock:
//   powershell -File Tools/CI/GpuLock.ps1 -Track V -- build/<t>/bin/unx_gate_visibility_streamgate.exe [--resolution 4K] [--frames 300]
//       [--scale 1] [--empty]
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/render/Tracks.h"
#include "unx/visibility/Visibility.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
ComPtr<ID3D12Resource> buffer(Device& d, uint64_t bytes, D3D12_HEAP_TYPE heap, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES hp{ heap };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(bytes, 256);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(d.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "buffer");
    r->SetName(name);
    return r;
}

ComPtr<ID3D12Resource> filled(Device& d, const void* data, uint64_t bytes, const wchar_t* name)
{
    ComPtr<ID3D12Resource> dst = buffer(d, bytes, D3D12_HEAP_TYPE_DEFAULT, name), up = buffer(d, bytes, D3D12_HEAP_TYPE_UPLOAD, L"gate upload");
    uint8_t* m = nullptr;
    check(up->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map");
    std::memcpy(m, data, bytes);
    up->Unmap(0, nullptr);
    CommandList cl = d.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(dst.Get(), 0, up.Get(), 0, bytes);
    d.queue(QueueType::Graphics).waitCpu(d.submit(cl));
    return dst;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string resolutionArg = "4K";
        uint32_t frames = 300;
        float scale = 1.0f;
        bool empty = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--resolution") resolutionArg = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--scale") scale = std::stof(next());
            else if (a == "--empty") empty = true;
            else fail("unknown argument %s", a.c_str());
        }
        requireGpuLock("stream gate");
        QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const char* o : { "visibility.coverage_layer = true" }) q.applyOverride(o);

        // Scene: one small box far behind the horizon (band A content; the water is the stream).
        scene::Scene s;
        s.name = "stream gate";
        s.materials.resize(2);
        scene::Mesh box;
        box.name = "box";
        for (int k = 0; k < 8; ++k) box.positions.push_back({ (k & 1) ? 1.0f : -1.0f, (k & 2) ? 1.0f : -1.0f, (k & 4) ? 1.0f : -1.0f });
        for (const float3& p : box.positions) box.normals.push_back(normalize(p));
        box.indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
        box.submeshes.push_back({ 0, (uint32_t)box.indices.size(), 0 });
        s.meshes.push_back(box);
        scene::Instance inst;
        inst.mesh = 0;
        inst.transform.m[1][3] = 30, inst.transform.m[2][3] = 6000;
        s.instances.push_back(inst);
        scene::Camera cam;
        cam.name = "above the water";
        cam.position = { 0, 2, 0 };
        cam.forward = normalize(float3{ 0, -0.05f, 1 });
        s.cameras.push_back(cam);
        scene::validate(s);

        // The ocean grid: rings d_k = d0 (1 + r)^k, angles every r over [-0.8, 0.8] rad; two triangles per cell.
        const double r = 0.005 / std::sqrt((double)scale), d0 = 1.0, d1 = 5000.0, fan = 1.6;
        const uint32_t nAngle = (uint32_t)std::ceil(fan / r), nRing = (uint32_t)std::ceil(std::log(d1 / d0) / std::log1p(r));
        const uint32_t triangles = 2 * nAngle * nRing;
        std::vector<float> vertices;
        vertices.reserve((size_t)triangles * 3 * 8);
        auto at = [&](uint32_t a, uint32_t k) {
            const double phi = -0.5 * fan + a * r, d = d0 * std::pow(1 + r, (double)k);
            return float3{ (float)(d * std::sin(phi)), 0.0f, (float)(d * std::cos(phi)) };
        };
        auto push = [&](float3 p) { vertices.insert(vertices.end(), { p.x, p.y, p.z, 1, 0, 1, 0, 0 }); };
        for (uint32_t k = 0; k < nRing; ++k)
            for (uint32_t a = 0; a < nAngle; ++a)
            {
                const float3 p00 = at(a, k), p10 = at(a + 1, k), p01 = at(a, k + 1), p11 = at(a + 1, k + 1);
                push(p00), push(p01), push(p11);  // counter-clockwise seen from above (+y)
                push(p00), push(p11), push(p10);
            }
        Device device({});
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene gs(device);
        gs.upload(s);
        gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));
        const uint64_t vertexBytes = (uint64_t)vertices.size() * 4;
        ComPtr<ID3D12Resource> vertexBuffer = filled(device, vertices.data(), vertexBytes, L"gate ocean vertices");
        const uint32_t args[4] = { empty ? 0u : 3 * triangles, 1, 0, 0 };
        ComPtr<ID3D12Resource> argBuffer = filled(device, args, 16, L"gate ocean args");
        logf("ocean stream: %u x %u cells, %u triangles (%s), %.1f MB of vertices\n", nAngle, nRing, triangles, empty ? "draw arguments 0" : "all drawn", vertexBytes / 1048576.0);

        const Resolution res = resolutionFromString(resolutionArg, q);
        ComPtr<ID3D12Resource> constants = buffer(device, 4 * 1024, D3D12_HEAP_TYPE_UPLOAD, L"gate constants");
        uint8_t* mapped = nullptr;
        check(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map");
        TrackState trackState;
        FrameServices services;
        services.rasterizeDepth = [](FramePassContext& c, const DepthRasterRequest& rq) { tracks::rasterizeDepth(c, rq); };
        ViewDesc view = ViewDesc::fromCamera(cam, res.width, res.height, {});
        view.prevViewProj = view.viewProj;
        Harness harness(device, q);
        HarnessOptions options;
        options.frames = frames;
        options.label = std::string("V ocean stream") + (empty ? " empty" : "");
        const HarnessResult result = harness.run(res, options, [&](RenderGraph& g, const Resolution&, uint64_t frame) {
            FrameContext fr;
            fr.frameIndex = frame;
            fr.mainView = view;
            const gpu::FrameConstants fcData = FrameRenderer::frameConstants(gs, fr, view);
            const uint32_t slot = (uint32_t)(frame % 4);
            std::memcpy(mapped + slot * 1024, &fcData, sizeof fcData);
            const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress() + slot * 1024;
            FrameResources resources;
            TriangleStream st;
            st.vertices = g.importBuffer(vertexBuffer.Get(), { "gate.ocean.vertices", vertexBytes, 0 });
            st.drawArgs = g.importBuffer(argBuffer.Get(), { "gate.ocean.args", 256, 0 });
            st.material = 1;
            st.maxTriangles = triangles;
            st.boundsMin = { -5000, 0, 0 }, st.boundsMax = { 5000, 0, 5000 };
            resources.triangleStreams.push_back(st);
            FramePassContext fc{ device, g, shaders, q, gs, fr, resources, services, [=](const ViewDesc&) { return address; }, &trackState, 2 };
            ViewResources main;
            main.view = view;
            main.frameConstants = address;
            tracks::visibility(fc, main);
        });
        harness.printSummary(result);
        auto sum = [&](auto match) {
            double ms = 0;
            for (const auto& [name, d] : result.passMs)
                if (match(name)) ms += d.median;
            return ms;
        };
        const double streamMs = sum([](const std::string& n) { return n == "v.coverage.stream"; });
        const double layerMs = sum([](const std::string& n) { return n.rfind("v.coverage.", 0) == 0 && n != "v.coverage.stream" && n != "v.coverage.raster"; });
        const visibility::Stats st = visibility::latestStats(trackState);
        const uint32_t groups = (triangles + 31) / 32;
        logf("[stream gate %s] %u triangles in %u groups%s: stream raster %.3f ms (%.3f ns per triangle, %.1f ns per group), %u coverage records "
             "(%.2f per pixel, %.3f ns per record), rest of the coverage layer %.3f ms | gpu frame %.3f ms [measured]\n",
             res.name.c_str(), triangles, groups, empty ? " (none drawn)" : "", streamMs, streamMs * 1e6 / triangles, streamMs * 1e6 / groups, st.coverageFragments,
             (double)st.coverageFragments / ((double)res.width * res.height), st.coverageFragments ? streamMs * 1e6 / st.coverageFragments : 0.0, layerMs,
             result.gpuFrameMs.median);
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL stream gate: %s\n", e.what());
        return 1;
    }
}
