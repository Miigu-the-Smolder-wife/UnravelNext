// C9 origin rebase (render C; request 20260926_C_origin_rebase.md), real device with the debug layer:
//   rebase_is_seamless   40 boxes and a ground slab about 5 km from the origin, a moving camera; at frame 6 the origin
//                        moves by (4096, 0, 2048) m (GpuScene::rebase + FrameContext::originShift + the camera in the
//                        new coordinates). Every later frame's V depth is bit-identical to a run that used the new
//                        coordinates from the start (the same float translations), and no instance gets motion
//                        (previous transforms moved with the current ones).
//   unx_test_visibility_originrebasetests [filter]
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"
#include "unx/visibility/Visibility.h"

#include <cmath>
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

scene::Scene makeScene(float3 offset)
{
    scene::Scene s;
    s.name = "c9";
    s.materials.resize(1);
    s.meshes.push_back(box({ 200, 0.5f, 200 }, 8));
    s.meshes.push_back(box({ 1.5f, 1.5f, 1.5f }, 4));
    auto at = [&](uint32_t mesh, float3 p) {
        scene::Instance i;
        i.mesh = mesh;
        i.transform.m[0][3] = p.x - offset.x;  // float subtraction, as GpuScene::rebase does
        i.transform.m[1][3] = p.y - offset.y;
        i.transform.m[2][3] = p.z - offset.z;
        s.instances.push_back(i);
    };
    at(0, { 5120.25f, -0.5f, 3072.75f });
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> u(-60.0f, 60.0f);
    for (int k = 0; k < 40; ++k) at(1, { 5120.25f + u(rng), 1.5f, 3072.75f + u(rng) });
    s.cameras.push_back(scene::Camera{});
    scene::validate(s);
    return s;
}

std::vector<std::vector<float>> run(const scene::Scene& s, float3 cameraOffset, int rebaseFrame, float3 shift)
{
    const uint32_t width = 1280, height = 720, frames = 12;
    const QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    GpuScene gs(device());
    gs.upload(s);
    gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));
    FrameRenderer renderer(device(), shaders(), q, gs, 2);
    RenderGraph graph(device());
    ComPtr<ID3D12Resource> rb = readbackBuffer((uint64_t)rowPitch(width) * height);
    std::vector<std::vector<float>> depth;
    float4x4 prev{};
    float3 offset = cameraOffset;
    for (uint32_t f = 0; f < frames; ++f)
    {
        FrameContext fr;
        fr.frameIndex = f;
        fr.time = f / 60.0;
        fr.deltaTime = 1.0f / 60;
        if ((int)f == rebaseFrame)
        {
            gs.rebase(shift);
            fr.originShift = shift;
            offset = offset + shift;
        }
        const float t = f / 11.0f;
        scene::Camera c;
        const float3 world{ 5120.25f - 40 + 60 * t, 6.0f, 3072.75f - 70 + 10 * t };
        c.position = world - offset;
        c.forward = normalize(float3{ 0.3f - 0.5f * t, -0.12f, 1.0f });
        c.nearPlane = 0.05f;
        fr.mainView = ViewDesc::fromCamera(c, width, height, f == 0 ? float4x4{} : prev);
        if (f == 0) fr.mainView.prevViewProj = fr.mainView.viewProj;
        TextureRef output = graph.createTexture({ "test output", width, height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
        const ViewResources main = renderer.record(graph, fr, output);
        ID3D12Resource* r = rb.Get();
        graph.addPass("test.readback", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(main.depth, Use::CopySrc);
                          b.keep();
                      },
                      [=](PassContext& ctx) {
                          D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                          to.pResource = r;
                          to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                          to.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, width, height, 1, rowPitch(width) };
                          from.pResource = ctx.resource(main.depth);
                          from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                          ctx.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                      });
        graph.execute(nullptr);
        device().waitIdle();
        std::vector<float> d((size_t)width * height);
        uint8_t* p = nullptr;
        check(r->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
        for (uint32_t y = 0; y < height; ++y) std::memcpy(&d[(size_t)y * width], p + (size_t)y * rowPitch(width), (size_t)width * 4);
        r->Unmap(0, nullptr);
        depth.push_back(std::move(d));
        prev = fr.mainView.viewProj;
        // No instance moved: previous transforms equal current ones everywhere.
        for (const gpu::Instance& g : gs.instances())
            CHECK(std::memcmp(g.objectToWorld, g.prevObjectToWorld, sizeof g.objectToWorld) == 0);
    }
    device().deferRelease(rb);
    return depth;
}
} // namespace

int main()
{
    try
    {
        const float3 shift{ 4096, 0, 2048 };
        const auto rebased = run(makeScene({}), {}, 6, shift);
        const auto fresh = run(makeScene(shift), shift, -1, {});
        size_t covered = 0, differing = 0;
        for (size_t f = 7; f < rebased.size(); ++f)
            for (size_t i = 0; i < rebased[f].size(); ++i)
            {
                covered += rebased[f][i] != 0;
                differing += rebased[f][i] != fresh[f][i];
            }
        logf("    frames 7-11 after the rebase at frame 6: %zu covered pixels, %zu differ from the run in the new coordinates\n", covered, differing);
        CHECK(covered > 1000000 && differing == 0);
        logf("PASS rebase_is_seamless\n1/1 passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL rebase_is_seamless: %s\n0/1 passed\n", e.what());
        return 1;
    }
}
