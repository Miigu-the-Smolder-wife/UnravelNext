// GI correctness against analytic answers (R track, ARCHITECTURE 2.5, quality definition section 3 "GI"):
//  1. White furnace: a closed box whose inner walls emit Le and reflect albedo rho. Radiance is uniform, L = Le / (1 - rho),
//     so the irradiance on every wall is E = pi Le / (1 - rho). Exercises multi-bounce through cached hit irradiance,
//     the hash grid over several cell levels, texel stratification, SH projection, probe gather and interpolation.
//  2. Open sky: a ground plane (albedo 0.5) under constant sky radiance L. The plane cannot see itself: E = pi L.
//  3. Sunlit ground and a black wall: a ground plane (albedo rho) lit by the sun alone (no sky), a vertical wall of albedo
//     0 facing the sun's side. The wall's indirect irradiance is the ground's single bounce, E = rho E_sun cos(theta_sun) F,
//     F = the view factor from the probe to the (unshadowed) ground in front of the wall, exact by Lambert's contour
//     integral per probe. Exercises the sun at GI hits (illuminance, cosine, shadow ray) that 1 and 2 do not.
//  5. Horizon band sky: a ground plane under a constant sky only between elevation 0 and 10 deg (a sunset sky is brightest
//     there). E = pi L cos^2(80 deg) exactly. The cache's irradiance comes from order-2 SH, whose truncated cosine kernel
//     overestimates light near the horizon (at 85 deg from the normal 0.141 against cos = 0.087): this measures it.
//  4. A single sunlit plane (albedo 0.5, no sky), level and tilted 25 deg: a plane cannot see itself, so its indirect
//     irradiance is exactly 0. Any light the cache gives it is spurious bounce (self-hits, cells that sample around the
//     surface, directions below a record's hemisphere); reported as E over the plane's direct sun irradiance.
// Each runs the frame path (RayScene::record -> ray-traced primary visibility standing in for V/M -> GiSystem::record)
// for N frames and evaluates screenProbeIrradiance (M's API) at every probe pixel. Also reports the frames needed to
// come within 1 % (reconvergence, gi.relight_frames_max).
//
//   unx_test_gi_gianalytic [--frames N] [--validate] [--determinism]
// --set key=value: quality overrides (e.g. gi.experiment_disable=256 for the emissive panel's texel-only control).
// --determinism: gi.deterministic off and on, two furnace runs each; on must be bit-identical.
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/gi/GiSystem.h"
#include "unx/render/GpuScene.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <tuple>

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

// The K path by the lobe (ScreenProbes.hlsli giProbeFootprintLobe) against the exact BRDF-weighted mean incident radiance
// of the GGX lobe: 32 x 32 stratified visible-normal samples weighted by G2 / G1(V) (the same estimator's density,
// converged), the incident radiance from 'radiance(p, direction)'.
double lobeMean(float3 p, float3 n, float3 v, float alpha, const std::function<double(float3, float3)>& radiance)
{
    const float3 up = std::fabs(n.y) < 0.9f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    const float3 tb = normalize(cross(up, n)), bb = cross(n, tb);
    const float3 ve = normalize(float3{ dot(v, tb), dot(v, bb), std::max(dot(v, n), 1e-4f) });
    const double a2 = (double)alpha * alpha;
    auto lambda = [&](double c) { const double c2 = std::max(c * c, 1e-8); return 0.5 * (std::sqrt(1 + a2 * (1 - c2) / c2) - 1); };
    const double lv = lambda(ve.z);
    const float3 vh = normalize(float3{ alpha * ve.x, alpha * ve.y, ve.z });
    double sum = 0, wsum = 0;
    constexpr int kN = 32;
    for (int i = 0; i < kN; ++i)
        for (int j = 0; j < kN; ++j)
        {
            const double u1 = (i + 0.5) / kN, u2 = (j + 0.5) / kN;
            const double phi = 2 * kPi * u1, z = (1 - u2) * (1 + vh.z) - vh.z, st = std::sqrt(std::max(0.0, 1 - z * z));
            const float3 hh = float3{ (float)(st * std::cos(phi)), (float)(st * std::sin(phi)), (float)z } + vh;
            const float3 m = normalize(float3{ alpha * hh.x, alpha * hh.y, std::max(hh.z, 1e-6f) });
            const float3 l = m * (2 * dot(ve, m)) - ve;
            if (l.z <= 0) continue;
            const double w = (1 + lv) / (1 + lv + lambda(l.z));
            sum += w * radiance(p, normalize(tb * l.x + bb * l.y + n * l.z));
            wsum += w;
        }
    return wsum > 0 ? sum / wsum : 0;
}

// Ground plane (albedo rho, 400 x 400 m) and a wall of albedo 0 in the plane x = 0 facing +x (60 m wide, 20 m tall),
// sun from the +x side: the ground in front of the wall is fully lit.
scene::Scene sunWall(float albedo, float3 sunDirection)
{
    scene::Scene s;
    s.name = "gi_sun_wall";
    scene::Material ground, wall;
    ground.name = "ground";
    ground.baseColor = { albedo, albedo, albedo };
    wall.name = "wall";
    wall.baseColor = { 0, 0, 0 };
    s.materials.push_back(ground);
    s.materials.push_back(wall);
    scene::Mesh mesh;
    mesh.name = "ground_and_wall";
    for (auto [x, z] : { std::pair{ -200.f, -200.f }, { 200.f, -200.f }, { 200.f, 200.f }, { -200.f, 200.f } })
    {
        mesh.positions.push_back({ x, 0, z });
        mesh.normals.push_back({ 0, 1, 0 });
        mesh.uv0.push_back({ x, z });
    }
    for (auto [y, z] : { std::pair{ 0.f, -30.f }, { 20.f, -30.f }, { 20.f, 30.f }, { 0.f, 30.f } })
    {
        mesh.positions.push_back({ 0, y, z });
        mesh.normals.push_back({ 1, 0, 0 });
        mesh.uv0.push_back({ z, y });
    }
    mesh.indices = { 0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7 };
    mesh.submeshes.push_back({ 0, 6, 0 });
    mesh.submeshes.push_back({ 6, 6, 1 });
    s.meshes.push_back(mesh);
    s.instances.push_back({});
    s.sun.direction = sunDirection;
    s.sun.illuminance = 0;  // GI's sun comes from setConstantSky
    scene::Camera cam;
    cam.name = "facing the wall";
    cam.position = { 14, 5, 0 };
    cam.forward = normalize(float3{ -1, -0.2f, 0 });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

// One plane through the origin with normal (-sin tilt, cos tilt, 0), 2 x 2 km, sun from above.
scene::Scene sunPlane(float albedo, float tiltDeg, float3 sunDirection)
{
    scene::Scene s;
    s.name = "gi_sun_plane";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { albedo, albedo, albedo };
    s.materials.push_back(ground);
    const float t = tiltDeg * kPi / 180;
    const float3 n{ -std::sin(t), std::cos(t), 0 }, u{ std::cos(t), std::sin(t), 0 }, w{ 0, 0, 1 };
    scene::Mesh plane;
    plane.name = "plane";
    for (auto [a, b] : { std::pair{ -1000.f, -1000.f }, { 1000.f, -1000.f }, { 1000.f, 1000.f }, { -1000.f, 1000.f } })
    {
        plane.positions.push_back(u * a + w * b);
        plane.normals.push_back(n);
        plane.uv0.push_back({ a, b });
    }
    plane.indices = { 0, 2, 1, 0, 3, 2 };
    plane.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(plane);
    s.instances.push_back({});
    s.sun.direction = sunDirection;
    s.sun.illuminance = 0;  // GI's sun comes from setConstantSky
    scene::Camera cam;
    cam.name = "above";
    cam.position = n * 3.0f;
    cam.forward = normalize(float3{ 0.2f, -0.6f, -1 });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

// View factor from a point p with unit normal n to a planar polygon (all of it in front of p), Lambert's contour
// integral: F = |sum_i angle(R_i, R_i+1) n . unit(R_i x R_i+1)| / (2 pi), R_i = unit(q_i - p).
// A black floor (400 x 400 m) under a 1 x 1 m emissive panel at height 1 facing down (radiance Le, one-sided), no sun or
// sky: the floor's irradiance is pi Le F (F the point-to-polygon view factor), all of it direct emission of a mesh (the
// GI rays' emissive-triangle samples with MIS, B2).
scene::Scene emissivePanel(float radiance)
{
    scene::Scene s;
    s.name = "gi_emissive_panel";
    scene::Material floor;
    floor.name = "black floor";
    floor.baseColor = { 0, 0, 0 };
    s.materials.push_back(floor);
    scene::Material panel;
    panel.name = "emitter";
    panel.baseColor = { 0, 0, 0 };
    panel.emissive = { radiance, radiance, radiance };
    s.materials.push_back(panel);
    scene::Mesh plane;
    plane.name = "floor";
    for (auto [x, z] : { std::pair{ -200.f, -200.f }, { 200.f, -200.f }, { 200.f, 200.f }, { -200.f, 200.f } })
    {
        plane.positions.push_back({ x, 0, z });
        plane.normals.push_back({ 0, 1, 0 });
        plane.uv0.push_back({ x, z });
    }
    plane.indices = { 0, 2, 1, 0, 3, 2 };
    plane.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(plane);
    scene::Mesh quad;
    quad.name = "panel";
    for (auto [x, z] : { std::pair{ -0.5f, -0.5f }, { 0.5f, -0.5f }, { 0.5f, 0.5f }, { -0.5f, 0.5f } })
    {
        quad.positions.push_back({ x, 1, z });
        quad.normals.push_back({ 0, -1, 0 });
        quad.uv0.push_back({ x + 0.5f, z + 0.5f });
    }
    quad.indices = { 0, 1, 2, 0, 2, 3 };  // front face (counter-clockwise) toward -y
    quad.submeshes.push_back({ 0, 6, 1 });
    s.meshes.push_back(quad);
    s.instances.push_back({});
    scene::Instance q;
    q.mesh = 1;
    s.instances.push_back(q);
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "above";
    cam.position = { 0, 2.5f, 3.5f };
    cam.forward = normalize(float3{ 0, -2.5f, -3.5f });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

// The open sky scene with a second mesh, a black 2 m cube (centre at the origin), that no instance uses at upload: the
// instance edit test appends one (B3).
scene::Scene openSkyWithCube(float albedo)
{
    scene::Scene s = openSky(albedo);
    scene::Material black;
    black.name = "black";
    black.baseColor = { 0, 0, 0 };
    s.materials.push_back(black);
    scene::Mesh cube;
    cube.name = "cube";
    const float3 axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int a = 0; a < 3; ++a)
        for (float sign : { -1.0f, 1.0f })
        {
            const float3 n = axes[a] * sign, u = axes[(a + 1) % 3], v = axes[(a + 2) % 3] * sign;
            const uint32_t base = (uint32_t)cube.positions.size();
            for (auto [x, y] : { std::pair{ -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } })
            {
                cube.positions.push_back(n + u * x + v * y);
                cube.normals.push_back(n);
                cube.uv0.push_back({ x, y });
            }
            cube.indices.insert(cube.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
    cube.submeshes.push_back({ 0, 36, 1 });
    s.meshes.push_back(cube);
    return s;
}

double viewFactor(float3 p, float3 n, const std::vector<float3>& polygon)
{
    double sum = 0;
    for (size_t i = 0; i < polygon.size(); ++i)
    {
        const float3 a = normalize(polygon[i] - p), b = normalize(polygon[(i + 1) % polygon.size()] - p);
        const float3 c = cross(a, b);
        const double len = std::sqrt((double)c.x * c.x + (double)c.y * c.y + (double)c.z * c.z);
        if (len < 1e-12) continue;
        sum += std::atan2(len, (double)dot(a, b)) * ((double)n.x * c.x + (double)n.y * c.y + (double)n.z * c.z) / len;
    }
    return std::fabs(sum) / (2 * kPi);
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
    double expectedMean = 0;                                 // mean expected E over the same probes
    double mapMean = 0, mapWorst = 0, mapExpectedMean = 0;   // the cache's irradiance maps at the probe points (giCacheIrradianceAt)
    uint32_t mapProbes = 0, mapBeyond3 = 0;                  // probes, and those beyond 3 % (a stale region shows as a cluster)
    double mapP99 = 0, mapWorstExpected = 0, mapWorstValue = 0;  // 99th percentile relative error; the worst probe's E pair
    float3 mapWorstAt{}, mapWorstNormal{};
    double radianceMean = 0, radianceWorst = 0;              // screenProbeRadiance against the uniform radiance
    uint32_t probes = 0;
    int converged = -1;  // first frame whose mean is within 1 %
    gi::GiStats stats;
    uint32_t tilePixels = 0, tileMismatches = 0;  // screenProbeGatherTile vs screenProbeGather (ProbeTileCompare), all frames
    std::vector<float> values;                    // the last frame's probe evaluations (GiTestEval), for determinism checks
    std::vector<uint32_t> resetsPerFrame;         // with an edit hook: each frame's entries updated from a reset history
    std::vector<uint32_t> epochPerFrame;          // with an edit hook: each frame's lighting epoch
};

// expected(position, normal): the analytic irradiance at a probe, < 0 to leave the probe out. expectedRadiance <= 0: the K
// radiance is not checked (not uniform).
Outcome run(Device& device, ShaderLibrary& shaders, const QualityConfig& quality, const scene::Scene& s, float3 sky, float3 sun,
            const std::function<double(float3, float3)>& expected, double expectedRadiance, uint32_t frames, uint32_t width, uint32_t height,
            float skyBand = 1, float lobeAlpha = 0, const std::function<void(uint32_t, GpuScene&)>& edit = {})
{
    GpuScene gpuScene(device);
    gpuScene.upload(s);
    Outcome out;
    Buffer tileResult = createBuffer(device, 512, D3D12_HEAP_TYPE_DEFAULT, true);  // zero-filled at creation
    Buffer tileReadback = createBuffer(device, 512, D3D12_HEAP_TYPE_READBACK, false);
    {
        TrackState state;
        RenderGraph graph(device);
        const ViewDesc view = ViewDesc::fromCamera(s.cameras[0], width, height, float4x4{});
        Buffer constants = createBuffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD, false);
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(constants.resource->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map constants");
        const uint32_t probesX = (width + 7) / 8 + 1, probesY = (height + 7) / 8 + 1;  // corner probes
        const uint64_t resultBytes = (uint64_t)probesX * probesY * 64;
        Buffer result = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_DEFAULT, true);
        Buffer readback = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_READBACK, false);
        std::vector<float> values((size_t)probesX * probesY * 16);
        gi::GiSystem* giSystem = nullptr;

        for (uint32_t f = 0; f < frames; ++f)
        {
            if (edit) edit(f, gpuScene);  // scene edits between frames (GpuScene::setInstances, as a host does)
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
            gi.setConstantSky(sky, sun);
            gi.setConstantSkyBand(skyBand);
            gi.record(fc, main, rays);
            const BufferRef resultRef = graph.importBuffer(result.resource.Get(), { "test result", resultBytes, 16 });
            const TextureRef probes = main.screenProbes, maps = main.screenProbeMaps;
            const BufferRef cacheRef = fc.resources.giCache;
            graph.addPass("test.eval", QueueType::Compute,
                          [&](PassBuilder& b) {
                              b.use(probes, Use::SrvCompute);
                              b.use(maps, Use::SrvCompute);
                              b.use(depth, Use::SrvCompute);
                              b.use(gbuffer, Use::SrvCompute);
                              b.use(resultRef, Use::UavCompute);
                              b.use(cacheRef, Use::SrvCompute);
                              b.keep();
                          },
                          [&, probes, maps, depth, gbuffer, resultRef, cacheRef, fcAddress](PassContext& c) {
                              uint32_t alphaBits;
                              std::memcpy(&alphaBits, &lobeAlpha, 4);
                              const uint32_t k[12] = { c.srv(probes), c.srv(depth), c.srv(gbuffer), c.uav(resultRef), probesX, probesY, width, height, c.srv(maps), c.srv(cacheRef), alphaBits, 0 };
                              c.cmd->SetPipelineState(shaders.compute("Passes/GI/Tests/GiTestEval"));
                              c.computeConstants(k, 12);
                              c.bindFrameConstants(fcAddress);
                              c.cmd->Dispatch((probesX + 7) / 8, (probesY + 7) / 8, 1);
                          });
            const BufferRef tileRef = graph.importBuffer(tileResult.resource.Get(), { "test tile compare", 512, 0 });
            graph.addPass("test.tilecompare", QueueType::Compute,
                          [&](PassBuilder& b) {
                              b.use(probes, Use::SrvCompute);
                              b.use(maps, Use::SrvCompute);
                              b.use(depth, Use::SrvCompute);
                              b.use(gbuffer, Use::SrvCompute);
                              b.use(tileRef, Use::UavCompute);
                              b.keep();
                          },
                          [&, probes, maps, depth, gbuffer, tileRef, fcAddress](PassContext& c) {
                              const uint32_t k[12] = { c.srv(probes), c.srv(depth), c.srv(gbuffer), c.uav(tileRef), 0, 0, width, height, c.srv(maps), 0, 0, 0 };
                              c.cmd->SetPipelineState(shaders.compute("Passes/GI/Tests/ProbeTileCompare"));
                              c.computeConstants(k, 12);
                              c.bindFrameConstants(fcAddress);
                              c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                          });
            graph.execute(nullptr);
            device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));

            CommandList cl = device.acquireCommandList(QueueType::Graphics);
            cl.list->CopyBufferRegion(readback.resource.Get(), 0, result.resource.Get(), 0, resultBytes);
            cl.list->CopyBufferRegion(tileReadback.resource.Get(), 0, tileResult.resource.Get(), 0, 272);
            device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
            {
                void* tm = nullptr;
                D3D12_RANGE tr{ 0, 272 };
                check(tileReadback.resource->Map(0, &tr, &tm), "map tile compare");
                uint32_t words[68];
                std::memcpy(words, tm, 272);
                tileReadback.resource->Unmap(0, &none);
                out.tilePixels = words[0];
                if (words[1] > out.tileMismatches)
                    for (uint32_t r = 0; r < std::min(words[2], 4u); ++r)
                    {
                        const uint32_t* w = words + 4 + r * 16;
                        auto fv = [&](uint32_t i) { float v; std::memcpy(&v, &w[i], 4); return v; };
                        logf("  tile mismatch at frame %u pixel (%u, %u) cone %u fields %x: E %.9g %.9g %.9g occ %.9g vs E %.9g %.9g %.9g occ %.9g; K.r %.9g vs %.9g; back.r %.9g vs %.9g\n",
                             f, w[0] & 0xFFFFu, w[0] >> 16, w[1], w[2], fv(4), fv(5), fv(6), fv(7), fv(8), fv(9), fv(10), fv(11), fv(12), fv(13), fv(14), fv(15));
                    }
                out.tileMismatches = words[1];
            }
            void* rb = nullptr;
            D3D12_RANGE all{ 0, (SIZE_T)values.size() * 4 };
            check(readback.resource->Map(0, &all, &rb), "map result");
            std::memcpy(values.data(), rb, values.size() * 4);
            readback.resource->Unmap(0, &none);
            if (edit && giSystem)
            {
                const gi::GiStats st = giSystem->readStats();
                out.resetsPerFrame.push_back(st.resets);
                out.epochPerFrame.push_back(st.epoch);
            }

            double sum = 0, esum = 0, lo = 1e30, hi = -1e30, worst = 0, rsum = 0, rworst = 0;
            uint32_t n = 0;
            for (size_t i = 0; i < values.size() / 16; ++i)
            {
                const float* v = &values[16 * i];
                if (v[3] < 0) continue;
                const double x = expected({ v[8], v[9], v[10] }, { v[12], v[13], v[14] });
                if (x < 0) continue;
                const double e = (v[0] + v[1] + v[2]) / 3.0;
                sum += e;
                esum += x;
                lo = std::min(lo, e);
                hi = std::max(hi, e);
                worst = std::max(worst, x > 0 ? std::fabs(e / x - 1) : std::fabs(e));  // expected 0: the absolute value
                const double r = (v[4] + v[5] + v[6]) / 3.0;
                rsum += r;
                if (expectedRadiance > 0) rworst = std::max(rworst, std::fabs(r / expectedRadiance - 1));
                ++n;
            }
            out.expectedMean = n ? esum / n : 0;
            {
                double msum = 0, mexp = 0, mworst = 0;
                uint32_t beyond3 = 0;
                uint32_t mn = 0;
                std::vector<double> rel;
                for (size_t i = 0; i < values.size() / 16; ++i)
                {
                    const float* v = &values[16 * i];
                    if (v[3] < 0 || v[11] < 0) continue;
                    const double x = expected({ v[8], v[9], v[10] }, { v[12], v[13], v[14] });
                    if (x < 0) continue;
                    msum += v[11];
                    mexp += x;
                    const double r = x > 0 ? std::fabs(v[11] / x - 1) : std::fabs((double)v[11]);
                    if (r > mworst)
                    {
                        out.mapWorstAt = { v[8], v[9], v[10] };
                        out.mapWorstNormal = { v[12], v[13], v[14] };
                        out.mapWorstExpected = x;
                        out.mapWorstValue = v[11];
                    }
                    mworst = std::max(mworst, r);
                    beyond3 += r > 0.03 ? 1u : 0u;
                    rel.push_back(r);
                    ++mn;
                }
                out.mapMean = mn ? msum / mn : 0;
                out.mapExpectedMean = mn ? mexp / mn : 0;
                out.mapWorst = mworst;
                out.mapBeyond3 = beyond3;
                out.mapProbes = mn;
                if (!rel.empty())
                {
                    std::sort(rel.begin(), rel.end());
                    out.mapP99 = rel[std::min(rel.size() - 1, (size_t)(0.99 * rel.size()))];
                }
            }
            out.radianceMean = n ? rsum / n : 0;
            out.radianceWorst = rworst;
            out.probes = n;
            out.values = values;
            out.mean = n ? sum / n : 0;
            out.minimum = lo;
            out.maximum = hi;
            out.worst = worst;
            if (out.converged < 0 && n && out.expectedMean > 0 && std::fabs(out.mean / out.expectedMean - 1) < 0.01) out.converged = (int)f;
            if (f == frames - 1 || (f & (f - 1)) == 0)
                logf("  frame %3u: mean E %.4f (expected %.4f, %+.2f %%), min %.4f max %.4f, worst probe %.2f %%; K radiance mean %.4f (expected %.4f), worst %.2f %%\n", f,
                     out.mean, out.expectedMean, 100 * (out.mean / out.expectedMean - 1), lo, hi, 100 * worst, out.radianceMean, expectedRadiance, 100 * rworst);
        }
        logf("  cache maps at %u probe points: mean E %.5f (expected %.5f, %+.3f %%), worst %.3f %%\n", out.mapProbes, out.mapMean, out.mapExpectedMean,
             out.mapExpectedMean > 0 ? 100 * (out.mapMean / out.mapExpectedMean - 1) : 0.0, 100 * out.mapWorst);
        logf("    P99 %.3f %%; worst at (%.3f, %.3f, %.3f) n (%.2f, %.2f, %.2f): %.5f against %.5f\n", 100 * out.mapP99, out.mapWorstAt.x, out.mapWorstAt.y,
             out.mapWorstAt.z, out.mapWorstNormal.x, out.mapWorstNormal.y, out.mapWorstNormal.z, out.mapWorstValue, out.mapWorstExpected);
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
        bool validate = false, determinism = false;
        std::vector<std::string> overrides;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--frames" && i + 1 < argc) frames = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--validate") validate = true;
            else if (a == "--determinism") determinism = true;
            else if (a == "--set" && i + 1 < argc) overrides.push_back(argv[++i]);
            else fail("unknown argument %s", a.c_str());
        }
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const std::string& o : overrides) quality.applyOverride(o);
        DeviceOptions options;
        options.debugLayer = validate;
        options.gpuValidation = validate;
        Device device(options);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        bool pass = true;

        if (determinism)
        {
            // gi.deterministic: two runs of the furnace in one process must give bit-identical probe values; without it the
            // atomic arrival order (selection, entry indices as seeds) makes them differ (shows the check can fail).
            for (const bool on : { false, true })
            {
                QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
                q.applyOverride(on ? "gi.deterministic=true" : "gi.deterministic=false");
                const float le = 1.0f, rho = 0.5f;
                auto expected = [&](float3, float3) { return (double)kPi * le / (1 - rho); };
                const Outcome r1 = run(device, shaders, q, furnace(le, rho), { 0, 0, 0 }, { 0, 0, 0 }, expected, le / (1 - rho), frames, 1920, 1080);
                const Outcome r2 = run(device, shaders, q, furnace(le, rho), { 0, 0, 0 }, { 0, 0, 0 }, expected, le / (1 - rho), frames, 1920, 1080);
                size_t differ = 0, field[4] = { 0, 0, 0, 0 };  // irradiance + occlusion, K radiance, position, normal
                for (size_t i = 0; i < std::min(r1.values.size(), r2.values.size()); ++i)
                    if (std::memcmp(&r1.values[i], &r2.values[i], 4) != 0)
                    {
                        ++differ;
                        ++field[(i % 16) / 4];
                    }
                logf("  differing values by field: irradiance/occlusion %zu, K radiance %zu, position %zu, normal %zu\n", field[0], field[1], field[2], field[3]);
                logf("determinism %s: two furnace runs, %zu of %zu probe values not bit-identical\n", on ? "on" : "off", differ, r1.values.size());
                if (on) pass = pass && differ == 0 && r1.values.size() == r2.values.size() && !r1.values.empty();
            }
            logf("RESULT %s\n", pass ? "PASS" : "FAIL");
            return pass ? 0 : 1;
        }

        const float le = 1.0f, rho = 0.5f;
        logf("white furnace: Le %.2f, albedo %.2f, expected E = pi Le / (1 - rho) = %.4f\n", le, rho, kPi * le / (1 - rho));
        const Outcome a = run(device, shaders, quality, furnace(le, rho), { 0, 0, 0 }, { 0, 0, 0 }, [&](float3, float3) { return (double)kPi * le / (1 - rho); },
                              le / (1 - rho), frames, 1920, 1080);
        // Gate: the cache irradiance maps (the representation pixels evaluate from design 2.5 rev. D); the screen probes' SH
        // (per ray, interim until D) is reported.
        const bool okA = std::fabs(a.mapMean / (kPi * le / (1 - rho)) - 1) < 0.01 && a.mapWorst < 0.03 && a.radianceWorst < 0.03 && a.tilePixels > 0 && a.tileMismatches == 0;
        logf("white furnace: %u probes, mean %+.3f %%, worst probe %.3f %%, within 1 %% from frame %d; K radiance worst %.3f %% -> %s\n", a.probes,
             100 * (a.mean / (kPi * le / (1 - rho)) - 1), 100 * a.worst, a.converged, 100 * a.radianceWorst, okA ? "PASS" : "FAIL");
        pass = pass && okA;

        logf("open sky: L 1, ground albedo 0.5, expected E = pi\n");
        const Outcome b = run(device, shaders, quality, openSky(0.5f), { 1, 1, 1 }, { 0, 0, 0 }, [](float3, float3) { return (double)kPi; }, 1.0, frames, 1920, 1080);
        const bool okB = std::fabs(b.mapMean / kPi - 1) < 0.01 && b.mapWorst < 0.03 && b.radianceWorst < 0.03 && b.tilePixels > 0 && b.tileMismatches == 0;
        logf("open sky: %u probes, mean %+.3f %%, worst probe %.3f %%, within 1 %% from frame %d; K radiance worst %.3f %% -> %s\n", b.probes, 100 * (b.mean / kPi - 1),
             100 * b.worst, b.converged, 100 * b.radianceWorst, okB ? "PASS" : "FAIL");
        pass = pass && okB;
        {
            // Instance edit (B3): after the cache converged under the open sky, a black 2 m cube appears as a new instance
            // of a mesh already uploaded (a destruction fragment's path: GpuScene::setInstances, an incremental ray scene).
            // The epoch stays; GiInvalidate restarts the entries whose texel rays can see the cube's box. Expected ground
            // irradiance: pi L (1 - F), F = the view factor of the cube faces the point sees (Lambert per face).
            scene::Scene cubeScene = openSkyWithCube(0.5f);
            const float3 centre{ 5, 1, -14 };
            const uint32_t editFrame = frames / 2;
            auto edit = [&](uint32_t f, GpuScene& gs) {
                if (f != editFrame) return;
                scene::Instance cube;
                cube.mesh = 1;
                cube.transform.m[0][3] = centre.x;
                cube.transform.m[1][3] = centre.y;
                cube.transform.m[2][3] = centre.z;
                cubeScene.instances.push_back(cube);
                const uint32_t added[1] = { (uint32_t)cubeScene.instances.size() - 1 };
                gs.setInstances(added);
            };
            auto withCube = [&](float3 p, float3 n) -> double {
                if (n.y < 0.99f || std::fabs(p.y) > 0.05f) return -1;  // ground probes only
                // Within 0.5 m of the cube (about 4 cache cells there): the irradiance falls faster at the contact than the
                // cells resolve; contact occlusion is the screen's near occlusion (M), not the cache's.
                if (std::fabs(p.x - centre.x) < 1.5f && std::fabs(p.z - centre.z) < 1.5f) return -1;
                double f = 0;
                const float3 axes[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
                for (int a = 0; a < 3; ++a)
                    for (float sign : { -1.0f, 1.0f })
                    {
                        const float3 fn = axes[a] * sign, u = axes[(a + 1) % 3], v = axes[(a + 2) % 3];
                        const float3 fc = centre + fn;
                        if (dot(fn, p - fc) <= 0) continue;  // a face the point does not see
                        f += viewFactor(p, { 0, 1, 0 }, { fc - u - v, fc + u - v, fc + u + v, fc - u + v });
                    }
                return kPi * (1 - f);
            };
            const Outcome e = run(device, shaders, quality, cubeScene, { 1, 1, 1 }, { 0, 0, 0 }, withCube, -1, frames, 1920, 1080, 1, 0, edit);
            uint32_t afterSum = 0, steadyBefore = 0;  // entries updated from a reset history, 8 frames each side of the edit
            for (uint32_t f = editFrame; f < e.resetsPerFrame.size() && f < editFrame + 8; ++f) afterSum += e.resetsPerFrame[f];
            for (uint32_t f = editFrame >= 8 ? editFrame - 8 : 0; f < editFrame; ++f) steadyBefore += e.resetsPerFrame[f];
            const bool sameEpoch = e.epochPerFrame.size() == frames && e.epochPerFrame[editFrame - 1] == e.epochPerFrame.back();
            // Control: the same cube there from the first frame (the cache's own resolution near the cube); the edited
            // run must match it (invalidation leaves no stale entry), both against the analytic answer.
            scene::Scene controlScene = openSkyWithCube(0.5f);
            {
                scene::Instance cube;
                cube.mesh = 1;
                cube.transform.m[0][3] = centre.x;
                cube.transform.m[1][3] = centre.y;
                cube.transform.m[2][3] = centre.z;
                controlScene.instances.push_back(cube);
            }
            const Outcome k = run(device, shaders, quality, controlScene, { 1, 1, 1 }, { 0, 0, 0 }, withCube, -1, frames - editFrame, 1920, 1080);
            logf("  control (the cube from the first frame, %u frames): mean E %.5f against %.5f (%+.3f %%), worst %.3f %%, P99 %.3f %%\n", frames - editFrame,
                 k.mapMean, k.mapExpectedMean, 100 * (k.mapMean / k.mapExpectedMean - 1), 100 * k.mapWorst, 100 * k.mapP99);
            // Stale entries after the edit show as a cluster of probes beyond 3 %: their count against the control's within
            // its Poisson spread (the single worst probe of ~32k noisy ones varied 4.8 - 6.5 % between identical runs).
            const double beyondLimit = k.mapBeyond3 + 3 * std::sqrt((double)k.mapBeyond3) + 5;
            const bool okE = e.mapProbes > 1000 && std::fabs(e.mapMean / e.mapExpectedMean - 1) < 0.01 && e.mapP99 < k.mapP99 + 0.005 &&
                             e.mapBeyond3 <= beyondLimit && afterSum > steadyBefore && sameEpoch;
            logf("  probes beyond 3 %%: %u (control %u, limit %.0f)\n", e.mapBeyond3, k.mapBeyond3, beyondLimit);
            logf("instance edit (B3): a cube appended after %u frames; epoch %s; resets in the 8 frames before %u, after %u (live entries %u); "
                 "ground under the cube's shadow of sky: cache maps at %u probe points, mean E %.5f against %.5f (%+.3f %%), worst %.3f %% -> %s\n",
                 editFrame, sameEpoch ? "kept" : "CHANGED", steadyBefore, afterSum, e.stats.live, e.mapProbes, e.mapMean, e.mapExpectedMean, 100 * (e.mapMean / e.mapExpectedMean - 1),
                 100 * e.mapWorst, okE ? "PASS" : "FAIL");
            logf("  P99 %.3f %%; worst at (%.3f, %.3f, %.3f): %.5f against %.5f\n", 100 * e.mapP99, e.mapWorstAt.x, e.mapWorstAt.y, e.mapWorstAt.z,
                 e.mapWorstValue, e.mapWorstExpected);
            pass = pass && okE;
        }
        {
            const float groundAlbedo = 0.5f;
            const float3 l = normalize(float3{ 1, 1.2f, 0.4f });
            logf("sunlit ground and a black wall: ground albedo %.2f, sun E 1 at elevation %.1f deg, no sky; wall probes expect rho E cos F (Lambert per probe)\n",
                 groundAlbedo, std::asin(l.y) * 180 / kPi);
            const std::vector<float3> lit = { { 0, 0, -200 }, { 200, 0, -200 }, { 200, 0, 200 }, { 0, 0, 200 } };  // ground in front of the wall
            // Wall probes only (normal +x, on the wall: |z| < 30, y < 20); the ground's own probes see a black wall and no sky.
            auto wallExpected = [&](float3 p, float3 n) -> double {
                if (n.x < 0.99f || std::fabs(p.x) > 0.05f || std::fabs(p.z) > 29.5f || p.y > 19.5f || p.y < 0.05f) return -1;
                return groundAlbedo * l.y * viewFactor(p, { 1, 0, 0 }, lit);
            };
            const float lobeAlpha = 0.25f;  // perceptual roughness 0.5
            const scene::Scene wallScene = sunWall(groundAlbedo, l);
            const Outcome c = run(device, shaders, quality, wallScene, { 0, 0, 0 }, { 1, 1, 1 }, wallExpected, 0, frames, 1920, 1080, 1, lobeAlpha);
            const bool okC = c.mapProbes > 1000 && std::fabs(c.mapMean / c.mapExpectedMean - 1) < 0.01 && c.mapWorst < 0.03 && c.tileMismatches == 0;
            logf("sunlit ground and a black wall: %u wall probes, mean E %.5f against %.5f (%+.3f %%), worst probe %.3f %%, within 1 %% from frame %d -> %s\n", c.probes,
                 c.mean, c.expectedMean, 100 * (c.mean / c.expectedMean - 1), 100 * c.worst, c.converged, okC ? "PASS" : "FAIL");
            pass = pass && okC;
            // K by the lobe at the wall probes (head-on above, then a grazing view along the wall): the ground in front of
            // the wall (x in [0, 200], |z| <= 200) is lit with radiance rho E cos / pi (the wall is black, no sky), every
            // other direction is dark.
            const double groundL = groundAlbedo * l.y / kPi;
            auto incident = [&](float3 p, float3 d) -> double {
                if (d.y >= 0) return 0;
                const float t = -p.y / d.y;
                const float x = p.x + d.x * t, z = p.z + d.z * t;
                return x >= 0 && x <= 200 && std::fabs(z) <= 200 ? groundL : 0;
            };
            auto lobeCheck = [&](const scene::Scene& ws, const Outcome& oc, const char* viewName) {
                double lobeErr = 0, splitErr = 0, lobeWorst = 0, splitWorst = 0, expSum = 0, lobeSigned = 0, splitSigned = 0;
                uint32_t nLobe = 0;
                double binL[5] = {}, binS[5] = {}, binX[5] = {}, binA[5] = {}, binB[5] = {};
                uint32_t binN[5] = {};
                for (size_t i = 0; i < oc.values.size() / 16; ++i)
                {
                    const float* v = &oc.values[16 * i];
                    const float3 p{ v[8], v[9], v[10] }, nn{ v[12], v[13], v[14] };
                    if (v[3] < 0 || wallExpected(p, nn) < 0) continue;
                    const float3 view = normalize(ws.cameras[0].position - p);
                    const double x = lobeMean(p, nn, view, lobeAlpha, incident);
                    lobeErr += std::fabs(v[15] - x);
                    splitErr += std::fabs(v[7] - x);
                    lobeWorst = std::max(lobeWorst, std::fabs(v[15] - x) / groundL);
                    splitWorst = std::max(splitWorst, std::fabs(v[7] - x) / groundL);
                    expSum += x;
                    const int bin = std::clamp((int)(dot(nn, view) * 5), 0, 4);
                    binL[bin] += v[15] - x;
                    binS[bin] += v[7] - x;
                    binA[bin] += std::fabs(v[15] - x);
                    binB[bin] += std::fabs(v[7] - x);
                    binX[bin] += x;
                    ++binN[bin];
                    lobeSigned += v[15] - x;
                    splitSigned += v[7] - x;
                    ++nLobe;
                }
                const double lobeMeanErr = nLobe ? lobeErr / expSum : 1, splitMeanErr = nLobe ? splitErr / expSum : 1;
                logf("K path by the lobe (alpha %.2f), %s, %u wall probes: mean |error| %.2f %% of the expected (worst %.2f %% of the ground radiance, signed "
                     "%+.2f %%); the split-sum lookup for the same lobe %.2f %% (worst %.2f %%, signed %+.2f %%)\n",
                     lobeAlpha, viewName, nLobe, 100 * lobeMeanErr, 100 * lobeWorst, 100 * lobeSigned / expSum, 100 * splitMeanErr, 100 * splitWorst,
                     100 * splitSigned / expSum);
                for (int bin = 0; bin < 5; ++bin)
                    if (binN[bin])
                        logf("  NoV %.1f-%.1f: %u probes, expected %.5f, lobe signed %+.2f %% |%.2f| %%, split signed %+.2f %% |%.2f| %%\n", bin * 0.2,
                             bin * 0.2 + 0.2, binN[bin], binX[bin] / binN[bin], 100 * binL[bin] / binX[bin], 100 * binA[bin] / binX[bin],
                             100 * binS[bin] / binX[bin], 100 * binB[bin] / binX[bin]);
                return std::tuple<double, double, double>{ lobeMeanErr, lobeWorst, splitMeanErr };
            };
            const auto headOn = lobeCheck(wallScene, c, "head-on view");
            scene::Scene grazingScene = wallScene;
            grazingScene.cameras[0].name = "along the wall";
            grazingScene.cameras[0].position = { 3.5f, 4, -29 };
            grazingScene.cameras[0].forward = normalize(float3{ -0.22f, -0.08f, 1 });
            grazingScene.cameras[0].up = normalize(cross(cross(grazingScene.cameras[0].forward, float3{ 0, 1, 0 }), grazingScene.cameras[0].forward));
            const Outcome cg = run(device, shaders, quality, grazingScene, { 0, 0, 0 }, { 1, 1, 1 }, wallExpected, 0, frames, 1920, 1080, 1, lobeAlpha);
            const auto grazing = lobeCheck(grazingScene, cg, "grazing view");
            // Gate: at both views within 6 % mean |error| and 10 % of the ground radiance at any probe (the 8 x 8 maps'
            // resolution bound, see ScreenProbes.hlsli), and at the grazing view less than half the split sum's error.
            const bool okK = std::get<0>(headOn) < 0.06 && std::get<1>(headOn) < 0.1 && std::get<0>(grazing) < 0.06 && std::get<1>(grazing) < 0.1 &&
                             std::get<0>(grazing) < 0.5 * std::get<2>(grazing);
            logf("K path by the lobe: head-on %.2f %%, grazing %.2f %% (split sum %.2f %%) -> %s\n", 100 * std::get<0>(headOn), 100 * std::get<0>(grazing),
                 100 * std::get<2>(grazing), okK ? "PASS" : "FAIL");
            pass = pass && okK;
        }
        for (const float tilt : { 0.0f, 25.0f })
        {
            const float3 l = normalize(float3{ 0.3f, 1, 0.5f });
            const float t = tilt * kPi / 180;
            const float direct = std::max(0.0f, dot(float3{ -std::sin(t), std::cos(t), 0 }, l));
            logf("single sunlit plane tilted %.0f deg: albedo 0.5, sun E 1 (%.3f on the plane), no sky; expected indirect E = 0\n", tilt, direct);
            const Outcome d = run(device, shaders, quality, sunPlane(0.5f, tilt, l), { 0, 0, 0 }, { 1, 1, 1 }, [](float3, float3) { return 0.0; }, 0, frames, 1920, 1080);
            const bool okD = d.probes > 1000 && d.mean < 1e-3 * direct && d.worst < 1e-2 * direct;
            logf("single sunlit plane tilted %.0f deg: %u probes, mean indirect E %.6f (%.4f %% of the direct sun on it), worst probe %.6f -> %s\n", tilt, d.probes,
                 d.mean, 100 * d.mean / direct, d.worst, okD ? "PASS" : "FAIL");
            pass = pass && okD;
        }
        {
            const double band = std::sin(10.0 * kPi / 180), exact = kPi * band * band;  // pi L cos^2(80 deg), L = 1
            logf("horizon band sky: L 1 between elevation 0 and 10 deg, ground albedo 0.5; expected E = %.5f\n", exact);
            const Outcome e = run(device, shaders, quality, openSky(0.5f), { 1, 1, 1 }, { 0, 0, 0 }, [&](float3, float3) { return exact; }, 0, frames, 1920, 1080,
                                  (float)band);
            // The cache's irradiance maps (per ray, 9 x 9, exact at the anchor normal) are the gate; the screen probes' SH
            // (layer D replaces it) is reported for comparison.
            const bool okE = e.mapProbes > 1000 && std::fabs(e.mapMean / exact - 1) < 0.01;
            logf("horizon band sky: cache maps at %u probe points, mean E %.5f against %.5f (%+.2f %%), worst %.2f %%; screen probes (SH) %.5f (%+.2f %%) -> %s\n",
                 e.mapProbes, e.mapMean, exact, 100 * (e.mapMean / exact - 1), 100 * e.mapWorst, e.mean, 100 * (e.mean / exact - 1), okE ? "PASS" : "FAIL");
            pass = pass && okE;
        }
        {
            // Emissive mesh (B2): floor irradiance pi Le F under the panel, from the GI rays' emitter samples with MIS.
            const float panelLe = 10;
            const std::vector<float3> panel = { { -0.5f, 1, -0.5f }, { 0.5f, 1, -0.5f }, { 0.5f, 1, 0.5f }, { -0.5f, 1, 0.5f } };
            auto panelExpected = [&](float3 p, float3 n) -> double {
                if (n.y < 0.99f || std::fabs(p.y) > 0.05f || std::fabs(p.x) > 2.5f || std::fabs(p.z) > 2.5f) return -1;
                if (std::fabs(p.x) < 0.6f && std::fabs(p.z) < 0.6f) return -1;  // under the panel's edge the floor is hidden from the camera
                return kPi * panelLe * viewFactor(p, { 0, 1, 0 }, panel);
            };
            logf("emissive panel: 1 x 1 m at height 1, Le %.0f, black floor, no sun or sky; floor probes expect pi Le F\n", panelLe);
            const Outcome f = run(device, shaders, quality, emissivePanel(panelLe), { 0, 0, 0 }, { 0, 0, 0 }, panelExpected, 0, frames, 1920, 1080);
            // Control: the texel rays alone (gi.experiment_disable 256). Both estimators are unbiased for the same cache, so
            // their means agree; the emitter samples with MIS must cut the per-probe error (P99) at least in half. What both
            // share (a small excess where E falls steeply: the cache cells' spatial resolution) is judged by the 1 % mean.
            QualityConfig textelOnly = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
            for (const std::string& ov : overrides) textelOnly.applyOverride(ov);
            textelOnly.applyOverride("gi.experiment_disable=256");
            const Outcome g = run(device, shaders, textelOnly, emissivePanel(panelLe), { 0, 0, 0 }, { 0, 0, 0 }, panelExpected, 0, frames, 1920, 1080);
            const bool okF = f.mapProbes > 1000 && std::fabs(f.mapMean / f.mapExpectedMean - 1) < 0.01 && std::fabs(f.mapMean / g.mapMean - 1) < 0.005 &&
                             f.mapP99 < 0.5 * g.mapP99;
            logf("emissive panel: cache maps at %u probe points, mean E %.4f against %.4f (%+.2f %%), P99 %.2f %%, worst %.2f %%; texel rays alone: mean %.4f, "
                 "P99 %.2f %% -> %s\n", f.mapProbes, f.mapMean, f.mapExpectedMean, 100 * (f.mapMean / f.mapExpectedMean - 1), 100 * f.mapP99, 100 * f.mapWorst,
                 g.mapMean, 100 * g.mapP99, okF ? "PASS" : "FAIL");
            logf("  worst probe at (%.3f, %.3f, %.3f): E %.4f against %.4f\n", f.mapWorstAt.x, f.mapWorstAt.y, f.mapWorstAt.z, f.mapWorstValue, f.mapWorstExpected);
            pass = pass && okF;
        }
        logf("probe tile cache: screenProbeGatherTile vs screenProbeGather, %u + %u pixel evaluations, %u + %u not bit-identical (2 cones each)\n", a.tilePixels,
             b.tilePixels, a.tileMismatches, b.tileMismatches);

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
