// GPU triangle streams in V's coverage layer (render C; W's request 20260926_W_gpu_triangle_stream; INTERFACES v1.60),
// real device with the debug layer, V called directly (tracks::visibility) with FrameResources::triangleStreams set as
// W's tracks::waterGeometry will set it:
//   stream_records_are_exact   a tilted water quad (2 triangles, capacity 64, the draw arguments say 6 vertices) in front
//                              of a far box: every stream record is see-through, names slot 0 and triangle 0 or 1, the
//                              records' areas add up to the two triangles' exact projected areas within the 10-bit
//                              rounding, and no record carries a triangle past the draw arguments' count.
//   unx_test_visibility_streamcoveragetests
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <cstring>
#include <exception>
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

// A default-heap buffer holding 'bytes' of data (one copy, waited for).
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

std::vector<uint32_t> readWords(ID3D12Resource* r, uint64_t words)
{
    std::vector<uint32_t> w(words);
    uint8_t* p = nullptr;
    check(r->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
    std::memcpy(w.data(), p, words * 4);
    r->Unmap(0, nullptr);
    return w;
}

struct Pixel
{
    double x, y;
};
Pixel toPixel(const float4x4& vp, float3 p, uint32_t width, uint32_t height)
{
    double c[4];
    for (int r = 0; r < 4; ++r) c[r] = (double)vp.m[r][0] * p.x + (double)vp.m[r][1] * p.y + (double)vp.m[r][2] * p.z + vp.m[r][3];
    return { (c[0] / c[3] * 0.5 + 0.5) * width, (0.5 - c[1] / c[3] * 0.5) * height };
}
} // namespace

int main()
{
    try
    {
        const uint32_t width = 640, height = 360, capacity = 64;
        QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const char* o : { "visibility.coverage_layer = true", "visibility.occlusion_culling = true" }) q.applyOverride(o);
        // A far box (band A behind the water) and a camera looking at a tilted quad 2 m ahead.
        scene::Scene s;
        s.name = "stream coverage";
        s.materials.resize(2);
        scene::Mesh box;
        box.name = "far box";
        for (int k = 0; k < 8; ++k) box.positions.push_back({ (k & 1) ? 3.0f : -3.0f, (k & 2) ? 3.0f : -3.0f, (k & 4) ? 0.5f : -0.5f });
        for (const float3& p : box.positions) box.normals.push_back(normalize(p));
        box.indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
        box.submeshes.push_back({ 0, (uint32_t)box.indices.size(), 0 });
        s.meshes.push_back(box);
        scene::Instance in;
        in.mesh = 0;
        in.transform.m[2][3] = 20;
        s.instances.push_back(in);
        scene::Camera cam;
        cam.name = "front";
        cam.position = { 0, 0, 0 };
        cam.forward = { 0, 0, 1 };
        s.cameras.push_back(cam);
        scene::validate(s);
        GpuScene gs(device());
        gs.upload(s);
        gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));

        // The stream: a quad tilted about x, 2 triangles (world position, normal), capacity 64 triangles.
        const float3 corners[4] = { { -0.5f, -0.3f, 1.8f }, { 0.5f, -0.3f, 1.8f }, { 0.5f, 0.3f, 2.4f }, { -0.5f, 0.3f, 2.4f } };
        const float3 normal = normalize(cross(corners[1] - corners[0], corners[3] - corners[0]));
        const uint32_t tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
        std::vector<float> vertexData;
        for (const auto& t : tri)
            for (uint32_t k : t)
                vertexData.insert(vertexData.end(), { corners[k].x, corners[k].y, corners[k].z, 1, normal.x, normal.y, normal.z, 0 });
        // Identical quads fully behind band A and outside the view. Early stream
        // culling must preserve precisely the records the pixel path kept.
        for (uint32_t hidden = 0; hidden < 2; ++hidden)
            for (const auto& t : tri)
                for (uint32_t k : t)
                    vertexData.insert(vertexData.end(), { corners[k].x + (hidden ? 100.0f : 0.0f), corners[k].y,
                                                         corners[k].z + (hidden ? 0.0f : 24.0f), 1,
                                                         normal.x, normal.y, normal.z, 0 });
        const uint32_t args[4] = { 18, 1, 0, 0 };
        ComPtr<ID3D12Resource> vertices = filled(vertexData.data(), vertexData.size() * 4, (uint64_t)capacity * 96, L"test stream vertices");
        ComPtr<ID3D12Resource> drawArgs = filled(args, sizeof args, 16, L"test stream args");
        std::vector<float> offscreenData = vertexData;
        for (size_t vertex = 0; vertex < offscreenData.size(); vertex += 8) offscreenData[vertex] += 1000.0f;
        ComPtr<ID3D12Resource> offscreenVertices = filled(offscreenData.data(), offscreenData.size() * 4, (uint64_t)capacity * 96, L"offscreen stream");

        RenderGraph graph(device());
        TrackState trackState;
        FrameServices services;
        services.rasterizeDepth = [](FramePassContext& c, const DepthRasterRequest& r) { tracks::rasterizeDepth(c, r); };
        ViewDesc mainView = ViewDesc::fromCamera(cam, width, height, {});
        mainView.prevViewProj = mainView.viewProj;
        ComPtr<ID3D12Resource> constants = buffer(2048, D3D12_HEAP_TYPE_UPLOAD, L"test constants");
        uint8_t* mapped = nullptr;
        check(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map");
        const uint64_t tileCount = (uint64_t)((width + 7) / 8) * ((height + 7) / 8);
        ComPtr<ID3D12Resource> tilesRb = buffer(tileCount * 32, D3D12_HEAP_TYPE_READBACK, L"test tiles"), recordsRb;
        uint64_t recordBytes = 0;
        std::vector<uint32_t> tiles, records;
        std::vector<std::array<uint32_t, 5>> reference;
        for (uint32_t f = 0; f < 6; ++f)
        {
            q.applyOverride(f < 3 ? "visibility.coverage_triangle_cull = false" : "visibility.coverage_triangle_cull = true");
            q.applyOverride(f < 3 ? "visibility.stream_frustum_cull = false" : "visibility.stream_frustum_cull = true");
            FrameContext frame;
            frame.frameIndex = f;
            frame.mainView = mainView;
            const gpu::FrameConstants fcData = FrameRenderer::frameConstants(gs, frame, mainView);
            std::memcpy(mapped + (f % 2) * 1024, &fcData, sizeof fcData);
            const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress() + (f % 2) * 1024;
            FrameResources resources;
            TriangleStream st;
            st.vertices = graph.importBuffer(vertices.Get(), { "test.stream.vertices", (uint64_t)capacity * 96, 0 });
            st.drawArgs = graph.importBuffer(drawArgs.Get(), { "test.stream.args", 256, 0 });
            st.material = 1;
            st.maxTriangles = capacity;
            st.boundsMin = { -0.5f, -0.3f, 1.8f }, st.boundsMax = { 100.5f, 0.3f, 26.4f };
            resources.triangleStreams.push_back(st);
            TriangleStream offscreen = st;
            offscreen.vertices = graph.importBuffer(offscreenVertices.Get(), { "offscreen.stream.vertices", (uint64_t)capacity * 96, 0 });
            offscreen.boundsMin.x += 1000.0f;
            offscreen.boundsMax.x += 1000.0f;
            resources.triangleStreams.push_back(offscreen);
            FramePassContext fc{ device(), graph, shaders(), q, gs, frame, resources, services, [=](const ViewDesc&) { return address; }, &trackState, 2 };
            if (f != 0) prepareTriangleStreamDraws(fc); // direct first, GPU-count consumers on subsequent frames
            ViewResources main;
            main.view = mainView;
            main.frameConstants = address;
            tracks::visibility(fc, main);
            CHECK(main.coverageTiles.valid() && main.coverageRecords.valid());
            const uint64_t poolBytes = graph.desc(main.coverageRecords).size;
            if (poolBytes != recordBytes)
            {
                if (recordsRb) device().deferRelease(recordsRb);
                recordsRb = buffer(poolBytes, D3D12_HEAP_TYPE_READBACK, L"test records");
                recordBytes = poolBytes;
            }
            ID3D12Resource *tl = tilesRb.Get(), *rc = recordsRb.Get();
            graph.addPass("test.readback", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(main.coverageTiles, Use::CopySrc);
                              b.use(main.coverageRecords, Use::CopySrc);
                              b.keep();
                          },
                          [=](PassContext& c) {
                              c.cmd->CopyBufferRegion(tl, 0, c.resource(main.coverageTiles), 0, tileCount * 32);
                              c.cmd->CopyBufferRegion(rc, 0, c.resource(main.coverageRecords), 0, poolBytes);
                          });
            graph.execute(nullptr);
            device().waitIdle();
            tiles = readWords(tl, tileCount * 8);
            records = readWords(rc, poolBytes / 4);
            if (f == 2 || f == 5)
            {
                std::vector<std::array<uint32_t, 5>> canonical;
                for (uint64_t t = 0; t < tileCount; ++t)
                {
                    if (!tiles[8 * t + 2]) continue;
                    for (uint32_t r = 0; r < tiles[8 * t]; ++r)
                    {
                        const uint32_t* rec = &records[4 * (size_t)(tiles[8 * t + 1] + r)];
                        if ((rec[0] >> 30) == 3u) canonical.push_back({ (uint32_t)t, rec[0], rec[1], rec[2], rec[3] });
                    }
                }
                std::sort(canonical.begin(), canonical.end());
                if (f == 2) reference = std::move(canonical);
                else CHECK(canonical == reference);
            }
        }

        double expected = 0;
        for (const auto& t : tri)
        {
            Pixel px[3];
            for (int k = 0; k < 3; ++k) px[k] = toPixel(mainView.viewProj, corners[t[k]], width, height);
            expected += std::fabs((px[1].x - px[0].x) * (px[2].y - px[0].y) - (px[2].x - px[0].x) * (px[1].y - px[0].y)) * 0.5;
        }
        double area = 0;
        uint64_t streamRecords = 0, opaqueStream = 0, badId = 0;
        for (uint64_t t = 0; t < tileCount; ++t)
        {
            const uint32_t count = tiles[8 * t], base = tiles[8 * t + 1], listed = tiles[8 * t + 2];
            if (listed == 0) continue;
            for (uint32_t r = 0; r < count; ++r)
            {
                const uint32_t* rec = &records[4 * (size_t)(base + r)];
                if ((rec[0] >> 30) != 3u) continue;
                ++streamRecords;
                area += ((rec[3] >> 16) & 0x3FFu) / 1023.0;
                if ((rec[1] & 0x80000000u) == 0) ++opaqueStream;
                if (((rec[0] >> 24) & 0x3Fu) != 0 || (rec[0] & 0xFFFFFFu) >= 2) ++badId;
            }
        }
        const double rounding = streamRecords * 0.5 / 1023.0;
        logf("    %llu stream records, area %.3f px^2 vs exact %.3f (rounding bound %.3f), %llu not see-through, %llu with a wrong slot or triangle\n",
             (unsigned long long)streamRecords, area, expected, rounding, (unsigned long long)opaqueStream, (unsigned long long)badId);
        CHECK(streamRecords > 1000 && opaqueStream == 0 && badId == 0);
        CHECK(std::fabs(area - expected) <= rounding + 1e-4 * expected);
        logf("PASS stream_records_are_exact; early HiZ/frustum culling preserves every retained record bit-for-bit\n1/1 passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL stream_records_are_exact: %s\n0/1 passed\n", e.what());
        return 1;
    }
}
