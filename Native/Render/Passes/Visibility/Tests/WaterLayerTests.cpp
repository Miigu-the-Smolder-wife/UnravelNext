// Water layer (render C; A's decision on W's request 20260926_W_ocean_patch_stream; INTERFACES v1.61), real device with the
// debug layer, V called directly (tracks::visibility) with a layer-1 triangle stream in FrameResources::triangleStreams:
//   water_layer_is_exact   a tilted water quad (2 triangles) in front of a far box, a small box standing in front of part of
//                          it: at every pixel whose centre sees the quad in front of band A, waterVis names slot 0 and
//                          triangle 0 or 1 and waterDepth is the ray-plane hit's view depth (relative error <= 1e-5);
//                          elsewhere VIS_NONE and +inf (pixels within 0.01 px of a quad edge or of a depth tie with band A are
//                          not judged: the rasteriser's tie rule); band A's depth is not written by the water (its edge
//                          coverage records, v1.64, are unx_test_visibility_wateredgetests).
//   unx_test_visibility_waterlayertests
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
        s.meshes = { box({ 3, 3, 0.5f }), box({ 0.05f, 0.05f, 0.05f }) };
        scene::Instance farBox, nearBox;
        farBox.mesh = 0;
        farBox.transform.m[2][3] = 20;
        nearBox.mesh = 1;
        nearBox.transform.m[0][3] = 0.1f, nearBox.transform.m[2][3] = 1.5f;  // in front of the quad's middle
        s.instances = { farBox, nearBox };
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
        std::vector<uint32_t> vis;
        std::vector<float> waterDepth, bandA;
        uint32_t coverageFragments = 0;
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
            ID3D12Resource *v = visRb.Get(), *d = depthRb.Get(), *a = bandARb.Get();
            graph.addPass("test.readback", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(wv, Use::CopySrc);
                              b.use(wd, Use::CopySrc);
                              b.use(da, Use::CopySrc);
                              b.keep();
                          },
                          [=](PassContext& c) {
                              copyTexture(c, wv, v, width, height);
                              copyTexture(c, wd, d, width, height);
                              copyTexture(c, da, a, width, height);
                          });
            graph.execute(nullptr);
            device().waitIdle();
            vis = readTexture<uint32_t>(v, width, height);
            waterDepth = readTexture<float>(d, width, height);
            bandA = readTexture<float>(a, width, height);
            coverageFragments = visibility::latestStats(trackState).coverageFragments;
        }

        // Reference per pixel centre: the ray through it, its hit with the quad's plane (inside the quad?), the hit's view
        // depth (camera at the origin looking along +z: the hit's z) and band A's view depth there (near / device depth).
        const float4x4 inv = inverse(mainView.viewProj);
        auto rayAt = [&](double px, double py) {
            const double ndc[2] = { px / width * 2 - 1, 1 - py / height * 2 };
            double w[4];
            for (int r = 0; r < 4; ++r) w[r] = inv.m[r][0] * ndc[0] + inv.m[r][1] * ndc[1] + inv.m[r][2] * 1.0 + inv.m[r][3];
            const double x = w[0] / w[3], y = w[1] / w[3], z = w[2] / w[3];
            const double l = std::sqrt(x * x + y * y + z * z);
            return std::array<double, 3>{ x / l, y / l, z / l };
        };
        const double nx = normal.x, ny = normal.y, nz = normal.z;
        const double planeD = nx * corners[0].x + ny * corners[0].y + nz * corners[0].z;
        const double focal = 0.5 * height / std::tan(0.5 * cam.verticalFov), nearPlane = mainView.nearPlane;
        uint64_t waterPixels = 0, missing = 0, extra = 0, badId = 0, bandTouched = 0, skipped = 0;
        double worstDepth = 0;
        for (uint32_t py = 0; py < height; ++py)
            for (uint32_t px = 0; px < width; ++px)
            {
                const size_t i = (size_t)py * width + px;
                const auto dir = rayAt(px + 0.5, py + 0.5);
                const double t = planeD / (nx * dir[0] + ny * dir[1] + nz * dir[2]);
                const double hx = dir[0] * t, hz = dir[2] * t;
                const double sl = (hz - 1.8) / 0.6;  // 0 at the near edge, 1 at the far edge
                const bool inside = t > 0 && hx > -0.5 && hx < 0.5 && sl > 0 && sl < 1;
                const double viewDepth = hz;
                const double bandDepth = bandA[i] > 0 ? nearPlane / bandA[i] : std::numeric_limits<double>::infinity();
                const bool expect = inside && viewDepth < bandDepth;
                // Tie zones: within 0.01 px of a quad edge (world length at the hit's depth), or a depth tie with band A.
                const double pixelWorld = viewDepth / focal;
                const double edge = std::min({ std::fabs(hx + 0.5), std::fabs(hx - 0.5), std::fabs(sl) * 0.67, std::fabs(sl - 1) * 0.67 });
                if (t > 0 && (edge < 0.01 * pixelWorld || std::fabs(viewDepth - bandDepth) < 1e-4))
                {
                    ++skipped;
                    continue;
                }
                if (bandA[i] > 0 && std::fabs(nearPlane / bandA[i] - viewDepth) < 1e-5 * viewDepth) ++bandTouched;
                if (expect)
                {
                    ++waterPixels;
                    if (vis[i] == 0) ++missing;
                    else if ((vis[i] >> 30) != 3u || ((vis[i] >> 24) & 0x3Fu) != 0 || (vis[i] & 0xFFFFFFu) >= 2) ++badId;
                    else worstDepth = std::max(worstDepth, std::fabs(waterDepth[i] - viewDepth) / viewDepth);
                }
                else if (vis[i] != 0 || !std::isinf(waterDepth[i])) ++extra;
            }
        logf("    %llu water pixels (%llu at an edge or tie, not judged): %llu missing, %llu extra, %llu wrong ids, worst depth error %.2e relative; band A depth "
             "equal to the water's at %llu pixels; %u coverage records\n",
             (unsigned long long)waterPixels, (unsigned long long)skipped, (unsigned long long)missing, (unsigned long long)extra, (unsigned long long)badId, worstDepth,
             (unsigned long long)bandTouched, coverageFragments);
        CHECK(waterPixels > 10000 && missing == 0 && extra == 0 && badId == 0 && worstDepth <= 1e-5);
        CHECK(bandTouched == 0);  // edge records (v1.64, coverage layer on) are unx_test_visibility_wateredgetests
        logf("PASS water_layer_is_exact\n1/1 passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL water_layer_is_exact: %s\n0/1 passed\n", e.what());
        return 1;
    }
}
