// Ocean in the water layer (render C; W's view grid, INTERFACES v1.73), real device with the debug layer, V called
// directly with a synthetic FrameResources::oceanDepth (a sea plane y = -0.5 limited to |x| < 2, z < 12, +inf elsewhere)
// and an opaque box in front of part of it:
//   ocean_layer_merges_and_lists_edges
//     - waterVis is COV_OCEAN_ID exactly where the sea is in front of band A (the box and the backdrop decide), VIS_NONE
//       elsewhere; waterDepth there is oceanDepth bit for bit, +inf elsewhere;
//     - the ocean edge list (ViewResources::oceanEdgePixels) is exactly the set of the CPU rule: an ocean pixel with a
//       3 x 3 neighbour that is not ocean or a depth bend, or a non-ocean pixel with an ocean neighbour; its header holds
//       the count and the dispatch arguments.
//   Pixels whose sea depth is within 1e-4 of the band A depth (the GPU's division may round either way) are not judged,
//   nor are their 3 x 3 neighbours for the edge set.
//   unx_test_visibility_oceanlayertests
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
        const uint32_t width = 640, height = 360;
        const uint32_t oceanId = 0xC0000000u | (63u << 24);
        QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const char* o : { "visibility.coverage_layer = true", "visibility.occlusion_culling = false" }) q.applyOverride(o);
        scene::Scene s;
        s.name = "ocean layer";
        s.materials.resize(1);
        s.meshes = { box({ 30, 30, 0.5f }), box({ 0.4f, 0.3f, 0.3f }) };
        scene::Instance backdrop, nearBox;
        backdrop.mesh = 0;
        backdrop.transform.m[2][3] = 20;
        nearBox.mesh = 1;
        nearBox.transform.m[0][3] = 0.6f;
        nearBox.transform.m[1][3] = -0.4f;
        nearBox.transform.m[2][3] = 3;
        s.instances = { backdrop, nearBox };
        scene::Camera cam;
        cam.name = "front";
        cam.forward = { 0, 0, 1 };
        s.cameras.push_back(cam);
        scene::validate(s);
        GpuScene gs(device());
        gs.upload(s);
        gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));

        ViewDesc mainView = ViewDesc::fromCamera(cam, width, height, {});
        mainView.prevViewProj = mainView.viewProj;
        // Synthetic sea: the view-space ray of each pixel centre meets y = -0.5; linear depth = the hit's view z.
        const double tanHalf = std::tan(cam.verticalFov * 0.5), aspect = (double)width / height;
        std::vector<float> ocean((size_t)width * height, INFINITY);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const double nx = (x + 0.5) / width * 2 - 1, ny = 1 - (y + 0.5) / height * 2;
                const double dx = nx * tanHalf * aspect, dy = ny * tanHalf;  // direction at view z = 1
                if (dy >= -1e-6) continue;
                const double t = 0.5 / -dy;  // view z of the hit
                const double hx = dx * t;
                if (std::fabs(hx) < 2 && t < 12) ocean[(size_t)y * width + x] = (float)t;
            }
        ComPtr<ID3D12Resource> oceanUpload = buffer((uint64_t)rowPitch(width) * height, D3D12_HEAP_TYPE_UPLOAD, L"ocean upload");
        {
            uint8_t* p = nullptr;
            check(oceanUpload->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
            for (uint32_t y = 0; y < height; ++y) std::memcpy(p + (size_t)y * rowPitch(width), &ocean[(size_t)y * width], width * 4);
            oceanUpload->Unmap(0, nullptr);
        }

        RenderGraph graph(device());
        TrackState trackState;
        FrameServices services;
        services.rasterizeDepth = [](FramePassContext& c, const DepthRasterRequest& r) { tracks::rasterizeDepth(c, r); };
        ComPtr<ID3D12Resource> constants = buffer(2048, D3D12_HEAP_TYPE_UPLOAD, L"test constants");
        uint8_t* mapped = nullptr;
        check(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map");
        const uint64_t texBytes = (uint64_t)rowPitch(width) * height;
        ComPtr<ID3D12Resource> visRb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb vis"), depthRb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb water depth"),
                               bandARb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb band A"), listRb;
        uint64_t listBytes = 0;
        std::vector<uint32_t> vis, list;
        std::vector<float> waterDepth, bandA;
        float nearPlane = 0;
        for (uint32_t f = 0; f < 3; ++f)
        {
            FrameContext frame;
            frame.frameIndex = f;
            frame.mainView = mainView;
            const gpu::FrameConstants fcData = FrameRenderer::frameConstants(gs, frame, mainView);
            nearPlane = fcData.nearPlane;
            std::memcpy(mapped + (f % 2) * 1024, &fcData, sizeof fcData);
            const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress() + (f % 2) * 1024;
            FrameResources resources;
            const TextureRef oceanTex = graph.createTexture({ "test.oceanDepth", width, height, 1, 1, DXGI_FORMAT_R32_FLOAT });
            ID3D12Resource* up = oceanUpload.Get();
            graph.addPass("test.ocean.upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(oceanTex, Use::CopyDst); },
                          [=](PassContext& c) {
                              D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                              to.pResource = c.resource(oceanTex);
                              to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                              from.pResource = up;
                              from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                              from.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, width, height, 1, rowPitch(width) };
                              c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                          });
            resources.oceanDepth = oceanTex;
            FramePassContext fc{ device(), graph, shaders(), q, gs, frame, resources, services, [=](const ViewDesc&) { return address; }, &trackState, 2 };
            ViewResources main;
            main.view = mainView;
            main.frameConstants = address;
            tracks::visibility(fc, main);
            CHECK(main.waterVis.valid() && main.waterDepth.valid() && main.oceanEdgePixels.valid());
            const TextureRef wv = main.waterVis, wd = main.waterDepth, da = main.depth;
            const BufferRef el = main.oceanEdgePixels;
            const uint64_t bytes = graph.desc(el).size;
            if (bytes != listBytes)
            {
                if (listRb) device().deferRelease(listRb);
                listRb = buffer(bytes, D3D12_HEAP_TYPE_READBACK, L"rb ocean edges");
                listBytes = bytes;
            }
            ID3D12Resource *v = visRb.Get(), *d = depthRb.Get(), *a = bandARb.Get(), *lr = listRb.Get();
            graph.addPass("test.readback", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(wv, Use::CopySrc);
                              b.use(wd, Use::CopySrc);
                              b.use(da, Use::CopySrc);
                              b.use(el, Use::CopySrc);
                              b.keep();
                          },
                          [=](PassContext& c) {
                              copyTexture(c, wv, v, width, height);
                              copyTexture(c, wd, d, width, height);
                              copyTexture(c, da, a, width, height);
                              c.cmd->CopyBufferRegion(lr, 0, c.resource(el), 0, bytes);
                          });
            graph.execute(nullptr);
            device().waitIdle();
            vis = readTexture<uint32_t>(v, width, height);
            waterDepth = readTexture<float>(d, width, height);
            bandA = readTexture<float>(a, width, height);
            list.resize(bytes / 4);
            uint8_t* p = nullptr;
            check(lr->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
            std::memcpy(list.data(), p, bytes);
            lr->Unmap(0, nullptr);
        }

        const size_t n = (size_t)width * height;
        std::vector<uint8_t> isOcean(n, 0), tie(n, 0);
        for (size_t i = 0; i < n; ++i)
        {
            const float d = ocean[i];
            if (!(d < INFINITY)) continue;
            const float bandLinear = bandA[i] > 0 ? nearPlane / bandA[i] : INFINITY;
            if (std::fabs((double)d - bandLinear) <= 1e-4 * d) tie[i] = 1;
            isOcean[i] = d < bandLinear;
        }
        uint64_t judged = 0, oceanPixels = 0, wrongVis = 0, wrongDepth = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (tie[i]) continue;
            ++judged;
            oceanPixels += isOcean[i];
            if ((vis[i] == oceanId) != (bool)isOcean[i] || (!isOcean[i] && vis[i] != 0)) ++wrongVis;
            if (isOcean[i] ? std::memcmp(&waterDepth[i], &ocean[i], 4) != 0 : !std::isinf(waterDepth[i])) ++wrongDepth;
        }
        // Edge set (the GPU's rule on its own outputs: ocean = the ocean id with the water depth in front of band A).
        auto gpuOcean = [&](size_t i) { return vis[i] == oceanId && waterDepth[i] < (bandA[i] > 0 ? nearPlane / std::max(bandA[i], 1e-30f) : INFINITY); };
        std::vector<uint8_t> want(n, 0), tieNear(n, 0);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t i = (size_t)y * width + x;
                bool anyOcean = false, anyOther = false, anyTie = false;
                float dd[3][3] = {};
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const size_t j = (size_t)std::clamp((int)y + dy, 0, (int)height - 1) * width + std::clamp((int)x + dx, 0, (int)width - 1);
                        const bool o = gpuOcean(j);
                        anyOcean = anyOcean || o;
                        anyOther = anyOther || !o;
                        anyTie = anyTie || tie[j];
                        dd[dy + 1][dx + 1] = o ? waterDepth[j] : 0;
                    }
                tieNear[i] = anyTie;
                if (gpuOcean(i))
                {
                    const float c = dd[1][1];
                    want[i] = anyOther || std::fabs(dd[1][0] + dd[1][2] - 2 * c) > 1e-3f * c || std::fabs(dd[0][1] + dd[2][1] - 2 * c) > 1e-3f * c;
                }
                else
                    want[i] = anyOcean;
            }
        const uint32_t count = list[0];
        std::vector<uint8_t> listed(n, 0);
        uint64_t duplicate = 0;
        for (uint32_t k = 0; k < count && 4 + (size_t)k < list.size(); ++k)
        {
            const uint32_t pxl = list[4 + k], x = pxl & 0xFFFFu, y = pxl >> 16;
            if (x >= width || y >= height) { ++duplicate; continue; }
            const size_t i = (size_t)y * width + x;
            duplicate += listed[i];
            listed[i] = 1;
        }
        uint64_t missing = 0, extra = 0, wantCount = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (tieNear[i]) continue;
            wantCount += want[i];
            missing += want[i] && !listed[i];
            extra += !want[i] && listed[i];
        }
        logf("    %llu judged pixels (%llu ocean): %llu wrong vis ids, %llu wrong depths; edge list %u entries (args %u, %u, %u), %llu wanted, %llu missing, %llu "
             "extra, %llu duplicate or outside\n",
             (unsigned long long)judged, (unsigned long long)oceanPixels, (unsigned long long)wrongVis, (unsigned long long)wrongDepth, count, list[1], list[2], list[3],
             (unsigned long long)wantCount, (unsigned long long)missing, (unsigned long long)extra, (unsigned long long)duplicate);
        CHECK(oceanPixels > 10000 && judged > 200000);
        CHECK(wrongVis == 0 && wrongDepth == 0);
        CHECK(wantCount > 100 && missing == 0 && extra == 0 && duplicate == 0);
        CHECK(list[1] == (count + 63) / 64 && list[2] == 1 && list[3] == 1);
        logf("PASS ocean_layer_merges_and_lists_edges\n1/1 passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL ocean_layer_merges_and_lists_edges: %s\n0/1 passed\n", e.what());
        return 1;
    }
}
