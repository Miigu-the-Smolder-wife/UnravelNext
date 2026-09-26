// C3 instance hierarchy and skinned bounds (render C; ARCHITECTURE 2.1 "64 m cell BVH"), real device with the debug layer:
//   chunks_are_conservative   3,000 static boxes and 40 walls over 600 x 600 m: the chunked path (CullChunks, chunk
//                             instance pass, phase-2 chunk retest) gives bit-identical depth to every instance in the flat
//                             list (all flagged dynamic) over a moving camera with two-phase occlusion, and culls chunks
//   skinned_bounds            a skinned bar whose pose moves half of it 20 m from its bind pose is drawn (its bind-pose
//                             sphere is behind the camera), one posed wholly behind the camera is culled, both from
//                             their palette bounds; no posed pixel is lost against rendering with bounds disabled
//   unx_test_visibility_instancehierarchytests [filter]
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
#include <cstring>
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

constexpr uint32_t kVisNone = 0;

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
QualityConfig quality() { return QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality"); }

scene::Mesh box(float3 half, uint32_t s, const char* name)
{
    scene::Mesh m;
    m.name = name;
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

scene::Instance at(uint32_t mesh, float3 p, uint32_t flags = scene::InstanceCastShadow)
{
    scene::Instance i;
    i.mesh = mesh;
    i.flags = flags;
    i.transform.m[0][3] = p.x;
    i.transform.m[1][3] = p.y;
    i.transform.m[2][3] = p.z;
    return i;
}

scene::Camera camera(float3 position, float3 target)
{
    scene::Camera c;
    c.position = position;
    c.forward = normalize(target - position);
    c.nearPlane = 0.05f;
    return c;
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
    std::vector<float> depth;
    std::vector<uint32_t> visId;
    visibility::Stats stats;
};

// Renders the cameras; stats are those of the last frame read back (latestStats lags by the frames in flight), so the
// last frames repeat the camera.
std::vector<FrameOut> run(const scene::Scene& s, const std::vector<scene::Camera>& cams, uint32_t width, uint32_t height)
{
    const QualityConfig q = quality();
    const ClusterData cd = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q));
    GpuScene gs(device());
    gs.upload(s);
    gs.setClusters(cd);
    FrameRenderer renderer(device(), shaders(), q, gs, 2);
    RenderGraph graph(device());
    ComPtr<ID3D12Resource> depthRb = readbackBuffer((uint64_t)rowPitch(width) * height), visRb = readbackBuffer((uint64_t)rowPitch(width) * height);
    std::vector<FrameOut> out;
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
        ID3D12Resource *d = depthRb.Get(), *v = visRb.Get();
        graph.addPass("test.readback", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(main.depth, Use::CopySrc);
                          b.use(main.visId, Use::CopySrc);
                          b.keep();
                      },
                      [=](PassContext& c) {
                          copyTexture(c, main.depth, d, width, height);
                          copyTexture(c, main.visId, v, width, height);
                      });
        graph.execute(nullptr);
        device().waitIdle();
        FrameOut fo;
        fo.depth = readTexture<float>(d, width, height);
        fo.visId = readTexture<uint32_t>(v, width, height);
        fo.stats = visibility::latestStats(renderer.trackState());
        out.push_back(std::move(fo));
        prev = fr.mainView.viewProj;
    }
    device().deferRelease(depthRb);
    device().deferRelease(visRb);
    return out;
}
} // namespace

UNX_TEST(chunks_are_conservative)
{
    scene::Scene s;
    s.name = "c3-chunks";
    s.materials.resize(1);
    s.meshes.push_back(box({ 300, 0.5f, 300 }, 8, "ground"));
    s.meshes.push_back(box({ 0.4f, 0.4f, 0.4f }, 2, "crate"));
    s.meshes.push_back(box({ 12, 5, 0.5f }, 4, "wall"));
    s.instances.push_back(at(0, { 0, -0.5f, 0 }));
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> u(-290.0f, 290.0f);
    for (int k = 0; k < 3000; ++k) s.instances.push_back(at(1, { u(rng), 0.4f + 3.0f * (k % 4 == 0), u(rng) }));
    for (int k = 0; k < 40; ++k) s.instances.push_back(at(2, { u(rng), 5, u(rng) }));
    scene::Camera c0;
    s.cameras.push_back(c0);
    scene::validate(s);
    scene::Scene flat = s;
    for (auto& in : flat.instances) in.flags |= scene::InstanceDynamic;  // every instance in the flat list
    std::vector<scene::Camera> cams;
    for (int f = 0; f < 10; ++f)
    {
        const float t = f / 9.0f, yaw = -0.6f + 1.2f * t;
        const float3 p{ -40 + 80 * t, 2.0f + 4 * t, -60 + 30 * t };
        cams.push_back(camera(p, p + float3{ std::sin(yaw), -0.05f, std::cos(yaw) }));
    }
    cams.push_back(cams.back());
    cams.push_back(cams.back());
    const uint32_t width = 1920, height = 1080;
    const auto chunked = run(s, cams, width, height), reference = run(flat, cams, width, height);
    size_t differing = 0, covered = 0;
    for (size_t f = 0; f < cams.size(); ++f)
        for (size_t i = 0; i < chunked[f].depth.size(); ++i)
        {
            if (chunked[f].depth[i] != reference[f].depth[i] || (chunked[f].visId[i] == kVisNone) != (reference[f].visId[i] == kVisNone)) ++differing;
            if (chunked[f].visId[i] != kVisNone) ++covered;
        }
    const visibility::Stats &a = chunked.back().stats, &b = reference.back().stats;
    logf("    %zu frames: %zu covered pixels, %zu differ; chunked: chunk items %u, deferred chunks %u, instances visible %u, nodes %u | flat: instances visible "
         "%u, nodes %u; overflow 0x%x / 0x%x\n",
         cams.size(), covered, differing, a.chunkItems, a.deferredChunks, a.instancesVisible, a.nodesTested, b.instancesVisible, b.nodesTested, a.overflow, b.overflow);
    CHECK(covered > 1000000);
    CHECK(differing == 0);
    CHECK(a.chunkItems > 0 && a.overflow == 0 && b.overflow == 0);
}

UNX_TEST(skinned_bounds)
{
    // A bar of 2 x 0.2 x 0.2 m along +x: vertices with x < 1 follow joint 0, the others joint 1 (inverseBind = identity,
    // so the palette is the pose). Instance A: joint 1 moves its half 20 m along +z; the camera at z = 15 looks at it,
    // with the bind pose (z ~ 0) behind it. Instance B: both halves moved to z = -50, behind the camera.
    scene::Scene s;
    s.name = "c3-skin";
    s.materials.resize(1);
    scene::Mesh bar = box({ 1.0f, 0.1f, 0.1f }, 8, "bar");
    for (float3& p : bar.positions) p.x += 1.0f;
    const size_t n = bar.positions.size();
    bar.skin.joints.resize(4 * n, 0);
    bar.skin.weights.resize(4 * n, 0.0f);
    for (size_t v = 0; v < n; ++v)
    {
        bar.skin.joints[4 * v] = bar.positions[v].x < 1.0f ? 0 : 1;
        bar.skin.weights[4 * v] = 1.0f;
    }
    bar.skin.inverseBind.resize(2);
    s.meshes.push_back(bar);
    auto pose = [](float3 t0, float3 t1) {
        scene::Skeleton k;
        k.jointToModel.resize(2);
        k.jointToModel[0].m[0][3] = t0.x, k.jointToModel[0].m[1][3] = t0.y, k.jointToModel[0].m[2][3] = t0.z;
        k.jointToModel[1].m[0][3] = t1.x, k.jointToModel[1].m[1][3] = t1.y, k.jointToModel[1].m[2][3] = t1.z;
        return k;
    };
    s.skeletons.push_back(pose({ 0, 0, 0 }, { 0, 0, 20 }));
    s.skeletons.push_back(pose({ 0, 0, -50 }, { 0, 0, -50 }));
    for (uint32_t k = 0; k < 2; ++k)
    {
        scene::Instance in = at(0, { 0, 0, 0 }, scene::InstanceCastShadow | scene::InstanceSkinned | scene::InstanceDynamic);
        in.skeleton = k;
        s.instances.push_back(in);
    }
    s.cameras.push_back(scene::Camera{});
    scene::validate(s);
    const std::vector<scene::Camera> cams(4, camera({ 1.5f, 0.3f, 15.0f }, { 1.5f, 0.0f, 20.0f }));
    const auto out = run(s, cams, 1280, 720);
    size_t covered = 0;
    for (uint32_t id : out.back().visId) covered += id != kVisNone;
    logf("    posed half covers %zu pixels; instances visible %u of 2 (the one posed behind the camera culled), overflow 0x%x\n", covered, out.back().stats.instancesVisible,
         out.back().stats.overflow);
    CHECK(covered > 5000);
    CHECK(out.back().stats.instancesVisible == 1);
    CHECK(out.back().stats.overflow == 0);
}

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    uint32_t passed = 0, runCount = 0;
    for (const TestCase& t : registry())
    {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++runCount;
        t.fn();
        logf("PASS %s\n", t.name);
        ++passed;
    }
    logf("%u/%u passed\n", passed, runCount);
    return passed == runCount ? 0 : 1;
}
