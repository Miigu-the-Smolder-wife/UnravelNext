// V correctness on the real device with the debug layer (no GPU lock; INTERFACES 3.3):
//   occlusion_is_conservative      two-phase HiZ culling over a moving camera gives bit-identical depth to no culling
//   vis_id_decodes_to_the_covering_triangle   every vis id names a triangle that covers the pixel centre at its depth
//   lod_cut_has_no_holes           the DAG cut covers every pixel the source geometry covers (away from silhouettes)
//   raster_service                 rasterizeDepth: hardware depth and a requester pixel kernel (DepthRasterPixel),
//                                  orthographic LOD, tile mask, tile-local raster (same fragments inside set tiles,
//                                  none outside)
//   raster_service_tile_atlas      tile atlas (DepthRasterRequest::atlasSlots): each set tile's slot of a D32 atlas holds
//                                  the tile-local raster of that tile up to the rasteriser's vertex snap, a D16 atlas the
//                                  D32 one to one step, unused slots stay cleared
//   planar_mask_draws_only_mirror_pixels   ViewDesc::planarMask: mirror pixels bit-identical to the unmasked view, the
//                                  others VIS_NONE at the nearest depth, clusters over mirror-free tiles culled
//   coverage_layer_is_exact        band B/C coverage layer: every fragment's area, depth and 32-subsample mask match the
//                                  exact clip of its (near-clipped) triangle by the pixel, no fragment missing or extra
//                                  (one-sided back faces culled, band A occlusion by the HiZ rule), fragments sorted
//                                  nearest first, pixel list and heads consistent, last frame's heads cleared
//   coverage_mask_lut_matches_exact   coverageTriangleMaskLut (64 x 64 table of conservative edge masks + exact tests of
//                                  the open subsamples) gives coverageTriangleMask's bits for random triangles
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
#include <intrin.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
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
    // These geometric oracles and readback footprints use the requested raster
    // size. Temporal upscaling changes both that size and its projection.
    q.applyOverride("output.render_scale=1");
    q.applyOverride("output.render_height_max=0");
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

ComPtr<ID3D12Resource> uploadWords(const std::vector<uint32_t>& words)
{
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = words.size() * sizeof(uint32_t);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> resource;
    check(device().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED,
          nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&resource)), "test word upload");
    void* mapped = nullptr;
    const D3D12_RANGE none{ 0, 0 };
    check(resource->Map(0, &none, &mapped), "map test word upload");
    std::memcpy(mapped, words.data(), (size_t)rd.Width);
    resource->Unmap(0, nullptr);
    return resource;
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
        CHECK(main.view.width == width && main.view.height == height);
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
        fo.view = main.view;
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

UNX_TEST(depth_ties_choose_stable_primitive)
{
    scene::Scene scene = makeScene();
    const auto originals = scene.instances;
    scene.instances.insert(scene.instances.end(), originals.begin(), originals.end());
    const auto cams = movingCameras(3);
    const auto q = quality({ "debug.deterministic = true", "visibility.occlusion_culling = false" });
    const auto a = renderRun(scene, q, cams, 320, 180);
    const auto b = renderRun(scene, q, cams, 320, 180);
    size_t checked = 0;
    for (size_t frame = 0; frame < a.frames.size(); ++frame)
    {
        const auto& x = a.frames[frame];
        const auto& y = b.frames[frame];
        CHECK(std::memcmp(x.depth.data(), y.depth.data(), x.depth.size() * sizeof(float)) == 0);
        for (size_t pixel = 0; pixel < x.visId.size(); ++pixel)
        {
            const uint32_t ix = x.visId[pixel], iy = y.visId[pixel];
            if (ix == kVisNone) { CHECK(iy == kVisNone); continue; }
            CHECK(iy != kVisNone);
            const uint32_t ex = (ix - 1) >> 7, ey = (iy - 1) >> 7;
            // Every original has an exactly coincident copy with a larger id.
            CHECK(x.visible[ex * 2] < originals.size());
            CHECK(x.visible[ex * 2] == y.visible[ey * 2]);
            CHECK(x.visible[ex * 2 + 1] == y.visible[ey * 2 + 1]);
            CHECK(((ix - 1) & 127) == ((iy - 1) & 127));
            ++checked;
        }
    }
    CHECK(checked > 100);
    CHECK(device().drainDebugMessages() == 0);
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
    // Sparse irregular mask (runs, full rows, empty rows), drawn twice: whole-view raster and tile-local raster.
    const Uav sparseBits[2] = { persistentUav(L"test sparse bits"), persistentUav(L"test local bits") };
    const Uav sparseIds[2] = { persistentUav(L"test sparse ids"), persistentUav(L"test local ids") };
    const Uav sparseCount[2] = { persistentUav(L"test sparse fragments"), persistentUav(L"test local fragments") };
    // Order-independent ids of the two sparse runs (TestDepthPixel KEY=1): uint64 depth << 32 | id per pixel, cleared by
    // copying from a buffer that is never written (committed buffers start zeroed).
    struct RawUav
    {
        ComPtr<ID3D12Resource> buffer;
        uint32_t index = 0;
    };
    const uint64_t keyBytes = (uint64_t)size * size * 8;
    auto persistentRaw = [&](const wchar_t* name, bool uav) {
        RawUav u;
        D3D12_HEAP_PROPERTIES dh{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = keyBytes;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        check(device().d3d()->CreateCommittedResource3(&dh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&u.buffer)),
              "test raw");
        u.buffer->SetName(name);
        if (!uav) return u;
        u.index = device().descriptors().allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = (UINT)(keyBytes / 4);
        ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device().d3d()->CreateUnorderedAccessView(u.buffer.Get(), nullptr, &ud, device().descriptors().resourceCpu(u.index));
        return u;
    };
    const RawUav sparseKeys[2] = { persistentRaw(L"test sparse keys", true), persistentRaw(L"test local keys", true) };
    const RawUav zeroKeys = persistentRaw(L"test zero keys", false);
    ComPtr<ID3D12Resource> rbKeys[2] = { readbackBuffer(keyBytes), readbackBuffer(keyBytes) };
    const uint8_t sparseRows[8] = { 0x36, 0xFF, 0x00, 0x81, 0x5A, 0xE7, 0x18, 0xF0 };  // bit x = tile column x
    auto sparseTile = [&](uint32_t x, uint32_t y) { return (sparseRows[y / 128] >> (x / 128)) & 1; };
    const uint32_t sparseWords[2] = { sparseRows[0] | sparseRows[1] << 8 | sparseRows[2] << 16 | (uint32_t)sparseRows[3] << 24,
                                      sparseRows[4] | sparseRows[5] << 8 | sparseRows[6] << 16 | (uint32_t)sparseRows[7] << 24 };
    const auto maskUpload = uploadWords({ 0x0F0F0F0Fu, 0x0F0F0F0Fu, sparseWords[0], sparseWords[1] });
    ComPtr<ID3D12Resource> rbDepth = readbackBuffer((uint64_t)rowPitch(size) * size), rbBits = readbackBuffer((uint64_t)rowPitch(size) * size),
                           rbIds = readbackBuffer((uint64_t)rowPitch(size) * size), rbFine = readbackBuffer((uint64_t)rowPitch(size) * size);
    ID3D12Resource *pd = rbDepth.Get(), *pb = rbBits.Get(), *pi = rbIds.Get(), *pf = rbFine.Get();
    ComPtr<ID3D12Resource> rbSparse[2][3];
    for (auto& run : rbSparse)
        for (auto& t : run) t = readbackBuffer((uint64_t)rowPitch(size) * size);
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
                          // Explicit destination identity avoids WARNING 926 on aliased transient GPU VAs.
                          c.cmd->CopyBufferRegion(c.resource(mask), 0, maskUpload.Get(), 0, 8);
                      });
        DepthRasterRequest pk;
        pk.name = "test.kernel";
        pk.views = { rv };
        pk.views[0].cullMaskOffset = 0;
        pk.pixelKernel = "Passes/Visibility/Tests/TestDepthPixel.KEY0";
        pk.textureUses = { { bits, Use::UavGraphics }, { ids, Use::UavGraphics } };
        pk.cullMask = mask;
        pk.cullTilePx = 128;
        pk.pixelConstants[0] = bitsUav.index;
        pk.pixelConstants[1] = idsUav.index;
        pk.pixelConstants[2] = gpu::kNone;
        services.rasterizeDepth(fc, pk);

        // 3. Sparse mask: whole-view raster (run 0) and tile-local raster (run 1), both counting fragments.
        const BufferRef sparseMask = graph.createBuffer({ "test.mask.sparse", 64 * 4, 0 });
        TextureRef sparse[2][3];
        for (uint32_t k = 0; k < 2; ++k)
        {
            const Uav* uavs[3] = { &sparseBits[k], &sparseIds[k], &sparseCount[k] };
            for (uint32_t t = 0; t < 3; ++t)
                sparse[k][t] = graph.importTexture(uavs[t]->texture.Get(), { "test.sparse", size, size, 1, 1, DXGI_FORMAT_R32_UINT }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        }
        const uint32_t sparseIndices[6] = { sparseBits[0].index, sparseIds[0].index, sparseCount[0].index, sparseBits[1].index, sparseIds[1].index, sparseCount[1].index };
        const BufferRef keys[2] = { graph.importBuffer(sparseKeys[0].buffer.Get(), { "test.keys.sparse", keyBytes, 0 }),
                                    graph.importBuffer(sparseKeys[1].buffer.Get(), { "test.keys.local", keyBytes, 0 }) };
        ID3D12Resource* zero = zeroKeys.buffer.Get();
        graph.addPass("test.clear.sparse", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          for (auto& run : sparse)
                              for (TextureRef t : run) b.use(t, Use::UavCompute);
                          b.use(sparseMask, Use::CopyDst);
                          for (BufferRef k : keys) b.use(k, Use::CopyDst);
                      },
                      [=](PassContext& c) {
                          for (BufferRef k : keys) c.cmd->CopyBufferRegion(c.resource(k), 0, zero, 0, keyBytes);
                          c.cmd->SetPipelineState(clear);
                          for (uint32_t index : sparseIndices)
                          {
                              const uint32_t k[4] = { index, size, size, 0 };
                              c.computeConstants(k, 4);
                              c.cmd->Dispatch(size / 8, size / 8, 1);
                          }
                          c.cmd->CopyBufferRegion(c.resource(sparseMask), 0, maskUpload.Get(), 8, 8);
                      });
        for (uint32_t k = 0; k < 2; ++k)
        {
            DepthRasterRequest sp = pk;
            sp.name = k == 0 ? "test.sparse" : "test.local";
            sp.pixelKernel = "Passes/Visibility/Tests/TestDepthPixel.KEY1";
            sp.textureUses = { { sparse[k][0], Use::UavGraphics }, { sparse[k][2], Use::UavGraphics } };
            sp.bufferUses = { { keys[k], Use::UavGraphics } };
            sp.cullMask = sparseMask;
            sp.tileLocal = k == 1;
            sp.pixelConstants[0] = sparseIndices[3 * k];
            sp.pixelConstants[1] = sparseKeys[k].index;
            sp.pixelConstants[2] = sparseIndices[3 * k + 2];
            sp.pixelConstants[3] = size;
            services.rasterizeDepth(fc, sp);
        }
        ID3D12Resource* rs[2][3] = { { rbSparse[0][0].Get(), rbSparse[0][1].Get(), rbSparse[0][2].Get() }, { rbSparse[1][0].Get(), rbSparse[1][1].Get(), rbSparse[1][2].Get() } };
        ID3D12Resource* rk[2] = { rbKeys[0].Get(), rbKeys[1].Get() };

        graph.addPass("test.readback", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(depth, Use::CopySrc);
                          b.use(fineDepth, Use::CopySrc);
                          b.use(bits, Use::CopySrc);
                          b.use(ids, Use::CopySrc);
                          for (auto& run : sparse)
                              for (TextureRef t : run) b.use(t, Use::CopySrc);
                          for (BufferRef k : keys) b.use(k, Use::CopySrc);
                          b.keep();
                      },
                      [=](PassContext& c) {
                          copyTexture(c, depth, pd, size, size);
                          copyTexture(c, fineDepth, pf, size, size);
                          copyTexture(c, bits, pb, size, size);
                          copyTexture(c, ids, pi, size, size);
                          for (uint32_t k = 0; k < 2; ++k)
                              for (uint32_t t = 0; t < 3; ++t) copyTexture(c, sparse[k][t], rs[k][t], size, size);
                          for (uint32_t k = 0; k < 2; ++k) c.cmd->CopyBufferRegion(rk[k], 0, c.resource(keys[k]), 0, keyBytes);
                      });
        graph.execute(nullptr);
        device().waitIdle();
    }
    constants->Unmap(0, nullptr);
    logStats("service, hardware depth", visibility::latestStats(trackState, "test.hw"));
    logStats("service, pixel kernel  ", visibility::latestStats(trackState, "test.kernel"));
    logStats("service, source detail ", visibility::latestStats(trackState, "test.fine"));
    logStats("service, sparse mask   ", visibility::latestStats(trackState, "test.sparse"));
    logStats("service, tile-local    ", visibility::latestStats(trackState, "test.local"));
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

    // Tile-local raster: inside set tiles the same fragments per pixel as the whole-view raster, outside set tiles no
    // fragment at all. Depth: the hardware clips triangles at the tile edges and snaps the new vertices to its 1/256 px
    // grid, so a clipped triangle's depth plane differs from the whole triangle's by that snapping. Checked as an
    // equivalent lateral displacement (depth difference / local depth slope) of at most 1/64 px; ids may differ only
    // where the depth differs (the ids come from the order-independent depth << 32 | id keys; equal depth from coincident
    // surfaces resolves to the larger id in both runs).
    std::vector<uint32_t> sp[2][3];
    for (uint32_t k = 0; k < 2; ++k)
    {
        for (uint32_t t = 0; t < 3; ++t) sp[k][t] = readTexture<uint32_t>(rbSparse[k][t].Get(), size, size);
        const uint64_t* key = nullptr;
        check(rbKeys[k]->Map(0, nullptr, (void**)&key), "map keys");
        for (size_t i = 0; i < (size_t)size * size; ++i) sp[k][1][i] = (uint32_t)key[i];
        rbKeys[k]->Unmap(0, nullptr);
    }
    uint64_t fragmentsInside[2] = {}, fragmentsOutside[2] = {};
    size_t countDiff = 0, depthDiff = 0, idDiff = 0, hwDiff = 0, setPixels = 0, outsideWritten = 0;
    uint32_t worstUlps = 0;
    float worstDepth = 0;
    float worstSnapRatio = 0;
    size_t beyondSnap = 0;
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            const size_t i = (size_t)y * size + x;
            const bool set = sparseTile(x, y) != 0;
            for (uint32_t k = 0; k < 2; ++k) (set ? fragmentsInside[k] : fragmentsOutside[k]) += sp[k][2][i];
            if (set)
            {
                ++setPixels;
                if (sp[0][2][i] != sp[1][2][i]) ++countDiff;
                if (sp[0][0][i] != sp[1][0][i]) ++depthDiff;
                if (sp[0][0][i] && sp[1][0][i])
                {
                    const uint32_t ulps = sp[0][0][i] > sp[1][0][i] ? sp[0][0][i] - sp[1][0][i] : sp[1][0][i] - sp[0][0][i];
                    float a, b;
                    std::memcpy(&a, &sp[0][0][i], 4);
                    std::memcpy(&b, &sp[1][0][i], 4);
                    worstUlps = std::max(worstUlps, ulps);
                    worstDepth = std::max(worstDepth, std::fabs(a - b));
                    // Against the rasteriser's vertex snapping (1/256 px): the local depth slope per pixel on the smooth
                    // side of each axis (forward or backward difference, whichever is smaller) times 1/256.
                    auto depthAt = [&](int u, int v) {
                        float d;
                        std::memcpy(&d, &sp[0][0][(size_t)std::clamp(v, 0, (int)size - 1) * size + std::clamp(u, 0, (int)size - 1)], 4);
                        return d;
                    };
                    const int xi = (int)x, yi = (int)y;
                    const float gx = std::min(std::fabs(depthAt(xi + 1, yi) - a), std::fabs(a - depthAt(xi - 1, yi)));
                    const float gy = std::min(std::fabs(depthAt(xi, yi + 1) - a), std::fabs(a - depthAt(xi, yi - 1)));
                    const float snap = (gx + gy) / 256 + 4 * std::max(std::nextafter(a, 2.0f) - a, 1e-12f);
                    worstSnapRatio = std::max(worstSnapRatio, std::fabs(a - b) / snap);
                    if (std::fabs(a - b) > 4 * snap) ++beyondSnap;
                }
                if (sp[0][1][i] != sp[1][1][i] && sp[0][0][i] == sp[1][0][i]) ++idDiff;
                float kd;
                std::memcpy(&kd, &sp[0][0][i], 4);
                if (kd != hwDepth[i]) ++hwDiff;
            }
            else if (sp[1][0][i] != 0 || sp[1][2][i] != 0)
                ++outsideWritten;
        }
    const visibility::Stats local = visibility::latestStats(trackState, "test.local");
    logf("    tile-local vs whole-view raster, sparse mask (%zu px in set tiles): fragments inside %llu vs %llu, outside %llu vs %llu; "
         "per-pixel differences in set tiles: fragments %zu, depth %zu, ids at equal depth %zu, whole-view vs hardware depth %zu; pixels written outside %zu; %u pairs; depth difference worst %u ulp = %.3g = %.4f px lateral (%zu px beyond 1/64 px)\n",
         setPixels, (unsigned long long)fragmentsInside[1], (unsigned long long)fragmentsInside[0], (unsigned long long)fragmentsOutside[1],
         (unsigned long long)fragmentsOutside[0], countDiff, depthDiff, idDiff, hwDiff, outsideWritten, local.tilePairs, worstUlps, worstDepth, worstSnapRatio / 256, beyondSnap);
    CHECK(fragmentsInside[0] > 0 && fragmentsOutside[0] > 0 && fragmentsOutside[1] == 0 && outsideWritten == 0 && countDiff == 0 && beyondSnap == 0 && idDiff == 0 &&
          hwDiff == 0 && local.tilePairs > 0 && local.overflow == 0);
    for (uint32_t k = 0; k < 2; ++k)
        for (const Uav* u : { &sparseBits[k], &sparseIds[k], &sparseCount[k] })
        {
            device().descriptors().freeResource(u->index);
            device().deferRelease(u->texture);
        }
    for (auto& run : rbSparse)
        for (auto& t : run) device().deferRelease(t);
    for (uint32_t k = 0; k < 2; ++k)
    {
        device().descriptors().freeResource(sparseKeys[k].index);
        device().deferRelease(sparseKeys[k].buffer);
        device().deferRelease(rbKeys[k]);
    }
    device().deferRelease(zeroKeys.buffer);
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

UNX_TEST(raster_service_tile_atlas)
{
    // S's VSM depth atlas (v1.32, request 20260925_S_vsm_depth_atlas.md): the sparse mask of raster_service over the
    // orthographic 1024^2 terrain view, drawn tile-local into a view-sized D32 target (the reference) and in atlas mode
    // into D32 and D16 atlases of 3 x 11 slots of 128 px, the 30 set tiles in scrambled slots and 3 slots unused.
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
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 1024;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> constants;
    check(device().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&constants)), "constants");
    uint8_t* mapped = nullptr;
    check(constants->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map");

    const uint32_t size = 1024, tilePx = 128, tiles = size / tilePx;
    RasterView rv;
    rv.viewProj = {};
    rv.viewProj.m[1][1] = rv.viewProj.m[2][2] = 0;
    rv.viewProj.m[0][0] = 1.0f / 40;
    rv.viewProj.m[0][3] = -1;
    rv.viewProj.m[1][2] = -1.0f / 40;
    rv.viewProj.m[1][3] = 1;
    rv.viewProj.m[2][1] = 1.0f / 30;
    rv.viewProj.m[2][3] = 10.0f / 30;
    rv.viewProj.m[3][3] = 1;
    rv.viewportWidth = rv.viewportHeight = size;
    rv.lodPixelsPerMetre = size / 80.0f;
    rv.cullMaskOffset = 0;

    const uint8_t rows[8] = { 0x36, 0xFF, 0x00, 0x81, 0x5A, 0xE7, 0x18, 0xF0 };  // bit x = tile column x
    const uint32_t words[2] = { rows[0] | rows[1] << 8 | rows[2] << 16 | (uint32_t)rows[3] << 24, rows[4] | rows[5] << 8 | rows[6] << 16 | (uint32_t)rows[7] << 24 };
    const uint32_t perRow = 3, slotCount = 33;
    std::vector<uint32_t> slotOf(tiles * tiles, UINT32_MAX);
    std::vector<bool> slotUsed(slotCount, false);
    uint32_t set = 0;
    for (uint32_t t = 0; t < tiles * tiles; ++t)
        if ((rows[t / tiles] >> (t % tiles)) & 1)
        {
            slotOf[t] = (set * 7 + 3) % slotCount;  // 7 is prime to 33: distinct slots
            slotUsed[slotOf[t]] = true;
            ++set;
        }
    CHECK(set == 30);
    std::vector<uint32_t> maskAndSlots{ words[0], words[1] };
    maskAndSlots.insert(maskAndSlots.end(), slotOf.begin(), slotOf.end());
    const auto maskUpload = uploadWords(maskAndSlots);
    const uint32_t atlasW = perRow * tilePx, atlasH = (slotCount + perRow - 1) / perRow * tilePx;

    ComPtr<ID3D12Resource> rbRef = readbackBuffer((uint64_t)rowPitch(size) * size), rb32 = readbackBuffer((uint64_t)rowPitch(atlasW) * atlasH),
                           rb16 = readbackBuffer((uint64_t)rowPitch(atlasW) * atlasH);
    ID3D12Resource *pr = rbRef.Get(), *p32 = rb32.Get(), *p16 = rb16.Get();
    {
        FrameContext frame;
        frame.frameIndex = 0;
        frame.mainView = mainView;
        const gpu::FrameConstants fcData = FrameRenderer::frameConstants(gs, frame, mainView);
        std::memcpy(mapped, &fcData, sizeof fcData);
        const D3D12_GPU_VIRTUAL_ADDRESS constantsAddress = constants->GetGPUVirtualAddress();
        FramePassContext fc{ device(), graph, shaders(), q, gs, frame, resources, services, [=](const ViewDesc&) { return constantsAddress; }, &trackState, 2 };
        ViewResources main;
        main.view = mainView;
        main.frameConstants = constantsAddress;
        tracks::visibility(fc, main);

        const TextureRef ref = graph.createTexture({ "test.atlas.ref", size, size, 1, 1, DXGI_FORMAT_D32_FLOAT });
        const TextureRef atlas32 = graph.createTexture({ "test.atlas.d32", atlasW, atlasH, 1, 1, DXGI_FORMAT_D32_FLOAT });
        const TextureRef atlas16 = graph.createTexture({ "test.atlas.d16", atlasW, atlasH, 1, 1, DXGI_FORMAT_D16_UNORM });
        const BufferRef mask = graph.createBuffer({ "test.atlas.mask", 64 * 4, 0 });
        const BufferRef slots = graph.createBuffer({ "test.atlas.slots", tiles * tiles * 4, 0 });
        graph.addPass("test.atlas.clear", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(ref, Use::DepthWrite);
                          b.use(atlas32, Use::DepthWrite);
                          b.use(atlas16, Use::DepthWrite);
                          b.use(mask, Use::CopyDst);
                          b.use(slots, Use::CopyDst);
                      },
                      [=](PassContext& c) {
                          for (TextureRef t : { ref, atlas32, atlas16 }) c.cmd->ClearDepthStencilView(c.dsv(t), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                          c.cmd->CopyBufferRegion(c.resource(mask), 0, maskUpload.Get(), 0, 8);
                          c.cmd->CopyBufferRegion(c.resource(slots), 0, maskUpload.Get(), 8, 64 * 4);
                      });
        DepthRasterRequest local;
        local.name = "test.atlas.local";
        local.views = { rv };
        local.depthTarget = ref;
        local.cullMask = mask;
        local.cullTilePx = tilePx;
        local.tileLocal = true;
        services.rasterizeDepth(fc, local);
        for (uint32_t k = 0; k < 2; ++k)
        {
            DepthRasterRequest at = local;
            at.name = k == 0 ? "test.atlas.d32" : "test.atlas.d16";
            at.depthTarget = k == 0 ? atlas32 : atlas16;
            at.atlasSlots = slots;
            at.atlasTilesPerRow = perRow;
            services.rasterizeDepth(fc, at);
        }
        graph.addPass("test.atlas.readback", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(ref, Use::CopySrc);
                          b.use(atlas32, Use::CopySrc);
                          b.use(atlas16, Use::CopySrc);
                          b.keep();
                      },
                      [=](PassContext& c) {
                          copyTexture(c, ref, pr, size, size);
                          copyTexture(c, atlas32, p32, atlasW, atlasH);
                          copyTexture(c, atlas16, p16, atlasW, atlasH);
                      });
        graph.execute(nullptr);
        device().waitIdle();
    }
    constants->Unmap(0, nullptr);
    const auto refDepth = readTexture<float>(pr, size, size);
    const auto a32 = readTexture<float>(p32, atlasW, atlasH);
    std::vector<uint16_t> a16((size_t)atlasW * atlasH);
    {
        const uint8_t* m = nullptr;
        check(rb16->Map(0, nullptr, (void**)&m), "map");
        for (uint32_t y = 0; y < atlasH; ++y) std::memcpy(&a16[(size_t)y * atlasW], m + (size_t)y * rowPitch(atlasW), atlasW * 2);
        rb16->Unmap(0, nullptr);
    }

    // Set tiles: the slot holds the same geometry rasterised at pixel coordinates shifted by whole pixels. The
    // rasteriser snaps vertices to 1/256 px of the float coordinate it gets, and the float of p + k is not the float of
    // p shifted, so a vertex may land one snap step away: a texel then differs from the reference by at most its local
    // depth gradient G (largest 3 x 3 neighbour step, per pixel) x 2/256 px. Where an edge between different surfaces
    // passes within that distance of the pixel centre the texel takes the other side's depth, a value of its 3 x 3
    // neighbourhood ("edge flip"); those are counted and must stay rare. The D16 slot is the D32 slot to one step (round
    // to nearest; depth ties decided at 16 bits).
    size_t texels = 0, exact = 0, withinSnap = 0, edgeFlips = 0, bad = 0, covered = 0, bad16 = 0;
    float worst16 = 0, worstSnap = 0;  // worstSnap: |d - r| / (G / 256) over the texels within the snap bound
    for (uint32_t t = 0; t < tiles * tiles; ++t)
    {
        if (slotOf[t] == UINT32_MAX) continue;
        const uint32_t tx = t % tiles * tilePx, ty = t / tiles * tilePx;
        const uint32_t sx = slotOf[t] % perRow * tilePx, sy = slotOf[t] / perRow * tilePx;
        for (uint32_t y = 0; y < tilePx; ++y)
            for (uint32_t x = 0; x < tilePx; ++x)
            {
                const float r = refDepth[(size_t)(ty + y) * size + tx + x];
                const float d = a32[(size_t)(sy + y) * atlasW + sx + x];
                ++texels;
                if (r > 0) ++covered;
                if (d == r) ++exact;
                else
                {
                    float gradient = 0;
                    bool neighbour = false;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                        {
                            const int u = (int)(tx + x) + dx, v = (int)(ty + y) + dy;
                            if (u < 0 || v < 0 || u >= (int)size || v >= (int)size) continue;
                            const float n = refDepth[(size_t)v * size + u];
                            gradient = std::max(gradient, std::fabs(n - r));
                            neighbour = neighbour || std::fabs(n - d) <= 1e-6f;
                        }
                    const float e = std::fabs(d - r);
                    if (e <= gradient * (2.0f / 256) + 1e-7f)
                    {
                        ++withinSnap;
                        if (gradient > 0) worstSnap = std::max(worstSnap, e / (gradient / 256));
                    }
                    else if (neighbour)
                        ++edgeFlips;
                    else
                        ++bad;
                }
                const float e16 = std::fabs(a16[(size_t)(sy + y) * atlasW + sx + x] / 65535.0f - d);
                worst16 = std::max(worst16, e16);
                if (e16 > 1.0f / 65535 + 1e-7f) ++bad16;
            }
    }
    // Unused slots: no fragment (the clear value 0) in either atlas.
    size_t leaked = 0;
    for (uint32_t slot = 0; slot < slotCount; ++slot)
    {
        if (slotUsed[slot]) continue;
        const uint32_t sx = slot % perRow * tilePx, sy = slot / perRow * tilePx;
        for (uint32_t y = 0; y < tilePx; ++y)
            for (uint32_t x = 0; x < tilePx; ++x)
                if (a32[(size_t)(sy + y) * atlasW + sx + x] != 0 || a16[(size_t)(sy + y) * atlasW + sx + x] != 0) ++leaked;
    }
    logf("    %zu texels in %u set tiles (%zu covered): %zu exact, %zu within the snap bound (worst %.2f snap steps x G), %zu edge flips, %zu other; "
         "D16 worst %.3g (1 step %.3g), %zu beyond; %zu texels written in unused slots\n",
         texels, set, covered, exact, withinSnap, worstSnap, edgeFlips, bad, worst16, 1.0 / 65535, bad16, leaked);
    CHECK(covered == texels && bad == 0 && edgeFlips * 1000 <= texels && bad16 == 0 && leaked == 0);
}

UNX_TEST(planar_mask_draws_only_mirror_pixels)
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
    const uint32_t width = 1280, height = 720;
    ViewDesc view = ViewDesc::fromCamera(camera({ 38, 4.5f, 2 }, { 42, 2, 40 }), width, height, {});
    view.prevViewProj = view.viewProj;
    view.kind = gpu::ViewKind::PlanarReflection;

    // Mask in the left third only (a mirror's rectangle is mostly not mirror): 16 x 16 blocks in a checkerboard (whole
    // tiles), every 37th pixel of the empty blocks (single mirror pixels in otherwise empty tiles), one empty row of
    // blocks; the rest of the view has no mirror pixel, so clusters there are culled.
    std::vector<uint8_t> mask((size_t)width * height, 0);
    size_t mirror = 0;
    for (uint32_t y = 0; y < height; ++y)
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint32_t bx = x / 16, by = y / 16;
            const bool region = x < width / 3 && by != 20;
            const bool block = region && ((bx + by) & 1) == 0;
            const bool single = region && !block && ((size_t)y * width + x) % 37 == 0;
            mask[(size_t)y * width + x] = (block || single) ? 1 : 0;
            mirror += mask[(size_t)y * width + x];
        }
    ComPtr<ID3D12Resource> constants, maskTexture, maskUpload;
    {
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD }, dh{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 1024;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device().d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&constants)),
              "constants");
        rd.Width = (uint64_t)width * height;  // row pitch = width (multiple of 256)
        check(device().d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&maskUpload)),
              "mask upload");
        uint8_t* p = nullptr;
        check(maskUpload->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map mask");
        std::memcpy(p, mask.data(), mask.size());
        maskUpload->Unmap(0, nullptr);
        D3D12_RESOURCE_DESC1 td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = width;
        td.Height = height;
        td.DepthOrArraySize = td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8_UINT;
        td.SampleDesc.Count = 1;
        check(device().d3d()->CreateCommittedResource3(&dh, D3D12_HEAP_FLAG_NONE, &td, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&maskTexture)),
              "mask");
    }
    CHECK(width % 256 == 0);
    const uint64_t capacity = (uint64_t)q.integer("visibility.max_visible_clusters");
    ComPtr<ID3D12Resource> depthRb = readbackBuffer((uint64_t)rowPitch(width) * height), visRb = readbackBuffer((uint64_t)rowPitch(width) * height),
                           listRb = readbackBuffer(capacity * 8);
    std::vector<float> depth[2];
    std::vector<uint32_t> vis[2], visible[2];
    visibility::Stats stats[2];
    for (uint32_t f = 0; f < 4; ++f)
    {
        const bool masked = (f & 1) != 0;
        FrameContext frame;
        frame.frameIndex = f;
        frame.mainView = view;
        const gpu::FrameConstants fcData = FrameRenderer::frameConstants(gs, frame, view);
        const uint32_t slot = f % 2;
        uint8_t* cp = nullptr;
        check(constants->Map(0, nullptr, reinterpret_cast<void**>(&cp)), "map constants");
        std::memcpy(cp + 512 * slot, &fcData, sizeof fcData);
        constants->Unmap(0, nullptr);
        const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress() + 512 * slot;
        FramePassContext fc{ device(), graph, shaders(), q, gs, frame, resources, services, [=](const ViewDesc&) { return address; }, &trackState, 2 };
        ViewResources vr;
        vr.view = view;
        vr.frameConstants = address;
        if (masked)
        {
            const TextureRef m = graph.importTexture(maskTexture.Get(), { "test planar mask", width, height, 1, 1, DXGI_FORMAT_R8_UINT }, D3D12_BARRIER_LAYOUT_COMMON);
            ID3D12Resource* src = maskUpload.Get();
            graph.addPass("test.mask.upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(m, Use::CopyDst); },
                          [=](PassContext& c) {
                              D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                              to.pResource = c.resource(m);
                              to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                              from.pResource = src;
                              from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                              from.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UINT, width, height, 1, width };
                              c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                          });
            vr.view.planarMask = m;
        }
        tracks::visibility(fc, vr);
        ID3D12Resource *d = depthRb.Get(), *v = visRb.Get(), *l = listRb.Get();
        graph.addPass("test.readback", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(vr.depth, Use::CopySrc);
                          b.use(vr.visId, Use::CopySrc);
                          b.use(vr.visibleClusters, Use::CopySrc);
                          b.keep();
                      },
                      [=](PassContext& c) {
                          copyTexture(c, vr.depth, d, width, height);
                          copyTexture(c, vr.visId, v, width, height);
                          c.cmd->CopyBufferRegion(l, 0, c.resource(vr.visibleClusters), 0, capacity * 8);
                      });
        graph.execute(nullptr);
        device().waitIdle();
        if (f < 2)
        {
            depth[f] = readTexture<float>(d, width, height);
            vis[f] = readTexture<uint32_t>(v, width, height);
            visible[f].resize(capacity * 2);
            uint8_t* lp = nullptr;
            check(l->Map(0, nullptr, reinterpret_cast<void**>(&lp)), "map list");
            std::memcpy(visible[f].data(), lp, capacity * 8);
            l->Unmap(0, nullptr);
        }
        else
            stats[f - 2] = visibility::latestStats(trackState, "secondary");  // frame f - 2 (two slots)
    }
    // Vis ids index each frame's visible-cluster list, whose order is not deterministic: compare what they name.
    auto names = [&](int f, uint32_t id) -> uint64_t {
        if (id == kVisNone) return ~0ull;
        const uint32_t entry = (id - 1) >> 7, tri = (id - 1) & 127;
        return (uint64_t)visible[f][2 * entry] << 40 | (uint64_t)(visible[f][2 * entry + 1] & 0xFFFFFFu) << 8 | tri;
    };
    size_t differ = 0, notCleared = 0, covered = 0;
    for (size_t i = 0; i < mask.size(); ++i)
    {
        if (mask[i])
        {
            if (names(1, vis[1][i]) != names(0, vis[0][i]) || std::memcmp(&depth[1][i], &depth[0][i], 4) != 0) ++differ;
            covered += vis[0][i] != kVisNone;
        }
        else if (vis[1][i] != kVisNone || depth[1][i] != 1.0f)
            ++notCleared;
    }
    logf("    %zu mirror pixels (%zu covered): %zu differ from the unmasked view; %zu other pixels not VIS_NONE at depth 1\n", mirror, covered, differ, notCleared);
    logStats("unmasked", stats[0]);
    logStats("masked  ", stats[1]);
    CHECK(stats[0].frameIndex == 0 && stats[1].frameIndex == 1);
    CHECK(covered > 10000 && differ == 0 && notCleared == 0);
    CHECK(stats[1].visibleClusters < stats[0].visibleClusters && stats[0].overflow == 0 && stats[1].overflow == 0);
    device().deferRelease(constants);
    device().deferRelease(maskTexture);
    device().deferRelease(maskUpload);
    device().deferRelease(depthRb);
    device().deferRelease(visRb);
    device().deferRelease(listRb);
}

// ------------------------------------------------------------------------------------------------ coverage layer

namespace
{
// A thin vertical card (blade) of width w and length len centred at c in the plane z = c.z, facing -z (the camera at the
// origin looks along +z). 'front' picks the winding the band A reference card uses (visible with back-face culling).
void addCard(scene::Mesh& m, float3 c, float w, float len, bool front)
{
    const uint32_t b = (uint32_t)m.positions.size();
    const float3 p[4] = { { c.x - 0.5f * w, c.y - 0.5f * len, c.z }, { c.x + 0.5f * w, c.y - 0.5f * len, c.z }, { c.x + 0.5f * w, c.y + 0.5f * len, c.z },
                          { c.x - 0.5f * w, c.y + 0.5f * len, c.z } };
    for (int k = 0; k < 4; ++k)
    {
        m.positions.push_back(p[k]);
        m.normals.push_back({ 0, 0, -1 });
        m.uv0.push_back({ (k == 1 || k == 2) ? 1.0f : 0.0f, k >= 2 ? 1.0f : 0.0f });
    }
    if (front) m.indices.insert(m.indices.end(), { b, b + 2, b + 1, b, b + 3, b + 2 });
    else m.indices.insert(m.indices.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
}

// A thin strip along z (from z0 to z1) at (x, y), width w along x, in the plane y = const: it passes the near plane. The
// vertex normals are n0 at z0 and n1 at z1.
void addStrip(scene::Mesh& m, float x, float y, float z0, float z1, float w, float3 n0 = { 0, 1, 0 }, float3 n1 = { 0, 1, 0 })
{
    const uint32_t b = (uint32_t)m.positions.size();
    const float3 p[4] = { { x - 0.5f * w, y, z0 }, { x + 0.5f * w, y, z0 }, { x + 0.5f * w, y, z1 }, { x - 0.5f * w, y, z1 } };
    for (int k = 0; k < 4; ++k)
    {
        m.positions.push_back(p[k]);
        m.normals.push_back(normalize(k < 2 ? n0 : n1));
        m.uv0.push_back({ 0, 0 });
    }
    m.indices.insert(m.indices.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
}

struct CoverageScene
{
    scene::Scene scene;
    uint32_t bladeMesh = 0, stripMesh = 0, columnMesh = 0, stackMesh = 0;
};

// Camera at the origin looking along +z (60 degree fov, near 0.05 m). Band A: a reference card (winding check), an
// occluder box in front of some blades. Bands B/C: blades 0.2 .. 2 mm wide at 0.6 .. 3 m (0.1 .. 1.2 px), in pairs that
// overlap on screen at different depths, half of them wound the other way (one-sided: culled), a two-sided group, and a
// 0.1 mm strip crossing the near plane (its vertex normals differ along it: the perspective-correct normal). Opaque
// columns 1.3 px wide in front of the upper blades cover whole pixels with the union of their two triangles
// (opaqueCovered). A stack of 4,000 glass cards (see-through records: no opaque cover),
// each 6 x 0.3 px on the same spot at 2 .. 3.6 m, puts 48,000 fragments in one tile (the extension root and its 2-level
// subtree).
CoverageScene coverageScene()
{
    CoverageScene cs;
    scene::Scene& s = cs.scene;
    s.name = "v-coverage";
    s.materials.resize(3);
    s.materials[1].twoSided = true;
    s.materials[2].cls = scene::MaterialClass::Glass;
    scene::Mesh card;
    card.name = "reference card";
    addCard(card, { -0.9f, 0.45f, 3.0f }, 0.3f, 0.3f, true);
    card.submeshes.push_back({ 0, (uint32_t)card.indices.size(), 0 });
    s.meshes.push_back(card);
    s.meshes.push_back(box({ 0.12f, 0.12f, 0.02f }, 2));
    scene::Mesh blades;
    blades.name = "blades";
    uint32_t oneSidedEnd = 0;
    for (int group = 0; group < 2; ++group)
    {
        for (int i = 0; i < 24; ++i)
            for (int j = 0; j < 3; ++j)
            {
                const float z = 0.6f + 0.1f * i + 0.8f * j;
                const float w = 0.0002f + 0.0008f * ((i * 7 + j * 3) % 10) / 9.0f;
                const float x = -0.55f + 0.047f * i + 0.013f * j;
                const float y = (group ? -0.25f : 0.1f) + 0.02f * j;
                addCard(blades, { x * z / 1.5f, y * z / 1.5f, z }, w, 0.1f * z, ((i + j) & 1) == 0 || group == 1);
                // Its partner: the same screen position 20 % farther (overlapping fragments to sort).
                addCard(blades, { x * z * 1.2f / 1.5f, y * z * 1.2f / 1.5f, z * 1.2f }, w * 1.2f, 0.1f * z * 1.2f, ((i + j) & 1) == 0 || group == 1);
            }
        if (group == 0) oneSidedEnd = (uint32_t)blades.indices.size();
    }
    blades.submeshes.push_back({ 0, oneSidedEnd, 0 });
    blades.submeshes.push_back({ oneSidedEnd, (uint32_t)blades.indices.size() - oneSidedEnd, 1 });
    cs.bladeMesh = (uint32_t)s.meshes.size();
    s.meshes.push_back(blades);
    scene::Mesh strip;
    strip.name = "near strip";
    addStrip(strip, 0.02f, -0.012f, -0.05f, 0.35f, 0.0001f, { 0, 1, 0 }, { 0.8f, 1, -0.5f });
    strip.submeshes.push_back({ 0, (uint32_t)strip.indices.size(), 1 });
    cs.stripMesh = (uint32_t)s.meshes.size();
    s.meshes.push_back(strip);
    // Screen position (pixels, 640 x 360) to world at depth z for the test camera (at the origin looking along +z), and
    // pixel sizes to world sizes there, from the view's own projection.
    const float4x4 vp = ViewDesc::fromCamera(camera({ 0, 0, 0 }, { 0, 0, 1 }), 640, 360, {}).viewProj;
    CHECK(vp.m[0][1] == 0 && vp.m[1][0] == 0 && vp.m[3][0] == 0 && vp.m[3][1] == 0 && vp.m[3][2] > 0);
    auto world = [&](float sx, float sy, float z) {
        const float w = vp.m[3][2] * z + vp.m[3][3];
        return float3{ ((2 * sx / 640 - 1) * w - vp.m[0][2] * z - vp.m[0][3]) / vp.m[0][0], ((1 - 2 * sy / 360) * w - vp.m[1][2] * z - vp.m[1][3]) / vp.m[1][1], z };
    };
    auto pxX = [&](float px, float z) { return px * 2 * (vp.m[3][2] * z + vp.m[3][3]) / (640 * std::fabs(vp.m[0][0])); };
    auto pxY = [&](float px, float z) { return px * 2 * (vp.m[3][2] * z + vp.m[3][3]) / (360 * std::fabs(vp.m[1][1])); };
    scene::Mesh columns;
    columns.name = "opaque columns";
    // 1.3 px wide: a whole pixel lies inside when the centre's fraction is in 0.35 .. 0.65, so centre fractions 0.15 apart
    // keep two such columns under any camera shift. Close together (35 x 20 px), so the cluster's conservative width at its
    // sphere's near side stays below band A's 1.5 px.
    for (const float sx : { 300.05f, 307.2f, 314.35f, 321.5f, 328.65f, 335.8f })
    {
        const float3 c = world(sx, 160.0f, 1.0f);
        addCard(columns, c, pxX(1.3f, 1.0f), pxY(20.0f, 1.0f), true);
    }
    columns.submeshes.push_back({ 0, (uint32_t)columns.indices.size(), 0 });
    cs.columnMesh = (uint32_t)s.meshes.size();
    s.meshes.push_back(columns);
    scene::Mesh stack;
    stack.name = "card stack";
    for (int k = 0; k < 4000; ++k)
    {
        const float z = 2.0f + 0.0004f * k;
        const float3 c = world(100.0f, 300.4f, z);
        // 6 px along x, 0.3 px along y: inside one pixel row.
        addCard(stack, c, pxX(6.0f, z), pxY(0.3f, z), true);
    }
    stack.submeshes.push_back({ 0, (uint32_t)stack.indices.size(), 2 });
    cs.stackMesh = (uint32_t)s.meshes.size();
    s.meshes.push_back(stack);
    s.instances.push_back(at(0, { 0, 0, 0 }));
    s.instances.push_back(at(1, { 0.25f, 0.13f, 1.4f }));  // occluder over part of the upper blade group
    s.instances.push_back(at(cs.bladeMesh, { 0, 0, 0 }));
    s.instances.push_back(at(cs.stripMesh, { 0, 0, 0 }));
    s.instances.push_back(at(cs.columnMesh, { 0, 0, 0 }));
    s.instances.push_back(at(cs.stackMesh, { 0, 0, 0 }));
    scene::Camera c;
    c.name = "main";
    s.cameras.push_back(c);
    scene::validate(s);
    return cs;
}

struct CoverageFrame
{
    ViewDesc view;
    std::vector<float> depth;
    std::vector<uint32_t> visId, visible, tiles, list, records, starts, range;  // tiles: 8 words each; range: 2 words per pixel
};

std::vector<CoverageFrame> renderCoverage(const scene::Scene& s, const QualityConfig& q, const std::vector<scene::Camera>& cams, uint32_t width, uint32_t height,
                                          ClusterData& clusters, visibility::Stats& stats)
{
    clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q));
    GpuScene gs(device());
    gs.upload(s);
    gs.setClusters(clusters);
    FrameRenderer renderer(device(), shaders(), q, gs, 2);
    RenderGraph graph(device());
    const uint64_t capacity = (uint64_t)q.integer("visibility.max_visible_clusters");
    const uint64_t tileCount = (uint64_t)((width + 7) / 8) * ((height + 7) / 8);
    const uint64_t tileWords = tileCount * 8, listWords = 16 + 4 * tileCount, startWords = tileCount * 64;
    const uint32_t rangePitch = (width * 8 + 255) / 256 * 256;
    ComPtr<ID3D12Resource> depthRb = readbackBuffer((uint64_t)rowPitch(width) * height), visRb = readbackBuffer((uint64_t)rowPitch(width) * height),
                           listRb = readbackBuffer(capacity * 8), tilesRb = readbackBuffer(tileWords * 4), tileListRb = readbackBuffer(listWords * 4),
                           startsRb = readbackBuffer(startWords * 4), rangeRb = readbackBuffer((uint64_t)rangePitch * height), recordsRb;
    uint64_t recordBytes = 0;
    std::vector<CoverageFrame> out;
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
        CHECK(main.coverageTiles.valid() && main.coverageRecords.valid() && main.coverageTileList.valid() && main.coverageTilePixels.valid() &&
              main.coverageDepthRange.valid() && !main.coverageChunkTable.valid());
        const uint64_t poolBytes = graph.desc(main.coverageRecords).size;
        if (poolBytes != recordBytes)
        {
            if (recordsRb) device().deferRelease(recordsRb);
            recordsRb = readbackBuffer(poolBytes);
            recordBytes = poolBytes;
        }
        ID3D12Resource *d = depthRb.Get(), *v = visRb.Get(), *l = listRb.Get(), *tl = tilesRb.Get(), *li = tileListRb.Get(), *rc = recordsRb.Get(),
                       *st = startsRb.Get(), *rg = rangeRb.Get();
        graph.addPass("test.readback", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(main.depth, Use::CopySrc);
                          b.use(main.visId, Use::CopySrc);
                          b.use(main.visibleClusters, Use::CopySrc);
                          b.use(main.coverageTiles, Use::CopySrc);
                          b.use(main.coverageTileList, Use::CopySrc);
                          b.use(main.coverageRecords, Use::CopySrc);
                          b.use(main.coverageTilePixels, Use::CopySrc);
                          b.use(main.coverageDepthRange, Use::CopySrc);
                          b.keep();
                      },
                      [=](PassContext& c) {
                          copyTexture(c, main.depth, d, width, height);
                          copyTexture(c, main.visId, v, width, height);
                          c.cmd->CopyBufferRegion(l, 0, c.resource(main.visibleClusters), 0, capacity * 8);
                          c.cmd->CopyBufferRegion(tl, 0, c.resource(main.coverageTiles), 0, tileWords * 4);
                          c.cmd->CopyBufferRegion(li, 0, c.resource(main.coverageTileList), 0, listWords * 4);
                          c.cmd->CopyBufferRegion(rc, 0, c.resource(main.coverageRecords), 0, poolBytes);
                          c.cmd->CopyBufferRegion(st, 0, c.resource(main.coverageTilePixels), 0, startWords * 4);
                          D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                          to.pResource = rg;
                          to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                          to.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32_UINT, width, height, 1, rangePitch };
                          from.pResource = c.resource(main.coverageDepthRange);
                          from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                          c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                      });
        graph.execute(nullptr);
        device().waitIdle();
        CoverageFrame fo;
        fo.view = fr.mainView;
        fo.depth = readTexture<float>(d, width, height);
        fo.visId = readTexture<uint32_t>(v, width, height);

        auto readBuffer = [](ID3D12Resource* r, uint64_t words) {
            std::vector<uint32_t> w(words);
            uint8_t* p = nullptr;
            check(r->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
            std::memcpy(w.data(), p, words * 4);
            r->Unmap(0, nullptr);
            return w;
        };
        fo.visible = readBuffer(l, capacity * 2);
        fo.tiles = readBuffer(tl, tileWords);
        fo.list = readBuffer(li, listWords);
        fo.records = readBuffer(rc, poolBytes / 4);
        fo.starts = readBuffer(st, startWords);
        {
            const std::vector<uint32_t> raw = readBuffer(rg, (uint64_t)rangePitch / 4 * height);
            fo.range.resize((size_t)width * height * 2);
            for (uint32_t y = 0; y < height; ++y) std::memcpy(&fo.range[(size_t)y * width * 2], &raw[(size_t)y * rangePitch / 4], (size_t)width * 8);
        }
        out.push_back(std::move(fo));
        prev = fr.mainView.viewProj;
    }
    stats = visibility::latestStats(renderer.trackState());
    for (ComPtr<ID3D12Resource>* r : std::initializer_list<ComPtr<ID3D12Resource>*>{ &depthRb, &visRb, &listRb, &tilesRb, &tileListRb, &recordsRb, &startsRb, &rangeRb })
        device().deferRelease(*r);
    return out;
}

struct D2
{
    double x, y;
};

// Polygon clipped to the pixel square [px, px + 1) x [py, py + 1): area and centroid.
double pixelArea(const std::vector<D2>& poly, int px, int py, D2& centre)
{
    std::vector<D2> p = poly, o;
    auto clip = [&](int axis, double limit, double sign) {
        o.clear();
        for (size_t i = 0; i < p.size(); ++i)
        {
            const D2 a = p[i], b = p[(i + 1) % p.size()];
            const double da = ((axis ? a.y : a.x) - limit) * sign, db = ((axis ? b.y : b.x) - limit) * sign;
            if (da <= 0) o.push_back(a);
            if ((da < 0 && db > 0) || (da > 0 && db < 0))
            {
                const double t = da / (da - db);
                o.push_back({ a.x + t * (b.x - a.x), a.y + t * (b.y - a.y) });
            }
        }
        p.swap(o);
    };
    clip(0, px, -1);
    clip(0, px + 1.0, 1);
    clip(1, py, -1);
    clip(1, py + 1.0, 1);
    double twice = 0, mx = 0, my = 0;
    for (size_t i = 0; i < p.size(); ++i)
    {
        const D2 u = p[i], v = p[(i + 1) % p.size()];
        const double cr = u.x * v.y - v.x * u.y;
        twice += cr;
        mx += (u.x + v.x) * cr;
        my += (u.y + v.y) * cr;
    }
    centre = std::fabs(twice) > 1e-18 ? D2{ mx / (3 * twice), my / (3 * twice) } : D2{ px + 0.5, py + 0.5 };
    return 0.5 * std::fabs(twice);
}

// Screen polygon of a world triangle: near-clipped in clip space (keep w - z >= 0), projected (x, y pixels), with the
// plane's device depth as an affine function of screen position (z/w at the polygon's vertices).
struct D3
{
    double x, y, z;
};

struct ScreenPoly
{
    std::vector<D2> xy;
    std::vector<double> z, invW;
    std::vector<D3> n;      // vertex normals (clip-space linear through the near clip)
    double signedArea = 0;  // y-down pixels
};

ScreenPoly project(const float4x4& m, const float3 w[3], uint32_t width, uint32_t height, const float3* normals = nullptr)
{
    struct C4
    {
        double x, y, z, w;
        D3 n;
    };
    C4 c[3];
    for (int k = 0; k < 3; ++k)
    {
        auto row = [&](int r) { return (double)m.m[r][0] * w[k].x + (double)m.m[r][1] * w[k].y + (double)m.m[r][2] * w[k].z + m.m[r][3]; };
        c[k] = { row(0), row(1), row(2), row(3), normals ? D3{ normals[k].x, normals[k].y, normals[k].z } : D3{ 0, 0, 0 } };
    }
    std::vector<C4> poly;
    for (int k = 0; k < 3; ++k)
    {
        const C4 a = c[k], b = c[(k + 1) % 3];
        const double ea = a.w - a.z, eb = b.w - b.z;
        if (ea >= 0) poly.push_back(a);
        if ((ea >= 0) != (eb >= 0))
        {
            const double t = ea / (ea - eb);
            poly.push_back({ a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z), a.w + t * (b.w - a.w),
                             { a.n.x + t * (b.n.x - a.n.x), a.n.y + t * (b.n.y - a.n.y), a.n.z + t * (b.n.z - a.n.z) } });
        }
    }
    ScreenPoly sp;
    for (const C4& v : poly)
    {
        sp.xy.push_back({ (v.x / v.w * 0.5 + 0.5) * width, (0.5 - v.y / v.w * 0.5) * height });
        sp.z.push_back(v.z / v.w);
        sp.invW.push_back(1 / v.w);
        sp.n.push_back(v.n);
    }
    for (size_t i = 0; i < sp.xy.size(); ++i)
    {
        const D2 u = sp.xy[i], v = sp.xy[(i + 1) % sp.xy.size()];
        sp.signedArea += 0.5 * (u.x * v.y - v.x * u.y);
    }
    return sp;
}

double depthAt(const ScreenPoly& sp, D2 q)
{
    // Affine z over the plane from the best-conditioned vertex triple.
    size_t bi = 0, bj = 1, bk = 2;
    double best = -1;
    for (size_t i = 0; i < sp.xy.size(); ++i)
        for (size_t j = i + 1; j < sp.xy.size(); ++j)
            for (size_t k = j + 1; k < sp.xy.size(); ++k)
            {
                const double a = std::fabs((sp.xy[j].x - sp.xy[i].x) * (sp.xy[k].y - sp.xy[i].y) - (sp.xy[k].x - sp.xy[i].x) * (sp.xy[j].y - sp.xy[i].y));
                if (a > best) best = a, bi = i, bj = j, bk = k;
            }
    const D2 a = sp.xy[bi], b = sp.xy[bj], c = sp.xy[bk];
    const double det = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    const double s = ((q.x - a.x) * (c.y - a.y) - (c.x - a.x) * (q.y - a.y)) / det, t = ((b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y)) / det;
    return (1 - s - t) * sp.z[bi] + s * sp.z[bj] + t * sp.z[bk];
}

// Unit normal at q: perspective-correct over the fan triangle (0, k, k + 1) holding q (1/w-weighted barycentrics).
D3 normalAt(const ScreenPoly& sp, D2 q)
{
    size_t best = 1;
    double bestMin = -1e30, wBest[3] = { 1, 0, 0 };
    for (size_t k = 1; k + 1 < sp.xy.size(); ++k)
    {
        const D2 a = sp.xy[0], b = sp.xy[k], c = sp.xy[k + 1];
        const double det = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (det == 0) continue;
        const double u = ((q.x - a.x) * (c.y - a.y) - (c.x - a.x) * (q.y - a.y)) / det, v = ((b.x - a.x) * (q.y - a.y) - (q.x - a.x) * (b.y - a.y)) / det;
        const double lo = std::min({ 1 - u - v, u, v });
        if (lo > bestMin)
        {
            bestMin = lo;
            best = k;
            wBest[0] = 1 - u - v;
            wBest[1] = u;
            wBest[2] = v;
        }
    }
    const size_t idx[3] = { 0, best, best + 1 };
    D3 n{ 0, 0, 0 };
    for (int i = 0; i < 3; ++i)
    {
        const D3 v = sp.n[idx[i]];
        const double len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), wgt = wBest[i] * sp.invW[idx[i]] / len;
        n = { n.x + v.x * wgt, n.y + v.y * wgt, n.z + v.z * wgt };
    }
    const double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    return { n.x / len, n.y / len, n.z / len };
}

// CoverageTiles.hlsli coverageFragmentNormal.
D3 decodeRecordNormal(uint32_t packed)
{
    const double ex = (packed & 0xFF) / 255.0 * 2 - 1, ey = ((packed >> 8) & 0xFF) / 255.0 * 2 - 1;
    double x = ex, y = ey;
    const double z = 1 - std::fabs(ex) - std::fabs(ey);
    if (z < 0)
    {
        x = (1 - std::fabs(ey)) * (ex >= 0 ? 1 : -1);
        y = (1 - std::fabs(ex)) * (ey >= 0 ? 1 : -1);
    }
    const double len = std::sqrt(x * x + y * y + z * z);
    return { x / len, y / len, z / len };
}

// Subsample i of Coverage.hlsli: x = (i + 0.5) / 32, y = bit-reversed i / 32 + 1/64.
D2 coverageSampleCpu(uint32_t i)
{
    uint32_t r = 0;
    for (int b = 0; b < 5; ++b) r |= ((i >> b) & 1u) << (4 - b);
    return { (i + 0.5) / 32.0, r / 32.0 + 1.0 / 64.0 };
}

// Expected and ambiguous subsample bits of a convex polygon (samples within 2e-4 px of an edge are ambiguous: float
// projection error).
void polygonMask(const std::vector<D2>& poly, int px, int py, uint32_t& inside, uint32_t& ambiguous)
{
    inside = ambiguous = 0;
    double orient = 0;
    for (size_t i = 0; i < poly.size(); ++i) orient += poly[i].x * poly[(i + 1) % poly.size()].y - poly[(i + 1) % poly.size()].x * poly[i].y;
    const double sgn = orient >= 0 ? 1 : -1;
    for (uint32_t i = 0; i < 32; ++i)
    {
        const D2 s0 = coverageSampleCpu(i), q{ px + s0.x, py + s0.y };
        bool in = true, edge = false;
        for (size_t k = 0; k < poly.size(); ++k)
        {
            const D2 a = poly[k], b = poly[(k + 1) % poly.size()];
            const double len = std::hypot(b.x - a.x, b.y - a.y);
            if (len == 0) continue;
            const double e = ((b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x)) * sgn / len;
            if (std::fabs(e) < 2e-4) edge = true;
            if (e < 0) in = false;
        }
        if (edge) ambiguous |= 1u << i;
        else if (in) inside |= 1u << i;
    }
}
} // namespace

UNX_TEST(coverage_mask_lut_matches_exact)
{
    // Design revision 1, 11 b: the LUT mask replaces 32 edge-function evaluations per triangle and pixel. A plain 64 x 64
    // table of masks at the bin centres missed the gate |coverage difference| P99 <= 1/32 (P99 2/32, worst 5/32): every
    // subsample within a bin's reach of the edge could land on the wrong side. The table now holds conservative masks and
    // the open subsamples are tested exactly, so the result must be the exact mask, bit for bit.
    const std::vector<uint32_t>& table = coverageMaskTable();
    const uint32_t cases = 3u << 18;  // three size classes (TestCoverageMaskLut.hlsl), 262,144 each
    auto buffer = [&](D3D12_HEAP_TYPE heap, uint64_t bytes, bool uav) {
        D3D12_HEAP_PROPERTIES hp{ heap };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        check(device().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
              "test buffer");
        return r;
    };
    ComPtr<ID3D12Resource> lut = buffer(D3D12_HEAP_TYPE_UPLOAD, table.size() * 4, false), out = buffer(D3D12_HEAP_TYPE_DEFAULT, (uint64_t)cases * 12, true),
                           rb = readbackBuffer((uint64_t)cases * 12);
    {
        void* m = nullptr;
        check(lut->Map(0, nullptr, &m), "map lut");
        std::memcpy(m, table.data(), table.size() * 4);
        lut->Unmap(0, nullptr);
    }
    const uint32_t lutSrv = device().descriptors().allocateResource(), outUav = device().descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = (UINT)table.size() / 2;
    sd.Buffer.StructureByteStride = 8;
    device().d3d()->CreateShaderResourceView(lut.Get(), &sd, device().descriptors().resourceCpu(lutSrv));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = cases * 3;
    ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device().d3d()->CreateUnorderedAccessView(out.Get(), nullptr, &ud, device().descriptors().resourceCpu(outUav));

    RenderGraph graph(device());
    ID3D12PipelineState* kernel = shaders().compute("Passes/Visibility/Tests/TestCoverageMaskLut");
    const BufferRef lutRef = graph.importBuffer(lut.Get(), { "test.lut", table.size() * 4, 8 });
    const BufferRef outRef = graph.importBuffer(out.Get(), { "test.lut.out", (uint64_t)cases * 12, 0 });
    ID3D12Resource* dst = rb.Get();
    graph.addPass("test.lut", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(lutRef, Use::SrvCompute);
                      b.use(outRef, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { lutSrv, outUav, cases, 0x1234567u };
                      c.cmd->SetPipelineState(kernel);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((cases + 63) / 64, 1, 1);
                  });
    graph.addPass("test.lut.readback", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(outRef, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(dst, 0, c.resource(outRef), 0, (uint64_t)cases * 12); });
    graph.execute(nullptr);
    device().waitIdle();

    const uint32_t* r = nullptr;
    check(rb->Map(0, nullptr, (void**)&r), "map");
    // Per size class: triangles cutting the pixel (a mask neither empty nor full), masks differing from the exact one,
    // and the subsamples tested exactly per such triangle (the LUT path's extra work; the exact mask tests 32 x 3 edges).
    const char* classNames[3] = { "within 0.75 px", "within 3 px", "within 30 px" };
    bool pass = true;
    for (uint32_t sc = 0; sc < 3; ++sc)
    {
        uint64_t cut = 0, differ = 0, tested = 0, testedAll = 0, all = 0;
        uint32_t worstTested = 0;
        for (uint32_t i = sc; i < cases; i += 3)
        {
            const uint32_t exact = r[3 * i], fast = r[3 * i + 1], t = r[3 * i + 2];
            ++all;
            testedAll += t;
            if (exact != fast) ++differ;
            if (exact != 0 && exact != 0xFFFFFFFFu)
            {
                ++cut;
                tested += t;
                worstTested = std::max(worstTested, t);
            }
        }
        logf("    %s: %llu triangles cut the pixel, masks differing from the exact one %llu; subsamples tested exactly: %.2f per cutting triangle (worst %u), "
             "%.2f per triangle overall\n",
             classNames[sc], (unsigned long long)cut, (unsigned long long)differ, cut ? (double)tested / cut : 0.0, worstTested, all ? (double)testedAll / all : 0.0);
        pass = pass && cut > 0 && differ == 0;
    }
    rb->Unmap(0, nullptr);
    device().descriptors().freeResource(lutSrv);
    device().descriptors().freeResource(outUav);
    device().deferRelease(lut);
    device().deferRelease(out);
    device().deferRelease(rb);
    CHECK(pass);
}

UNX_TEST(coverage_layer_is_exact)
{
    // Coverage layer v1.41 (INTERFACES 7.1, CoverageTiles.hlsli) against the exact clip of every band B/C triangle. Per
    // frame: every record is an expected (triangle, pixel) fragment with its exact area (10-bit), centroid depth, mask and
    // perspective-correct normal (8 + 8 bit octahedral) and see-through flag (the glass stack); no expected fragment is
    // missing; the tile list's header and per-tile entries (record and block bases: exclusive prefixes in list order)
    // match the tile headers; each tile's range is pixel-major with the published pixel starts; coverageDepthRange is the
    // nearest and farthest record depth of every pixel ((0, 0xFFFFFFFF) where there is none); opaqueCovered
    // (COVERAGE_REDESIGN 4.6 union rule, from the records) is set exactly where the union of the pixel's opaque record masks
    // is full and the band A surface lies behind the farthest of those records; the tiles of earlier frames were emptied.
    // The card stack's tile holds more than 16 blocks of records (the scratch path of tiles of more than one block), and
    // the capacity floor of the second configuration is below the first frame's need (the pool overflows, then grows):
    // an overflowing frame stores a consistent subset.
    const CoverageScene cs = coverageScene();
    const scene::Scene& s = cs.scene;
    struct Config
    {
        const char* name;
        std::vector<std::string> keys;
    };
    const Config configs[] = {
        { "defaults", { "visibility.coverage_layer = true", "visibility.occlusion_culling = false" } },
        { "small capacity floor",
          { "visibility.coverage_layer = true", "visibility.occlusion_culling = false", "visibility.coverage_pool_min_fragments_per_pixel = 0.05" } },
    };
    for (const Config& config : configs)
    {
        const QualityConfig q = quality(config.keys);
        const uint32_t width = 640, height = 360, tilesX = (width + 7) / 8, tileCount = tilesX * ((height + 7) / 8);
        const bool smallFloor = std::string(config.name) == "small capacity floor";
        std::vector<scene::Camera> cams(4, camera({ 0, 0, 0 }, { 0, 0, 1 }));
        cams[1].position = { 0.004f, 0.002f, 0 };  // moves every fragment: last frame's tiles must be emptied
        cams[2].position = { -0.003f, 0.001f, 0 };  // (and the statistics of frame 0 are read back by frame 2)
        cams[3].position = { 0.001f, -0.002f, 0 };
        ClusterData cd;
        visibility::Stats stats;
        const std::vector<CoverageFrame> frames = renderCoverage(s, q, cams, width, height, cd, stats);
        logf("    [%s] stats (frame %llu): triangles A/B/C %u/%u/%u, list B %u, coverage fragments %u in %u tiles, %u blocks, %u tiles of more than one "
             "block, capacity %u, overflow 0x%x\n",
             config.name, (unsigned long long)stats.frameIndex, stats.triangles[0], stats.triangles[1], stats.triangles[2], stats.listEntries[4], stats.coverageFragments,
             stats.coverageTiles, stats.coverageBlocks, stats.coverageHeavyTiles, stats.coveragePoolRecords, stats.overflow);
        // The latest statistics are of frame 1: with the small floor its pool was full (0x100), nothing else.
        CHECK(smallFloor ? (stats.overflow & ~0x100u) == 0 : stats.overflow == 0);
        CHECK(stats.triangles[1] + stats.triangles[2] > 0);

        // Winding: the reference card (band A, one-sided, the "front" blade winding) must be visible with back faces culled.
        {
            const CoverageFrame& fo = frames[0];
            size_t cardPixels = 0;
            for (size_t i = 0; i < fo.visId.size(); ++i)
                if (fo.visId[i] != kVisNone && fo.visible[2 * ((fo.visId[i] - 1) >> 7)] == 0) ++cardPixels;
            // The CPU front-face rule below (negative signed area in y-down pixels) must call the visible card front.
            const scene::Mesh& cm = s.meshes[0];
            float3 w[3];
            for (int k = 0; k < 3; ++k) w[k] = s.instances[0].transform.transformPoint(cm.positions[cm.indices[k]]);
            const double cardArea = project(fo.view.viewProj, w, width, height).signedArea;
            logf("    reference card (front winding, band A, back faces culled): %zu pixels, signed area %.1f px (front < 0)\n", cardPixels, cardArea);
            CHECK(cardPixels > 500 && cardArea < 0);
        }

        // Source triangle key: (instance, sorted mesh vertex indices).
        auto key = [](uint32_t instance, uint32_t a, uint32_t b, uint32_t c) {
            uint32_t v[3] = { a, b, c };
            std::sort(v, v + 3);
            return (uint64_t)instance << 60 ^ (uint64_t)v[0] << 40 ^ (uint64_t)v[1] << 20 ^ v[2];
        };
        for (size_t f = 0; f < frames.size(); ++f)
        {
            const CoverageFrame& fo = frames[f];
            const float4x4& vp = fo.view.viewProj;
            const uint32_t capacity = (uint32_t)(fo.records.size() / 4);
            // HiZ mip 0 of this frame (farthest of 2 x 2), the pixel kernel's occlusion rule.
            const uint32_t hw = width / 2, hh = height / 2;
            std::vector<float> hiz((size_t)hw * hh);
            for (uint32_t y = 0; y < hh; ++y)
                for (uint32_t x = 0; x < hw; ++x)
                {
                    float m = 1;
                    for (int k = 0; k < 4; ++k) m = std::min(m, fo.depth[(size_t)(2 * y + (k >> 1)) * width + 2 * x + (k & 1)]);
                    hiz[(size_t)y * hw + x] = m;
                }
            auto farthest = [&](int px, int py) {
                const int x0 = std::max(px - 1, 0) >> 1, y0 = std::max(py - 1, 0) >> 1, x1 = std::min((px + 1) >> 1, (int)hw - 1), y1 = std::min((py + 1) >> 1, (int)hh - 1);
                return std::min({ hiz[(size_t)y0 * hw + x0], hiz[(size_t)y0 * hw + x1], hiz[(size_t)y1 * hw + x0], hiz[(size_t)y1 * hw + x1] });
            };

            // Expected fragments from the source triangles of the band B/C meshes.
            struct Expect
            {
                double area, depth, depthMin = 0, depthMax = 0;  // (the polygon's depth range)
                D3 normal{ 0, 0, 0 };
                uint32_t mask, ambiguous;
                bool required = false;  // not a float-rounding sliver, not at the occlusion threshold
                bool found = false;
                uint32_t foundVis = 0, foundCluster = 0, foundTri = 0, foundEntry = 0;
                uint32_t foundRecord[4] = {};
            };
            std::map<std::pair<uint64_t, uint32_t>, Expect> expected;  // (triangle key, pixel)
            size_t culledBack = 0, occluded = 0, marginal = 0;
            for (uint32_t ii = 2; ii < s.instances.size(); ++ii)
            {
                const scene::Instance& inst = s.instances[ii];
                const scene::Mesh& mesh = s.meshes[inst.mesh];
                for (const scene::Submesh& sm : mesh.submeshes)
                {
                    const bool twoSided = s.materials[sm.material].twoSided;
                    for (uint32_t t = sm.indexOffset; t < sm.indexOffset + sm.indexCount; t += 3)
                    {
                        const uint32_t vi[3] = { mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2] };
                        float3 w[3], n[3];
                        for (int k = 0; k < 3; ++k)
                        {
                            w[k] = inst.transform.transformPoint(mesh.positions[vi[k]]);
                            n[k] = inst.transform.transformVector(mesh.normals[vi[k]]);
                        }
                        const ScreenPoly sp = project(vp, w, width, height, n);
                        if (sp.xy.size() < 3 || sp.signedArea == 0) continue;
                        const bool back = sp.signedArea > 0;  // counter-clockwise on screen (negative in y-down pixels) is front
                        if (!twoSided && back)
                        {
                            ++culledBack;
                            continue;
                        }
                        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
                        for (const D2& v : sp.xy) x0 = std::min(x0, v.x), y0 = std::min(y0, v.y), x1 = std::max(x1, v.x), y1 = std::max(y1, v.y);
                        // Pixels touching the polygon's box (+1: float rounding may give a touching pixel a sliver).
                        for (int py = std::max((int)std::floor(y0) - 1, 0); py <= std::min((int)std::floor(y1) + 1, (int)height - 1); ++py)
                            for (int px = std::max((int)std::floor(x0) - 1, 0); px <= std::min((int)std::floor(x1) + 1, (int)width - 1); ++px)
                            {
                                D2 centre;
                                const double area = pixelArea(sp.xy, px, py, centre);
                                const double depth = depthAt(sp, centre);
                                const float hizFar = farthest(px, py);
                                const double margin = 1e-7 + 2e-4 * depth;  // float projection vs double
                                if (area > 0 && depth < hizFar - margin)
                                {
                                    ++occluded;
                                    continue;
                                }
                                const uint32_t pi = (uint32_t)py * width + px;
                                Expect e;
                                e.area = area;
                                e.depth = depth;
                                e.depthMin = *std::min_element(sp.z.begin(), sp.z.end());
                                e.depthMax = *std::max_element(sp.z.begin(), sp.z.end());
                                if (area > 0)
                                {
                                    e.normal = normalAt(sp, centre);
                                    if (back) e.normal = { -e.normal.x, -e.normal.y, -e.normal.z };
                                }
                                polygonMask(sp.xy, px, py, e.mask, e.ambiguous);
                                e.required = area >= 5e-4 && std::fabs(depth - hizFar) > margin;
                                if (area > 0 && !e.required) ++marginal;
                                expected[{ key(ii, vi[0], vi[1], vi[2]), pi }] = e;
                            }
                    }
                }
            }

            // The tile list and the tiles.
            const std::vector<uint32_t>& L = fo.list;
            const uint32_t listed = L[3], heavyCount = L[11];
            CHECK(listed > 0 && listed <= tileCount && L[0] == std::min(listed, 65535u) && L[1] == (listed + 65534u) / 65535u && L[2] == 1);
            CHECK(L[6] == capacity && L[7] == tilesX && L[5] > 0 && L[8] == std::min(L[5], 65535u) && L[9] == (L[5] + 65534u) / 65535u && L[10] == 1);
            CHECK(heavyCount <= listed && L[12] == std::min(heavyCount, 65535u) && L[13] == (heavyCount + 65534u) / 65535u && L[14] == 1);
            std::vector<uint8_t> tileListed(tileCount, 0);
            size_t checked = 0, duplicates = 0, depthOver = 0, coveredPixels = 0, opaqueBits = 0, seeThroughRecords = 0, heavyTiles = 0;
            uint64_t fragmentSum = 0, blockSum = 0;
            uint32_t deepest = 0;
            double worstArea = 0, worstDepth = 0, worstSliverDepth = 0, worstNormalDeg = 0;
            std::vector<uint8_t> rangeSeen((size_t)width * height, 0);
            for (uint32_t k = 0; k < listed; ++k)
            {
                const uint32_t* info = &L[16 + 4 * k];
                const uint32_t tile = info[0], n = info[1], base = info[2];
                CHECK(tile < tileCount);
                if (tileListed[tile]) fail("frame %zu: tile %u listed twice", f, tile);
                tileListed[tile] = 1;
                const uint32_t* h = &fo.tiles[(size_t)tile * 8];
                if (n == 0 || h[0] != n || h[1] != base || h[2] != k + 1)
                    fail("frame %zu: listed tile %u (entry %u): %u records at %u, header %u records at %u, listed %u", f, tile, k, n, base, h[0], h[1], h[2]);
                if (base != fragmentSum || info[3] != blockSum)
                    fail("frame %zu: listed tile %u: record base %u, block base %u; prefixes %llu, %llu", f, tile, base, info[3], (unsigned long long)fragmentSum,
                         (unsigned long long)blockSum);
                if ((h[5] != 0) != (n > 1024)) fail("frame %zu: tile %u with %u records: scratch slot word %u", f, tile, n, h[5]);
                heavyTiles += n > 1024;
                fragmentSum += n;
                blockSum += (n + 1023) / 1024;
                deepest = std::max(deepest, n);
                CHECK((uint64_t)base + n <= capacity);
                // Pixel starts: pixel p's records are [base + start(p), base + start(p + 1)), start(64) = n.
                const uint32_t* start = &fo.starts[(size_t)k * 64];
                CHECK(start[0] == 0);
                for (uint32_t pix = 0; pix < 64; ++pix)
                {
                    const uint32_t end = pix < 63 ? start[pix + 1] : n;
                    if (end < start[pix]) fail("frame %zu: tile %u: pixel %u starts at %u, the next at %u", f, tile, pix, start[pix], end);
                    for (uint32_t i = start[pix]; i < end; ++i)
                        if ((fo.records[((size_t)base + i) * 4 + 3] >> 26) != pix)
                            fail("frame %zu: tile %u: record %u in pixel %u's range belongs to pixel %u", f, tile, i, pix, fo.records[((size_t)base + i) * 4 + 3] >> 26);
                }
                std::map<uint32_t, std::vector<uint32_t>> byPixel;  // pixel -> record indices in the tile
                for (uint32_t i = 0; i < n; ++i)
                {
                    const uint32_t* fr = &fo.records[((size_t)base + i) * 4];
                    const uint32_t p = fr[3] >> 26, px = (tile % tilesX) * 8 + p % 8, py = (tile / tilesX) * 8 + p / 8;
                    if (px >= width || py >= height) fail("frame %zu: tile %u record %u: pixel (%u, %u) outside the view", f, tile, i, px, py);
                    const uint32_t pi = py * width + px;
                    const uint32_t depthBits = fr[1] & 0x7FFFFFFFu;
                    float depth;
                    std::memcpy(&depth, &depthBits, 4);
                    const double area = ((fr[3] >> 16) & 0x3FF) / 1023.0;
                    const uint32_t vis = fr[0], entry = (vis - 1) >> 7, tri = (vis - 1) & 127;
                    const uint32_t instance = fo.visible[2 * entry], cluster = fo.visible[2 * entry + 1] & 0xFFFFFF;
                    CHECK(instance < s.instances.size() && cluster < cd.clusters.size());
                    const bool seeThrough = (fr[1] >> 31) != 0;
                    if (seeThrough != (s.instances[instance].mesh == cs.stackMesh))
                        fail("frame %zu: pixel (%u, %u): see-through flag %d on a record of instance %u", f, px, py, (int)seeThrough, instance);
                    seeThroughRecords += seeThrough;
                    const gpu::Cluster& c = cd.clusters[cluster];
                    const uint32_t tp = cd.clusterTriangles[c.triangleOffset + tri];
                    uint32_t mv[3];
                    for (int m = 0; m < 3; ++m) mv[m] = cd.clusterVertexIndices[c.vertexOffset + ((tp >> (8 * m)) & 0xFF)];
                    const auto it = expected.find({ key(instance, mv[0], mv[1], mv[2]), pi });
                    if (it == expected.end())
                        fail("frame %zu: pixel (%u, %u) has a fragment of instance %u vertices %u %u %u (area %g, depth %g) that the exact clip does not expect", f, px, py,
                             instance, mv[0], mv[1], mv[2], area, depth);
                    Expect& e = it->second;
                    if (e.found)
                    {
                        // A primitive the hardware clipped is shaded once per piece along the cuts: the same vis id with the
                        // same values. Anything else is the same triangle drawn twice.
                        if (e.foundVis == vis && std::memcmp(e.foundRecord, fr, 16) == 0)
                        {
                            ++duplicates;
                            ++checked;
                            byPixel[p].push_back(i);
                            continue;
                        }
                        fail("frame %zu: pixel (%u, %u): the same triangle twice: entry %u cluster %u tri %u and entry %u cluster %u tri %u (records %08x %08x %08x "
                             "%08x / %08x %08x %08x %08x)",
                             f, px, py, e.foundEntry, e.foundCluster, e.foundTri, entry, cluster, tri, e.foundRecord[0], e.foundRecord[1], e.foundRecord[2],
                             e.foundRecord[3], fr[0], fr[1], fr[2], fr[3]);
                    }
                    e.found = true;
                    e.foundVis = vis;
                    e.foundCluster = cluster;
                    e.foundTri = tri;
                    e.foundEntry = entry;
                    std::memcpy(e.foundRecord, fr, 16);
                    const double da = std::fabs(area - e.area), dd = std::fabs(depth - e.depth);
                    worstArea = std::max(worstArea, da);
                    if (e.area >= 1e-2) worstDepth = std::max(worstDepth, dd / std::max(e.depth, 1e-6));
                    else worstSliverDepth = std::max(worstSliverDepth, dd / std::max(e.depth, 1e-6));
                    // Area: 10 bits (half a step) on top of the float clip's error.
                    if (da > 0.5 / 1023 + 2e-4 + 1e-4 * e.area) fail("frame %zu: pixel (%u, %u): area %.7f, exact %.7f", f, px, py, area, e.area);
                    // Depth at the covered region's centroid: Green's moments (Coverage.hlsli) give the centroid to float
                    // rounding divided by the area, so a sliver's depth is looser. Depth only orders overlapping fragments,
                    // and a misordered fragment moves the pixel's coverage by at most its own area: below 1e-2 px^2 that is
                    // inside the 1/32 overlap bound of the quality definition (ARCHITECTURE 3) with margin, and such a
                    // fragment's depth need only lie in its triangle's depth range.
                    const bool depthOk = e.area >= 1e-2 ? dd <= 1e-7 + 1e-4 * e.depth : depth >= e.depthMin - 1e-6 && depth <= e.depthMax + 1e-6;
                    if (!depthOk)
                    {
                        ++depthOver;
                        if (depthOver <= 3) logf("    depth over tolerance: frame %zu pixel (%u, %u): depth %.9g, exact %.9g, area %.3g\n", f, px, py, depth, e.depth, e.area);
                    }
                    if (((fr[2] ^ e.mask) & ~e.ambiguous) != 0)
                        fail("frame %zu: pixel (%u, %u): mask 0x%08x, exact 0x%08x (ambiguous 0x%08x)", f, px, py, fr[2], e.mask, e.ambiguous);
                    // Normal: the perspective-correct interpolation in 8 + 8 bit octahedral code, whose rounding is at most
                    // 0.95 degrees (400,000 random directions); 1.1 leaves the float interpolation its share. The centroid
                    // of a sliver is float rounding over its area, so only fragments of 1e-2 px^2 or more count.
                    if (e.area >= 1e-2)
                    {
                        const D3 g = decodeRecordNormal(fr[3]);
                        const double cosA = std::clamp(g.x * e.normal.x + g.y * e.normal.y + g.z * e.normal.z, -1.0, 1.0), deg = std::acos(cosA) * 57.29578;
                        worstNormalDeg = std::max(worstNormalDeg, deg);
                        if (deg > 1.1)
                            fail("frame %zu: pixel (%u, %u): normal (%.4f %.4f %.4f), exact (%.4f %.4f %.4f): %.2f degrees", f, px, py, g.x, g.y, g.z, e.normal.x, e.normal.y,
                                 e.normal.z, deg);
                    }
                    byPixel[p].push_back(i);
                    ++checked;
                }
                // opaqueCovered: the union of the pixel's opaque records' masks is full and band A lies behind the farthest
                // of them (reversed Z: a smaller depth). Depth range: the pixel's nearest and farthest record.
                for (uint32_t p = 0; p < 64; ++p)
                {
                    const uint32_t px = (tile % tilesX) * 8 + p % 8, py = (tile / tilesX) * 8 + p / 8;
                    const bool bit = ((p < 32 ? h[3] : h[4]) >> (p & 31)) & 1u;
                    if (px >= width || py >= height)
                    {
                        if (bit) fail("frame %zu: tile %u pixel %u outside the view is opaqueCovered", f, tile, p);
                        continue;
                    }
                    uint32_t unionMask = 0, farthestOpaque = 0xFFFFFFFFu, nearest = 0, farthestAll = 0xFFFFFFFFu;
                    for (uint32_t i : byPixel[p])
                    {
                        const uint32_t* fr = &fo.records[((size_t)base + i) * 4];
                        nearest = std::max(nearest, fr[1] & 0x7FFFFFFFu);
                        farthestAll = std::min(farthestAll, fr[1] & 0x7FFFFFFFu);
                        if ((fr[1] >> 31) != 0 || fr[2] == 0) continue;
                        unionMask |= fr[2];
                        farthestOpaque = std::min(farthestOpaque, fr[1]);
                    }
                    const size_t pi = (size_t)py * width + px;
                    rangeSeen[pi] = 1;
                    if (fo.range[2 * pi] != nearest || fo.range[2 * pi + 1] != farthestAll)
                        fail("frame %zu: pixel (%u, %u): depth range %08x .. %08x, records %08x .. %08x", f, px, py, fo.range[2 * pi + 1], fo.range[2 * pi], farthestAll, nearest);
                    uint32_t depthA;
                    std::memcpy(&depthA, &fo.depth[pi], 4);
                    const bool expectBit = unionMask == 0xFFFFFFFFu && depthA < farthestOpaque;
                    if (bit != expectBit)
                        fail("frame %zu: pixel (%u, %u): opaqueCovered %d, opaque union %08x, farthest %08x, band A depth %08x", f, px, py, (int)bit, unionMask, farthestOpaque, depthA);
                    coveredPixels += unionMask == 0xFFFFFFFFu;
                    opaqueBits += bit;
                }
            }
            CHECK(heavyTiles == heavyCount && blockSum == L[5]);
            CHECK(fragmentSum == L[4]);
            // Tiles not listed: empty (last frame's were emptied); pixels outside the listed tiles have no depth range.
            size_t staleTiles = 0, staleRanges = 0;
            for (uint32_t t = 0; t < tileCount; ++t)
            {
                if (tileListed[t]) continue;
                const uint32_t* h = &fo.tiles[(size_t)t * 8];
                if (h[0] != 0 || h[2] != 0 || h[3] != 0 || h[4] != 0) ++staleTiles;
            }
            for (size_t pi = 0; pi < rangeSeen.size(); ++pi)
                if (!rangeSeen[pi] && (fo.range[2 * pi] != 0 || fo.range[2 * pi + 1] != 0xFFFFFFFFu)) ++staleRanges;
            size_t missing = 0;
            for (const auto& [k, e] : expected)
            {
                if (!e.found && e.required)
                    if (++missing <= 5) logf("    missing: pixel %u, area %g, depth %g\n", k.second, e.area, e.depth);
            }
            const bool full = fragmentSum < capacity;  // a frame at capacity may have lost fragments (OVERFLOW_COVERAGE)
            logf("    [%s] frame %zu: %u tiles (%u of more than one block, deepest %u records), %llu records of capacity %u (%llu blocks); %zu checked against the "
                 "exact clip (%zu duplicates of clipped pieces; worst area error %.2e px, depth %.2e relative, slivers < 1e-2 px2 %.2e, normal %.2f degrees); %zu "
                 "see-through records; %zu pixels with a full opaque union, %zu opaqueCovered; %zu expected missing%s; %zu back-facing triangles culled, %zu pixel "
                 "fragments occluded by band A, %zu marginal; %zu stale tiles, %zu stale depth ranges\n",
                 config.name, f, listed, heavyCount, deepest, (unsigned long long)fragmentSum, capacity, (unsigned long long)blockSum, checked, duplicates, worstArea,
                 worstDepth, worstSliverDepth, worstNormalDeg, seeThroughRecords, coveredPixels, opaqueBits, missing, full ? "" : " (the pool was full)", culledBack,
                 occluded, marginal, staleTiles, staleRanges);
            CHECK(staleTiles == 0 && staleRanges == 0 && checked == fragmentSum && depthOver == 0);
            CHECK(missing == 0 || (smallFloor && !full));
            if (full) CHECK(culledBack > 0 && occluded > 0 && coveredPixels > 0 && opaqueBits > 0 && seeThroughRecords > 16384 && deepest > 16 * 1024 && heavyCount > 0);
            if (smallFloor && f == 0) CHECK(!full);  // the floor is below the first frame's need
        }
    }
    logf("    coverage layer exact over %zu frames in 2 configurations\n", (size_t)3);
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
