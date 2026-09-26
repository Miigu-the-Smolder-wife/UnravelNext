// C4 blend shapes and vertex animation on the GPU (render C), real device with the debug layer:
//   blend_shapes_match_reference   a box whose +x face moves 3 m out (shape 0) and whose top bulges (shape 1, with normal
//                                  offsets) drawn with weights (0.7, 1.3) gives bit-identical depth to the same box
//                                  morphed on the CPU by scene::evaluateMorph; the camera sees only the moved face, whose
//                                  bind-pose bounds lie outside the view (the culling inflation keeps it); after
//                                  GpuScene::setMorph to (1.0, -0.4) the frame equals the reference with those weights
//   vertex_animation_matches       a 4-frame vertex animation at t between frames 1 and 2 equals the CPU interpolation
//                                  (depth relative difference <= 1e-5: float frame arithmetic on the GPU)
//   unx_test_visibility_morphtests [filter]
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
#include <exception>
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
                m.indices.insert(m.indices.end(), { p, q, t, p, t, r });
            }
    }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), 0 });
    return m;
}

// The same mesh with its morph applied on the CPU (and an empty blend shape, so the cluster builder treats both alike:
// source clusters only).
scene::Mesh baked(const scene::Mesh& m, const std::vector<float>& weights, float time)
{
    scene::Mesh b = m;
    for (uint32_t v = 0; v < (uint32_t)m.positions.size(); ++v) scene::evaluateMorph(m, weights, time, v, b.positions[v], b.normals[v]);
    b.blendShapes = { scene::BlendShape{ "none", {}, {}, {} } };
    b.vertexAnimation = {};
    return b;
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

struct FrameOut
{
    std::vector<float> depth;
    std::vector<uint32_t> visId;
};

std::vector<FrameOut> run(const scene::Scene& s, const scene::Camera& cam, uint32_t frames, const std::function<void(uint32_t, GpuScene&)>& edit)
{
    const uint32_t width = 1280, height = 720;
    const QualityConfig q = quality();
    GpuScene gs(device());
    gs.upload(s);
    gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));
    FrameRenderer renderer(device(), shaders(), q, gs, 2);
    RenderGraph graph(device());
    ComPtr<ID3D12Resource> depthRb = readbackBuffer((uint64_t)rowPitch(width) * height), visRb = readbackBuffer((uint64_t)rowPitch(width) * height);
    std::vector<FrameOut> out;
    const float4x4 vp = ViewDesc::fromCamera(cam, width, height, {}).viewProj;
    for (uint32_t f = 0; f < frames; ++f)
    {
        edit(f, gs);
        FrameContext fr;
        fr.frameIndex = f;
        fr.time = f / 60.0;
        fr.deltaTime = 1.0f / 60;
        fr.mainView = ViewDesc::fromCamera(cam, width, height, vp);
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
        fo.depth.resize((size_t)width * height);
        fo.visId.resize((size_t)width * height);
        for (auto [rb, dst] : { std::pair<ID3D12Resource*, void*>{ d, fo.depth.data() }, std::pair<ID3D12Resource*, void*>{ v, fo.visId.data() } })
        {
            uint8_t* p = nullptr;
            check(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
            for (uint32_t y = 0; y < height; ++y) std::memcpy((uint8_t*)dst + (size_t)y * width * 4, p + (size_t)y * rowPitch(width), (size_t)width * 4);
            rb->Unmap(0, nullptr);
        }
        out.push_back(std::move(fo));
    }
    device().deferRelease(depthRb);
    device().deferRelease(visRb);
    return out;
}

scene::Scene oneInstance(const scene::Mesh& m, const std::vector<float>& weights, float time)
{
    scene::Scene s;
    s.name = "c4";
    s.materials.resize(1);
    s.meshes.push_back(m);
    scene::Instance in;
    in.mesh = 0;
    in.transform.m[0][3] = 0, in.transform.m[1][3] = 1, in.transform.m[2][3] = 6;
    in.blendWeights = weights;
    in.vertexAnimationTime = time;
    s.instances.push_back(in);
    s.cameras.push_back(scene::Camera{});
    scene::validate(s);
    return s;
}

struct Diff
{
    size_t covered = 0, coverage = 0, depthExact = 0;
    double maxRel = 0;
};
Diff compare(const FrameOut& a, const FrameOut& b)
{
    Diff d;
    for (size_t i = 0; i < a.depth.size(); ++i)
    {
        const bool ca = a.visId[i] != kVisNone, cb = b.visId[i] != kVisNone;
        d.coverage += ca != cb;
        if (!ca || !cb) continue;
        ++d.covered;
        d.depthExact += a.depth[i] == b.depth[i];
        d.maxRel = std::max(d.maxRel, std::fabs((double)a.depth[i] - b.depth[i]) / std::max(a.depth[i], 1e-30f));
    }
    return d;
}
} // namespace

UNX_TEST(blend_shapes_match_reference)
{
    scene::Mesh m = box({ 0.5f, 0.5f, 0.5f }, 16);
    scene::BlendShape pushOut{ "push +x face", {}, {}, {} }, bulge{ "bulge top", {}, {}, {} };
    for (uint32_t v = 0; v < (uint32_t)m.positions.size(); ++v)
    {
        const float3 p = m.positions[v];
        if (p.x > 0.499f)
        {
            pushOut.vertices.push_back(v);
            pushOut.deltaPositions.push_back({ 3.0f, 0.0f, 0.0f });
        }
        if (p.y > 0.499f)
        {
            bulge.vertices.push_back(v);
            const float r2 = p.x * p.x + p.z * p.z;
            bulge.deltaPositions.push_back({ 0.0f, 0.4f * (0.5f - r2), 0.0f });
            bulge.deltaNormals.push_back({ -0.8f * p.x, 0.0f, -0.8f * p.z });
        }
    }
    m.blendShapes = { pushOut, bulge };
    const std::vector<float> w0 = { 0.7f, 1.3f }, w1 = { 1.0f, -0.4f };
    // The camera looks at the pushed-out face (about x = 3.5, bind pose at x = 0.5 is left of the view).
    const scene::Camera cam = camera({ 4.8f, 2.2f, 3.2f }, { 3.3f, 1.2f, 6.0f });
    const auto morph = run(oneInstance(m, w0, 0), cam, 6, [&](uint32_t f, GpuScene& gs) {
        if (f == 3) gs.setMorph(f, 0, w1, 0);
    });
    const auto ref0 = run(oneInstance(baked(m, w0, 0), {}, 0), cam, 2, [](uint32_t, GpuScene&) {});
    const auto ref1 = run(oneInstance(baked(m, w1, 0), {}, 0), cam, 2, [](uint32_t, GpuScene&) {});
    const Diff a = compare(morph[2], ref0.back()), b = compare(morph[3], ref1.back()), c = compare(morph[5], ref1.back());
    logf("    weights (0.7, 1.3): %zu covered pixels, %zu depth-identical, %zu coverage differences; after setMorph (1.0, -0.4): %zu / %zu / %zu; "
         "settled: %zu / %zu / %zu\n",
         a.covered, a.depthExact, a.coverage, b.covered, b.depthExact, b.coverage, c.covered, c.depthExact, c.coverage);
    CHECK(a.covered > 50000 && a.depthExact == a.covered && a.coverage == 0);
    CHECK(b.covered > 50000 && b.depthExact == b.covered && b.coverage == 0);
    CHECK(c.depthExact == c.covered && c.coverage == 0);
}

UNX_TEST(vertex_animation_matches)
{
    scene::Mesh m = box({ 0.5f, 0.5f, 0.5f }, 12);
    m.vertexAnimation.framesPerSecond = 10;
    m.vertexAnimation.frameCount = 4;
    m.vertexAnimation.loop = true;
    for (uint32_t f = 0; f < 4; ++f)
        for (const float3& p : m.positions)
            m.vertexAnimation.positions.push_back({ p.x * (1 + 0.2f * f), p.y + 0.3f * f * p.x, p.z });
    const float t = 0.137f;  // between frames 1 and 2
    const scene::Camera cam = camera({ 1.8f, 2.0f, 3.5f }, { 0.0f, 1.2f, 6.0f });
    const auto vat = run(oneInstance(m, {}, t), cam, 3, [](uint32_t, GpuScene&) {});
    const auto ref = run(oneInstance(baked(m, {}, t), {}, 0), cam, 3, [](uint32_t, GpuScene&) {});
    const Diff d = compare(vat.back(), ref.back());
    logf("    t = %.3f s: %zu covered pixels, %zu depth-identical, max relative depth difference %.2e, %zu coverage differences\n", t, d.covered, d.depthExact, d.maxRel,
         d.coverage);
    CHECK(d.covered > 50000 && d.maxRel <= 1e-5 && d.coverage <= d.covered / 1000);
}

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    uint32_t passed = 0, runCount = 0;
    for (const TestCase& t : registry())
    {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++runCount;
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
    logf("%u/%u passed\n", passed, runCount);
    return passed == runCount ? 0 : 1;
}
