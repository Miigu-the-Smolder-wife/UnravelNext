// Track W view grid microbench (FEATURES_GAME 1.8 B; before the implementation, as the coordination asked): the floors
// the view grid's cost formula rests on, on the real ocean field at 4K:
//   (1) displacement sampling: 3 cascades per grid point, SampleLevel (isotropic) and SampleGrad (anisotropic 16)
//   (2) 64-bit atomic min on the pixel key buffer: screen order at depth complexity 1..3, and hashed pixels
//   (3) the whole far-field scatter (displace + project + 1 px triangles + atomic min) for open sea, a waterside lake and
//       a swimming camera, each with both samplers, one pass (halo points evaluated again by the neighbouring group) and
//       two passes (every vertex evaluated once, then the triangles)
// Correctness, on every device (WARP runs it at 480 x 270 without timing): the two-pass scatter covers every pixel whose
// water lies well inside the far field (no holes; the one-pass variant's holes are printed: it is watertight only if
// the sampler returns identical bits for a point evaluated twice, which WARP's SampleGrad does not); the hashed atomic
// min equals a CPU min per pixel.
//   unx_test_water_viewgridbench [--no-debug-layer] [--time | --warp]
#include "unx/water/LinearDispatch.h"
#include "unx/water/Ocean.h"

#include "unx/render/GpuProfiler.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;
using unx::water::Ocean;
using unx::water::OceanDesc;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
constexpr double kPi = 3.14159265358979323846;

ComPtr<ID3D12Device> warpDevice()
{
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "WARP device");
    return device;
}
struct Gpu
{
    ComPtr<ID3D12Device> external;
    Device device;
    ShaderLibrary shaders;
    Gpu(bool debugLayer, bool warp)
        : external(warp ? warpDevice() : nullptr), device([&] {
              DeviceOptions o;
              o.debugLayer = debugLayer && !warp;
              o.externalDevice = external.Get();
              return o;
          }()),
          shaders(device, executableDirectory() / "shaders") {}
};
ComPtr<ID3D12Resource> buffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "test buffer");
    return r;
}
struct Input
{
    ComPtr<ID3D12Resource> resource;
    uint32_t srv = 0;
    Device* device = nullptr;
    Input(Device& d, const void* data, uint64_t bytes) : device(&d)
    {
        resource = buffer(d, bytes, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped = nullptr;
        check(resource->Map(0, nullptr, &mapped), "map input");
        std::memcpy(mapped, data, bytes);
        resource->Unmap(0, nullptr);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = UINT(std::max<uint64_t>(bytes, 256) / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        srv = d.descriptors().allocateResource();
        d.d3d()->CreateShaderResourceView(resource.Get(), &sd, d.descriptors().resourceCpu(srv));
    }
    ~Input()
    {
        device->deferRelease(resource);
        device->descriptors().freeResource(srv);
    }
};

struct V3
{
    double x, y, z;
};
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator*(double s, V3 a) { return { s * a.x, s * a.y, s * a.z }; }
V3 cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
V3 normalize(V3 a) { const double l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); return { a.x / l, a.y / l, a.z / l }; }

struct Scene
{
    const char* name;
    V3 camera;
    double yaw, pitch;  // radians; yaw from +x towards +z
    bool lake = false;
    double lakeCentre[2] = {}, lakeRadius = 0;
};
struct View
{
    uint32_t width, height;
    double hfov = kPi / 3;
    double nearRadius = 16, farDistance = 1e6, bound = 3.5;  // far: open sea to the horizon; bound: horizontal + vertical displacement (m)
};
struct Grid
{
    std::vector<float> params;  // 32 floats of header, then (r_j, along spacing) per row (ViewGrid.hlsli)
    double phiLo, phiHi, eLo, eHi;  // screen window
    uint32_t columns, rows;
    V3 right, up, forward;
    double tanX, tanY;
};
V3 pixelDirection(const Grid& g, const View& v, double px, double py)
{
    const double sx = (px / v.width) * 2 - 1, sy = 1 - (py / v.height) * 2;
    return normalize(g.forward + (sx * g.tanX) * g.right + (sy * g.tanY) * g.up);
}
Grid makeGrid(const Scene& s, const View& v, float waterLevel, const float lengths[3])
{
    Grid g;
    g.forward = { std::cos(s.pitch) * std::cos(s.yaw), std::sin(s.pitch), std::cos(s.pitch) * std::sin(s.yaw) };
    g.right = normalize(cross(g.forward, { 0, 1, 0 }));
    g.up = cross(g.right, g.forward);
    g.tanX = std::tan(v.hfov / 2);
    g.tanY = g.tanX * v.height / v.width;
    const double theta = 2 * g.tanX / v.width;  // one pixel at the image centre
    // Screen window in (world azimuth, elevation) from the border pixels.
    g.phiLo = g.eLo = 1e9;
    g.phiHi = g.eHi = -1e9;
    auto add = [&](double px, double py) {
        const V3 d = pixelDirection(g, v, px, py);
        double phi = std::atan2(d.z, d.x);
        while (phi - s.yaw > kPi) phi -= 2 * kPi;
        while (phi - s.yaw < -kPi) phi += 2 * kPi;
        const double e = std::asin(d.y);
        g.phiLo = std::min(g.phiLo, phi); g.phiHi = std::max(g.phiHi, phi);
        g.eLo = std::min(g.eLo, e); g.eHi = std::max(g.eHi, e);
    };
    for (uint32_t x = 0; x <= v.width; x += 4) { add(x, 0); add(x, v.height); }
    for (uint32_t y = 0; y <= v.height; y += 4) { add(0, y); add(v.width, y); }
    const double h = s.camera.y - waterLevel, margin = v.bound / v.nearRadius;
    double phi0 = g.phiLo - margin, phi1 = g.phiHi + margin;
    double e0 = std::max(g.eLo - margin, -std::atan(h / v.nearRadius)), e1 = std::min(g.eHi + margin, 0.0);  // rows at the horizon clamp to the far ring
    if (s.lake)
    {
        // The lake's angular box (its boundary circle and the ring inside it) narrows the window.
        double lp0 = 1e9, lp1 = -1e9, le0 = 1e9, le1 = -1e9;
        for (int k = 0; k < 720; ++k)
            for (double r : { s.lakeRadius + v.bound, 0.5 * s.lakeRadius, 0.0 })
            {
                const double a = 2 * kPi * k / 720;
                const double x = s.lakeCentre[0] + r * std::cos(a) - s.camera.x, z = s.lakeCentre[1] + r * std::sin(a) - s.camera.z;
                double phi = std::atan2(z, x);
                while (phi - s.yaw > kPi) phi -= 2 * kPi;
                while (phi - s.yaw < -kPi) phi += 2 * kPi;
                const double e = -std::atan(h / std::max(std::hypot(x, z), 1e-3));
                lp0 = std::min(lp0, phi); lp1 = std::max(lp1, phi); le0 = std::min(le0, e); le1 = std::max(le1, e);
            }
        phi0 = std::max(phi0, lp0 - margin); phi1 = std::min(phi1, lp1 + margin);
        e0 = std::max(e0, le0 - margin); e1 = std::min(e1, le1 + margin);
    }
    g.columns = uint32_t(std::max(0.0, std::ceil((phi1 - phi0) / theta))) + 1;
    // Rows by rest distance: spacing min(one pixel row on the still plane, 2.5 footprints while a crest of the displacement
    // bound's height can reach 0.5 px there (r <= 2 A / theta)), from the window's nearest to its farthest distance.
    const double rStart = std::max(v.nearRadius, h / std::tan(std::min(-e0, 0.5 * kPi - 1e-6)));
    const double rEnd = e1 >= 0 ? v.farDistance : std::min(v.farDistance, h / std::tan(-e1));
    std::vector<float> table;
    for (double r = rStart;;)
    {
        const double flat = theta * (h * h + r * r) / h, crest = r <= 2 * v.bound / theta ? 2.5 * r * theta : 1e30;
        const double step = std::min(flat, crest);
        table.push_back(float(r));
        table.push_back(float(step));
        if (r >= rEnd) break;
        r = std::min(r + step, rEnd);
    }
    g.rows = uint32_t(table.size() / 2);
    g.params.assign(128, 0.0f);  // ViewGrid.hlsli header (512 B; no near levels in the bench)
    g.params.insert(g.params.end(), table.begin(), table.end());
    float* p = g.params.data();
    const float row0[4] = { float(s.camera.x), float(s.camera.y), float(s.camera.z), waterLevel };
    const float row1[4] = { float(g.right.x), float(g.right.y), float(g.right.z), float(g.tanX) };
    const float row2[4] = { float(g.up.x), float(g.up.y), float(g.up.z), float(g.tanY) };
    const float row3[4] = { float(g.forward.x), float(g.forward.y), float(g.forward.z), float(v.farDistance) };
    const float row4[4] = { float(phi0), 0, float(theta), float(v.nearRadius) };
    const uint32_t row5[4] = { g.columns, g.rows, v.width, v.height };
    const float row6[4] = { lengths[0], lengths[1], lengths[2], 0 };
    const float lake[3] = { float(s.lakeCentre[0]), float(s.lakeCentre[1]), float(s.lakeRadius) };
    const uint32_t lakeOn = s.lake ? 1 : 0;
    std::memcpy(p, row0, 16); std::memcpy(p + 4, row1, 16); std::memcpy(p + 8, row2, 16); std::memcpy(p + 12, row3, 16);
    std::memcpy(p + 16, row4, 16); std::memcpy(p + 20, row5, 16); std::memcpy(p + 24, row6, 16); std::memcpy(p + 28, lake, 12); std::memcpy(p + 31, &lakeOn, 4);
    return g;
}

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, time = false, warp = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--time") time = true;
            if (std::string(argv[i]) == "--warp") warp = true;
        }
        if (warp && time) fail("--time measures hardware; WARP runs correctness only");
        Gpu gpu(debugLayer, warp);
        OceanDesc od;
        od.windSpeed = 10.0f;
        od.windDirection = 0.4f;
        od.seed = 3;
        Ocean ocean(gpu.device, gpu.shaders, od);
        View view;
        view.width = warp ? 480 : 3840;
        view.height = warp ? 270 : 2160;
        const float waterLevel = 0;
        const Scene scenes[] = {
            { "open sea, camera 12 m, pitch -10 deg", { 3.7, 12.0, -5.3 }, 0.3, -10 * kPi / 180 },
            { "waterside lake (r 150 m, 220 m ahead), camera 12 m, pitch -10 deg", { 3.7, 12.0, -5.3 }, 0.3, -10 * kPi / 180, true, { 3.7 + 220 * std::cos(0.3), -5.3 + 220 * std::sin(0.3) }, 150 },
            { "swimming, camera 0.3 m, pitch 0 (far field only)", { 3.7, 0.3, -5.3 }, 0.3, 0 },
        };
        ID3D12PipelineState* raster = gpu.shaders.compute("Passes/Water/Tests/ViewGridBenchRaster");
        ID3D12PipelineState* sampler = gpu.shaders.compute("Passes/Water/Tests/ViewGridBenchSample");
        ID3D12PipelineState* atomics = gpu.shaders.compute("Passes/Water/Tests/ViewGridBenchAtomic");
        ID3D12PipelineState* vertexPass = gpu.shaders.compute("Passes/Water/Tests/ViewGridBenchVertex");
        const uint32_t pixels = view.width * view.height;
        GpuProfiler profiler(gpu.device, 1, 64);
        uint64_t frameIndex = 0;

        for (const Scene& scene : scenes)
        {
            const Grid grid = makeGrid(scene, view, waterLevel, od.lengths);
            Input params(gpu.device, grid.params.data(), grid.params.size() * 4);
            const uint32_t groupsX = (grid.columns + 7) / 8, groupsY = (grid.rows + 7) / 8;
            for (uint32_t mode = 0; mode < 4; ++mode)
            {
                const uint32_t aniso = mode & 1;
                const bool twoPass = mode >= 2;
                const char* variant = mode == 0 ? "one pass, SampleLevel" : mode == 1 ? "one pass, SampleGrad aniso 16" : mode == 2 ? "two passes, SampleLevel" : "two passes, SampleGrad aniso 16";
                std::vector<double> ms, sampleMs;
                std::vector<uint64_t> keys;
                uint32_t counters[3] = {};
                const int frames = time ? 24 : 1;
                for (int f = 0; f < frames; ++f)
                {
                    RenderGraph g(gpu.device);
                    const auto fields = ocean.record(g, 37.25);
                    const BufferRef key = g.createBuffer({ "view grid keys", uint64_t(pixels) * 8, 0 });
                    const BufferRef counter = g.createBuffer({ "view grid counters", 256, 0 });
                    const BufferRef waves = g.createBuffer({ "view grid sample sums", uint64_t(grid.columns) * grid.rows + 256, 0 });
                    const BufferRef vertices = g.createBuffer({ "view grid vertices", uint64_t(grid.columns) * grid.rows * 12 + 256, 0 });
                    const TextureRef displacement = fields.displacement;
                    const uint32_t paramSrv = params.srv;
                    if (twoPass)
                        g.addPass("view grid vertices", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(displacement, Use::SrvCompute); pb.use(vertices, Use::UavCompute); },
                                  [=](PassContext& c) {
                                      const uint32_t k[8] = { paramSrv, c.srv(displacement), c.uav(vertices), 0, aniso };
                                      c.cmd->SetPipelineState(vertexPass);
                                      c.computeConstants(k, 8);
                                      unx::water::dispatchLinear(c.cmd, (grid.columns * grid.rows + 63) / 64);
                                  });
                    g.addPass("view grid clear", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(key, Use::UavCompute); pb.use(counter, Use::UavCompute); },
                              [=](PassContext& c) {
                                  const uint32_t k[8] = { 0, 0, c.uav(key), 0, 0, pixels, pixels, 0xFFFFFFFFu };
                                  c.cmd->SetPipelineState(atomics);
                                  c.computeConstants(k, 8);
                                  unx::water::dispatchLinear(c.cmd, (pixels + 63) / 64);
                                  const uint32_t z[8] = { 0, 0, c.uav(counter), 0, 0, 32, 32, 0 };  // 256 B of zero
                                  c.computeConstants(z, 8);
                                  c.cmd->Dispatch(1, 1, 1);
                              });
                    g.addPass("view grid scatter", QueueType::Graphics,
                              [&](PassBuilder& pb) {
                                  pb.use(displacement, Use::SrvCompute); pb.use(key, Use::UavCompute); pb.use(counter, Use::UavCompute);
                                  if (twoPass) pb.use(vertices, Use::SrvCompute);
                              },
                              [=](PassContext& c) {
                                  uint32_t k[12] = { paramSrv, c.srv(displacement), c.uav(key), c.uav(counter), mode };
                                  const float bound = float(view.bound), window[4] = { float(grid.phiLo), float(grid.phiHi), float(grid.eLo), float(grid.eHi) };
                                  std::memcpy(&k[5], &bound, 4);
                                  std::memcpy(&k[6], window, 16);
                                  k[10] = twoPass ? c.srv(vertices) : 0;
                                  c.cmd->SetPipelineState(raster);
                                  c.computeConstants(k, 12);
                                  c.cmd->Dispatch(groupsX, groupsY, 1);
                              });
                    if (time && !twoPass && &scene == &scenes[0])
                        g.addPass("view grid sampling", QueueType::Graphics,
                                  [&](PassBuilder& pb) { pb.use(displacement, Use::SrvCompute); pb.use(waves, Use::UavCompute); },
                                  [=](PassContext& c) {
                                      const uint32_t k[8] = { paramSrv, c.srv(displacement), c.uav(waves), 0, aniso };
                                      c.cmd->SetPipelineState(sampler);
                                      c.computeConstants(k, 8);
                                      unx::water::dispatchLinear(c.cmd, (grid.columns * grid.rows + 63) / 64);
                                  });
                    ComPtr<ID3D12Resource> rb;
                    if (!time)
                    {
                        rb = buffer(gpu.device, uint64_t(pixels) * 8 + 256, D3D12_HEAP_TYPE_READBACK);
                        ID3D12Resource* r = rb.Get();
                        g.addPass("view grid read", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(key, Use::CopySrc); pb.use(counter, Use::CopySrc); pb.keep(); },
                                  [=](PassContext& c) {
                                      c.cmd->CopyBufferRegion(r, 0, c.resource(key), 0, uint64_t(pixels) * 8);
                                      c.cmd->CopyBufferRegion(r, uint64_t(pixels) * 8, c.resource(counter), 0, 12);
                                  });
                    }
                    else
                        g.addPass("view grid keep", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(key, Use::SrvCompute); pb.keep(); }, [](PassContext&) {});
                    if (time) profiler.beginFrame(frameIndex++);
                    g.execute(time ? &profiler : nullptr);
                    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
                    if (rb)
                    {
                        const uint8_t* m = nullptr;
                        check(rb->Map(0, nullptr, (void**)&m), "map keys");
                        keys.assign((const uint64_t*)m, (const uint64_t*)m + pixels);
                        std::memcpy(counters, m + uint64_t(pixels) * 8, 12);
                        rb->Unmap(0, nullptr);
                    }
                    double scatterMs = 0;
                    if (time && f >= 8 && profiler.lastCompleted())
                        for (const auto& p : profiler.lastCompleted()->passes)
                        {
                            if (p.name == "view grid scatter" || p.name == "view grid vertices") scatterMs += p.durationMs();
                            if (p.name == "view grid sampling") sampleMs.push_back(p.durationMs());
                        }
                    if (time && f >= 8 && profiler.lastCompleted()) ms.push_back(scatterMs);
                }
                if (!time)
                {
                    // No holes: every pixel whose still-water hit lies well inside the far field (and the lake) is covered.
                    const double h = scene.camera.y - waterLevel;
                    uint64_t required = 0, uncovered = 0, covered = 0;
                    for (uint32_t y = 0; y < view.height; ++y)
                        for (uint32_t x = 0; x < view.width; ++x)
                        {
                            const bool hit = keys[size_t(y) * view.width + x] != ~0ull;
                            covered += hit;
                            const V3 d = pixelDirection(grid, view, x + 0.5, y + 0.5);
                            if (d.y >= 0) continue;
                            const double t = h / -d.y, horizontal = t * std::hypot(d.x, d.z);
                            if (horizontal < view.nearRadius + view.bound + 1 || horizontal > view.farDistance - view.bound - 1) continue;
                            // A displaced hit lies within bound (1 + 1 / tan e) of the still-water hit along the view (the vertical
                            // displacement moves a grazing ray's hit by A / tan e): the lake's edge is checked only that far inside.
                            const double reach = view.bound * (1 + horizontal / h) + 1;
                            if (scene.lake &&
                                std::hypot(scene.camera.x + t * d.x - scene.lakeCentre[0], scene.camera.z + t * d.z - scene.lakeCentre[1]) > scene.lakeRadius - reach)
                                continue;
                            ++required;
                            if (!hit)
                            {
                                if (uncovered < 4) std::printf("  hole at pixel (%u, %u), still-water distance %.1f m\n", x, y, horizontal);
                                ++uncovered;
                            }
                        }
                    std::printf("%s, %s: grid %u x %u, %u triangles past the 8 x 8 loop, %u groups ran, %u non-finite displacements; covered %llu pixels, required %llu, holes %llu\n", scene.name,
                                variant, grid.columns, grid.rows, counters[0], counters[1], counters[2], (unsigned long long)covered,
                                (unsigned long long)required, (unsigned long long)uncovered);
                    W_CHECK(!twoPass || uncovered == 0, "%s, %s: %llu holes", scene.name, variant, (unsigned long long)uncovered);
                    W_CHECK(required > 0, "%s: no pixel to check", scene.name);
                }
                else
                    std::printf("[performance] %s, %s: grid %u x %u (%.2f M points), scatter median %.4f ms%s\n", scene.name, variant,
                                grid.columns, grid.rows, grid.columns * double(grid.rows) * 1e-6, median(ms),
                                sampleMs.empty() ? "" : unx::format(", sampling alone %.4f ms", median(sampleMs)).c_str());
            }
        }

        // 64-bit atomic min: exactness (hashed pixels, CPU min) and throughput.
        {
            const uint32_t depths[] = { 1, 2, 3 };
            for (uint32_t mode = 1; mode <= 2; ++mode)
                for (uint32_t k : depths)
                {
                    if (mode == 2 && k != 1) continue;
                    const uint32_t threads = pixels * k;
                    std::vector<double> ms;
                    std::vector<uint64_t> keys;
                    const int frames = time ? 24 : 1;
                    for (int f = 0; f < frames; ++f)
                    {
                        RenderGraph g(gpu.device);
                        const BufferRef key = g.createBuffer({ "atomic keys", uint64_t(pixels) * 8, 0 });
                        g.addPass("atomic clear", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(key, Use::UavCompute); },
                                  [=](PassContext& c) {
                                      const uint32_t z[8] = { 0, 0, c.uav(key), 0, 0, pixels, pixels, 0xFFFFFFFFu };
                                      c.cmd->SetPipelineState(atomics);
                                      c.computeConstants(z, 8);
                                      unx::water::dispatchLinear(c.cmd, (pixels + 63) / 64);
                                  });
                        g.addPass("atomic min", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(key, Use::UavCompute); },
                                  [=](PassContext& c) {
                                      const uint32_t z[8] = { 0, 0, c.uav(key), 0, mode, threads, pixels, 0 };
                                      c.cmd->SetPipelineState(atomics);
                                      c.computeConstants(z, 8);
                                      unx::water::dispatchLinear(c.cmd, (threads + 63) / 64);
                                  });
                        ComPtr<ID3D12Resource> rb;
                        if (!time)
                        {
                            rb = buffer(gpu.device, uint64_t(pixels) * 8, D3D12_HEAP_TYPE_READBACK);
                            ID3D12Resource* r = rb.Get();
                            g.addPass("atomic read", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(key, Use::CopySrc); pb.keep(); },
                                      [=](PassContext& c) { c.cmd->CopyBufferRegion(r, 0, c.resource(key), 0, uint64_t(pixels) * 8); });
                        }
                        else
                            g.addPass("atomic keep", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(key, Use::SrvCompute); pb.keep(); }, [](PassContext&) {});
                        if (time) profiler.beginFrame(frameIndex++);
                        g.execute(time ? &profiler : nullptr);
                        for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
                        if (rb)
                        {
                            const uint64_t* m = nullptr;
                            check(rb->Map(0, nullptr, (void**)&m), "map atomic keys");
                            keys.assign(m, m + pixels);
                            rb->Unmap(0, nullptr);
                        }
                        if (time && f >= 8 && profiler.lastCompleted())
                            for (const auto& p : profiler.lastCompleted()->passes)
                                if (p.name == "atomic min") ms.push_back(p.durationMs());
                    }
                    if (!time)
                    {
                        auto hash = [](uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; };
                        std::vector<uint64_t> expect(pixels, ~0ull);
                        for (uint32_t i = 0; i < threads; ++i)
                        {
                            const uint32_t pixel = mode == 1 ? i % pixels : hash(i ^ 0x9E3779B9u) % pixels;
                            expect[pixel] = std::min(expect[pixel], (uint64_t(hash(i) & 0x7FFFFFFFu) << 32) | i);
                        }
                        W_CHECK(keys == expect, "atomic min mode %u depth %u differs from the CPU min", mode, k);
                        std::printf("atomic min %s, depth complexity %u: equals the CPU min at all %u pixels\n", mode == 1 ? "screen order" : "hashed pixels", k, pixels);
                    }
                    else
                        std::printf("[performance] 64-bit atomic min, %s, depth complexity %u: %.4f ms for %.2f M atomics (%.2f G/s)\n", mode == 1 ? "screen order" : "hashed pixels", k,
                                    median(ms), threads * 1e-6, threads / (median(ms) * 1e-3) * 1e-9);
                }
        }
        if (!warp)
        {
            const uint32_t errors = gpu.device.drainDebugMessages();
            W_CHECK(errors == 0, "%u debug-layer errors", errors);
        }
        std::printf("view grid bench %s\n", time ? "done" : "correctness passed");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
