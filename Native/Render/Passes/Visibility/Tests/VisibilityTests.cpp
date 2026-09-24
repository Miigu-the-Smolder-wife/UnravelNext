// V correctness on the real device with the debug layer (no GPU lock; INTERFACES 3.3):
//   occlusion_is_conservative      two-phase HiZ culling over a moving camera gives bit-identical depth to no culling
//   vis_id_decodes_to_the_covering_triangle   every vis id names a triangle that covers the pixel centre at its depth
//   lod_cut_has_no_holes           the DAG cut covers every pixel the source geometry covers (away from silhouettes)
//   raster_service                 rasterizeDepth: hardware depth and a requester pixel kernel (DepthRasterPixel),
//                                  orthographic LOD, tile mask
//   unx_test_visibility_visibilitytests [filter]        (UNX_GPU_VALIDATION=1: GPU-based validation as well)
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"
#include "unx/visibility/Visibility.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
struct TestCase
{
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}
struct Register
{
    Register(const char* name, std::function<void()> fn) { registry().push_back({ name, std::move(fn) }); }
};
#define UNX_TEST(name) \
    static void name(); \
    static Register reg_##name(#name, name); \
    static void name()
#define CHECK(cond) \
    do { if (!(cond)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #cond); } while (0)

constexpr uint32_t kVisNone = 0;  // VisBuffer.hlsli: vis id = (visibleCluster << 7 | triangle) + 1

Device& device()
{
    static Device d([] {
        DeviceOptions o;
        o.debugLayer = true;
        char* env = nullptr;
        size_t length = 0;
        o.gpuValidation = _dupenv_s(&env, &length, "UNX_GPU_VALIDATION") == 0 && env != nullptr;  // GPU-based validation (slow)
        std::free(env);
        return o;
    }());
    return d;
}
ShaderLibrary& shaders()
{
    static ShaderLibrary lib(device(), executableDirectory() / "shaders");
    return lib;
}
QualityConfig quality(const std::vector<std::string>& overrides = {})
{
    QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    for (const std::string& o : overrides) q.applyOverride(o);
    return q;
}

// ---------------------------------------------------------------------------------------------------------- scene

float terrainHeight(float x, float z) { return 2.0f * std::sin(0.2f * x) * std::cos(0.25f * z) + 0.5f * std::sin(0.9f * x + 0.7f * z); }

scene::Mesh terrain(uint32_t n, float size)
{
    scene::Mesh m;
    m.name = "terrain";
    const float d = size / n;
    for (uint32_t i = 0; i <= n; ++i)
        for (uint32_t j = 0; j <= n; ++j)
        {
            const float x = j * d, z = i * d;
            m.positions.push_back({ x, terrainHeight(x, z), z });
            m.normals.push_back({ 0, 1, 0 });
            m.uv0.push_back({ x / size, z / size });
        }
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t j = 0; j < n; ++j)
        {
            const uint32_t a = i * (n + 1) + j, b = a + 1, c = a + n + 1, e = c + 1;
            m.indices.insert(m.indices.end(), { a, c, b, b, c, e });
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    return m;
}

// Closed box centred at the origin, each face split into s x s quads, counter-clockwise seen from outside.
scene::Mesh box(float3 half, uint32_t s)
{
    scene::Mesh m;
    m.name = "box";
    const float3 axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int f = 0; f < 6; ++f)
    {
        const int a = f / 2;
        const float sign = (f & 1) ? -1.0f : 1.0f;
        const float3 n = axes[a] * sign, u = axes[(a + 1) % 3], v = cross(n, u);
        auto extent = [&](float3 dir) { return std::fabs(dir.x) * half.x + std::fabs(dir.y) * half.y + std::fabs(dir.z) * half.z; };
        const uint32_t base = (uint32_t)m.positions.size();
        for (uint32_t i = 0; i <= s; ++i)
            for (uint32_t j = 0; j <= s; ++j)
            {
                const float fu = (2.0f * j / s - 1) * extent(u), fv = (2.0f * i / s - 1) * extent(v);
                m.positions.push_back(n * extent(n) + u * fu + v * fv);
                m.normals.push_back(n);
                m.uv0.push_back({ (float)j / s, (float)i / s });
            }
        for (uint32_t i = 0; i < s; ++i)
            for (uint32_t j = 0; j < s; ++j)
            {
                const uint32_t p = base + i * (s + 1) + j, q = p + 1, r = p + s + 1, t = r + 1;
                m.indices.insert(m.indices.end(), { p, q, t, p, t, r });  // u x v = n: counter-clockwise from outside
            }
    }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    return m;
}

scene::Mesh sphere(float radius, uint32_t rings, uint32_t segments)
{
    scene::Mesh m;
    m.name = "sphere";
    const float pi = 3.14159265f;
    for (uint32_t r = 0; r <= rings; ++r)
        for (uint32_t s = 0; s <= segments; ++s)
        {
            const float th = pi * r / rings, ph = 2 * pi * s / segments;
            const float3 n{ std::sin(th) * std::cos(ph), std::cos(th), std::sin(th) * std::sin(ph) };
            m.positions.push_back(n * radius);
            m.normals.push_back(n);
            m.uv0.push_back({ (float)s / segments, (float)r / rings });
        }
    for (uint32_t r = 0; r < rings; ++r)
        for (uint32_t s = 0; s < segments; ++s)
        {
            const uint32_t a = r * (segments + 1) + s, b = a + 1, c = a + segments + 1, d = c + 1;
            if (r > 0) m.indices.insert(m.indices.end(), { a, b, c });
            if (r + 1 < rings) m.indices.insert(m.indices.end(), { b, d, c });
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    return m;
}

scene::Instance at(uint32_t mesh, float3 p)
{
    scene::Instance i;
    i.mesh = mesh;
    i.transform.m[0][3] = p.x;
    i.transform.m[1][3] = p.y;
    i.transform.m[2][3] = p.z;
    return i;
}

// Terrain 80 m, a row of walls (occluders) at z = 25, spheres behind them (mostly hidden) and in front.
scene::Scene makeScene()
{
    scene::Scene s;
    s.name = "v-test";
    s.materials.resize(1);
    s.meshes.push_back(terrain(160, 80.0f));
    s.meshes.push_back(box({ 5.5f, 4.0f, 0.5f }, 6));
    s.meshes.push_back(sphere(1.0f, 24, 48));
    s.instances.push_back(at(0, { 0, 0, 0 }));
    for (int k = 0; k < 6; ++k)
    {
        const float x = 7 + 13.2f * k;
        s.instances.push_back(at(1, { x, terrainHeight(x, 25) + 3.5f, 25 }));
    }
    for (int iz = 0; iz < 6; ++iz)
        for (int ix = 0; ix < 12; ++ix)
        {
            const float x = 4 + 6.6f * ix, z = 32 + 7.0f * iz;
            s.instances.push_back(at(2, { x, terrainHeight(x, z) + 1.2f, z }));
        }
    for (int ix = 0; ix < 5; ++ix)
    {
        const float x = 20 + 10.0f * ix, z = 14;
        s.instances.push_back(at(2, { x, terrainHeight(x, z) + 1.0f, z }));
    }
    scene::Camera c;
    c.name = "main";
    s.cameras.push_back(c);
    scene::validate(s);
    return s;
}

scene::Camera camera(float3 position, float3 target)
{
    scene::Camera c;
    c.position = position;
    c.forward = normalize(target - position);
    c.nearPlane = 0.05f;
    return c;
}

// ----------------------------------------------------------------------------------------------------- rendering

struct FrameOut
{
    ViewDesc view;
    std::vector<float> depth;
    std::vector<uint32_t> visId;
    std::vector<uint32_t> visible;  // uint2 per entry
};

struct RunOut
{
    std::vector<FrameOut> frames;
    visibility::Stats stats;
    ClusterData clusters;
};

uint32_t rowPitch(uint32_t width) { return (width * 4 + 255) / 256 * 256; }

ComPtr<ID3D12Resource> readbackBuffer(uint64_t bytes)
{
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "readback");
    return r;
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
    for (uint32_t y = 0; y < height; ++y) std::memcpy(&out[(size_t)y * width], p + (size_t)y * rowPitch(width), width * 4);
    rb->Unmap(0, nullptr);
    return out;
}

RunOut renderRun(const scene::Scene& s, const QualityConfig& q, const std::vector<scene::Camera>& cams, uint32_t width, uint32_t height)
{
    RunOut out;
    out.clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q));
    GpuScene gs(device());
    gs.upload(s);
    gs.setClusters(out.clusters);
    FrameRenderer renderer(device(), shaders(), q, gs, 2);
    RenderGraph graph(device());
    const uint64_t capacity = (uint64_t)q.integer("visibility.max_visible_clusters");
    ComPtr<ID3D12Resource> depthRb = readbackBuffer((uint64_t)rowPitch(width) * height), visRb = readbackBuffer((uint64_t)rowPitch(width) * height),
                           listRb = readbackBuffer(capacity * 8);
    float4x4 prev = ViewDesc::fromCamera(cams[0], width, height, {}).viewProj;
    for (uint32_t f = 0; f < cams.size(); ++f)
    {
        FrameContext fr;
        fr.frameIndex = f;
        fr.time = f / 60.0;
        fr.deltaTime = 1.0f / 60;
        fr.mainView = ViewDesc::fromCamera(cams[f], width, height, prev);
        TextureRef output = graph.createTexture({ "test output", width, height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
        const ViewResources main = renderer.record(graph, fr, output);
        ID3D12Resource *d = depthRb.Get(), *v = visRb.Get(), *l = listRb.Get();
        graph.addPass("test.readback", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(main.depth, Use::CopySrc);
                          b.use(main.visId, Use::CopySrc);
                          b.use(main.visibleClusters, Use::CopySrc);
                          b.keep();
                      },
                      [=](PassContext& c) {
                          copyTexture(c, main.depth, d, width, height);
                          copyTexture(c, main.visId, v, width, height);
                          c.cmd->CopyBufferRegion(l, 0, c.resource(main.visibleClusters), 0, capacity * 8);
                      });
        graph.execute(nullptr);
        device().waitIdle();
        FrameOut fo;
        fo.view = fr.mainView;
        fo.depth = readTexture<float>(d, width, height);
        fo.visId = readTexture<uint32_t>(v, width, height);
        fo.visible.resize(capacity * 2);
        uint8_t* p = nullptr;
        check(l->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
        std::memcpy(fo.visible.data(), p, capacity * 8);
        l->Unmap(0, nullptr);
        out.frames.push_back(std::move(fo));
        prev = fr.mainView.viewProj;
    }
    out.stats = visibility::latestStats(renderer.trackState());
    device().deferRelease(depthRb);
    device().deferRelease(visRb);
    device().deferRelease(listRb);
    return out;
}

std::vector<scene::Camera> movingCameras(uint32_t frames)
{
    std::vector<scene::Camera> cams;
    for (uint32_t f = 0; f < frames; ++f)
    {
        const float t = (float)f / std::max(frames - 1, 1u);
        const float yaw = -0.3f + 0.6f * t;
        const float3 p{ 38 + 6 * t, 4.5f, 2 + 5 * t };
        cams.push_back(camera(p, p + float3{ std::sin(yaw), -0.08f, std::cos(yaw) }));
    }
    return cams;
}

void logStats(const char* label, const visibility::Stats& st)
{
    logf("    %s: frame %llu, %u instances, %u nodes, %u clusters tested, %u visible, triangles A/B/C %u/%u/%u, deferred %u/%u/%u, overflow 0x%x\n", label,
         (unsigned long long)st.frameIndex, st.instancesVisible, st.nodesTested, st.clustersTested, st.visibleClusters, st.triangles[0], st.triangles[1], st.triangles[2],
         st.deferredInstances, st.deferredNodes, st.deferredClusters, st.overflow);
}
} // namespace

UNX_TEST(occlusion_is_conservative)
{
    const scene::Scene s = makeScene();
    const auto cams = movingCameras(8);
    const RunOut culled = renderRun(s, quality(), cams, 2560, 1440);
    const RunOut reference = renderRun(s, quality({ "visibility.occlusion_culling = false" }), cams, 2560, 1440);
    logStats("occlusion on ", culled.stats);
    logStats("occlusion off", reference.stats);
    CHECK(culled.stats.overflow == 0 && reference.stats.overflow == 0);
    CHECK(culled.stats.deferredInstances + culled.stats.deferredNodes + culled.stats.deferredClusters > 0);  // phase 2 was exercised
    CHECK(culled.stats.visibleClusters < reference.stats.visibleClusters);                                   // and culled something
    for (size_t f = 0; f < cams.size(); ++f)
    {
        size_t differ = 0, sky = 0;
        for (size_t i = 0; i < culled.frames[f].depth.size(); ++i)
        {
            if (std::memcmp(&culled.frames[f].depth[i], &reference.frames[f].depth[i], 4) != 0) ++differ;
            if (culled.frames[f].depth[i] == 0) ++sky;
            if ((culled.frames[f].depth[i] == 0) != (culled.frames[f].visId[i] == kVisNone))
                fail("frame %zu pixel %zu: depth %g with vis id 0x%08x", f, i, culled.frames[f].depth[i], culled.frames[f].visId[i]);
        }
        if (differ) fail("frame %zu: %zu pixels differ from rendering without occlusion culling", f, differ);
        if (f == 0) logf("    frame 0: %zu of %zu pixels sky\n", sky, culled.frames[f].depth.size());
    }
    logf("    %zu frames: depth identical to rendering without occlusion culling\n", cams.size());
}

UNX_TEST(vis_id_decodes_to_the_covering_triangle)
{
    const scene::Scene s = makeScene();
    const auto cams = movingCameras(3);
    const uint32_t width = 2560, height = 1440;
    const RunOut run = renderRun(s, quality(), cams, width, height);
    const FrameOut& fo = run.frames.back();
    const ClusterData& cd = run.clusters;
    size_t checked = 0, edgeSnapped = 0;
    for (uint32_t y = 0; y < height; y += 3)
        for (uint32_t x = 0; x < width; x += 3)
        {
            const size_t i = (size_t)y * width + x;
            const uint32_t id = fo.visId[i];
            if (id == kVisNone)
            {
                CHECK(fo.depth[i] == 0);
                continue;
            }
            const uint32_t entry = (id - 1) >> 7, tri = (id - 1) & 127;
            const uint32_t instance = fo.visible[2 * entry], cluster = fo.visible[2 * entry + 1] & 0xFFFFFFu;
            CHECK(instance < s.instances.size() && cluster < cd.clusters.size());
            const gpu::Cluster& c = cd.clusters[cluster];
            CHECK(tri < ((c.counts >> 8) & 0xFFu));
            const scene::Instance& inst = s.instances[instance];
            const scene::Mesh& mesh = s.meshes[inst.mesh];
            const uint32_t packed = cd.clusterTriangles[c.triangleOffset + tri];
            float2 p[3];
            float z[3];
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t mv = cd.clusterVertexIndices[c.vertexOffset + ((packed >> (8 * k)) & 0xFFu)];
                const float3 w = inst.transform.transformPoint(mesh.positions[mv]);
                const float4x4& m = fo.view.viewProj;
                const float cx = m.m[0][0] * w.x + m.m[0][1] * w.y + m.m[0][2] * w.z + m.m[0][3];
                const float cy = m.m[1][0] * w.x + m.m[1][1] * w.y + m.m[1][2] * w.z + m.m[1][3];
                const float cz = m.m[2][0] * w.x + m.m[2][1] * w.y + m.m[2][2] * w.z + m.m[2][3];
                const float cw = m.m[3][0] * w.x + m.m[3][1] * w.y + m.m[3][2] * w.z + m.m[3][3];
                p[k] = { (cx / cw * 0.5f + 0.5f) * width, (0.5f - cy / cw * 0.5f) * height };
                z[k] = cz / cw;
            }
            const float px = x + 0.5f, py = y + 0.5f;
            const float area = (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
            CHECK(std::fabs(area) > 0);
            const float b0 = ((p[1].x - px) * (p[2].y - py) - (p[2].x - px) * (p[1].y - py)) / area;
            const float b1 = ((p[2].x - px) * (p[0].y - py) - (p[0].x - px) * (p[2].y - py)) / area;
            const float b2 = 1 - b0 - b1;
            // Inside, or within 1/64 px of an edge: rasterisation snaps vertices to 1/256 px, which moves the
            // barycentrics of sub-pixel slivers far more than it moves the edge.
            auto segmentDistance = [&](float2 u, float2 v) {
                const float dx = v.x - u.x, dy = v.y - u.y, len2 = dx * dx + dy * dy;
                const float t = len2 > 0 ? std::clamp(((px - u.x) * dx + (py - u.y) * dy) / len2, 0.0f, 1.0f) : 0.0f;
                const float ex = u.x + t * dx - px, ey = u.y + t * dy - py;
                return std::sqrt(ex * ex + ey * ey);
            };
            if (b0 < 0 || b1 < 0 || b2 < 0)
            {
                const float outside = std::min({ segmentDistance(p[0], p[1]), segmentDistance(p[1], p[2]), segmentDistance(p[2], p[0]) });
                if (outside > 1.0f / 64)
                    fail("pixel (%u, %u): vis id triangle is %g px from the centre (triangle (%.3f %.3f) (%.3f %.3f) (%.3f %.3f), instance %u cluster %u tri %u)", x, y,
                         outside, p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y, instance, cluster, tri);
                ++edgeSnapped;
            }
            const float d = b0 * z[0] + b1 * z[1] + b2 * z[2];
            if (std::fabs(d - fo.depth[i]) > 1e-6f + 1e-3f * fo.depth[i]) fail("pixel (%u, %u): depth %g, triangle %g", x, y, fo.depth[i], d);
            ++checked;
        }
    logf("    %zu pixels: vis id -> (instance, cluster, triangle) covers the pixel centre at the stored depth (%zu within 1/64 px of an edge)\n", checked,
         edgeSnapped);
    CHECK(checked > 100000);
}

UNX_TEST(lod_cut_has_no_holes)
{
    const scene::Scene s = makeScene();
    // Far and high: the terrain spans many LOD levels across the screen.
    const std::vector<scene::Camera> cams(3, camera({ 40, 35, -110 }, { 40, 0, 40 }));
    const uint32_t width = 2560, height = 1440;
    const RunOut lod = renderRun(s, quality({ "visibility.occlusion_culling = false" }), cams, width, height);
    const RunOut fine = renderRun(s, quality({ "visibility.occlusion_culling = false", "visibility.lod_error_px = 0" }), cams, width, height);
    logStats("LOD 1 px ", lod.stats);
    logStats("source   ", fine.stats);
    CHECK(lod.stats.triangles[0] + lod.stats.triangles[1] + lod.stats.triangles[2] < fine.stats.triangles[0] + fine.stats.triangles[1] + fine.stats.triangles[2]);
    const auto& a = lod.frames.back().depth;
    const auto& b = fine.frames.back().depth;
    auto nearSilhouette = [&](int x, int y) {  // a sky pixel of either render within 2 px
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
            {
                const int u = std::clamp(x + dx, 0, (int)width - 1), v = std::clamp(y + dy, 0, (int)height - 1);
                if (a[(size_t)v * width + u] == 0 || b[(size_t)v * width + u] == 0) return true;
            }
        return false;
    };
    size_t holes = 0, extra = 0, covered = 0;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const size_t i = (size_t)y * width + x;
            const bool skyA = a[i] == 0, skyB = b[i] == 0;
            covered += skyB ? 0 : 1;
            if (skyA == skyB || nearSilhouette((int)x, (int)y)) continue;
            if (skyA) ++holes;
            else ++extra;
        }
    logf("    %zu covered pixels at source detail; LOD cut: %zu holes, %zu extra pixels away from silhouettes\n", covered, holes, extra);
    CHECK(holes == 0 && extra == 0);
}

UNX_TEST(raster_service)
{
    const QualityConfig q = quality();
    const scene::Scene s = makeScene();
    GpuScene gs(device());
    gs.upload(s);
    gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));
    RenderGraph graph(device());
    TrackState trackState;
    FrameResources resources;
    FrameServices services;
    services.rasterizeDepth = [](FramePassContext& c, const DepthRasterRequest& r) { tracks::rasterizeDepth(c, r); };
    ViewDesc mainView = ViewDesc::fromCamera(camera({ 40, 5, 2 }, { 40, 0, 40 }), 2560, 1440, {});
    mainView.prevViewProj = mainView.viewProj;
    // Frame constants of the main view: one 1 KB slot per frame in flight.
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 2048;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> constants;
    check(device().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&constants)), "constants");
    uint8_t* mapped = nullptr;
    check(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map");

    // Orthographic top-down view of the 80 m terrain, 1024^2: depth = (height + 10) / 30 (nearest to the sky wins).
    const uint32_t size = 1024;
    RasterView rv;
    rv.viewProj = {};
    rv.viewProj.m[1][1] = rv.viewProj.m[2][2] = 0;  // float4x4 starts as identity
    rv.viewProj.m[0][0] = 1.0f / 40;
    rv.viewProj.m[0][3] = -1;
    rv.viewProj.m[1][2] = -1.0f / 40;
    rv.viewProj.m[1][3] = 1;
    rv.viewProj.m[2][1] = 1.0f / 30;
    rv.viewProj.m[2][3] = 10.0f / 30;
    rv.viewProj.m[3][3] = 1;
    rv.viewportWidth = rv.viewportHeight = size;
    rv.lodPixelsPerMetre = size / 80.0f;
    rv.userData = 7;

    // The pixel kernel's bindless indices are root constants fixed at request time, so its targets are persistent
    // resources with their own descriptors (as S's VSM pool), imported into the graph.
    struct Uav
    {
        ComPtr<ID3D12Resource> texture;
        uint32_t index = 0;
    };
    auto persistentUav = [&](const wchar_t* name) {
        Uav u;
        D3D12_HEAP_PROPERTIES dh{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = td.Height = size;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R32_UINT;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device().d3d()->CreateCommittedResource3(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&u.texture)),
              "test uav");
        u.texture->SetName(name);
        u.index = device().descriptors().allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_UINT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device().d3d()->CreateUnorderedAccessView(u.texture.Get(), nullptr, &ud, device().descriptors().resourceCpu(u.index));
        return u;
    };
    const Uav bitsUav = persistentUav(L"test bits"), idsUav = persistentUav(L"test ids");
    ComPtr<ID3D12Resource> rbDepth = readbackBuffer((uint64_t)rowPitch(size) * size), rbBits = readbackBuffer((uint64_t)rowPitch(size) * size),
                           rbIds = readbackBuffer((uint64_t)rowPitch(size) * size), rbFine = readbackBuffer((uint64_t)rowPitch(size) * size);
    ID3D12Resource *pd = rbDepth.Get(), *pb = rbBits.Get(), *pi = rbIds.Get(), *pf = rbFine.Get();
    ID3D12PipelineState* clear = shaders().compute("Passes/Visibility/Tests/TestClear");

    // Three frames: statistics of frame 0 are read back when its slot comes round (frame 2).
    for (uint32_t f = 0; f < 3; ++f)
    {
        FrameContext frame;
        frame.frameIndex = f;
        frame.mainView = mainView;
        const gpu::FrameConstants fcData = FrameRenderer::frameConstants(gs, frame, mainView);
        std::memcpy(mapped + (f % 2) * 1024, &fcData, sizeof fcData);
        const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = constants->GetGPUVirtualAddress() + (f % 2) * 1024;
        FramePassContext fc{ device(), graph, shaders(), q, gs, frame, resources, services, [=](const ViewDesc&) { return constantsAddress; }, &trackState, 2 };
        ViewResources main;
        main.view = mainView;
        main.frameConstants = constantsAddress;
        tracks::visibility(fc, main);

        // 1. Hardware depth.
        const TextureRef depth = graph.createTexture({ "test.depth", size, size, 1, 1, DXGI_FORMAT_D32_FLOAT });
        graph.addPass("test.clear.depth", QueueType::Graphics, [&](PassBuilder& b) { b.use(depth, Use::DepthWrite); },
                      [=](PassContext& c) { c.cmd->ClearDepthStencilView(c.dsv(depth), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); });
        DepthRasterRequest hw;
        hw.name = "test.hw";
        hw.views = { rv };
        hw.depthTarget = depth;
        services.rasterizeDepth(fc, hw);
        // Same view at source detail (LOD scale far above any error): the reference for the LOD cut's deviation.
        const TextureRef fineDepth = graph.createTexture({ "test.depth.fine", size, size, 1, 1, DXGI_FORMAT_D32_FLOAT });
        graph.addPass("test.clear.depth.fine", QueueType::Graphics, [&](PassBuilder& b) { b.use(fineDepth, Use::DepthWrite); },
                      [=](PassContext& c) { c.cmd->ClearDepthStencilView(c.dsv(fineDepth), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); });
        DepthRasterRequest fine = hw;
        fine.name = "test.fine";
        fine.views[0].lodPixelsPerMetre = 1e9f;
        fine.depthTarget = fineDepth;
        services.rasterizeDepth(fc, fine);

        // 2. Pixel kernel with a tile mask: only the left half of the tiles (128 px) is requested.
        const TextureRef bits = graph.importTexture(bitsUav.texture.Get(), { "test.bits", size, size, 1, 1, DXGI_FORMAT_R32_UINT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const TextureRef ids = graph.importTexture(idsUav.texture.Get(), { "test.ids", size, size, 1, 1, DXGI_FORMAT_R32_UINT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        const BufferRef mask = graph.createBuffer({ "test.mask", 64 * 4, 0 });
        const uint32_t bitsIndex = bitsUav.index, idsIndex = idsUav.index;
        graph.addPass("test.clear.uav", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(bits, Use::UavCompute);
                          b.use(ids, Use::UavCompute);
                          b.use(mask, Use::CopyDst);
                      },
                      [=](PassContext& c) {
                          c.cmd->SetPipelineState(clear);
                          for (uint32_t index : { bitsIndex, idsIndex })
                          {
                              const uint32_t k[4] = { index, size, size, 0 };
                              c.computeConstants(k, 4);
                              c.cmd->Dispatch(size / 8, size / 8, 1);
                          }
                          // 8 x 8 tiles of 128 px (two words for the one view): columns 0-3 set in every row.
                          D3D12_WRITEBUFFERIMMEDIATE_PARAMETER words[2];
                          for (uint32_t w = 0; w < 2; ++w) words[w] = { c.address(mask) + 4 * w, 0x0F0F0F0Fu };
                          c.cmd->WriteBufferImmediate(2, words, nullptr);
                      });
        DepthRasterRequest pk;
        pk.name = "test.kernel";
        pk.views = { rv };
        pk.views[0].cullMaskOffset = 0;
        pk.pixelKernel = "Passes/Visibility/Tests/TestDepthPixel";
        pk.textureUses = { { bits, Use::UavGraphics }, { ids, Use::UavGraphics } };
        pk.cullMask = mask;
        pk.cullTilePx = 128;
        pk.pixelConstants[0] = bitsUav.index;
        pk.pixelConstants[1] = idsUav.index;
        services.rasterizeDepth(fc, pk);

        graph.addPass("test.readback", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(depth, Use::CopySrc);
                          b.use(fineDepth, Use::CopySrc);
                          b.use(bits, Use::CopySrc);
                          b.use(ids, Use::CopySrc);
                          b.keep();
                      },
                      [=](PassContext& c) {
                          copyTexture(c, depth, pd, size, size);
                          copyTexture(c, fineDepth, pf, size, size);
                          copyTexture(c, bits, pb, size, size);
                          copyTexture(c, ids, pi, size, size);
                      });
        graph.execute(nullptr);
        device().waitIdle();
    }
    constants->Unmap(0, nullptr);
    logStats("service, hardware depth", visibility::latestStats(trackState, "test.hw"));
    logStats("service, pixel kernel  ", visibility::latestStats(trackState, "test.kernel"));
    logStats("service, source detail ", visibility::latestStats(trackState, "test.fine"));
    const auto hwDepth = readTexture<float>(pd, size, size);
    const auto fineDepthCpu = readTexture<float>(pf, size, size);
    const auto kernelDepth = readTexture<uint32_t>(pb, size, size);
    const auto kernelIds = readTexture<uint32_t>(pi, size, size);

    // LOD cut against source detail: vertical deviation in units of the LOD threshold (1 px = 1 / 12.8 m here), away
    // from silhouettes (a silhouette moved by the allowed error changes the height by the step there: a sphere top
    // against the terrain). meshoptimizer's error is a quadric estimate of the distance to the source, not a pointwise
    // bound, so the deviation is reported against the threshold and bounded at twice it.
    const float pixel = 80.0f / size;
    auto silhouette = [&](int x, int y) {  // a height step above 0.5 m within 2 px in the source raster
        const float h = fineDepthCpu[(size_t)y * size + x];
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
            {
                const int u = std::clamp(x + dx, 0, (int)size - 1), v = std::clamp(y + dy, 0, (int)size - 1);
                if (std::fabs(fineDepthCpu[(size_t)v * size + u] - h) * 30 > 0.5f) return true;
            }
        return false;
    };
    size_t uncovered = 0, beyondOnePixel = 0, mismatch = 0, badIds = 0, left = 0, silhouettes = 0, terrainCompared = 0;
    float worst = 0;
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            const size_t i = (size_t)y * size + x;
            if (hwDepth[i] == 0) ++uncovered;
            // Terrain pixels (instance 0, known in the pixel kernel's half) away from silhouettes: the LOD error is a
            // 3D distance, so a vertical difference is compared with it through the source slope s: e sqrt(1 + s^2).
            const bool terrainPixel = x < size / 2 && (kernelIds[i] & 0xFFFFFFu) == 0;
            if (silhouette((int)x, (int)y)) ++silhouettes;
            else if (terrainPixel && x > 0 && y > 0 && x + 1 < size && y + 1 < size)
            {
                const float sx = (fineDepthCpu[i + 1] - fineDepthCpu[i - 1]) * 30 / (2 * pixel);
                const float sz = (fineDepthCpu[i + size] - fineDepthCpu[i - size]) * 30 / (2 * pixel);
                const float deviation = std::fabs(hwDepth[i] - fineDepthCpu[i]) * 30 / std::sqrt(1 + sx * sx + sz * sz);  // metres along the normal
                worst = std::max(worst, deviation);
                if (deviation > pixel) ++beyondOnePixel;
                ++terrainCompared;
            }
            if (x < size / 2)
            {
                ++left;
                float kd;
                std::memcpy(&kd, &kernelDepth[i], 4);
                if (kd != hwDepth[i]) ++mismatch;
                if ((kernelIds[i] >> 24) != 7 || (kernelIds[i] & 0xFFFFFFu) >= s.instances.size()) ++badIds;
            }
        }
    logf("    hardware depth: %zu uncovered; LOD vs source on %zu terrain pixels (normal distance, %zu silhouette pixels excluded): worst %.3f m = %.2f LOD px, "
         "%zu beyond 1 LOD px; "
         "pixel kernel (masked left half, %zu px): "
         "%zu depth mismatches, %zu bad instance/userData\n",
         uncovered, terrainCompared, silhouettes, worst, worst / pixel, beyondOnePixel, left, mismatch, badIds);
    CHECK(uncovered == 0 && terrainCompared > 100000 && worst <= 2 * pixel && mismatch == 0 && badIds == 0);
    device().descriptors().freeResource(bitsUav.index);
    device().descriptors().freeResource(idsUav.index);
    device().deferRelease(bitsUav.texture);
    device().deferRelease(idsUav.texture);
    device().deferRelease(constants);
    device().deferRelease(rbDepth);
    device().deferRelease(rbBits);
    device().deferRelease(rbIds);
    device().deferRelease(rbFine);
}

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int failed = 0, run = 0;
    for (const TestCase& t : registry())
    {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++run;
        try
        {
            t.fn();
            logf("PASS %s\n", t.name);
        }
        catch (const std::exception& e)
        {
            ++failed;
            logf("FAIL %s: %s\n", t.name, e.what());
        }
    }
    device().waitIdle();
    const uint32_t debugErrors = device().drainDebugMessages();
    logf("%d/%d passed, D3D12 debug-layer errors: %u\n", run - failed, run, debugErrors);
    return failed == 0 && debugErrors == 0 ? 0 : 1;
}
