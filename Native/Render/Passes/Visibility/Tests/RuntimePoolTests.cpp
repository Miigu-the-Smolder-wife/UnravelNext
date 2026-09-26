// C2b runtime mesh / instance pool (render C; FEATURES_GAME 2.1 CARVE fragments), real device with the debug layer:
//   runtime_geometry_matches_upload   a wall uploaded as the scene; at frame 3, 30 fragment meshes (each built on its own
//                                     with clusterbuilder, no simplification) and 60 instances of them are added with
//                                     GpuScene::addRuntimeMesh / addRuntimeInstance (no scene revision): V depth is bit-
//                                     identical to a scene that uploaded the same meshes and instances; after removing them
//                                     the frame equals the wall alone; after the release delay the pool takes them again
//                                     (freed ranges reused) and the frame again equals the uploaded scene
//   unx_test_visibility_runtimepooltests
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

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
QualityConfig quality() { return QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality"); }

scene::Mesh box(float3 half, uint32_t s, float3 jitterSeed = {})
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
                float3 p = n * extent(n) + u * fu + v * fv;
                p = p * (1.0f + 0.2f * std::sin(p.x * 7 + jitterSeed.x) * std::cos(p.z * 5 + jitterSeed.y));  // irregular chunk
                m.positions.push_back(p);
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

scene::Instance at(uint32_t mesh, float3 p)
{
    scene::Instance i;
    i.mesh = mesh;
    i.flags |= scene::InstanceDynamic;
    i.transform.m[0][3] = p.x, i.transform.m[1][3] = p.y, i.transform.m[2][3] = p.z;
    return i;
}

struct Content
{
    std::vector<scene::Mesh> fragments;
    std::vector<std::pair<uint32_t, float3>> placements;  // fragment, position
};

Content fragments()
{
    Content c;
    std::mt19937 rng(4);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int k = 0; k < 30; ++k) c.fragments.push_back(box({ 0.2f + 0.1f * u(rng), 0.15f + 0.05f * u(rng), 0.2f }, 4 + k % 3, { (float)k, (float)(2 * k) }));
    for (int k = 0; k < 60; ++k) c.placements.push_back({ (uint32_t)(k % 30), { 3.0f * u(rng), 0.6f + 1.2f * (u(rng) + 1), 5.0f + 0.5f * u(rng) } });
    return c;
}

scene::Scene wallScene()
{
    scene::Scene s;
    s.name = "c2b";
    s.materials.resize(1);
    s.meshes.push_back(box({ 5, 3, 0.25f }, 8));
    scene::Instance w;
    w.mesh = 0;
    w.transform.m[1][3] = 3, w.transform.m[2][3] = 8;
    s.instances.push_back(w);
    s.cameras.push_back(scene::Camera{});
    scene::validate(s);
    return s;
}

uint32_t rowPitch(uint32_t width) { return (width * 4 + 255) / 256 * 256; }

using Edit = std::function<void(uint32_t, GpuScene&)>;
// GPU-written instances of a frame (A3 mesh particles' path): copied into GpuScene::gpuInstanceRange after the scene
// update, as a writer pass would.
using GpuWriter = std::function<std::vector<gpu::Instance>(uint32_t)>;
std::vector<std::vector<float>> run(const scene::Scene& s, bool pool, uint32_t frames, const Edit& edit, const GpuWriter& gpuWriter = {})
{
    const uint32_t width = 1280, height = 720;
    const QualityConfig q = quality();
    GpuScene gs(device());
    if (pool)
    {
        RuntimeCapacity c;
        c.meshes = 64, c.submeshes = 64, c.vertices = 1 << 16, c.indices = 3 << 16, c.clusters = 4096, c.clusterVertexIndices = 1 << 18, c.clusterTriangles = 1 << 18;
        c.nodes = 4096, c.instances = 256;
        c.gpuInstances = gpuWriter ? 256 : 0;
        gs.reserveRuntime(c);
    }
    gs.upload(s);
    clusterbuilder::Settings cs = clusterbuilder::Settings::fromQuality(q);
    cs.noSimplification = true;  // static and runtime fragments alike (same triangulation)
    gs.setClusters(clusterbuilder::build(s, cs));
    FrameRenderer renderer(device(), shaders(), q, gs, 2);
    RenderGraph graph(device());
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (uint64_t)rowPitch(width) * height;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    check(device().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    scene::Camera cam;
    cam.position = { 0.5f, 2.2f, -1.0f };
    cam.forward = normalize(float3{ 0, -0.1f, 1 });
    cam.nearPlane = 0.05f;
    const float4x4 vp = ViewDesc::fromCamera(cam, width, height, {}).viewProj;
    std::vector<std::vector<float>> out;
    for (uint32_t f = 0; f < frames; ++f)
    {
        logf("    frame %u (pool %d)\n", f, pool ? 1 : 0);
        edit(f, gs);
        FrameContext fr;
        fr.frameIndex = f;
        fr.time = f / 60.0;
        fr.deltaTime = 1.0f / 60;
        fr.mainView = ViewDesc::fromCamera(cam, width, height, vp);
        TextureRef output = graph.createTexture({ "test output", width, height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
        const ViewResources main = renderer.record(graph, fr, output);
        if (gpuWriter)
        {
            // After record (the scene update was submitted there), before the graph: records + count by copies.
            const std::vector<gpu::Instance> records = gpuWriter(f);
            const GpuScene::GpuInstanceRange range = gs.gpuInstanceRange();
            CHECK(range.capacity == 256 && records.size() <= range.capacity);
            D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC1 ud = rd;
            ud.Width = records.size() * sizeof(gpu::Instance) + 16;
            ud.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> upload;
            check(device().d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &ud, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&upload)), "upload");
            uint8_t* m = nullptr;
            check(upload->Map(0, nullptr, reinterpret_cast<void**>(&m)), "map upload");
            const uint32_t count[4] = { (uint32_t)records.size(), 0, 0, 0 };
            std::memcpy(m, count, 16);
            if (!records.empty()) std::memcpy(m + 16, records.data(), records.size() * sizeof(gpu::Instance));
            upload->Unmap(0, nullptr);
            CommandList cl = device().acquireCommandList(QueueType::Graphics);
            ID3D12GraphicsCommandList7* cmd = cl.list.Get();
            D3D12_BUFFER_BARRIER toCopy[2], toRead[2];
            ID3D12Resource* targets[2] = { range.instanceBuffer, range.countBuffer };
            for (int k = 0; k < 2; ++k)
            {
                toCopy[k] = { D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_COPY_DEST, targets[k], 0, UINT64_MAX };
                toRead[k] = { D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, targets[k], 0, UINT64_MAX };
            }
            D3D12_BARRIER_GROUP g{ D3D12_BARRIER_TYPE_BUFFER, 2 };
            g.pBufferBarriers = toCopy;
            cmd->Barrier(1, &g);
            cmd->CopyBufferRegion(range.countBuffer, range.countByteOffset, upload.Get(), 0, 16);
            if (!records.empty())
                cmd->CopyBufferRegion(range.instanceBuffer, (uint64_t)range.first * sizeof(gpu::Instance), upload.Get(), 16, records.size() * sizeof(gpu::Instance));
            g.pBufferBarriers = toRead;
            cmd->Barrier(1, &g);
            const uint64_t fence = device().submit(cl);
            for (uint32_t queue = 0; queue < kQueueTypeCount; ++queue)
                if (queue != (uint32_t)QueueType::Graphics) device().queue((QueueType)queue).waitGpu(device().queue(QueueType::Graphics), fence);
            device().deferRelease(upload);
        }
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
        out.push_back(std::move(d));
    }
    device().deferRelease(rb);
    return out;
}

size_t differing(const std::vector<float>& a, const std::vector<float>& b)
{
    size_t n = 0;
    for (size_t i = 0; i < a.size(); ++i) n += a[i] != b[i];
    return n;
}
} // namespace

int main()
{
    try
    {
        const Content c = fragments();
        // Reference: fragments uploaded with the scene.
        scene::Scene ref = wallScene();
        const uint32_t firstMesh = (uint32_t)ref.meshes.size();
        for (const scene::Mesh& m : c.fragments) ref.meshes.push_back(m);
        for (const auto& [frag, p] : c.placements) ref.instances.push_back(at(firstMesh + frag, p));
        scene::validate(ref);
        const auto reference = run(ref, false, 3, [](uint32_t, GpuScene&) {});
        const auto wall = run(wallScene(), false, 3, [](uint32_t, GpuScene&) {});
        // Runtime path.
        const QualityConfig q = quality();
        clusterbuilder::Settings cs = clusterbuilder::Settings::fromQuality(q);
        cs.noSimplification = true;
        std::vector<uint32_t> meshes, instances;
        auto add = [&](GpuScene& gs) {
            meshes.clear();
            instances.clear();
            for (const scene::Mesh& m : c.fragments)
            {
                scene::Scene one;
                one.materials.resize(1);
                one.meshes.push_back(m);
                const uint32_t mi = gs.addRuntimeMesh(m, clusterbuilder::build(one, cs));
                CHECK(mi != gpu::kNone);
                meshes.push_back(mi);
            }
            for (const auto& [frag, p] : c.placements) instances.push_back(gs.addRuntimeInstance(at(meshes[frag], p)));
        };
        auto remove = [&](GpuScene& gs) {
            for (uint32_t i : instances) gs.removeRuntimeInstance(i);
            for (uint32_t m : meshes) gs.removeRuntimeMesh(m);
        };
        std::vector<uint32_t> firstMeshes;
        const auto frames = run(wallScene(), true, 14, [&](uint32_t f, GpuScene& gs) {
            if (f == 3) add(gs), firstMeshes = meshes;
            if (f == 6) remove(gs);
            if (f == 10) add(gs);  // after the release delay: the freed ranges come back
        });
        const size_t before = differing(frames[2], wall.back()), added = differing(frames[4], reference.back()), gone = differing(frames[8], wall.back()),
                     again = differing(frames[12], reference.back()), withFragments = differing(reference.back(), wall.back());
        logf("    fragments change %zu pixels; differences vs the uploaded scene: before %zu (vs wall), added %zu, removed %zu (vs wall), added again %zu; "
             "mesh slots reused: %s\n",
             withFragments, before, added, gone, again, meshes == firstMeshes ? "yes" : "no");
        CHECK(withFragments > 20000 && before == 0 && added == 0 && gone == 0 && again == 0 && meshes == firstMeshes);
        logf("PASS runtime_geometry_matches_upload\n");

        // GPU-written instances (A3 mesh particles): the fragments' meshes uploaded without instances, their instances
        // written on the GPU each frame (frames 3-5: all 60; frames 6-7: none; frames 8-9: all again). Same depth as the
        // uploaded instances, and the wall alone when the count is 0.
        scene::Scene meshesOnly = wallScene();
        for (const scene::Mesh& m : c.fragments) meshesOnly.meshes.push_back(m);
        scene::validate(meshesOnly);
        std::vector<gpu::Instance> records;
        for (const auto& [frag, p] : c.placements)
        {
            gpu::Instance g{};
            const scene::Instance in = at(firstMesh + frag, p);
            for (int r = 0; r < 3; ++r)
            {
                g.objectToWorld[r] = { in.transform.m[r][0], in.transform.m[r][1], in.transform.m[r][2], in.transform.m[r][3] };
                g.prevObjectToWorld[r] = g.objectToWorld[r];
            }
            g.mesh = in.mesh;
            g.flags = in.flags;
            g.materialRemap = g.bonePalette = g.morph = g.patch = gpu::kNone;
            records.push_back(g);
        }
        const auto written = run(meshesOnly, true, 10, [](uint32_t, GpuScene&) {}, [&](uint32_t f) {
            return (f >= 3 && f <= 5) || f >= 8 ? records : std::vector<gpu::Instance>{};
        });
        const size_t gpuAdded = differing(written[5], reference.back()), gpuNone = differing(written[7], wall.back()), gpuAgain = differing(written[9], reference.back());
        logf("    GPU-written instances: differences vs the uploaded scene %zu, with count 0 vs wall %zu, again %zu\n", gpuAdded, gpuNone, gpuAgain);
        CHECK(gpuAdded == 0 && gpuNone == 0 && gpuAgain == 0);
        logf("PASS gpu_written_instances_match_upload\n2/2 passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL runtime_geometry_matches_upload: %s\n0/1 passed\n", e.what());
        return 1;
    }
}
