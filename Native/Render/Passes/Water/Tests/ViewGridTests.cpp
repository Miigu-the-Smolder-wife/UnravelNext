// Track W water view grid correctness (FEATURES_GAME 1.8 B gates; ViewGrid.hlsli; the first run of new kernels on WARP):
//   0. no holes: every pixel whose water lies inside the far field and the water body (the body's edge checked only
//      bound (1 + 1 / tan e) inside: a grazing ray's displaced hit moves that far along the view) is covered; no
//      triangle passes the scatter's 8 x 8 loop
//   1. first hit: the GPU keys equal a CPU raster of the same vertices (the probe evaluates every grid point as the
//      scatter does; exact integer edge functions, the same inclusive rule and water-body rule): coverage identical,
//      view depth within 1e-5 relative
//   2. continuous surface: the resolved point's distance to the mesh hit along the surface normal (|dt v_y| / f) is
//      <= 0.5 px at 99.9 % of the water pixels; the polished fraction and residual are printed
// Scenes: open sea (camera 12 m, pitch -10 deg), a waterside lake, a swimming camera (0.3 m; far field only).
//   unx_test_water_viewgridtests [--no-debug-layer] [--warp] [--size W H]
#include "unx/water/ViewGrid.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;
using unx::water::Ocean;
using unx::water::OceanDesc;
using unx::water::ViewGrid;
using unx::water::ViewGridCamera;
using unx::water::ViewGridWater;

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
ViewGridCamera makeCamera(const double position[3], double yaw, double pitch, uint32_t width, uint32_t height)
{
    ViewGridCamera c;
    const double f[3] = { std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw) };
    double r[3] = { -f[2], 0, f[0] };  // forward x up
    const double rl = std::hypot(r[0], r[2]);
    r[0] /= rl; r[2] /= rl;
    const double u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    for (int a = 0; a < 3; ++a) { c.position[a] = float(position[a]); c.forward[a] = float(f[a]); c.right[a] = float(r[a]); c.up[a] = float(u[a]); }
    c.tanX = float(std::tan(kPi / 6));
    c.tanY = c.tanX * float(height) / float(width);
    c.width = width;
    c.height = height;
    return c;
}
void rayOf(const ViewGridCamera& c, double px, double py, double out[3])
{
    const double sx = px / c.width * 2 - 1, sy = 1 - py / c.height * 2;
    double l = 0;
    for (int a = 0; a < 3; ++a) { out[a] = c.forward[a] + sx * c.tanX * c.right[a] + sy * c.tanY * c.up[a]; l += out[a] * out[a]; }
    l = std::sqrt(l);
    for (int a = 0; a < 3; ++a) out[a] /= l;
}
int64_t edge(const int32_t* a, const int32_t* b, int64_t qx, int64_t qy) { return int64_t(b[0] - a[0]) * (qy - a[1]) - int64_t(b[1] - a[1]) * (qx - a[0]); }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, warp = false;
        uint32_t width = 0, height = 0;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--warp") warp = true;
            if (std::string(argv[i]) == "--size" && i + 2 < argc) { width = uint32_t(std::atoi(argv[i + 1])); height = uint32_t(std::atoi(argv[i + 2])); i += 2; }
        }
        if (!width) { width = warp ? 320 : 1280; height = warp ? 180 : 720; }
        Gpu gpu(debugLayer, warp);
        OceanDesc od;
        od.windSpeed = 10.0f;
        od.windDirection = 0.4f;
        od.seed = 3;
        Ocean ocean(gpu.device, gpu.shaders, od);
        ViewGrid grid(gpu.device, gpu.shaders);
        ID3D12PipelineState* probe = gpu.shaders.compute("Passes/Water/Tests/ViewGridProbe");
        struct Scene { const char* name; double position[3], yaw, pitch; bool lake; double lakeCentre[2], lakeRadius; };
        const Scene scenes[] = {
            { "open sea, camera 12 m, pitch -10 deg", { 3.7, 12.0, -5.3 }, 0.3, -10 * kPi / 180, false, {}, 0 },
            { "waterside lake (r 150 m, 220 m ahead)", { 3.7, 12.0, -5.3 }, 0.3, -10 * kPi / 180, true, { 3.7 + 220 * std::cos(0.3), -5.3 + 220 * std::sin(0.3) }, 150 },
            { "swimming, camera 0.3 m (far field only)", { 3.7, 0.3, -5.3 }, 0.3, 0, false, {}, 0 },
        };
        const uint32_t pixels = width * height;
        uint64_t frame = 0;
        for (const Scene& scene : scenes)
        {
            const ViewGridCamera camera = makeCamera(scene.position, scene.yaw, scene.pitch, width, height);
            ViewGridWater water;
            water.lake = scene.lake;
            water.lakeCentre[0] = float(scene.lakeCentre[0]);
            water.lakeCentre[1] = float(scene.lakeCentre[1]);
            water.lakeRadius = float(scene.lakeRadius);
            const auto layout = ViewGrid::layout(camera, water, od.lengths);
            const uint32_t points = layout.columns * layout.rows;

            RenderGraph g(gpu.device);
            const auto fields = ocean.record(g, 37.25);
            const auto out = grid.record(g, frame++, fields, od.lengths, camera, water);
            // The probe reads the same parameters: upload them once more for it.
            ComPtr<ID3D12Resource> params = buffer(gpu.device, layout.params.size() * 4, D3D12_HEAP_TYPE_UPLOAD);
            {
                void* m = nullptr;
                check(params->Map(0, nullptr, &m), "map params");
                std::memcpy(m, layout.params.data(), layout.params.size() * 4);
                params->Unmap(0, nullptr);
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.NumElements = UINT(std::max<size_t>(layout.params.size(), 64));
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            const uint32_t paramSrv = gpu.device.descriptors().allocateResource();
            gpu.device.d3d()->CreateShaderResourceView(params.Get(), &sd, gpu.device.descriptors().resourceCpu(paramSrv));
            const BufferRef probeOut = g.createBuffer({ "view grid probe", uint64_t(points) * 16 + 256, 0 });
            const TextureRef displacement = fields.displacement;
            g.addPass("view grid probe", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(displacement, Use::SrvCompute); pb.use(probeOut, Use::UavCompute); },
                      [=](PassContext& c) {
                          const uint32_t k[4] = { paramSrv, c.srv(displacement), c.uav(probeOut), points };
                          c.cmd->SetPipelineState(probe);
                          c.computeConstants(k, 4);
                          c.cmd->Dispatch((points + 63) / 64, 1, 1);
                      });
            const uint64_t keyBytes = uint64_t(pixels) * 8, surfaceBytes = uint64_t(pixels) * 16, probeBytes = uint64_t(points) * 16;
            ComPtr<ID3D12Resource> rb = buffer(gpu.device, keyBytes + surfaceBytes + probeBytes + 256, D3D12_HEAP_TYPE_READBACK);
            ID3D12Resource* r = rb.Get();
            const uint32_t w = width, h = height;
            const BufferRef keys = out.keys, counters = out.counters;
            const TextureRef surface = out.surface;
            g.addPass("view grid read", QueueType::Graphics,
                      [&](PassBuilder& pb) { pb.use(keys, Use::CopySrc); pb.use(counters, Use::CopySrc); pb.use(surface, Use::CopySrc); pb.use(probeOut, Use::CopySrc); pb.keep(); },
                      [=](PassContext& c) {
                          c.cmd->CopyBufferRegion(r, 0, c.resource(keys), 0, keyBytes);
                          D3D12_TEXTURE_COPY_LOCATION dst{ r, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                          dst.PlacedFootprint.Offset = keyBytes;
                          dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, 1, UINT(w * 16) };
                          D3D12_TEXTURE_COPY_LOCATION src{ c.resource(surface), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                          c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                          c.cmd->CopyBufferRegion(r, keyBytes + surfaceBytes, c.resource(probeOut), 0, probeBytes);
                          c.cmd->CopyBufferRegion(r, keyBytes + surfaceBytes + probeBytes, c.resource(counters), 0, 4);
                      });
            g.execute(nullptr);
            for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
            gpu.device.descriptors().freeResource(paramSrv);
            const uint8_t* m = nullptr;
            check(rb->Map(0, nullptr, (void**)&m), "map readback");
            std::vector<uint64_t> key((const uint64_t*)m, (const uint64_t*)(m + keyBytes));
            std::vector<float> surf((const float*)(m + keyBytes), (const float*)(m + keyBytes + surfaceBytes));
            std::vector<int32_t> vert((const int32_t*)(m + keyBytes + surfaceBytes), (const int32_t*)(m + keyBytes + surfaceBytes + probeBytes));
            uint32_t overflow = 0;
            std::memcpy(&overflow, m + keyBytes + surfaceBytes + probeBytes, 4);
            rb->Unmap(0, nullptr);
            W_CHECK(overflow == 0, "%s: %u triangles past the 8 x 8 loop", scene.name, overflow);

            // 1. CPU raster of the probe's vertices.
            std::vector<double> ref(pixels, std::numeric_limits<double>::infinity());
            auto vtx = [&](uint32_t i, uint32_t j) { return &vert[size_t(j * layout.columns + i) * 4]; };
            auto tri = [&](const int32_t* a, const int32_t* b, const int32_t* c) {
                const uint32_t fa = uint32_t(a[3]), fb = uint32_t(b[3]), fc = uint32_t(c[3]);
                if (!fa || !fb || !fc || (fa == 2 && fb == 2 && fc == 2)) return;
                int64_t area = edge(a, b, c[0], c[1]);
                if (!area) return;
                const int64_t sign = area > 0 ? 1 : -1;
                area *= sign;
                const int64_t lox = std::min({ a[0], b[0], c[0] }), hix = std::max({ a[0], b[0], c[0] });
                const int64_t loy = std::min({ a[1], b[1], c[1] }), hiy = std::max({ a[1], b[1], c[1] });
                auto ceilDiv = [](int64_t v) { return v >= 0 ? (v + 255) / 256 : -((-v) / 256); };
                auto floorDiv = [](int64_t v) { return v >= 0 ? v / 256 : -((-v + 255) / 256); };
                const int64_t x0 = std::max<int64_t>(ceilDiv(lox - 128), 0), x1 = std::min<int64_t>(floorDiv(hix - 128), int64_t(width) - 1);
                const int64_t y0 = std::max<int64_t>(ceilDiv(loy - 128), 0), y1 = std::min<int64_t>(floorDiv(hiy - 128), int64_t(height) - 1);
                float za, zb, zc;
                std::memcpy(&za, &a[2], 4); std::memcpy(&zb, &b[2], 4); std::memcpy(&zc, &c[2], 4);
                for (int64_t y = y0; y <= y1; ++y)
                    for (int64_t x = x0; x <= x1; ++x)
                    {
                        const int64_t qx = x * 256 + 128, qy = y * 256 + 128;
                        const int64_t w0 = edge(b, c, qx, qy) * sign, w1 = edge(c, a, qx, qy) * sign, w2 = edge(a, b, qx, qy) * sign;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        const double depth = double(area) / (double(w0) * za + double(w1) * zb + double(w2) * zc);
                        double& d = ref[size_t(y) * width + size_t(x)];
                        d = std::min(d, depth);
                    }
            };
            for (uint32_t j = 0; j + 1 < layout.rows; ++j)
                for (uint32_t i = 0; i + 1 < layout.columns; ++i)
                {
                    tri(vtx(i, j), vtx(i + 1, j), vtx(i + 1, j + 1));
                    tri(vtx(i, j), vtx(i + 1, j + 1), vtx(i, j + 1));
                }
            uint64_t coverageMismatch = 0, covered = 0;
            double worstDepth = 0;
            for (uint32_t p = 0; p < pixels; ++p)
            {
                const bool g1 = key[p] != ~0ull, c1 = std::isfinite(ref[p]);
                if (g1 != c1) { ++coverageMismatch; continue; }
                if (!g1) continue;
                ++covered;
                float gd;
                const uint32_t bits = uint32_t(key[p] >> 32);
                std::memcpy(&gd, &bits, 4);
                worstDepth = std::max(worstDepth, std::abs(gd - ref[p]) / ref[p]);
            }

            // 0. holes, 2. continuous surface.
            const double hCam = scene.position[1] - water.level;
            uint64_t required = 0, holes = 0, polished = 0, water2 = 0, over = 0;
            std::vector<double> err;
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                {
                    const size_t p = size_t(y) * width + x;
                    double v[3];
                    rayOf(camera, x + 0.5, y + 0.5, v);
                    const bool hit = key[p] != ~0ull;
                    if (hit)
                    {
                        ++water2;
                        const float* s = &surf[p * 4];
                        if (s[3] == 1) ++polished;
                        float gd;
                        const uint32_t bits = uint32_t(key[p] >> 32);
                        std::memcpy(&gd, &bits, 4);
                        const double along = v[0] * camera.forward[0] + v[1] * camera.forward[1] + v[2] * camera.forward[2];
                        const double tMesh = gd / along, tRes = s[2] / along;
                        const double footprint = tRes * 2 * camera.tanX / width;
                        const double e = std::abs((tRes - tMesh) * v[1]) / footprint;
                        err.push_back(e);
                        if (e > 0.5) ++over;
                    }
                    if (v[1] >= 0) continue;
                    const double t = hCam / -v[1], horizontal = t * std::hypot(v[0], v[2]);
                    if (horizontal < water.nearRadius + water.bound + 1 || horizontal > water.extent - water.bound - 1) continue;
                    const double reach = water.bound * (1 + horizontal / hCam) + 1;
                    if (scene.lake && std::hypot(scene.position[0] + t * v[0] - scene.lakeCentre[0], scene.position[2] + t * v[2] - scene.lakeCentre[1]) > scene.lakeRadius - reach) continue;
                    ++required;
                    if (!hit) ++holes;
                }
            std::sort(err.begin(), err.end());
            const double p999 = err.empty() ? 0 : err[std::min(err.size() - 1, size_t(0.999 * err.size()))];
            std::printf("%s: grid %u x %u; water pixels %llu (required %llu, holes %llu); CPU raster: coverage mismatches %llu, depth max rel %.2e; polished %.2f %%; "
                        "|mesh - surface| along the normal: median %.3f px, 99.9 %% %.3f px, max %.3f px (%llu > 0.5 px)\n",
                        scene.name, layout.columns, layout.rows, (unsigned long long)water2, (unsigned long long)required, (unsigned long long)holes,
                        (unsigned long long)coverageMismatch, worstDepth, water2 ? 100.0 * polished / water2 : 0.0, err.empty() ? 0 : err[err.size() / 2], p999,
                        err.empty() ? 0 : err.back(), (unsigned long long)over);
            W_CHECK(required > 0 && holes == 0, "%s: %llu holes of %llu required pixels", scene.name, (unsigned long long)holes, (unsigned long long)required);
            W_CHECK(coverageMismatch == 0, "%s: %llu pixels differ in coverage from the CPU raster", scene.name, (unsigned long long)coverageMismatch);
            W_CHECK(worstDepth <= 1e-5, "%s: depth differs from the CPU raster by %.3g relative", scene.name, worstDepth);
            W_CHECK(p999 <= 0.5, "%s: the mesh is %.3f px from the continuous surface at the 99.9th percentile", scene.name, p999);
        }
        if (!warp)
        {
            const uint32_t errors = gpu.device.drainDebugMessages();
            W_CHECK(errors == 0, "%u debug-layer errors", errors);
        }
        std::printf("view grid tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
