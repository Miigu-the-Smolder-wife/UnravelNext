// GI correctness against analytic answers (R track, ARCHITECTURE 2.5, quality definition section 3 "GI"):
//  1. White furnace: a closed box whose inner walls emit Le and reflect albedo rho. Radiance is uniform, L = Le / (1 - rho),
//     so the irradiance on every wall is E = pi Le / (1 - rho). Exercises multi-bounce through cached hit irradiance,
//     the hash grid over several cell levels, texel stratification, SH projection, probe gather and interpolation.
//  2. Open sky: a ground plane (albedo 0.5) under constant sky radiance L. The plane cannot see itself: E = pi L.
// Each runs the frame path (RayScene::record -> ray-traced primary visibility standing in for V/M -> GiSystem::record)
// for N frames and evaluates screenProbeIrradiance (M's API) at every probe pixel. Also reports the frames needed to
// come within 1 % (reconvergence, gi.relight_frames_max).
//
//   unx_test_gi_gianalytic [--frames N] [--validate]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/gi/GiSystem.h"
#include "unx/render/GpuScene.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace unx;
using namespace unx::render;

namespace
{
constexpr float kPi = 3.14159265358979f;

// Axis-aligned box whose faces point inward (CCW seen from inside).
void addInwardBox(scene::Mesh& mesh, float3 lo, float3 hi)
{
    const float3 n[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int f = 0; f < 6; ++f)
    {
        const float3 outward = n[f];
        const float3 inward = -outward;
        const float3 u = std::fabs(outward.y) > 0.5f ? float3{ 1, 0, 0 } : float3{ 0, 1, 0 };
        const float3 v = cross(outward, u);
        const float3 c = (lo + hi) * 0.5f, h = (hi - lo) * 0.5f;
        auto corner = [&](float a, float b) {
            const float3 p = outward + u * a + v * b;
            return float3{ c.x + p.x * h.x, c.y + p.y * h.y, c.z + p.z * h.z };
        };
        const uint32_t base = (uint32_t)mesh.positions.size();
        for (auto [a, b] : { std::pair{ -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } })
        {
            mesh.positions.push_back(corner(a, b));
            mesh.normals.push_back(inward);
            mesh.uv0.push_back({ a * 0.5f + 0.5f, b * 0.5f + 0.5f });
        }
        mesh.indices.insert(mesh.indices.end(), { base, base + 2, base + 1, base, base + 3, base + 2 });  // reversed: CCW from inside
    }
}

scene::Scene furnace(float emission, float albedo)
{
    scene::Scene s;
    s.name = "gi_furnace";
    scene::Material wall;
    wall.name = "wall";
    wall.baseColor = { albedo, albedo, albedo };
    wall.emissive = { emission, emission, emission };
    s.materials.push_back(wall);
    scene::Mesh box;
    box.name = "room";
    addInwardBox(box, { -6, 0, -6 }, { 6, 5, 6 });
    box.submeshes.push_back({ 0, (uint32_t)box.indices.size(), 0 });
    s.meshes.push_back(box);
    s.instances.push_back({});
    s.instances.back().mesh = 0;
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "inside";
    cam.position = { 0, 2.5f, 4.5f };
    cam.forward = normalize(float3{ 0.3f, -0.35f, -1 });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

scene::Scene openSky(float albedo)
{
    scene::Scene s;
    s.name = "gi_open_sky";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { albedo, albedo, albedo };
    s.materials.push_back(ground);
    scene::Mesh plane;
    plane.name = "ground";
    for (auto [x, z] : { std::pair{ -200.f, -200.f }, { 200.f, -200.f }, { 200.f, 200.f }, { -200.f, 200.f } })
    {
        plane.positions.push_back({ x, 0, z });
        plane.normals.push_back({ 0, 1, 0 });
        plane.uv0.push_back({ x, z });
    }
    plane.indices = { 0, 2, 1, 0, 3, 2 };
    plane.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(plane);
    s.instances.push_back({});
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "above";
    cam.position = { 0, 3, 0 };
    cam.forward = normalize(float3{ 0, -0.6f, -1 });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

struct Buffer
{
    ComPtr<ID3D12Resource> resource;
};

Buffer createBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, bool uav)
{
    Buffer b;
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (uav) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&b.resource)), "buffer");
    return b;
}

struct Outcome
{
    double mean = 0, minimum = 0, maximum = 0, worst = 0;  // worst = max |E / expected - 1| over valid probes
    double radianceMean = 0, radianceWorst = 0;              // screenProbeRadiance against the uniform radiance
    uint32_t probes = 0;
    int converged = -1;  // first frame whose mean is within 1 %
    gi::GiStats stats;
};

Outcome run(Device& device, ShaderLibrary& shaders, const QualityConfig& quality, const scene::Scene& s, float3 sky, double expected, double expectedRadiance,
            uint32_t frames, uint32_t width, uint32_t height)
{
    GpuScene gpuScene(device);
    gpuScene.upload(s);
    Outcome out;
    {
        TrackState state;
        RenderGraph graph(device);
        const ViewDesc view = ViewDesc::fromCamera(s.cameras[0], width, height, float4x4{});
        Buffer constants = createBuffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD, false);
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(constants.resource->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map constants");
        const uint32_t probesX = (width + 7) / 8, probesY = (height + 7) / 8;
        const uint64_t resultBytes = (uint64_t)probesX * probesY * 32;
        Buffer result = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_DEFAULT, true);
        Buffer readback = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_READBACK, false);
        std::vector<float> values((size_t)probesX * probesY * 8);
        gi::GiSystem* giSystem = nullptr;

        for (uint32_t f = 0; f < frames; ++f)
        {
            FrameContext frame;
            frame.frameIndex = f;
            frame.time = f / 60.0;
            frame.deltaTime = 1 / 60.0f;
            frame.mainView = view;
            FrameResources resources;
            FrameServices services;
            auto frameConstantsFor = [&](const ViewDesc& v) {
                gpu::FrameConstants c{};
                c.viewProj = v.viewProj;
                c.prevViewProj = v.prevViewProj;
                c.invViewProj = v.invViewProj;
                c.view = v.view;
                c.proj = v.proj;
                c.cameraPosition = v.position;
                c.nearPlane = v.nearPlane;
                c.viewWidth = v.width;
                c.viewHeight = v.height;
                c.frameIndex = f;
                c.time = (float)frame.time;
                c.deltaTime = frame.deltaTime;
                c.exposure = 1.0f;
                c.tanHalfFovY = std::tan(v.verticalFov * 0.5f);
                c.sunDirection = s.sun.direction;
                c.sunIlluminance = s.sun.illuminance;
                c.sunColor = s.sun.color;
                c.sunAngularRadius = s.sun.angularRadius;
                gpuScene.fill(c);
                std::memcpy(mapped, &c, sizeof c);
                return constants.resource->GetGPUVirtualAddress();
            };
            FramePassContext fc{ device, graph, shaders, quality, gpuScene, frame, resources, services, frameConstantsFor, &state };
            ViewResources main;
            main.view = view;
            main.frameConstants = frameConstantsFor(view);
            main.depth = graph.createTexture({ "test depth", width, height, 1, 1, DXGI_FORMAT_R32_FLOAT });
            main.gbuffer = graph.createTexture({ "test gbuffer", width, height, 1, 1, DXGI_FORMAT_R32G32_UINT });
            rt::RayScene& rays = rt::RayScene::get(fc);
            rays.record(fc);
            uint32_t scene[8];
            rays.rootConstants(scene);
            rt::RayPipeline& primary = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline("Passes/GI/Tests/GiTestPrimary", { "GiTestPrimaryGen" }));
            const TextureRef depth = main.depth, gbuffer = main.gbuffer;
            const D3D12_GPU_VIRTUAL_ADDRESS fcAddress = main.frameConstants;
            graph.addPass("test.primary", QueueType::Compute,
                          [&](PassBuilder& b) {
                              b.use(depth, Use::UavGraphics);
                              b.use(gbuffer, Use::UavGraphics);
                              rays.declareTraversal(b);
                          },
                          [&, depth, gbuffer, scene, fcAddress](PassContext& c) {
                              uint32_t k[32] = {};
                              k[0] = c.uav(depth);
                              k[1] = c.uav(gbuffer);
                              std::memcpy(&k[24], scene, sizeof scene);
                              c.computeConstants(k, 32);
                              c.bindFrameConstants(fcAddress);
                              primary.dispatch(c.cmd, 0, width, height, 1);
                          });
            gi::GiSystem& gi = gi::GiSystem::get(fc);
            giSystem = &gi;
            gi.setConstantSky(sky, { 0, 0, 0 });
            gi.record(fc, main, rays);
            const BufferRef resultRef = graph.importBuffer(result.resource.Get(), { "test result", resultBytes, 16 });
            const TextureRef probes = main.screenProbes, maps = main.screenProbeMaps;
            graph.addPass("test.eval", QueueType::Compute,
                          [&](PassBuilder& b) {
                              b.use(probes, Use::SrvCompute);
                              b.use(maps, Use::SrvCompute);
                              b.use(depth, Use::SrvCompute);
                              b.use(gbuffer, Use::SrvCompute);
                              b.use(resultRef, Use::UavCompute);
                              b.keep();
                          },
                          [&, probes, maps, depth, gbuffer, resultRef, fcAddress](PassContext& c) {
                              const uint32_t k[12] = { c.srv(probes), c.srv(depth), c.srv(gbuffer), c.uav(resultRef), probesX, probesY, width, height, c.srv(maps), 0, 0, 0 };
                              c.cmd->SetPipelineState(shaders.compute("Passes/GI/Tests/GiTestEval"));
                              c.computeConstants(k, 12);
                              c.bindFrameConstants(fcAddress);
                              c.cmd->Dispatch((probesX + 7) / 8, (probesY + 7) / 8, 1);
                          });
            graph.execute(nullptr);
            device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));

            CommandList cl = device.acquireCommandList(QueueType::Graphics);
            cl.list->CopyBufferRegion(readback.resource.Get(), 0, result.resource.Get(), 0, resultBytes);
            device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
            void* rb = nullptr;
            D3D12_RANGE all{ 0, (SIZE_T)values.size() * 4 };
            check(readback.resource->Map(0, &all, &rb), "map result");
            std::memcpy(values.data(), rb, values.size() * 4);
            readback.resource->Unmap(0, &none);

            double sum = 0, lo = 1e30, hi = -1e30, worst = 0, rsum = 0, rworst = 0;
            uint32_t n = 0;
            for (size_t i = 0; i < values.size() / 8; ++i)
            {
                if (values[8 * i + 3] < 0) continue;
                const double e = (values[8 * i] + values[8 * i + 1] + values[8 * i + 2]) / 3.0;
                sum += e;
                lo = std::min(lo, e);
                hi = std::max(hi, e);
                worst = std::max(worst, std::fabs(e / expected - 1));
                const double r = (values[8 * i + 4] + values[8 * i + 5] + values[8 * i + 6]) / 3.0;
                rsum += r;
                rworst = std::max(rworst, std::fabs(r / expectedRadiance - 1));
                ++n;
            }
            out.radianceMean = n ? rsum / n : 0;
            out.radianceWorst = rworst;
            out.probes = n;
            out.mean = n ? sum / n : 0;
            out.minimum = lo;
            out.maximum = hi;
            out.worst = worst;
            if (out.converged < 0 && n && std::fabs(out.mean / expected - 1) < 0.01) out.converged = (int)f;
            if (f == frames - 1 || (f & (f - 1)) == 0)
                logf("  frame %3u: mean E %.4f (expected %.4f, %+.2f %%), min %.4f max %.4f, worst probe %.2f %%; K radiance mean %.4f (expected %.4f), worst %.2f %%\n", f,
                     out.mean, expected, 100 * (out.mean / expected - 1), lo, hi, 100 * worst, out.radianceMean, expectedRadiance, 100 * rworst);
        }
        if (giSystem) out.stats = giSystem->readStats();
        logf("  cache after the last frame: %u live, %u free, %u requested, %u selected + %u background updates, %u hit entries, %u created, %u resets, "
             "%u allocation failures, %u table overflows\n",
             out.stats.live, out.stats.free, out.stats.requested, out.stats.selected, out.stats.background, out.stats.hits, out.stats.created, out.stats.resets,
             out.stats.allocationFailures, out.stats.tableFull);
        device.waitIdle();
    }
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        uint32_t frames = 160;
        bool validate = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--frames" && i + 1 < argc) frames = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--validate") validate = true;
            else fail("unknown argument %s", a.c_str());
        }
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        DeviceOptions options;
        options.debugLayer = validate;
        options.gpuValidation = validate;
        Device device(options);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        bool pass = true;

        const float le = 1.0f, rho = 0.5f;
        logf("white furnace: Le %.2f, albedo %.2f, expected E = pi Le / (1 - rho) = %.4f\n", le, rho, kPi * le / (1 - rho));
        const Outcome a = run(device, shaders, quality, furnace(le, rho), { 0, 0, 0 }, kPi * le / (1 - rho), le / (1 - rho), frames, 1920, 1080);
        const bool okA = std::fabs(a.mean / (kPi * le / (1 - rho)) - 1) < 0.01 && a.worst < 0.03 && a.radianceWorst < 0.03;
        logf("white furnace: %u probes, mean %+.3f %%, worst probe %.3f %%, within 1 %% from frame %d; K radiance worst %.3f %% -> %s\n", a.probes,
             100 * (a.mean / (kPi * le / (1 - rho)) - 1), 100 * a.worst, a.converged, 100 * a.radianceWorst, okA ? "PASS" : "FAIL");
        pass = pass && okA;

        logf("open sky: L 1, ground albedo 0.5, expected E = pi\n");
        const Outcome b = run(device, shaders, quality, openSky(0.5f), { 1, 1, 1 }, kPi, 1.0, frames, 1920, 1080);
        const bool okB = std::fabs(b.mean / kPi - 1) < 0.01 && b.worst < 0.03 && b.radianceWorst < 0.03;
        logf("open sky: %u probes, mean %+.3f %%, worst probe %.3f %%, within 1 %% from frame %d; K radiance worst %.3f %% -> %s\n", b.probes, 100 * (b.mean / kPi - 1),
             100 * b.worst, b.converged, 100 * b.radianceWorst, okB ? "PASS" : "FAIL");
        pass = pass && okB;

        rt::RayPipeline::releaseDevice(device);
        rt::RayScene::releaseDevice(device);
        device.waitIdle();
        const uint32_t errors = device.drainDebugMessages();
        if (validate) logf("debug layer + GPU-based validation errors: %u\n", errors);
        pass = pass && errors == 0;
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
