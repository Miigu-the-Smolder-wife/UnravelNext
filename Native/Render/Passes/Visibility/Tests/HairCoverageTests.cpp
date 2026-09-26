// B10 strand hair in V's coverage layer (render C; E's request 20260926_E_hair_strands, V part; INTERFACES v1.59), real
// device with the debug layer. A hair body of 8 straight guides (12 nodes, 2 cm segments, radius 1.5 mm root and tip) held
// at rest (no gravity, no wind: the simulation keeps the rest pose), one follow strand per guide on the guide itself, seen
// from 0.6 m through the whole renderer with visibility.coverage_layer and visibility.coverage_hair:
//   hair_records_are_exact   every hair record names a segment of the frame, its u lies in that segment's range, and the
//                            records' areas add up to the ribbons' exact projected areas (the CPU ribbon of each segment,
//                            as HairRaster.ms builds it) within the records' 10-bit area rounding.
// M reads hair records only when told to (it does not yet): the frame runs with shading.experiment_disable 8192 (M's
// allowance for the coverage layer next to S's shadows; not an image), since only V's records are checked here.
//   unx_test_visibility_haircoveragetests
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/hair/Hair.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuProfiler.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <cstring>
#include <exception>
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

ComPtr<ID3D12Resource> readbackBuffer(uint64_t bytes)
{
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(bytes, 256);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "readback");
    return r;
}

std::vector<uint32_t> readWords(ID3D12Resource* r, uint64_t words)
{
    std::vector<uint32_t> w(words);
    uint8_t* p = nullptr;
    check(r->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
    std::memcpy(w.data(), p, words * 4);
    r->Unmap(0, nullptr);
    return w;
}

// Screen position (pixels, y down) of a world point (row-major viewProj, column vector).
struct Pixel
{
    double x, y;
};
Pixel toPixel(const float4x4& vp, float3 p, uint32_t width, uint32_t height)
{
    double c[4];
    for (int r = 0; r < 4; ++r) c[r] = (double)vp.m[r][0] * p.x + (double)vp.m[r][1] * p.y + (double)vp.m[r][2] * p.z + vp.m[r][3];
    return { (c[0] / c[3] * 0.5 + 0.5) * width, (0.5 - c[1] / c[3] * 0.5) * height };
}
} // namespace

int main()
{
    try
    {
        const uint32_t width = 640, height = 360, N = 12, G = 8, S = N - 1;
        const float radius = 0.0015f, spacing = 0.02f;
        QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const char* o : { "visibility.coverage_layer = true", "visibility.coverage_hair = true", "visibility.occlusion_culling = false", "shading.experiment_disable = 8192" })
            q.applyOverride(o);
        // A small box far behind (the scene needs content and a material; nothing in front of the strands).
        scene::Scene s;
        s.name = "hair coverage";
        s.materials.resize(1);
        scene::Mesh box;
        box.name = "far box";
        for (int k = 0; k < 8; ++k) box.positions.push_back({ (k & 1) ? 0.1f : -0.1f, (k & 2) ? 0.1f : -0.1f, (k & 4) ? 0.1f : -0.1f });
        for (const float3& p : box.positions) box.normals.push_back(normalize(p));
        box.indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
        box.submeshes.push_back({ 0, (uint32_t)box.indices.size(), 0 });
        s.meshes.push_back(box);
        scene::Instance in;
        in.mesh = 0;
        in.transform.m[0][3] = 3, in.transform.m[1][3] = 1, in.transform.m[2][3] = 20;
        s.instances.push_back(in);
        scene::Camera cam;
        cam.name = "front";
        cam.position = { 0.11f, 1.035f, -0.6f };
        cam.forward = { 0, 0, 1 };
        s.cameras.push_back(cam);
        scene::validate(s);
        GpuScene gs(device());
        gs.upload(s);
        gs.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));
        FrameRenderer renderer(device(), shaders(), q, gs, 2);
        RenderGraph graph(device());

        // Guides along +x from the joint at (0, 1, 0), 1 cm apart in y; the follow strand of each guide is the guide.
        hair::HairSystem& hs = hair::hairSystem(renderer.trackState());
        hair::BodyDesc d;
        d.nodesPerStrand = N;
        d.joints = 1;
        for (uint32_t g = 0; g < G; ++g)
        {
            for (uint32_t i = 0; i < N; ++i) d.restPositions.push_back({ spacing * i, 0.01f * g, 0 });
            d.guideJoint.push_back(0);
            d.follows.push_back({ g, float3{ 0, 0, 0 }, 1.0f });
        }
        d.rootRadius = d.tipRadius = radius;
        d.params.gravity = { 0, 0, 0 };
        const uint32_t body = hs.addBody(d);
        float3x4 joint;
        joint.m[1][3] = 1;

        const uint64_t tileCount = (uint64_t)((width + 7) / 8) * ((height + 7) / 8);
        ComPtr<ID3D12Resource> tilesRb = readbackBuffer(tileCount * 32), recordsRb;
        uint64_t recordBytes = 0;
        std::vector<uint32_t> tiles, records;
        float4x4 vp{};
        const float4x4 first = ViewDesc::fromCamera(cam, width, height, {}).viewProj;
        // Worst GPU time of every pass over the frames, reported (the 2026-09-26 TDR ran this test beside a game; INTERFACES
        // 3.6 bounds each dispatch).
        GpuProfiler profiler(device(), 2, 4096);
        std::map<std::string, double> worstPass;
        double worstFrame = 0;
        auto collect = [&]() {
            if (const FrameTiming* t = profiler.lastCompleted())
            {
                worstFrame = std::max(worstFrame, t->gpuFrameMs);
                for (const PassTiming& pt : t->passes) worstPass[pt.name] = std::max(worstPass[pt.name], pt.durationMs());
            }
        };
        for (uint32_t f = 0; f < 4; ++f)
        {
            profiler.beginFrame(f);
            collect();
            hs.tick(body, &joint, 1, nullptr, 0, { 0, 0, 0 }, 1.0f / 60);
            FrameContext fr;
            fr.frameIndex = f;
            fr.time = f / 60.0;
            fr.deltaTime = 1.0f / 60;
            fr.mainView = ViewDesc::fromCamera(cam, width, height, first);
            vp = fr.mainView.viewProj;
            TextureRef output = graph.createTexture({ "test output", width, height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
            const ViewResources main = renderer.record(graph, fr, output);
            CHECK(main.coverageTiles.valid() && main.coverageRecords.valid());
            const uint64_t poolBytes = graph.desc(main.coverageRecords).size;
            if (poolBytes != recordBytes)
            {
                if (recordsRb) device().deferRelease(recordsRb);
                recordsRb = readbackBuffer(poolBytes);
                recordBytes = poolBytes;
            }
            ID3D12Resource *tl = tilesRb.Get(), *rc = recordsRb.Get();
            graph.addPass("test.readback", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(main.coverageTiles, Use::CopySrc);
                              b.use(main.coverageRecords, Use::CopySrc);
                              b.keep();
                          },
                          [=](PassContext& c) {
                              c.cmd->CopyBufferRegion(tl, 0, c.resource(main.coverageTiles), 0, tileCount * 32);
                              c.cmd->CopyBufferRegion(rc, 0, c.resource(main.coverageRecords), 0, poolBytes);
                          });
            graph.execute(&profiler);
            device().waitIdle();
            tiles = readWords(tl, tileCount * 8);
            records = readWords(rc, poolBytes / 4);
        }
        for (uint32_t f = 4; f < 6; ++f)
        {
            profiler.beginFrame(f);  // reads back frames 2 and 3
            collect();
        }
        {
            std::vector<std::pair<double, std::string>> byTime;
            for (const auto& [name, ms] : worstPass) byTime.push_back({ ms, name });
            std::sort(byTime.rbegin(), byTime.rend());
            logf("    GPU: worst frame %.3f ms over 4 frames; %zu passes; longest passes (worst of the frames):\n", worstFrame, byTime.size());
            for (size_t i = 0; i < byTime.size() && i < 12; ++i) logf("      %8.3f ms  %s\n", byTime[i].first, byTime[i].second.c_str());
            // A pass may hold many dispatches (S builds its multiple-scattering table in 32 slices); the per-dispatch bound
            // (INTERFACES 3.6) is judged from these with the pass split per dispatch (Results/C/Tdr).
        }

        // Expected: the ribbons of the rest pose (world = joint + rest position), built as HairRaster.ms builds them.
        const float3 eye = cam.position;
        double expectedArea = 0;
        for (uint32_t g = 0; g < G; ++g)
            for (uint32_t i = 0; i < S; ++i)
            {
                const float3 a{ spacing * i, 1 + 0.01f * g, 0 }, b{ spacing * (i + 1), 1 + 0.01f * g, 0 };
                const float3 ra = a - eye, rb = b - eye, dir = rb - ra, mid = (ra + rb) * 0.5f;
                const float3 side = normalize(cross(dir, mid));
                const float3 c[4] = { a - side * radius, a + side * radius, b + side * radius, b - side * radius };
                Pixel px[4];
                for (int k = 0; k < 4; ++k) px[k] = toPixel(vp, c[k], width, height);
                double area2 = 0;
                for (int k = 0; k < 4; ++k) area2 += px[k].x * px[(k + 1) % 4].y - px[(k + 1) % 4].x * px[k].y;
                expectedArea += std::fabs(area2) * 0.5;
            }
        // Records of the last frame: the listed tiles' ranges (tile header: records, base, listed index + 1).
        double area = 0;
        uint64_t hairRecords = 0, otherRecords = 0;
        uint32_t badSegment = 0, badU = 0;
        const uint32_t segments = G * S;  // follow strands only (one per guide), in body order
        for (uint64_t t = 0; t < tileCount; ++t)
        {
            const uint32_t count = tiles[8 * t], base = tiles[8 * t + 1], listed = tiles[8 * t + 2];
            if (listed == 0) continue;
            for (uint32_t r = 0; r < count; ++r)
            {
                const uint32_t* rec = &records[4 * (size_t)(base + r)];
                if ((rec[0] & 0x80000000u) == 0)
                {
                    ++otherRecords;
                    continue;
                }
                ++hairRecords;
                const uint32_t seg = rec[0] & 0x7FFFFFFFu;
                area += ((rec[3] >> 16) & 0x3FFu) / 1023.0;
                if (seg >= segments)
                {
                    ++badSegment;
                    continue;
                }
                const double u = (rec[3] & 0xFFFFu) / 65535.0, lo = (double)(seg % S) / S, hi = (double)(seg % S + 1) / S;
                if (u < lo - 2e-4 || u > hi + 2e-4) ++badU;
            }
        }
        const double rounding = hairRecords * 0.5 / 1023.0;
        logf("    %llu hair records (%llu others), area %.4f px^2 vs exact ribbons %.4f px^2 (rounding bound %.4f); %u records off the frame's segments, %u with u "
             "outside their segment\n",
             (unsigned long long)hairRecords, (unsigned long long)otherRecords, area, expectedArea, rounding, badSegment, badU);
        CHECK(hairRecords > 100 && badSegment == 0 && badU == 0);
        CHECK(std::fabs(area - expectedArea) <= rounding + 1e-3 * expectedArea);
        logf("PASS hair_records_are_exact\n1/1 passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAIL hair_records_are_exact: %s\n0/1 passed\n", e.what());
        return 1;
    }
}
