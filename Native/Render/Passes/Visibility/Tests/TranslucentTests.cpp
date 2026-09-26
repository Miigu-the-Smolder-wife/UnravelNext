// A6 translucent layer (render C; A's decision, INTERFACES v1.67), real device with the debug layer, V called directly with
// the coverage layer on. Scene: a far opaque backdrop, two two-sided glass quads (pane 1 nearer, pane 2 behind it and
// partly overlapping it) and a two-sided opaque quad in front of pane 1's corner.
//   translucent_layer_is_exact
//     - band A holds no glass (every band A pixel is the backdrop or the occluder);
//     - per pixel centre, against a CPU reference (projected quads, 1/w interpolated in screen space; pixels within
//       0.01 px of an outline, and their 3 x 3 neighbours, are not judged): the class (0 none in the 3 x 3 block, 1 one
//       surface over the whole block, 2 overlap or an edge, either side of an outline) matches, and where a sample exists its surface is the nearest pane and its
//       depth is within 1e-5 relative;
//     - translucent coverage records come only from class 2 pixels, all see-through, all of the panes;
//     - pane 2 (not occluded, partly behind pane 1): its class 1 pixels plus its records' areas add up to its exact
//       projected area within the records' 10-bit rounding.
//   unx_test_visibility_translucenttests
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

scene::Mesh quad(const float3 (&c)[4], uint32_t material)
{
    scene::Mesh m;
    m.name = "quad";
    const float3 n = normalize(cross(c[1] - c[0], c[3] - c[0]));
    for (const float3& p : c)
    {
        m.positions.push_back(p);
        m.normals.push_back(n);
    }
    m.indices = { 0, 1, 2, 0, 2, 3 };
    m.submeshes.push_back({ 0, 6, material });
    return m;
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

struct Projected
{
    std::array<double, 2> p[4];
    double iw[4];  // 1 / w
};

double cross2(std::array<double, 2> a, std::array<double, 2> b, std::array<double, 2> c) { return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]); }

double segmentDistance(std::array<double, 2> a, std::array<double, 2> b, double x, double y)
{
    const double dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
    const double t = l2 > 0 ? std::clamp(((x - a[0]) * dx + (y - a[1]) * dy) / l2, 0.0, 1.0) : 0.0;
    return std::hypot(a[0] + t * dx - x, a[1] + t * dy - y);
}

// Inside the convex projected quad (either winding); depth = w at (x, y) from 1/w interpolated over the triangle holding it.
bool quadAt(const Projected& q, double x, double y, double& depth)
{
    const std::array<double, 2> s{ x, y };
    double sign = 0;
    for (int k = 0; k < 4; ++k)
    {
        const double c = cross2(q.p[k], q.p[(k + 1) % 4], s);
        if (sign == 0) sign = c;
        if (c * sign < 0) return false;
    }
    for (int t = 0; t < 2; ++t)
    {
        const int i0 = 0, i1 = 1 + t, i2 = 2 + t;
        const double area = cross2(q.p[i0], q.p[i1], q.p[i2]);
        const double b1 = cross2(q.p[i0], s, q.p[i2]) / area, b2 = cross2(q.p[i0], q.p[i1], s) / area, b0 = 1 - b1 - b2;
        if (b0 >= -1e-9 && b1 >= -1e-9 && b2 >= -1e-9)
        {
            depth = 1.0 / (b0 * q.iw[i0] + b1 * q.iw[i1] + b2 * q.iw[i2]);
            return true;
        }
    }
    return false;
}
} // namespace

int main()
{
    try
    {
        const uint32_t width = 640, height = 360;
        QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const char* o : { "visibility.coverage_layer = true", "visibility.occlusion_culling = false" }) q.applyOverride(o);
        scene::Scene s;
        s.name = "translucent layer";
        s.materials.resize(3);
        s.materials[1].cls = scene::MaterialClass::Glass;
        s.materials[1].twoSided = true;
        s.materials[2].twoSided = true;
        const float3 pane1[4] = { { -0.6f, -0.35f, 2.0f }, { 0.2f, -0.35f, 2.16f }, { 0.2f, 0.35f, 2.16f }, { -0.6f, 0.35f, 2.0f } };
        const float3 pane2[4] = { { -0.1f, -0.25f, 3.0f }, { 0.7f, -0.25f, 3.0f }, { 0.7f, 0.45f, 3.07f }, { -0.1f, 0.45f, 3.07f } };
        const float3 occluder[4] = { { -0.75f, -0.5f, 1.5f }, { -0.3f, -0.5f, 1.5f }, { -0.3f, -0.1f, 1.5f }, { -0.75f, -0.1f, 1.5f } };
        s.meshes = { box({ 30, 30, 0.5f }), quad(occluder, 2), quad(pane1, 1), quad(pane2, 1) };
        s.instances.resize(4);
        s.instances[0].mesh = 0;
        s.instances[0].transform.m[2][3] = 20;
        for (uint32_t i = 1; i < 4; ++i) s.instances[i].mesh = i;
        scene::Camera cam;
        cam.name = "front";
        cam.forward = { 0, 0, 1 };
        s.cameras.push_back(cam);
        scene::validate(s);
        GpuScene gs(device());
        gs.upload(s);
        gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));

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
        ComPtr<ID3D12Resource> tvisRb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb translucent vis"), tdepthRb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb translucent depth"),
                               classRb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb class"), visRb = buffer(texBytes, D3D12_HEAP_TYPE_READBACK, L"rb band A vis");
        std::vector<uint32_t> tvis, vis, tiles, records, visible;
        std::vector<float> tdepth;
        std::vector<uint8_t> cls((size_t)width * height);
        const uint64_t tileCount = (uint64_t)((width + 7) / 8) * ((height + 7) / 8);
        ComPtr<ID3D12Resource> tilesRb = buffer(tileCount * 32, D3D12_HEAP_TYPE_READBACK, L"rb tiles"), recordsRb, visibleRb;
        uint64_t recordBytes = 0, visibleBytes = 0;
        for (uint32_t f = 0; f < 3; ++f)
        {
            FrameContext frame;
            frame.frameIndex = f;
            frame.mainView = mainView;
            const gpu::FrameConstants fcData = FrameRenderer::frameConstants(gs, frame, mainView);
            std::memcpy(mapped + (f % 2) * 1024, &fcData, sizeof fcData);
            const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress() + (f % 2) * 1024;
            FrameResources resources;
            FramePassContext fc{ device(), graph, shaders(), q, gs, frame, resources, services, [=](const ViewDesc&) { return address; }, &trackState, 2 };
            ViewResources main;
            main.view = mainView;
            main.frameConstants = address;
            tracks::visibility(fc, main);
            CHECK(main.translucentVis.valid() && main.translucentDepth.valid() && main.translucentClass.valid());
            CHECK(main.coverageTiles.valid() && main.coverageRecords.valid());
            const TextureRef tv = main.translucentVis, td = main.translucentDepth, tc = main.translucentClass, bv = main.visId;
            const BufferRef ct = main.coverageTiles, cr = main.coverageRecords, vl = main.visibleClusters;
            const uint64_t poolBytes = graph.desc(cr).size, listBytes = graph.desc(vl).size;
            if (poolBytes != recordBytes)
            {
                if (recordsRb) device().deferRelease(recordsRb);
                recordsRb = buffer(poolBytes, D3D12_HEAP_TYPE_READBACK, L"rb records");
                recordBytes = poolBytes;
            }
            if (listBytes != visibleBytes)
            {
                if (visibleRb) device().deferRelease(visibleRb);
                visibleRb = buffer(listBytes, D3D12_HEAP_TYPE_READBACK, L"rb visible");
                visibleBytes = listBytes;
            }
            ID3D12Resource *a = tvisRb.Get(), *b2 = tdepthRb.Get(), *c2 = classRb.Get(), *v = visRb.Get(), *tl = tilesRb.Get(), *rc = recordsRb.Get(), *vr = visibleRb.Get();
            graph.addPass("test.readback", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(tv, Use::CopySrc);
                              b.use(td, Use::CopySrc);
                              b.use(tc, Use::CopySrc);
                              b.use(bv, Use::CopySrc);
                              b.use(ct, Use::CopySrc);
                              b.use(cr, Use::CopySrc);
                              b.use(vl, Use::CopySrc);
                              b.keep();
                          },
                          [=](PassContext& c) {
                              copyTexture(c, tv, a, width, height);
                              copyTexture(c, td, b2, width, height);
                              copyTexture(c, tc, c2, width, height);
                              copyTexture(c, bv, v, width, height);
                              c.cmd->CopyBufferRegion(tl, 0, c.resource(ct), 0, tileCount * 32);
                              c.cmd->CopyBufferRegion(rc, 0, c.resource(cr), 0, poolBytes);
                              c.cmd->CopyBufferRegion(vr, 0, c.resource(vl), 0, listBytes);
                          });
            graph.execute(nullptr);
            device().waitIdle();
            tvis = readTexture<uint32_t>(a, width, height);
            tdepth = readTexture<float>(b2, width, height);
            vis = readTexture<uint32_t>(v, width, height);
            {
                uint8_t* p = nullptr;
                check(c2->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
                for (uint32_t y = 0; y < height; ++y) std::memcpy(&cls[(size_t)y * width], p + (size_t)y * rowPitch(width), width);
                c2->Unmap(0, nullptr);
            }
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
            visible = words(vr, listBytes / 4);
        }
        auto instanceOf = [&](uint32_t visId) -> uint32_t {
            const uint64_t e = (uint64_t)((visId - 1) >> 7);
            return 2 * e < visible.size() ? visible[2 * e] : 0xFFFFFFFFu;
        };

        // Band A holds no glass.
        uint64_t glassInBandA = 0;
        for (uint32_t id : vis)
            if (id != 0 && instanceOf(id) >= 2) ++glassInBandA;

        // CPU reference per pixel centre.
        auto project = [&](const float3 (&c)[4]) {
            Projected p;
            for (int k = 0; k < 4; ++k)
            {
                double h[4];
                for (int r = 0; r < 4; ++r) h[r] = (double)mainView.viewProj.m[r][0] * c[k].x + (double)mainView.viewProj.m[r][1] * c[k].y + (double)mainView.viewProj.m[r][2] * c[k].z + mainView.viewProj.m[r][3];
                p.p[k] = { (h[0] / h[3] * 0.5 + 0.5) * width, (0.5 - h[1] / h[3] * 0.5) * height };
                p.iw[k] = 1.0 / h[3];
            }
            return p;
        };
        const Projected quads[3] = { project(occluder), project(pane1), project(pane2) };  // instances 1, 2, 3
        const size_t n = (size_t)width * height;
        std::vector<uint8_t> refCount(n, 0), nearOutline(n, 0);
        std::vector<uint32_t> refNearest(n, 0);
        std::vector<double> refDepth(n, INFINITY);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t i = (size_t)y * width + x;
                const double px = x + 0.5, py = y + 0.5;
                for (const Projected& qd : quads)
                    for (int k = 0; k < 4; ++k)
                        if (segmentDistance(qd.p[k], qd.p[(k + 1) % 4], px, py) < 0.01) nearOutline[i] = 1;
                double d = 0;
                if (quadAt(quads[0], px, py, d)) continue;  // the occluder is nearer than both panes
                for (uint32_t pane = 1; pane <= 2; ++pane)
                    if (quadAt(quads[pane], px, py, d))
                    {
                        ++refCount[i];
                        if (d < refDepth[i])
                        {
                            refDepth[i] = d;
                            refNearest[i] = pane + 1;  // instance
                        }
                    }
            }
        uint64_t judged = 0, classWrong = 0, surfaceWrong = 0, depthWrong = 0, samples = 0, class2 = 0;
        double worstDepth = 0;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
            {
                const size_t i = (size_t)y * width + x;
                bool skip = false, edge = false, nearby = false;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const size_t j = (size_t)std::clamp((int)y + dy, 0, (int)height - 1) * width + std::clamp((int)x + dx, 0, (int)width - 1);
                        skip = skip || nearOutline[j];
                        edge = edge || refCount[j] != 1 || refNearest[j] != refNearest[i];
                        nearby = nearby || refCount[j] != 0;
                    }
                class2 += cls[i] == 2;
                if (skip) continue;
                ++judged;
                const uint8_t want = refCount[i] == 0 ? (nearby ? 2 : 0) : (refCount[i] >= 2 || edge ? 2 : 1);
                if (cls[i] != want) ++classWrong;
                if (refCount[i] > 0)
                {
                    ++samples;
                    if (tvis[i] == 0 || instanceOf(tvis[i]) != refNearest[i]) ++surfaceWrong;
                    const double rel = std::fabs(tdepth[i] - refDepth[i]) / refDepth[i];
                    worstDepth = std::max(worstDepth, rel);
                    if (!(rel <= 1e-5)) ++depthWrong;
                }
                else if (tvis[i] != 0 || !std::isinf(tdepth[i]))
                    ++surfaceWrong;
            }

        // Translucent records (cluster ids of the panes): pixels, flags, and pane 2's area.
        const uint32_t tilesX = (width + 7) / 8;
        uint64_t paneRecords = 0, badRecords = 0, outsideClass2 = 0;
        double pane2RecordArea = 0;
        std::vector<double> pane2PixelArea(n, 0.0);
        for (uint64_t t = 0; t < tileCount; ++t)
        {
            const uint32_t count = tiles[8 * t], base = tiles[8 * t + 1], listed = tiles[8 * t + 2];
            if (listed == 0) continue;
            for (uint32_t r = 0; r < count; ++r)
            {
                const uint32_t* rec = &records[4 * (size_t)(base + r)];
                if ((rec[0] >> 30) >= 2u) continue;  // hair, streams
                const uint32_t inst = instanceOf(rec[0]);
                if (inst < 2) continue;  // band B records of opaque geometry (none expected here)
                ++paneRecords;
                if ((rec[1] & 0x80000000u) == 0 || inst > 3) ++badRecords;
                const uint32_t p = rec[3] >> 26, px = (uint32_t)(t % tilesX) * 8 + p % 8, py = (uint32_t)(t / tilesX) * 8 + p / 8;
                if (px >= width || py >= height || cls[(size_t)py * width + px] != 2) ++outsideClass2;
                if (inst == 3)
                {
                    pane2RecordArea += ((rec[3] >> 16) & 0x3FFu) / 1023.0;
                    if (px < width && py < height) pane2PixelArea[(size_t)py * width + px] += ((rec[3] >> 16) & 0x3FFu) / 1023.0;
                }
            }
        }
        uint64_t pane2Full = 0, pane2RecordCount = 0;
        for (size_t i = 0; i < n; ++i)
        {
            if (cls[i] == 1 && tvis[i] != 0 && instanceOf(tvis[i]) == 3) ++pane2Full;
        }
        for (uint64_t t = 0; t < tileCount; ++t)
        {
            const uint32_t count = tiles[8 * t], base = tiles[8 * t + 1];
            if (tiles[8 * t + 2] == 0) continue;
            for (uint32_t r = 0; r < count; ++r)
                if ((records[4 * (size_t)(base + r)] >> 30) < 2u && instanceOf(records[4 * (size_t)(base + r)]) == 3) ++pane2RecordCount;
        }
        double area2 = 0;
        for (int k = 0; k < 4; ++k)
        {
            const auto a = quads[2].p[k], b = quads[2].p[(k + 1) % 4];
            area2 += a[0] * b[1] - b[0] * a[1];
        }
        {
            // Per pixel: the exact area of pane 2 inside the pixel (convex clip) against what the layer gives it.
            auto clipArea = [&](uint32_t x, uint32_t y) {
                std::vector<std::array<double, 2>> poly(quads[2].p, quads[2].p + 4);
                const double bounds[4] = { (double)x, (double)x + 1, (double)y, (double)y + 1 };
                for (int e = 0; e < 4; ++e)
                {
                    std::vector<std::array<double, 2>> out;
                    auto inside = [&](const std::array<double, 2>& q) { return e == 0 ? q[0] >= bounds[0] : e == 1 ? q[0] <= bounds[1] : e == 2 ? q[1] >= bounds[2] : q[1] <= bounds[3]; };
                    for (size_t k = 0; k < poly.size(); ++k)
                    {
                        const auto a = poly[k], b = poly[(k + 1) % poly.size()];
                        const int axis = e < 2 ? 0 : 1;
                        if (inside(a)) out.push_back(a);
                        if (inside(a) != inside(b))
                        {
                            const double t = (bounds[e] - a[axis]) / (b[axis] - a[axis]);
                            out.push_back({ a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1]) });
                        }
                    }
                    poly = out;
                    if (poly.empty()) return 0.0;
                }
                double a2 = 0;
                for (size_t k = 0; k < poly.size(); ++k) a2 += poly[k][0] * poly[(k + 1) % poly.size()][1] - poly[(k + 1) % poly.size()][0] * poly[k][1];
                return std::fabs(a2) * 0.5;
            };
            uint32_t shown = 0;
            double missing = 0;
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    const size_t i = (size_t)y * width + x;
                    const double want = clipArea(x, y);
                    const double got = (cls[i] == 1 && tvis[i] != 0 && instanceOf(tvis[i]) == 3) ? 1.0 : pane2PixelArea[i];
                    if (std::fabs(want - got) > 0.01)
                    {
                        missing += want - got;
                        if (shown++ < 12)
                            logf("      pixel (%u, %u): pane 2 exact %.4f, layer %.4f (class %u, count ref %u, nearest %u, records %.4f)\n", x, y, want, got, cls[i], refCount[i],
                                 tvis[i] ? instanceOf(tvis[i]) : 0u, pane2PixelArea[i]);
                    }
                }
            logf("    pane 2 per-pixel differences: %u pixels, %.3f px^2 net missing\n", shown, missing);
        }
        const double exact = std::fabs(area2) * 0.5, total = (double)pane2Full + pane2RecordArea, rounding = pane2RecordCount * 0.5 / 1023.0;
        logf("    band A glass pixels %llu; judged %llu pixels (%llu with a sample): %llu class, %llu surface, %llu depth mismatches (worst depth %.2e); "
             "class 2 pixels %llu\n",
             (unsigned long long)glassInBandA, (unsigned long long)judged, (unsigned long long)samples, (unsigned long long)classWrong, (unsigned long long)surfaceWrong,
             (unsigned long long)depthWrong, worstDepth, (unsigned long long)class2);
        logf("    pane records %llu (%llu bad, %llu outside class 2); pane 2: %llu full pixels + %.3f px^2 in %llu records = %.3f px^2 vs exact %.3f (rounding %.3f)\n",
             (unsigned long long)paneRecords, (unsigned long long)badRecords, (unsigned long long)outsideClass2, (unsigned long long)pane2Full, pane2RecordArea,
             (unsigned long long)pane2RecordCount, total, exact, rounding);
        CHECK(glassInBandA == 0);
        CHECK(judged > 100000 && samples > 10000);
        CHECK(classWrong == 0 && surfaceWrong == 0 && depthWrong == 0);
        CHECK(paneRecords > 0 && badRecords == 0 && outsideClass2 == 0);
        CHECK(std::fabs(total - exact) <= rounding + 1e-4 * exact);
        logf("PASS translucent_layer_is_exact\n1/1 passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL translucent_layer_is_exact: %s\n0/1 passed\n", e.what());
        return 1;
    }
}
