// Destruction fragments in V (render C, C2; FEATURES_GAME 2), real device with the debug layer (no GPU lock: existing
// kernels only):
//   fragment_transition_is_exact   a wall drawn as one merged mesh, then, on the event frame, as its pre-authored
//                                  fragment instances (shown with setInstanceVisible, the wall hidden, same frame): every
//                                  pixel keeps its coverage and its depth up to the interpolation of a different
//                                  triangulation, and every covered pixel names a fragment instance
//   fragments_fly_apart            the fragments then move for several frames (updateTransforms): each frame's vis ids
//                                  name fragment instances whose triangle covers the pixel, nothing overflows
//   unx_test_visibility_fragmenttests [filter]
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
#include <exception>
#include <functional>
#include <random>
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
        return o;
    }());
    return d;
}
ShaderLibrary& shaders()
{
    static ShaderLibrary lib(device(), executableDirectory() / "shaders");
    return lib;
}
QualityConfig quality()
{
    QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    // The oracles project triangles with the requested view and the readbacks use the requested size: the main view at
    // its output resolution, without the temporal upscale's internal size and per-frame jitter.
    q.applyOverride("output.render_scale=1");
    q.applyOverride("output.render_height_max=0");
    return q;
}

// A closed box of (2s)^2 quads per face, counter-clockwise from outside.
scene::Mesh box(float3 half, uint32_t s)
{
    scene::Mesh m;
    m.name = "wall";
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
                m.indices.insert(m.indices.end(), { p, q, t, p, t, r });
            }
    }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    return m;
}

// Pre-authored fracture of 'wall' (as a bake would do it): every triangle goes to the Voronoi cell of its centroid; a
// fragment's exterior is exactly its triangles (same positions), plus one interior cut face (a quad through the cell's
// site, inside the wall volume, hidden while the fragments are assembled). Fragment meshes keep the wall's object space,
// so their instances start at the wall's transform.
std::vector<scene::Mesh> fracture(const scene::Mesh& wall, float3 half, uint32_t cells, uint32_t seed, std::vector<float3>& sites)
{
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> ux(-half.x, half.x), uy(-half.y, half.y);
    sites.clear();
    for (uint32_t c = 0; c < cells; ++c) sites.push_back({ ux(rng), uy(rng), 0 });
    std::vector<scene::Mesh> out(cells);
    std::vector<std::vector<int32_t>> remap(cells, std::vector<int32_t>(wall.positions.size(), -1));
    for (size_t t = 0; t < wall.indices.size(); t += 3)
    {
        const float3 c = (wall.positions[wall.indices[t]] + wall.positions[wall.indices[t + 1]] + wall.positions[wall.indices[t + 2]]) * (1.0f / 3);
        uint32_t best = 0;
        float bestD = 1e30f;
        for (uint32_t k = 0; k < cells; ++k)
        {
            const float dx = c.x - sites[k].x, dy = c.y - sites[k].y, d = dx * dx + dy * dy;
            if (d < bestD) bestD = d, best = k;
        }
        scene::Mesh& m = out[best];
        for (int k = 0; k < 3; ++k)
        {
            const uint32_t v = wall.indices[t + k];
            if (remap[best][v] < 0)
            {
                remap[best][v] = (int32_t)m.positions.size();
                m.positions.push_back(wall.positions[v]);
                m.normals.push_back(wall.normals[v]);
                m.uv0.push_back(wall.uv0[v]);
            }
            m.indices.push_back((uint32_t)remap[best][v]);
        }
    }
    for (uint32_t k = 0; k < cells; ++k)
    {
        scene::Mesh& m = out[k];
        m.name = "fragment " + std::to_string(k);
        // Interior cut face: a small quad in the wall's mid-plane (z = 0), facing +z, well inside the volume.
        const float r = 0.05f;
        const float3 s = sites[k];
        const uint32_t b = (uint32_t)m.positions.size();
        for (float3 p : { float3{ s.x - r, s.y - r, 0 }, float3{ s.x + r, s.y - r, 0 }, float3{ s.x + r, s.y + r, 0 }, float3{ s.x - r, s.y + r, 0 } })
        {
            m.positions.push_back({ std::clamp(p.x, -half.x * 0.95f, half.x * 0.95f), std::clamp(p.y, -half.y * 0.95f, half.y * 0.95f), 0 });
            m.normals.push_back({ 0, 0, 1 });
            m.uv0.push_back({ 0, 0 });
        }
        m.indices.insert(m.indices.end(), { b, b + 1, b + 2, b, b + 2, b + 3 });
        m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    }
    return out;
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

struct FrameOut
{
    ViewDesc view;
    std::vector<float> depth;
    std::vector<uint32_t> visId;
    std::vector<uint32_t> visible;  // uint2 per entry: instance, cluster
    visibility::Stats stats;
};

// Wall at (0, 2, 8) of 6 x 4 x 0.5 m in front of a ground slab; its fragments (hidden at start) share its transform.
struct FragmentScene
{
    scene::Scene scene;
    uint32_t wall = 0, firstFragment = 0, fragmentCount = 0;
    std::vector<float3> sites;
    float3 wallPos{ 0, 2, 8 };
};

FragmentScene fragmentScene(uint32_t cells)
{
    FragmentScene f;
    scene::Scene& s = f.scene;
    s.name = "c2-fragments";
    s.materials.resize(1);
    const float3 half{ 3.0f, 2.0f, 0.25f };
    const scene::Mesh wall = box(half, 16);
    s.meshes.push_back(box({ 20, 0.5f, 20 }, 4));  // ground
    s.meshes.push_back(wall);
    for (scene::Mesh& m : fracture(wall, half, cells, 7, f.sites)) s.meshes.push_back(std::move(m));
    s.instances.push_back(at(0, { 0, -0.5f, 10 }));
    f.wall = (uint32_t)s.instances.size();
    s.instances.push_back(at(1, f.wallPos));
    f.firstFragment = (uint32_t)s.instances.size();
    f.fragmentCount = cells;
    for (uint32_t k = 0; k < cells; ++k) s.instances.push_back(at(2 + k, f.wallPos));
    scene::Camera c;
    c.name = "main";
    s.cameras.push_back(c);
    scene::validate(s);
    return f;
}

scene::Camera camera(float3 position, float3 target)
{
    scene::Camera c;
    c.position = position;
    c.forward = normalize(target - position);
    c.nearPlane = 0.05f;
    return c;
}

// Renders frames; before frame f, edit(f, gpuScene) applies that frame's scene changes.
std::vector<FrameOut> run(const scene::Scene& s, const ClusterData& clusters, const std::vector<scene::Camera>& cams, uint32_t width, uint32_t height,
                          const std::function<void(uint32_t, GpuScene&)>& edit)
{
    const QualityConfig q = quality();
    GpuScene gs(device());
    gs.upload(s);
    gs.setClusters(clusters);
    FrameRenderer renderer(device(), shaders(), q, gs, 2);
    RenderGraph graph(device());
    const uint64_t capacity = (uint64_t)q.integer("visibility.max_visible_clusters");
    ComPtr<ID3D12Resource> depthRb = readbackBuffer((uint64_t)rowPitch(width) * height), visRb = readbackBuffer((uint64_t)rowPitch(width) * height),
                           listRb = readbackBuffer(capacity * 8);
    std::vector<FrameOut> out;
    float4x4 prev = ViewDesc::fromCamera(cams[0], width, height, {}).viewProj;
    for (uint32_t f = 0; f < cams.size(); ++f)
    {
        edit(f, gs);
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
        fo.stats = visibility::latestStats(renderer.trackState());
        out.push_back(std::move(fo));
        prev = fr.mainView.viewProj;
    }
    device().deferRelease(depthRb);
    device().deferRelease(visRb);
    device().deferRelease(listRb);
    return out;
}

uint32_t instanceOf(const FrameOut& fo, uint32_t id)
{
    const uint32_t entry = (id - 1) >> 7;
    return fo.visible[2 * entry];
}

// Does the triangle named by vis id 'id' cover pixel (x, y) (centre) in frame 'fo' (instance transforms 'xf')?
bool covers(const FrameOut& fo, const scene::Scene& s, const ClusterData& cd, const std::vector<float3x4>& xf, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const uint32_t entry = (id - 1) >> 7, tri = (id - 1) & 127;
    const uint32_t instance = fo.visible[2 * entry], cluster = fo.visible[2 * entry + 1] & 0xFFFFFFu;
    if (instance >= s.instances.size() || cluster >= cd.clusters.size()) return false;
    const gpu::Cluster& c = cd.clusters[cluster];
    const scene::Mesh& mesh = s.meshes[s.instances[instance].mesh];
    const uint32_t packed = cd.clusterTriangles[c.triangleOffset + tri];
    float2 p[3];
    for (int k = 0; k < 3; ++k)
    {
        const uint32_t mv = cd.clusterVertexIndices[c.vertexOffset + ((packed >> (8 * k)) & 0xFFu)];
        const float3 wp = xf[instance].transformPoint(mesh.positions[mv]);
        const float4x4& m = fo.view.viewProj;
        const float cx = m.m[0][0] * wp.x + m.m[0][1] * wp.y + m.m[0][2] * wp.z + m.m[0][3];
        const float cy = m.m[1][0] * wp.x + m.m[1][1] * wp.y + m.m[1][2] * wp.z + m.m[1][3];
        const float cw = m.m[3][0] * wp.x + m.m[3][1] * wp.y + m.m[3][2] * wp.z + m.m[3][3];
        p[k] = { (cx / cw * 0.5f + 0.5f) * w, (0.5f - cy / cw * 0.5f) * h };
    }
    const float2 q{ x + 0.5f, y + 0.5f };
    auto edge = [](float2 a, float2 b, float2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
    const float e0 = edge(p[0], p[1], q), e1 = edge(p[1], p[2], q), e2 = edge(p[2], p[0], q);
    const float tol = 0.02f * (std::fabs(edge(p[0], p[1], p[2])) + 1.0f);  // half-pixel snap slack on edges
    return (e0 >= -tol && e1 >= -tol && e2 >= -tol) || (e0 <= tol && e1 <= tol && e2 <= tol);
}
// Distance in pixels from pixel (x, y)'s centre to the nearest edge (line) of the triangle vis id 'id' names (the scene's
// instance transforms).
double edgeDistance(const FrameOut& fo, const scene::Scene& s, const ClusterData& cd, uint32_t id, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    const uint32_t entry = (id - 1) >> 7, tri = (id - 1) & 127;
    const uint32_t instance = fo.visible[2 * entry], cluster = fo.visible[2 * entry + 1] & 0xFFFFFFu;
    const gpu::Cluster& c = cd.clusters[cluster];
    const scene::Mesh& mesh = s.meshes[s.instances[instance].mesh];
    const uint32_t packed = cd.clusterTriangles[c.triangleOffset + tri];
    double px[3], py[3];
    for (int k = 0; k < 3; ++k)
    {
        const uint32_t mv = cd.clusterVertexIndices[c.vertexOffset + ((packed >> (8 * k)) & 0xFFu)];
        const float3 wp = s.instances[instance].transform.transformPoint(mesh.positions[mv]);
        const float4x4& m = fo.view.viewProj;
        const double cx = (double)m.m[0][0] * wp.x + (double)m.m[0][1] * wp.y + (double)m.m[0][2] * wp.z + m.m[0][3];
        const double cy = (double)m.m[1][0] * wp.x + (double)m.m[1][1] * wp.y + (double)m.m[1][2] * wp.z + m.m[1][3];
        const double cw = (double)m.m[3][0] * wp.x + (double)m.m[3][1] * wp.y + (double)m.m[3][2] * wp.z + m.m[3][3];
        px[k] = (cx / cw * 0.5 + 0.5) * w;
        py[k] = (0.5 - cy / cw * 0.5) * h;
    }
    const double qx = x + 0.5, qy = y + 0.5;
    double best = 1e30;
    for (int k = 0; k < 3; ++k)
    {
        const int j = (k + 1) % 3;
        const double ex = px[j] - px[k], ey = py[j] - py[k], len = std::sqrt(ex * ex + ey * ey);
        if (len > 0) best = std::min(best, std::fabs(ex * (qy - py[k]) - ey * (qx - px[k])) / len);
    }
    return best;
}
} // namespace

UNX_TEST(fragment_transition_is_exact)
{
    const FragmentScene fs = fragmentScene(48);
    const ClusterData cd = clusterbuilder::build(fs.scene, clusterbuilder::Settings::fromQuality(quality()));
    const uint32_t width = 1920, height = 1080, event = 3;
    // Close (LOD 0 of the wall and fragments at 4 m) and far (30 m: DAG cuts of both, different triangulations).
    for (const float distance : { 4.0f, 30.0f })
    {
        std::vector<scene::Camera> cams(6, camera(fs.wallPos + float3{ 0.8f, 0.6f, -distance }, fs.wallPos));
        const auto frames = run(fs.scene, cd, cams, width, height, [&](uint32_t f, GpuScene& gs) {
            if (f == 0)
                for (uint32_t k = 0; k < fs.fragmentCount; ++k) gs.setInstanceVisible(fs.firstFragment + k, false);
            if (f == event)  // the event frame: wall out, fragments in
            {
                gs.setInstanceVisible(fs.wall, false);
                for (uint32_t k = 0; k < fs.fragmentCount; ++k) gs.setInstanceVisible(fs.firstFragment + k, true);
            }
        });
        const FrameOut &before = frames[event - 1], &after = frames[event];
        // Exactness condition: the fragments' exterior triangles are the wall's surface, but the wall's DAG and the
        // fragments' DAGs triangulate it differently (flat faces simplify), so a pixel may change coverage only when its
        // centre lies on the shared silhouette within the rasteriser's vertex snap (1/256 px), where the tie-break of
        // two triangulations of one edge line may differ.
        constexpr double kSnap = 2.0 / 256;
        size_t wallPixels = 0, coverageChanged = 0, tiesOffEdge = 0, fragmentPixels = 0, wrongInstance = 0;
        double maxRel = 0, worstTie = 0;
        for (size_t i = 0; i < before.depth.size(); ++i)
        {
            const bool wasWall = before.visId[i] != kVisNone && instanceOf(before, before.visId[i]) == fs.wall;
            const bool isFrag = after.visId[i] != kVisNone && instanceOf(after, after.visId[i]) >= fs.firstFragment;
            if ((before.visId[i] == kVisNone) != (after.visId[i] == kVisNone))
            {
                ++coverageChanged;
                const FrameOut& drawn = before.visId[i] != kVisNone ? before : after;
                const double d = edgeDistance(drawn, fs.scene, cd, drawn.visId[i], (uint32_t)(i % width), (uint32_t)(i / width), width, height);
                worstTie = std::max(worstTie, d);
                if (d > kSnap) ++tiesOffEdge;
                continue;
            }
            if (wasWall)
            {
                ++wallPixels;
                if (!isFrag) ++wrongInstance;
                else ++fragmentPixels;
                if (after.depth[i] > 0) maxRel = std::max(maxRel, std::fabs((double)after.depth[i] - before.depth[i]) / after.depth[i]);
            }
            else if (isFrag)
                ++wrongInstance;  // a fragment where the wall was not
        }
        logf("    %4.0f m: wall pixels %zu, on the event frame fragment pixels %zu, other %zu; coverage changed %zu (all within %.1e px of the edge: "
             "%s), depth max relative difference %.2e; visible clusters %u -> %u, overflow 0x%x\n",
             distance, wallPixels, fragmentPixels, wrongInstance, coverageChanged, worstTie, tiesOffEdge ? "NO" : "yes", maxRel, before.stats.visibleClusters,
             after.stats.visibleClusters, after.stats.overflow);
        CHECK(wallPixels > 10000);
        CHECK(wrongInstance == 0 && tiesOffEdge == 0);
        CHECK(maxRel < 1e-4);  // the same surface: only the interpolation of a different triangulation differs
        CHECK(after.stats.overflow == 0);
        // Frames after the event are the same image (static fragments).
        CHECK(frames.back().depth == after.depth);
    }
}

UNX_TEST(fragments_fly_apart)
{
    const FragmentScene fs = fragmentScene(48);
    const ClusterData cd = clusterbuilder::build(fs.scene, clusterbuilder::Settings::fromQuality(quality()));
    const uint32_t width = 1920, height = 1080, frames = 12;
    std::vector<scene::Camera> cams(frames, camera(fs.wallPos + float3{ 2.0f, 1.5f, -9.0f }, fs.wallPos));
    std::vector<float3x4> xf(fs.scene.instances.size());
    for (size_t i = 0; i < xf.size(); ++i) xf[i] = fs.scene.instances[i].transform;
    std::vector<std::vector<float3x4>> perFrame;
    const auto out = run(fs.scene, cd, cams, width, height, [&](uint32_t f, GpuScene& gs) {
        if (f == 0)
        {
            gs.setInstanceVisible(fs.wall, false);  // already broken
        }
        else
        {
            // Each fragment flies away from the wall centre and tumbles about z (rigid motion).
            std::vector<InstanceTransformUpdate> ups;
            for (uint32_t k = 0; k < fs.fragmentCount; ++k)
            {
                const uint32_t i = fs.firstFragment + k;
                const float3 dir = normalize(fs.sites[k] + float3{ 0, 0, -1.5f });
                const float t = f / 60.0f, a = 2.0f * t * (1 + k % 3);
                const float cs = std::cos(a), sn = std::sin(a);
                float3x4 m{};
                m.m[0][0] = cs, m.m[0][1] = -sn, m.m[1][0] = sn, m.m[1][1] = cs, m.m[2][2] = 1;
                const float3 p = fs.wallPos + dir * (6.0f * t) + float3{ 0, -4.9f * t * t, 0 };
                m.m[0][3] = p.x, m.m[1][3] = p.y, m.m[2][3] = p.z;
                xf[i] = m;
                ups.push_back({ i, m, 0 });
            }
            gs.updateTransforms(f, ups);
        }
        perFrame.push_back(xf);
    });
    for (uint32_t f = 1; f < frames; ++f)
    {
        const FrameOut& fo = out[f];
        size_t fragmentPixels = 0, checked = 0, bad = 0;
        for (uint32_t y = 0; y < height; y += 2)
            for (uint32_t x = 0; x < width; x += 2)
            {
                const uint32_t id = fo.visId[(size_t)y * width + x];
                if (id == kVisNone) continue;
                const uint32_t inst = instanceOf(fo, id);
                if (inst < fs.firstFragment) continue;
                ++fragmentPixels;
                ++checked;
                if (!covers(fo, fs.scene, cd, perFrame[f], id, x, y, width, height)) ++bad;
            }
        if (f == 1 || f + 1 == frames)
            logf("    frame %u: fragment pixels (every 2nd) %zu, vis ids not covering their pixel %zu, visible clusters %u, overflow 0x%x\n", f, fragmentPixels, bad,
                 fo.stats.visibleClusters, fo.stats.overflow);
        CHECK(fragmentPixels > 1000 && bad == 0 && fo.stats.overflow == 0);
    }
}

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    uint32_t passed = 0, run = 0;
    for (const TestCase& t : registry())
    {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++run;
        try
        {
            t.fn();
        }
        catch (const std::exception& e)
        {
            logf("FAIL %s: %s\n", t.name, e.what());
            continue;
        }
        logf("PASS %s\n", t.name);
        ++passed;
    }
    logf("%u/%u passed\n", passed, run);
    return passed == run ? 0 : 1;
}
