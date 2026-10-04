// Water layer edges (render C; A's rule, INTERFACES v1.64), real device with the debug layer, V called directly with a layer-1
// triangle stream and the coverage layer on:
//   water_edges_are_exact   a tilted water quad (2 triangles) alone in front of a far box: the layer's pixels without
//                           coverage records (the layer's full samples) plus the areas of the water records in the edge
//                           pixels add up to the quad's exact projected area within the records' 10-bit rounding; records
//                           come only from pixels next to the quad's outline (none inside), every one see-through with
//                           slot 0 and triangle 0 or 1. The special record list (v1.73) names exactly those records:
//                           as many entries as stream records, each a distinct element holding a stream id, kind 2.
//   unx_test_visibility_wateredgetests
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
} // namespace

int main()
{
    try
    {
        const uint32_t width = 640, height = 360, capacity = 64;
        QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const char* o : { "visibility.coverage_layer = true", "visibility.occlusion_culling = false" }) q.applyOverride(o);
        scene::Scene s;
        s.name = "water layer";
        s.materials.resize(2);
        s.meshes = { box({ 3, 3, 0.5f }) };
        scene::Instance farBox;
        farBox.mesh = 0;
        farBox.transform.m[2][3] = 20;
        s.instances = { farBox };
        scene::Camera cam;
        cam.name = "front";
        cam.forward = { 0, 0, 1 };
        s.cameras.push_back(cam);
        scene::validate(s);
        GpuScene gs(device());
        gs.upload(s);
        gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));

        const float3 corners[4] = { { -0.5f, -0.3f, 1.8f }, { 0.5f, -0.3f, 1.8f }, { 0.5f, 0.3f, 2.4f }, { -0.5f, 0.3f, 2.4f } };
        const float3 normal = normalize(cross(corners[1] - corners[0], corners[3] - corners[0]));
        const uint32_t tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
        std::vector<float> vertexData;
        for (const auto& t : tri)
            for (uint32_t k : t) vertexData.insert(vertexData.end(), { corners[k].x, corners[k].y, corners[k].z, 1, normal.x, normal.y, normal.z, 0 });
        const uint32_t args[4] = { 6, 1, 0, 0 };
        ComPtr<ID3D12Resource> vertices = filled(vertexData.data(), vertexData.size() * 4, (uint64_t)capacity * 96, L"test water vertices");
        ComPtr<ID3D12Resource> drawArgs = filled(args, sizeof args, 16, L"test water args");

        RenderGraph graph(device());
        TrackState trackState;
        FrameServices services;
        services.rasterizeDepth = [](FramePassContext& c, const DepthRasterRequest& r) { tracks::rasterizeDepth(c, r); };
        ViewDesc mainView = ViewDesc::fromCamera(cam, width, height, {});
        mainView.prevViewProj = mainView.viewProj;
        ComPtr<ID3D12Resource> constants = buffer(2048, D3D12_HEAP_TYPE_UPLOAD, L"test constants");
        uint8_t* mapped = nullptr;
        check(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map");
        const uint64_t texBytes = (uint64_t)rowPitch(width) * height;
        ComPtr<ID3D12Resource> visRb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb vis"), depthRb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb water depth"),
                               bandARb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb band A depth");
        std::vector<uint32_t> vis, tiles, records;
        std::vector<float> waterDepth, bandA;
        const uint64_t tileCount = (uint64_t)((width + 7) / 8) * ((height + 7) / 8);
        ComPtr<ID3D12Resource> tilesRb = buffer(tileCount * 32, D3D12_HEAP_TYPE_READBACK, L"rb tiles"), recordsRb, specialRb;
        uint64_t recordBytes = 0, specialBytes = 0;
        std::vector<uint32_t> special;
        for (uint32_t f = 0; f < 3; ++f)
        {
            FrameContext frame;
            frame.frameIndex = f;
            frame.mainView = mainView;
            const gpu::FrameConstants fcData = FrameRenderer::frameConstants(gs, frame, mainView);
            std::memcpy(mapped + (f % 2) * 1024, &fcData, sizeof fcData);
            const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress() + (f % 2) * 1024;
            FrameResources resources;
            TriangleStream st;
            st.vertices = graph.importBuffer(vertices.Get(), { "test.water.vertices", (uint64_t)capacity * 96, 0 });
            st.drawArgs = graph.importBuffer(drawArgs.Get(), { "test.water.args", 256, 0 });
            st.material = 1;
            st.maxTriangles = capacity;
            st.layer = 1;
            st.boundsMin = { -0.5f, -0.3f, 1.8f }, st.boundsMax = { 0.5f, 0.3f, 2.4f };
            resources.triangleStreams.push_back(st);
            FramePassContext fc{ device(), graph, shaders(), q, gs, frame, resources, services, [=](const ViewDesc&) { return address; }, &trackState, 2 };
            prepareTriangleStreamDraws(fc);
            ViewResources main;
            main.view = mainView;
            main.frameConstants = address;
            tracks::visibility(fc, main);
            CHECK(resources.waterVis.valid() && resources.waterDepth.valid());
            const TextureRef wv = resources.waterVis, wd = resources.waterDepth, da = main.depth;
            CHECK(main.coverageTiles.valid() && main.coverageRecords.valid());
            const BufferRef ct = main.coverageTiles, cr = main.coverageRecords, cs = main.coverageSpecial;
            CHECK(cs.valid());
            const uint64_t poolBytes = graph.desc(cr).size, listBytes = graph.desc(cs).size;
            if (listBytes != specialBytes)
            {
                if (specialRb) device().deferRelease(specialRb);
                specialRb = buffer(listBytes, D3D12_HEAP_TYPE_READBACK, L"rb special");
                specialBytes = listBytes;
            }
            if (poolBytes != recordBytes)
            {
                if (recordsRb) device().deferRelease(recordsRb);
                recordsRb = buffer(poolBytes, D3D12_HEAP_TYPE_READBACK, L"rb records");
                recordBytes = poolBytes;
            }
            ID3D12Resource *v = visRb.Get(), *d = depthRb.Get(), *a = bandARb.Get(), *tl = tilesRb.Get(), *rc = recordsRb.Get(), *sp = specialRb.Get();
            graph.addPass("test.readback", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(wv, Use::CopySrc);
                              b.use(wd, Use::CopySrc);
                              b.use(da, Use::CopySrc);
                              b.use(ct, Use::CopySrc);
                              b.use(cr, Use::CopySrc);
                              b.use(cs, Use::CopySrc);
                              b.keep();
                          },
                          [=](PassContext& c) {
                              copyTexture(c, wv, v, width, height);
                              copyTexture(c, wd, d, width, height);
                              copyTexture(c, da, a, width, height);
                              c.cmd->CopyBufferRegion(tl, 0, c.resource(ct), 0, tileCount * 32);
                              c.cmd->CopyBufferRegion(rc, 0, c.resource(cr), 0, poolBytes);
                              c.cmd->CopyBufferRegion(sp, 0, c.resource(cs), 0, listBytes);
                          });
            graph.execute(nullptr);
            device().waitIdle();
            vis = readTexture<uint32_t>(v, width, height);
            waterDepth = readTexture<float>(d, width, height);
            bandA = readTexture<float>(a, width, height);
            auto words = [](ID3D12Resource* r, uint64_t n) {
                std::vector<uint32_t> w(n);
                uint8_t* p = nullptr;
                check(r->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
                std::memcpy(w.data(), p, n * 4);
                r->Unmap(0, nullptr);
                return w;
            };
            tiles = words(tl, tileCount * 8);
            records = words(rc, poolBytes / 4);
            special = words(sp, listBytes / 4);
        }

        // Records per pixel (tile headers: records, base, listed index + 1; the record's pixel in its tile).
        const uint32_t tilesX = (width + 7) / 8;
        std::vector<uint8_t> hasRecord((size_t)width * height, 0);
        double recordArea = 0;
        uint64_t streamRecords = 0, badRecords = 0;
        for (uint64_t t = 0; t < tileCount; ++t)
        {
            const uint32_t count = tiles[8 * t], base = tiles[8 * t + 1], listed = tiles[8 * t + 2];
            if (listed == 0) continue;
            for (uint32_t r = 0; r < count; ++r)
            {
                const uint32_t* rec = &records[4 * (size_t)(base + r)];
                if ((rec[0] >> 30) != 3u) continue;
                ++streamRecords;
                if ((rec[1] & 0x80000000u) == 0 || ((rec[0] >> 24) & 0x3Fu) != 0 || (rec[0] & 0xFFFFFFu) >= 2) ++badRecords;
                recordArea += ((rec[3] >> 16) & 0x3FFu) / 1023.0;
                const uint32_t p = rec[3] >> 26, px = (uint32_t)(t % tilesX) * 8 + p % 8, py = (uint32_t)(t / tilesX) * 8 + p / 8;
                if (px < width && py < height) hasRecord[(size_t)py * width + px] = 1;
            }
        }
        uint64_t interior = 0, recordPixels = 0;
        for (size_t i = 0; i < vis.size(); ++i)
        {
            recordPixels += hasRecord[i];
            if (vis[i] != 0 && !hasRecord[i]) ++interior;
        }
        // The quad's exact projected area (it lies inside the view).
        auto toPixel = [&](float3 p) {
            double c[4];
            for (int r = 0; r < 4; ++r) c[r] = (double)mainView.viewProj.m[r][0] * p.x + (double)mainView.viewProj.m[r][1] * p.y + (double)mainView.viewProj.m[r][2] * p.z + mainView.viewProj.m[r][3];
            return std::array<double, 2>{ (c[0] / c[3] * 0.5 + 0.5) * width, (0.5 - c[1] / c[3] * 0.5) * height };
        };
        double area2 = 0, perimeter = 0;
        for (int k = 0; k < 4; ++k)
        {
            const auto a = toPixel(corners[k]), b = toPixel(corners[(k + 1) % 4]);
            area2 += a[0] * b[1] - b[0] * a[1];
            perimeter += std::hypot(b[0] - a[0], b[1] - a[1]);
        }
        const double exact = std::fabs(area2) * 0.5, total = (double)interior + recordArea, rounding = streamRecords * 0.5 / 1023.0;
        logf("    water layer: %llu full pixels + %.3f px^2 in %llu edge records (%llu pixels) = %.3f px^2 vs the quad's exact %.3f (rounding bound %.3f); outline "
             "%.0f px; %llu bad records\n",
             (unsigned long long)interior, recordArea, (unsigned long long)streamRecords, (unsigned long long)recordPixels, total, exact, rounding, perimeter,
             (unsigned long long)badRecords);
        // Special list: header { count, groups, 1, 1 }, then { element, kind }.
        const uint32_t specialCount = special[0];
        uint64_t specialBad = 0;
        std::vector<uint8_t> seen(records.size() / 4, 0);
        for (uint32_t k = 0; k < specialCount && 4 + 2 * (size_t)k + 1 < special.size(); ++k)
        {
            const uint32_t element = special[4 + 2 * k], kind = special[4 + 2 * k + 1];
            if (element >= seen.size() || seen[element] || kind != 2 || (records[4 * (size_t)element] >> 30) != 3u) ++specialBad;
            else seen[element] = 1;
        }
        logf("    special list: %u entries (args %u, %u, %u), %llu bad\n", specialCount, special[1], special[2], special[3], (unsigned long long)specialBad);
        CHECK(specialCount == streamRecords && specialBad == 0);
        CHECK(special[1] == (specialCount + 63) / 64 && special[2] == 1 && special[3] == 1);
        CHECK(streamRecords > 0 && badRecords == 0);
        CHECK(recordPixels <= (uint64_t)(4 * perimeter));  // the outline's pixels (and their 3 x 3 neighbours), not the interior
        CHECK(std::fabs(total - exact) <= rounding + 1e-4 * exact);
        logf("PASS water_edges_are_exact\n1/1 passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL water_edges_are_exact: %s\n0/1 passed\n", e.what());
        return 1;
    }
}
