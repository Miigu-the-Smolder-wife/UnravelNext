// Planar reflection path (ARCHITECTURE 2.6, INTERFACES 5.4) against the M ray path on the same pixels. A room whose back
// wall is a checkerboard of emissive tiles (10 : 1) and whose other walls emit distinct colours (albedo 0), with a
// perfect mirror floor. Run A disables the planar path (mirror pixels take one VNDF ray each at alpha 1e-4); run B
// enables it with a test renderView that ray-traces the reflection camera and shades its hits exactly like the ray
// path (V/M/S are other tracks). Both are the exact mirror reflection of the same radiance: emission plus the walls'
// specular reflection of the GI cache (f0 0.04), whose texels are Monte Carlo estimates that differ between the two
// independent runs by ~1 % per pixel. So a pixel mismatches when it differs by more than 10 % (a misregistered
// reflection differs by the checker contrast); fewer than 1 % may (checker edges, sub-pixel ray jitter), and the mean
// signed difference must stay under 0.5 % (no bias). Run B forces the camera (setPlanarForced) to test the path; run C
// leaves the choice to the cost formula with a view prior of 1 s (reflection.planar_view_fixed_ms): every mirror pixel
// must then stay on rays. In run B every mirror mask texel of the view (ViewDesc::planarMask, INTERFACES v1.22) must be 1
// exactly where the main pixel is a planar pixel of the view in a tile R wrote, and every tile mask texel must be the OR
// of its 8 x 8 mask texels (PlanarMaskCheck).
//
//   unx_test_reflection_planarmirror [--validate]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/gi/GiSystem.h"
#include "unx/refl/ReflectionSystem.h"
#include "unx/render/GpuScene.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"
#include "unx/rt/SpecularAlbedo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace unx;
using namespace unx::render;

namespace
{
uint32_t asU(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

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

scene::Scene checkerRoom()
{
    scene::Scene s;
    s.name = "planar_checker_room";
    auto material = [&](float3 emission, float roughness) {
        scene::Material m;
        m.name = "m";
        m.baseColor = { 0, 0, 0 };
        m.emissive = emission;
        m.roughness = roughness;
        s.materials.push_back(m);
        return (uint32_t)s.materials.size() - 1;
    };
    const uint32_t bright = material({ 5, 5, 5 }, 0.5f), dark = material({ 0.5f, 0.5f, 0.5f }, 0.5f), red = material({ 3, 0.2f, 0.2f }, 0.5f),
                   green = material({ 0.2f, 3, 0.2f }, 0.5f), blue = material({ 0.2f, 0.2f, 3 }, 0.5f), mirror = material({ 0, 0, 0 }, 0.0f);
    const float x0 = -6, x1 = 6, y0 = 0, y1 = 5, z0 = -8, z1 = 6;
    scene::Mesh walls;
    walls.name = "walls";
    auto quad = [&](scene::Mesh& m, float3 a, float3 b, float3 c, float3 d, float3 n, uint32_t mat) {
        const uint32_t first = (uint32_t)m.indices.size();
        addQuad(m, a, b, c, d, n);
        m.submeshes.push_back({ first, 6, mat });
    };
    // Back wall (z0, facing +z): 1 m checker.
    for (int iy = 0; iy < 5; ++iy)
        for (int ix = 0; ix < 12; ++ix)
        {
            const float xa = x0 + ix, xb = xa + 1, ya = y0 + iy, yb = ya + 1;
            quad(walls, { xb, ya, z0 }, { xb, yb, z0 }, { xa, yb, z0 }, { xa, ya, z0 }, { 0, 0, 1 }, ((ix + iy) & 1) ? bright : dark);
        }
    quad(walls, { x0, y0, z0 }, { x0, y1, z0 }, { x0, y1, z1 }, { x0, y0, z1 }, { 1, 0, 0 }, red);
    quad(walls, { x1, y0, z1 }, { x1, y1, z1 }, { x1, y1, z0 }, { x1, y0, z0 }, { -1, 0, 0 }, green);
    quad(walls, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x0, y1, z1 }, { 0, -1, 0 }, blue);
    quad(walls, { x0, y0, z1 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y0, z1 }, { 0, 0, -1 }, dark);
    s.meshes.push_back(walls);
    scene::Mesh floor;
    floor.name = "mirror floor";
    quad(floor, { x0, y0, z1 }, { x1, y0, z1 }, { x1, y0, z0 }, { x0, y0, z0 }, { 0, 1, 0 }, mirror);
    s.meshes.push_back(floor);
    s.instances.push_back({});
    s.instances.push_back({});
    s.instances.back().mesh = 1;
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "inside";
    cam.position = { 0.3f, 1.6f, 5.0f };
    cam.forward = normalize(float3{ 0.05f, -0.3f, -1 });
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

// Reflection values (rgb) and modes (w) of every 2nd pixel after a few frames.
enum class Planar
{
    Off,
    Forced,
    Cost
};

struct MaskCheck
{
    uint32_t pixelErrors = 0, tileErrors = 0, mirrorTexels = 0, viewTexels = 0;
};

std::vector<float> run(Device& device, ShaderLibrary& shaders, const QualityConfig& quality, const scene::Scene& s, Planar planar, uint32_t width, uint32_t height,
                       uint32_t& planarViews, MaskCheck* maskCheck = nullptr)
{
    GpuScene gpuScene(device);
    gpuScene.upload(s);
    TrackState state;
    RenderGraph graph(device);
    const ViewDesc view = ViewDesc::fromCamera(s.cameras[0], width, height, float4x4{});
    constexpr uint32_t kSlots = 16;
    Buffer constants = createBuffer(device, kSlots * 1024, D3D12_HEAP_TYPE_UPLOAD, false);
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(constants.resource->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map constants");
    const uint32_t stride = 2, countX = width / stride, countY = height / stride;
    const uint64_t resultBytes = (uint64_t)countX * countY * 16;
    Buffer result = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_DEFAULT, true);
    Buffer readback = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_READBACK, false);
    Buffer maskResult = createBuffer(device, 16, D3D12_HEAP_TYPE_DEFAULT, true);
    Buffer maskReadback = createBuffer(device, 16, D3D12_HEAP_TYPE_READBACK, false);
    refl::ReflectionSystem* reflSystem = nullptr;
    const uint32_t frames = 64;  // the hits read the GI cache (specular term): both runs compare converged caches (~14 frames)
    for (uint32_t f = 0; f < frames; ++f)
    {
        FrameContext frame;
        frame.frameIndex = f;
        frame.time = f / 60.0;
        frame.deltaTime = 1 / 60.0f;
        frame.mainView = view;
        FrameResources resources;
        FrameServices services;
        uint32_t slot = 0;
        auto frameConstantsFor = [&](const ViewDesc& v) {
            if (slot >= kSlots) fail("test: more than %u views", kSlots);
            gpu::FrameConstants c{};
            c.viewProj = v.viewProj;
            c.prevViewProj = v.prevViewProj;
            c.invViewProj = v.invViewProj;
            c.view = v.view;
            c.proj = v.proj;
            c.cameraPosition = v.position;
            c.nearPlane = v.nearPlane;
            c.clipPlane = v.clipPlane;
            c.viewWidth = v.width;
            c.viewHeight = v.height;
            c.viewKind = (uint32_t)v.kind;
            c.frameIndex = f;
            c.time = (float)frame.time;
            c.deltaTime = frame.deltaTime;
            c.exposure = 1.0f;
            c.tanHalfFovY = std::tan(v.verticalFov * 0.5f);
            c.sunDirection = s.sun.direction;
            gpuScene.fill(c);
            std::memcpy(mapped + slot * 1024, &c, sizeof c);
            return constants.resource->GetGPUVirtualAddress() + 1024ull * slot++;
        };
        FramePassContext fc{ device, graph, shaders, quality, gpuScene, frame, resources, services, frameConstantsFor, &state };
        rt::RayScene& rays = rt::RayScene::get(fc);
        rays.record(fc);
        uint32_t scene[8];
        rays.rootConstants(scene);
        rt::RayPipeline& standIn = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline("Passes/Reflection/Tests/PlanarTestView", { "PlanarTestViewGen" }));
        struct PendingMask
        {
            TextureRef mask, tileMask;
            refl::ReflectionSystem::Rect rect;
        } pendingMask;
        uint32_t viewsThisFrame = 0;
        services.renderView = [&](FramePassContext& c, const ViewDesc& v) {
            if (viewsThisFrame++ == 0 && v.planarMask.valid())
                pendingMask = { v.planarMask, v.planarTileMask, refl::ReflectionSystem::get(c).planarViewRect(0) };
            ViewResources rv;
            rv.view = v;
            rv.frameConstants = c.frameConstantsFor(v);
            rv.color = c.graph.createTexture({ "stand-in reflection view", v.width, v.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
            const TextureRef colour = rv.color;
            const BufferRef cache = c.resources.giCache;
            const uint32_t lut = rt::specularAlbedoSrv(device);
            const float rayLength = gi::GiSettings::fromQuality(quality).rayLength;
            const D3D12_GPU_VIRTUAL_ADDRESS address = rv.frameConstants;
            const uint32_t w = v.width, h = v.height;
            c.graph.addPass("test.planar.view", QueueType::Compute,
                            [&](PassBuilder& b) {
                                b.use(colour, Use::UavGraphics);
                                b.use(cache, Use::UavGraphics);
                                rays.declareTraversal(b);
                            },
                            [&standIn, colour, cache, lut, rayLength, address, w, h, scene](PassContext& pc) {
                                uint32_t k[32] = {};
                                k[0] = pc.uav(colour);
                                k[7] = asU(rayLength);
                                k[18] = pc.uav(cache);
                                k[21] = lut;
                                k[22] = k[23] = 0xFFFFFFFFu;  // no VSM, no exact set counts
                                std::memcpy(&k[24], scene, sizeof scene);
                                pc.computeConstants(k, 32);
                                pc.bindFrameConstants(address);
                                standIn.dispatch(pc.cmd, 0, w, h, 1);
                            });
            return rv;
        };
        ViewResources main;
        main.view = view;
        main.frameConstants = frameConstantsFor(view);
        main.depth = graph.createTexture({ "test depth", width, height, 1, 1, DXGI_FORMAT_R32_FLOAT });
        main.gbuffer = graph.createTexture({ "test gbuffer", width, height, 1, 1, DXGI_FORMAT_R32G32_UINT });
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
        gi::GiSystem::get(fc).record(fc, main, rays);
        reflSystem = &refl::ReflectionSystem::get(fc);
        reflSystem->setPlanarEnabled(planar != Planar::Off);
        reflSystem->setPlanarForced(planar == Planar::Forced);
        reflSystem->record(fc, main, rays);
        const TextureRef reflection = main.reflection, modes = reflSystem->modes();
        const BufferRef resultRef = graph.importBuffer(result.resource.Get(), { "test result", resultBytes, 16 });
        if (maskCheck && f + 1 == frames && pendingMask.mask.valid())
        {
            const BufferRef maskRef = graph.importBuffer(maskResult.resource.Get(), { "test mask result", 16, 0 });
            const PendingMask pm = pendingMask;
            graph.addPass("test.mask.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(maskRef, Use::CopyDst); },
                          [maskRef](PassContext& c) {
                              D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p[4];
                              for (int i = 0; i < 4; ++i) p[i] = { c.resource(maskRef)->GetGPUVirtualAddress() + 4 * i, 0 };
                              c.cmd->WriteBufferImmediate(4, p, nullptr);
                          });
            graph.addPass("test.mask.check", QueueType::Compute,
                          [&](PassBuilder& b) {
                              b.use(pm.mask, Use::SrvCompute);
                              b.use(pm.tileMask, Use::SrvCompute);
                              b.use(modes, Use::SrvCompute);
                              b.use(reflection, Use::SrvCompute);
                              b.use(maskRef, Use::UavCompute);
                              b.keep();
                          },
                          [&shaders, pm, modes, reflection, maskRef](PassContext& c) {
                              const uint32_t k[12] = { c.srv(pm.mask), c.srv(pm.tileMask), c.srv(modes), c.srv(reflection), pm.rect.x, pm.rect.y, pm.rect.width,
                                                       pm.rect.height, c.uav(maskRef), 0, 0, 0 };
                              c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/Tests/PlanarMaskCheck"));
                              c.computeConstants(k, 12);
                              c.cmd->Dispatch((pm.rect.width + 7) / 8, (pm.rect.height + 7) / 8, 1);
                          });
        }
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
    planarViews = reflSystem ? reflSystem->readStats().planarViews : 0;
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(readback.resource.Get(), 0, result.resource.Get(), 0, resultBytes);
    cl.list->CopyBufferRegion(maskReadback.resource.Get(), 0, maskResult.resource.Get(), 0, 16);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    std::vector<float> values((size_t)countX * countY * 4);
    void* rb = nullptr;
    D3D12_RANGE all{ 0, (SIZE_T)resultBytes };
    check(readback.resource->Map(0, &all, &rb), "map result");
    std::memcpy(values.data(), rb, resultBytes);
    readback.resource->Unmap(0, &none);
    if (maskCheck)
    {
        D3D12_RANGE maskRange{ 0, 16 };
        check(maskReadback.resource->Map(0, &maskRange, &rb), "map mask result");
        std::memcpy(maskCheck, rb, 16);
        maskReadback.resource->Unmap(0, &none);
    }
    device.waitIdle();
    return values;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool validate = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--validate") validate = true;
            else fail("unknown argument %s", a.c_str());
        }
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        DeviceOptions options;
        options.debugLayer = validate;
        options.gpuValidation = validate;
        Device device(options);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        const scene::Scene s = checkerRoom();
        uint32_t viewsA = 0, viewsB = 0;
        const std::vector<float> a = run(device, shaders, quality, s, Planar::Off, 1920, 1080, viewsA);
        MaskCheck mask;
        const std::vector<float> b = run(device, shaders, quality, s, Planar::Forced, 1920, 1080, viewsB, &mask);
        // Run C: the cost formula with a view that costs more than any ray count: every mirror pixel M as in run A.
        QualityConfig costly = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        costly.applyOverride("reflection.planar_view_fixed_ms=1000");
        uint32_t viewsC = 0, rayPixelsC = 0;
        const std::vector<float> runC = run(device, shaders, costly, s, Planar::Cost, 1920, 1080, viewsC);
        for (size_t i = 0; i < runC.size() / 4; ++i)
            if (a[4 * i + 3] == 1 && runC[4 * i + 3] == 1) ++rayPixelsC;
        uint32_t compared = 0, mismatched = 0, planarPixels = 0, mirrorPixels = 0;
        double sumDiff = 0, sumSigned = 0;
        for (size_t i = 0; i < a.size() / 4; ++i)
        {
            if (a[4 * i + 3] == 1) ++mirrorPixels;
            if (b[4 * i + 3] == 3) ++planarPixels;
            if (a[4 * i + 3] != 1 || b[4 * i + 3] != 3) continue;
            ++compared;
            double diff = 0, ref = 0;
            for (int c = 0; c < 3; ++c)
            {
                diff += std::fabs(a[4 * i + c] - b[4 * i + c]);
                ref += std::fabs(a[4 * i + c]);
            }
            sumDiff += diff / std::max(ref, 1e-3);
            sumSigned += (b[4 * i] + b[4 * i + 1] + b[4 * i + 2] - a[4 * i] - a[4 * i + 1] - a[4 * i + 2]) / std::max(ref, 1e-3);
            if (diff > 0.1 * ref + 1e-3) ++mismatched;
        }
        logf("planar mirror: mean signed (planar - rays) / rays %.4f %%\n", compared ? 100 * sumSigned / compared : 0.0);
        const double mismatchFraction = compared ? (double)mismatched / compared : 1;
        uint32_t mirrorPixelsA = 0;
        for (size_t i = 0; i < a.size() / 4; ++i) mirrorPixelsA += a[4 * i + 3] == 1 ? 1u : 0u;
        const double bias = compared ? sumSigned / compared : 1;
        const bool maskPass = mask.viewTexels > 0 && mask.mirrorTexels > 1000 && mask.pixelErrors == 0 && mask.tileErrors == 0;
        const bool pass = viewsA == 0 && viewsB == 1 && compared > 1000 && mismatchFraction < 0.01 && std::fabs(bias) < 0.005 && viewsC == 0 &&
                          rayPixelsC == mirrorPixelsA && maskPass;
        logf("planar mirror: run B mirror mask %u of %u view texels, %u texels and %u tiles disagree with the classification -> %s\n", mask.mirrorTexels,
             mask.viewTexels, mask.pixelErrors, mask.tileErrors, maskPass ? "PASS" : "FAIL");
        logf("planar mirror: run A (rays) %u M pixels, run B %u planar view(s), %u planar pixels; %u compared, %u differ by > 10 %% (%.3f %%, checker "
             "edges), mean relative difference %.4f %% -> %s\n",
             mirrorPixels, viewsB, planarPixels, compared, mismatched, 100 * mismatchFraction, compared ? 100 * sumDiff / compared : 0.0, pass ? "PASS" : "FAIL");
        logf("planar mirror: run C (cost formula, view prior 1 s) %u planar view(s), %u of %u mirror pixels on rays\n", viewsC, rayPixelsC, mirrorPixelsA);
        rt::RayPipeline::releaseDevice(device);
        rt::RayScene::releaseDevice(device);
        device.waitIdle();
        const uint32_t errors = device.drainDebugMessages();
        if (validate) logf("debug layer + GPU-based validation errors: %u\n", errors);
        logf("RESULT %s\n", pass && errors == 0 ? "PASS" : "FAIL");
        return pass && errors == 0 ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
