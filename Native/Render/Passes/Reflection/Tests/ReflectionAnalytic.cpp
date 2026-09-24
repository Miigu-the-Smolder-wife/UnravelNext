// Reflection correctness against analytic answers (R track, ARCHITECTURE 2.6):
//  1. White furnace with a mirror floor half (roughness 0.05: M path) and a glossy half (0.3: G at grazing, K elsewhere):
//     radiance is uniform (L = Le / (1 - rho)), so every M and G pixel must resolve to L; the G path's control variate
//     cancels exactly in a uniform field, so any deviation is a transport or plumbing error.
//  2. A mirror ground under a constant sky L: M pixels see the sky, value L.
// Frame path: RayScene::record -> ray-traced primary visibility standing in for V/M -> GI -> reflections; the check reads
// reflectionRadiance (M's API) and the per-pixel mode.
//
//   unx_test_reflection_reflectionanalytic [--frames N] [--validate]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/gi/GiSystem.h"
#include "unx/refl/ReflectionSystem.h"
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
void addQuad(scene::Mesh& m, float3 a, float3 b, float3 c, float3 d, float3 n)
{
    const uint32_t base = (uint32_t)m.positions.size();
    for (float3 p : { a, b, c, d })
    {
        m.positions.push_back(p);
        m.normals.push_back(n);
        m.uv0.push_back({ p.x, p.z });
    }
    m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

scene::Scene furnaceWithMirrors(float le, float rho)
{
    scene::Scene s;
    s.name = "refl_furnace";
    for (float r : { 0.5f, 0.05f, 0.3f })
    {
        scene::Material m;
        m.name = "wall";
        m.baseColor = { rho, rho, rho };
        m.emissive = { le, le, le };
        m.roughness = r;
        s.materials.push_back(m);
    }
    scene::Mesh room;
    room.name = "room";
    const float x0 = -6, x1 = 6, y0 = 0, y1 = 5, z0 = -6, z1 = 6;
    // Walls and ceiling facing inward (CCW seen from inside).
    addQuad(room, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x0, y1, z1 }, { 0, -1, 0 });
    addQuad(room, { x0, y0, z0 }, { x0, y1, z0 }, { x0, y1, z1 }, { x0, y0, z1 }, { 1, 0, 0 });
    addQuad(room, { x1, y0, z1 }, { x1, y1, z1 }, { x1, y1, z0 }, { x1, y0, z0 }, { -1, 0, 0 });
    addQuad(room, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 }, { x0, y0, z0 }, { 0, 0, 1 });
    addQuad(room, { x0, y0, z1 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y0, z1 }, { 0, 0, -1 });
    room.submeshes.push_back({ 0, (uint32_t)room.indices.size(), 0 });
    const uint32_t floorStart = (uint32_t)room.indices.size();
    addQuad(room, { x0, y0, z1 }, { 0, y0, z1 }, { 0, y0, z0 }, { x0, y0, z0 }, { 0, 1, 0 });  // mirror half (x < 0)
    room.submeshes.push_back({ floorStart, 6, 1 });
    addQuad(room, { 0, y0, z1 }, { x1, y0, z1 }, { x1, y0, z0 }, { 0, y0, z0 }, { 0, 1, 0 });  // glossy half
    room.submeshes.push_back({ floorStart + 6, 6, 2 });
    s.meshes.push_back(room);
    s.instances.push_back({});
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "inside";
    cam.position = { 0, 1.2f, 5.5f };
    cam.forward = normalize(float3{ 0, -0.25f, -1 });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

scene::Scene mirrorUnderSky()
{
    scene::Scene s;
    s.name = "refl_sky_mirror";
    scene::Material m;
    m.name = "mirror";
    m.baseColor = { 0.9f, 0.9f, 0.9f };
    m.metallic = 1;
    m.roughness = 0.05f;
    s.materials.push_back(m);
    scene::Mesh plane;
    plane.name = "ground";
    addQuad(plane, { -200, 0, 200 }, { 200, 0, 200 }, { 200, 0, -200 }, { -200, 0, -200 }, { 0, 1, 0 });
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
    uint32_t surface = 0, k = 0, mirror = 0, glossy = 0;
    double worstM = 0, worstG = 0, meanM = 0, meanG = 0;
    uint32_t outliersM = 0;
};

Outcome run(Device& device, ShaderLibrary& shaders, const QualityConfig& quality, const scene::Scene& s, float3 sky, double expected, uint32_t frames, uint32_t width,
            uint32_t height)
{
    GpuScene gpuScene(device);
    gpuScene.upload(s);
    Outcome out;
    TrackState state;
    RenderGraph graph(device);
    const ViewDesc view = ViewDesc::fromCamera(s.cameras[0], width, height, float4x4{});
    Buffer constants = createBuffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD, false);
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(constants.resource->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map constants");
    const uint32_t stride = 4, countX = width / stride, countY = height / stride;
    const uint64_t resultBytes = (uint64_t)countX * countY * 16;
    Buffer result = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_DEFAULT, true);
    Buffer readback = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_READBACK, false);
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
        gi.setConstantSky(sky, { 0, 0, 0 });
        gi.record(fc, main, rays);
        refl::ReflectionSystem& reflections = refl::ReflectionSystem::get(fc);
        reflections.setConstantSky(sky, { 0, 0, 0 });
        reflections.record(fc, main, rays);
        const TextureRef reflection = main.reflection, modes = reflections.modes();
        const BufferRef resultRef = graph.importBuffer(result.resource.Get(), { "test result", resultBytes, 16 });
        graph.addPass("test.eval", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(reflection, Use::SrvCompute);
                          b.use(depth, Use::SrvCompute);
                          b.use(modes, Use::SrvCompute);
                          b.use(resultRef, Use::UavCompute);
                          b.keep();
                      },
                      [&, reflection, depth, modes, resultRef](PassContext& c) {
                          const uint32_t k[8] = { c.srv(reflection), c.srv(depth), c.srv(modes), c.uav(resultRef), width, height, stride, 0 };
                          c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/Tests/ReflTestEval"));
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch((countX + 7) / 8, (countY + 7) / 8, 1);
                      });
        graph.execute(nullptr);
        device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));
    }
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(readback.resource.Get(), 0, result.resource.Get(), 0, resultBytes);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    std::vector<float> values((size_t)countX * countY * 4);
    void* rb = nullptr;
    D3D12_RANGE all{ 0, (SIZE_T)resultBytes };
    check(readback.resource->Map(0, &all, &rb), "map result");
    std::memcpy(values.data(), rb, resultBytes);
    readback.resource->Unmap(0, &none);
    double sumM = 0, sumG = 0;
    for (size_t i = 0; i < values.size() / 4; ++i)
    {
        const float w = values[4 * i + 3];
        if (w < 0) continue;
        ++out.surface;
        const double v = (values[4 * i] + values[4 * i + 1] + values[4 * i + 2]) / 3.0;
        if (w == 0) ++out.k;
        else if (w == 1)
        {
            ++out.mirror;
            sumM += v;
            out.worstM = std::max(out.worstM, std::fabs(v / expected - 1));
            if (std::fabs(v / expected - 1) > 0.03 && out.outliersM++ < 6)
                logf("  M outlier at pixel (%u, %u): %.4f\n", (uint32_t)(i % countX) * stride, (uint32_t)(i / countX) * stride, v);
        }
        else
        {
            ++out.glossy;
            sumG += v;
            out.worstG = std::max(out.worstG, std::fabs(v / expected - 1));
        }
    }
    out.meanM = out.mirror ? sumM / out.mirror : 0;
    out.meanG = out.glossy ? sumG / out.glossy : 0;
    device.waitIdle();
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        uint32_t frames = 128;  // cold cache: cells only reflections reach converge in ~100 frames (48: 4 of 38962 M pixels still 4 % low)
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

        const Outcome a = run(device, shaders, quality, furnaceWithMirrors(1, 0.5f), { 0, 0, 0 }, 2.0, frames, 1920, 1080);
        const bool okA = a.mirror > 0 && a.glossy > 0 && std::fabs(a.meanM / 2 - 1) < 0.01 && std::fabs(a.meanG / 2 - 1) < 0.01 && a.worstM < 0.03 && a.worstG < 0.03;
        logf("furnace (L = 2): %u surface samples: K %u, M %u (mean %.4f, worst %.2f %%, %u beyond 3 %%), G %u (mean %.4f, worst %.2f %%) -> %s\n", a.surface, a.k,
             a.mirror, a.meanM, 100 * a.worstM, a.outliersM, a.glossy, a.meanG, 100 * a.worstG, okA ? "PASS" : "FAIL");
        pass = pass && okA;

        const Outcome b = run(device, shaders, quality, mirrorUnderSky(), { 1, 1, 1 }, 1.0, frames, 1920, 1080);
        const bool okB = b.mirror > 0 && std::fabs(b.meanM - 1) < 0.01 && b.worstM < 0.03;
        logf("sky mirror (L = 1): %u surface samples: K %u, M %u (mean %.4f, worst %.2f %%), G %u -> %s\n", b.surface, b.k, b.mirror, b.meanM, 100 * b.worstM, b.glossy,
             okB ? "PASS" : "FAIL");
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
