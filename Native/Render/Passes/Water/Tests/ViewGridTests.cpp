// Track W water view grid correctness (FEATURES_GAME 1.8 B gates; ViewGrid.hlsli; the first run of new kernels on WARP):
//   0. no holes: every pixel whose water lies inside the water body (its edge checked only bound (1 + 1 / tan e)
//      inside: a grazing ray's displaced hit moves that far along the view) is covered; no big triangle is lost
//   1. first hit: the GPU keys equal a CPU raster of the same vertices, far field and every near level (the probe
//      evaluates every grid point as the scatters do; exact integer edge functions, the same inclusive, water-body and
//      ring rules; big triangles included): coverage identical, view depth within 1e-5 relative
//   2. continuous surface: every output point's distance to the continuous surface along its normal, (S(x0) - P) . n
//      (the resolve's diagnostic output; a polished point's residual, else the mesh point's), is <= 0.5 px at 99.9 % of
//      the water pixels; the polished fraction and the mesh-to-polished move along the view are printed
// Scenes: open sea (camera 12 m, pitch -10 deg), a waterside lake, swimming (0.3 m: the camera within the wave height,
// the adaptive near field), looking down at the water under the camera (2 m,
// pitch -70 deg: the near field fills the screen), a calm sea (wind 2 m/s, bounds 0.15 m) under a camera 0.17 m above it
// (pitch -80 deg, twice the resolution: near-field triangles past 8 x 8 px go through the big-triangle tiles; the scene
// requires some).
//   unx_test_water_viewgridtests [--no-debug-layer] [--warp] [--size W H] [--theta-of W]: the pixel angle of a 60 deg view W
//   pixels wide (the image is then a window of that view; --theta-of 3840 checks at 4K's pixel angle)
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
using unx::water::ViewGridLayout;
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
// thetaWidth: the width whose 60 deg field of view gives the pixel angle (a window of a 4K view: 3840).
ViewGridCamera makeCamera(const double position[3], double yaw, double pitch, uint32_t width, uint32_t height, uint32_t thetaWidth)
{
    ViewGridCamera c;
    const double f[3] = { std::cos(pitch) * std::cos(yaw), std::sin(pitch), std::cos(pitch) * std::sin(yaw) };
    double r[3] = { -f[2], 0, f[0] };  // forward x up
    const double rl = std::hypot(r[0], r[2]);
    r[0] /= rl; r[2] /= rl;
    const double u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    for (int a = 0; a < 3; ++a) { c.position[a] = float(position[a]); c.forward[a] = float(f[a]); c.right[a] = float(r[a]); c.up[a] = float(u[a]); }
    c.tanX = float(std::tan(kPi / 6) * width / thetaWidth);
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
        uint32_t width = 0, height = 0, thetaWidth = 0;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--warp") warp = true;
            if (std::string(argv[i]) == "--size" && i + 2 < argc) { width = uint32_t(std::atoi(argv[i + 1])); height = uint32_t(std::atoi(argv[i + 2])); i += 2; }
            if (std::string(argv[i]) == "--theta-of" && i + 1 < argc) thetaWidth = uint32_t(std::atoi(argv[++i]));
        }
        if (!width) { width = warp ? 320 : 1280; height = warp ? 180 : 720; }
        Gpu gpu(debugLayer, warp);
        OceanDesc od;
        od.windSpeed = 10.0f;
        od.windDirection = 0.4f;
        od.seed = 3;
        Ocean ocean(gpu.device, gpu.shaders, od);
        OceanDesc calmDesc = od;
        calmDesc.windSpeed = 2.0f;  // significant height about 9 cm
        Ocean calmOcean(gpu.device, gpu.shaders, calmDesc);
        ViewGrid grid(gpu.device, gpu.shaders);
        ID3D12PipelineState* probe = gpu.shaders.compute("Passes/Water/Tests/ViewGridProbe");
        struct Scene { const char* name; double position[3], yaw, pitch; bool lake; double lakeCentre[2], lakeRadius; };
        const Scene scenes[] = {
            { "open sea, camera 12 m, pitch -10 deg", { 3.7, 12.0, -5.3 }, 0.3, -10 * kPi / 180, false, {}, 0 },
            { "waterside lake (r 150 m, 220 m ahead)", { 3.7, 12.0, -5.3 }, 0.3, -4 * kPi / 180, true, { 3.7 + 220 * std::cos(0.3), -5.3 + 220 * std::sin(0.3) }, 150 },
            { "swimming, camera 0.3 m, pitch 0", { 3.7, 0.3, -5.3 }, 0.3, 0, false, {}, 0 },
            { "looking down, camera 2 m, pitch -70 deg", { 3.7, 2.0, -5.3 }, 0.3, -70 * kPi / 180, false, {}, 0 },
            { "calm sea, camera 0.17 m, pitch -80 deg, twice the resolution", { 3.7, 0.17, -5.3 }, 0.3, -80 * kPi / 180, false, {}, 0 },
        };
        const uint32_t baseWidth = width, baseHeight = height;
        uint64_t frame = 0;
        for (const Scene& scene : scenes)
        {
            const bool calm = std::strncmp(scene.name, "calm", 4) == 0;
            const uint32_t scale = calm ? 2 : 1;
            const uint32_t sceneWidth = baseWidth * scale, sceneHeight = baseHeight * scale, pixels = sceneWidth * sceneHeight;
            const ViewGridCamera camera = makeCamera(scene.position, scene.yaw, scene.pitch, sceneWidth, sceneHeight, (thetaWidth ? thetaWidth : baseWidth) * scale);
            ViewGridWater water;
            if (calm) water.horizontalBound = water.verticalBound = 0.15f;
            water.lake = scene.lake;
            water.lakeCentre[0] = float(scene.lakeCentre[0]);
            water.lakeCentre[1] = float(scene.lakeCentre[1]);
            water.lakeRadius = float(scene.lakeRadius);
            // Warm-up records: the grid measures the ocean's bounds from its pyramid a few frames later (the records below then
            // use the same measured bounds: the same ocean time).
            for (int warm = 0; warm < 4; ++warm)
            {
                RenderGraph gw(gpu.device);
                const auto fw = (calm ? calmOcean : ocean).record(gw, 37.25);
                const auto ow = grid.record(gw, frame++, fw, od.lengths, camera, water);
                gw.addPass("view grid warm-up keep", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(ow.depth, Use::SrvCompute); pb.keep(); }, [](PassContext&) {});
                gw.execute(nullptr);
                for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(gw.lastFence((QueueType)q));
            }
            ViewGridLayout layout;
            ViewGridWater used = water;
            // The adaptive near field's drawn blocks (a first frame; the second frame decides the same blocks: the same ocean
            // time and camera).
            std::vector<uint32_t> drawnBlocks;  // (level, x, z, 0) per block
            {
                RenderGraph g0(gpu.device);
                const auto f0 = (calm ? calmOcean : ocean).record(g0, 37.25);
                const auto o0 = grid.record(g0, frame++, f0, od.lengths, camera, water, true);
                layout = o0.layout;
                used = o0.water;
                if (o0.nearDrawn.valid())
                {
                    ComPtr<ID3D12Resource> rb0 = buffer(gpu.device, 16 + (uint64_t(1) << 20) * 16, D3D12_HEAP_TYPE_READBACK);
                    ID3D12Resource* r0 = rb0.Get();
                    const BufferRef nd = o0.nearDrawn;
                    g0.addPass("view grid drawn read", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(nd, Use::CopySrc); pb.keep(); },
                               [=](PassContext& c) { c.cmd->CopyBufferRegion(r0, 0, c.resource(nd), 0, 16 + (uint64_t(1) << 20) * 16); });
                    g0.execute(nullptr);
                    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g0.lastFence((QueueType)q));
                    const uint32_t* m0 = nullptr;
                    check(rb0->Map(0, nullptr, (void**)&m0), "map drawn blocks");
                    const uint32_t count = std::min(m0[0], 1u << 20);
                    drawnBlocks.assign(m0 + 4, m0 + 4 + size_t(count) * 4);
                    rb0->Unmap(0, nullptr);
                    W_CHECK(m0[0] <= (1u << 20), "%s: %u drawn blocks exceed the diagnostic list", scene.name, m0[0]);
                }
                else
                {
                    g0.execute(nullptr);
                    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g0.lastFence((QueueType)q));
                }
            }
            // The grids: the far field, then the drawn near blocks (11 x 11 vertices each).
            struct GridPart { uint32_t mode, across, points; };
            std::vector<GridPart> parts = { { 0, layout.columns, layout.columns * layout.rows } };
            if (!drawnBlocks.empty()) parts.push_back({ 2, 11, uint32_t(drawnBlocks.size() / 4) * 121 });
            ComPtr<ID3D12Resource> entries = buffer(gpu.device, std::max<size_t>(drawnBlocks.size() * 4, 16), D3D12_HEAP_TYPE_UPLOAD);
            if (!drawnBlocks.empty())
            {
                void* m = nullptr;
                check(entries->Map(0, nullptr, &m), "map entries");
                std::memcpy(m, drawnBlocks.data(), drawnBlocks.size() * 4);
                entries->Unmap(0, nullptr);
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC ed{};
            ed.Format = DXGI_FORMAT_R32_TYPELESS;
            ed.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            ed.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            ed.Buffer.NumElements = UINT(std::max<size_t>(drawnBlocks.size(), 64));
            ed.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            const uint32_t entrySrv = gpu.device.descriptors().allocateResource();
            gpu.device.d3d()->CreateShaderResourceView(entries.Get(), &ed, gpu.device.descriptors().resourceCpu(entrySrv));
            uint64_t points = 0;
            for (const GridPart& part : parts) points += part.points;

            RenderGraph g(gpu.device);
            const auto fields = (calm ? calmOcean : ocean).record(g, 37.25);
            const auto out = grid.record(g, frame++, fields, od.lengths, camera, water, true);
            // Bytes, not floats: the parameters hold integers' bits too (a NaN pattern never compares equal as a float).
            W_CHECK(out.layout.params.size() == layout.params.size() && std::memcmp(out.layout.params.data(), layout.params.data(), layout.params.size() * 4) == 0,
                    "%s: the second record's layout differs from the first's", scene.name);
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
            const TextureRef displacement = fields.displacement, slopeField = fields.slopes;
            uint64_t offset = 0;
            for (const GridPart& part : parts)
            {
                const BufferRef partOut = g.createBuffer({ "view grid probe part", uint64_t(part.points) * 16 + 256, 0 });
                g.addPass("view grid probe", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(displacement, Use::SrvCompute); pb.use(slopeField, Use::SrvCompute); pb.use(partOut, Use::UavCompute); },
                          [=](PassContext& c) {
                              const uint32_t k[8] = { paramSrv, c.srv(displacement), c.uav(partOut), part.points, part.mode, 0, c.srv(slopeField), entrySrv };
                              c.cmd->SetPipelineState(probe);
                              c.computeConstants(k, 8);
                              c.cmd->Dispatch((part.points + 63) / 64, 1, 1);
                          });
                const uint64_t at = offset, bytes = uint64_t(part.points) * 16;
                g.addPass("view grid probe gather", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(partOut, Use::CopySrc); pb.use(probeOut, Use::CopyDst); },
                          [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(probeOut), at, c.resource(partOut), 0, bytes); });
                offset += bytes;
            }
            const uint64_t keyBytes = uint64_t(pixels) * 8, surfaceBytes = uint64_t(pixels) * 16, probeBytes = uint64_t(points) * 16;
            const uint32_t errorPitch = (sceneWidth * 4 + 255) / 256 * 256;
            const uint64_t errorAt = keyBytes + surfaceBytes + probeBytes + 256;
            ComPtr<ID3D12Resource> rb = buffer(gpu.device, errorAt + uint64_t(errorPitch) * sceneHeight, D3D12_HEAP_TYPE_READBACK);
            ID3D12Resource* r = rb.Get();
            const uint32_t w = sceneWidth, h = sceneHeight;
            const BufferRef keys = out.keys, counters = out.counters;
            const TextureRef surface = out.surface, errorTexture = out.error;
            g.addPass("view grid read", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(keys, Use::CopySrc); pb.use(counters, Use::CopySrc); pb.use(surface, Use::CopySrc); pb.use(probeOut, Use::CopySrc); pb.use(errorTexture, Use::CopySrc);
                          pb.keep();
                      },
                      [=](PassContext& c) {
                          c.cmd->CopyBufferRegion(r, 0, c.resource(keys), 0, keyBytes);
                          D3D12_TEXTURE_COPY_LOCATION dst{ r, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                          dst.PlacedFootprint.Offset = keyBytes;
                          dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, 1, UINT(w * 16) };
                          D3D12_TEXTURE_COPY_LOCATION src{ c.resource(surface), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                          c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                          c.cmd->CopyBufferRegion(r, keyBytes + surfaceBytes, c.resource(probeOut), 0, probeBytes);
                          c.cmd->CopyBufferRegion(r, keyBytes + surfaceBytes + probeBytes, c.resource(counters), 0, 16);
                          D3D12_TEXTURE_COPY_LOCATION edst{ r, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                          edst.PlacedFootprint.Offset = errorAt;
                          edst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, w, h, 1, errorPitch };
                          D3D12_TEXTURE_COPY_LOCATION esrc{ c.resource(errorTexture), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                          c.cmd->CopyTextureRegion(&edst, 0, 0, 0, &esrc, nullptr);
                      });
            g.execute(nullptr);
            for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
            gpu.device.descriptors().freeResource(paramSrv);
            gpu.device.descriptors().freeResource(entrySrv);
            const uint8_t* m = nullptr;
            check(rb->Map(0, nullptr, (void**)&m), "map readback");
            std::vector<uint64_t> key((const uint64_t*)m, (const uint64_t*)(m + keyBytes));
            std::vector<float> surf((const float*)(m + keyBytes), (const float*)(m + keyBytes + surfaceBytes));
            std::vector<int32_t> vert((const int32_t*)(m + keyBytes + surfaceBytes), (const int32_t*)(m + keyBytes + surfaceBytes + probeBytes));
            std::vector<float> pointError(pixels);
            for (uint32_t y = 0; y < sceneHeight; ++y) std::memcpy(&pointError[size_t(y) * sceneWidth], m + errorAt + uint64_t(y) * errorPitch, sceneWidth * 4);
            uint32_t bigCounts[4] = {};  // big appended, big lost, big tiles, near blocks the lists or ids could not hold
            std::memcpy(bigCounts, m + keyBytes + surfaceBytes + probeBytes, 16);
            rb->Unmap(0, nullptr);
            W_CHECK(bigCounts[1] == 0, "%s: %u big triangles lost past the list", scene.name, bigCounts[1]);
            W_CHECK(bigCounts[3] == 0, "%s: %u near blocks drawn at a coarser level (lists or ids full)", scene.name, bigCounts[3]);

            // 1. CPU raster of the probe's vertices.
            std::vector<double> ref(pixels, std::numeric_limits<double>::infinity());
            std::vector<uint64_t> refWho(pixels, 0);  // (part, quad index x 2 + triangle) of the CPU's winner
            int64_t largestBox = 0;  // pixels across the largest drawn triangle's box
            size_t partBase = 0;
            uint32_t partAcross = 0;
            auto vtx = [&](uint32_t i, uint32_t j) { return &vert[partBase + size_t(j * partAcross + i) * 4]; };
            uint64_t who = 0;
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
                const int64_t x0 = std::max<int64_t>(ceilDiv(lox - 128), 0), x1 = std::min<int64_t>(floorDiv(hix - 128), int64_t(sceneWidth) - 1);
                const int64_t y0 = std::max<int64_t>(ceilDiv(loy - 128), 0), y1 = std::min<int64_t>(floorDiv(hiy - 128), int64_t(sceneHeight) - 1);
                if (x1 >= x0 && y1 >= y0) largestBox = std::max(largestBox, std::max(x1 - x0, y1 - y0) + 1);
                float za, zb, zc;
                std::memcpy(&za, &a[2], 4); std::memcpy(&zb, &b[2], 4); std::memcpy(&zc, &c[2], 4);
                for (int64_t y = y0; y <= y1; ++y)
                    for (int64_t x = x0; x <= x1; ++x)
                    {
                        const int64_t qx = x * 256 + 128, qy = y * 256 + 128;
                        const int64_t w0 = edge(b, c, qx, qy) * sign, w1 = edge(c, a, qx, qy) * sign, w2 = edge(a, b, qx, qy) * sign;
                        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                        const double depth = double(area) / (double(w0) * za + double(w1) * zb + double(w2) * zc);
                        double& d = ref[size_t(y) * sceneWidth + size_t(x)];
                        if (depth < d) { d = depth; refWho[size_t(y) * sceneWidth + size_t(x)] = who; }
                    }
            };
            for (const GridPart& part : parts)
            {
                partAcross = part.across;
                // The far field is one grid; the drawn blocks are 11 x 11-vertex grids one after another.
                const uint32_t grids = part.mode == 2 ? part.points / 121 : 1, down = part.mode == 2 ? 11 : part.points / part.across;
                for (uint32_t gIndex = 0; gIndex < grids; ++gIndex)
                {
                    for (uint32_t j = 0; j + 1 < down; ++j)
                        for (uint32_t i = 0; i + 1 < part.across; ++i)
                        {
                            who = (uint64_t(&part - parts.data()) << 32) | ((j * (part.across - 1) + i) * 2);
                            tri(vtx(i, j), vtx(i + 1, j), vtx(i + 1, j + 1));
                            ++who;
                            tri(vtx(i, j), vtx(i + 1, j + 1), vtx(i, j + 1));
                        }
                    partBase += size_t(part.across) * down * 4;
                }
            }
            uint64_t coverageMismatch = 0, covered = 0;
            double worstDepth = 0;
            for (uint32_t p = 0; p < pixels; ++p)
            {
                const bool g1 = key[p] != ~0ull, c1 = std::isfinite(ref[p]);
                if (g1 != c1)
                {
                    if (coverageMismatch < 4)
                    {
                        const uint64_t winner = refWho[p];
                        const uint32_t part = uint32_t(winner >> 32), local = uint32_t(winner), across = parts[part].across - 1;
                        std::printf("  pixel %u: GPU %s, CPU winner part %u (mode %u) quad (%u, %u) triangle %u\n", p, g1 ? "covered" : "empty", part, parts[part].mode,
                                    (local >> 1) % across, (local >> 1) / across, local & 1);
                    }
                    ++coverageMismatch;
                    continue;
                }
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
            std::vector<double> err, moved;
            uint64_t overBy[9] = {};  // pixels past 0.5 px by the winning triangle's grid: far field, near levels 0..7
            for (uint32_t y = 0; y < sceneHeight; ++y)
                for (uint32_t x = 0; x < sceneWidth; ++x)
                {
                    const size_t p = size_t(y) * sceneWidth + x;
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
                        const double footprint = tRes * 2 * camera.tanX / sceneWidth;
                        moved.push_back(std::abs((tRes - tMesh) * v[1]) / footprint);
                        const double e = pointError[p];
                        err.push_back(e);
                        if (e > 0.5)
                        {
                            ++over;
                            const uint32_t id = uint32_t(key[p]);
                            ++overBy[(id & 0x80000000u) ? 1 + ((id >> 28) & 7u) : 0];
                        }
                    }
                    if (v[1] >= 0) continue;
                    const double t = hCam / -v[1], horizontal = t * std::hypot(v[0], v[2]);
                    if (horizontal > used.extent - used.bound() - 1) continue;
                    const double reach = used.bound() * (1 + horizontal / hCam) + 1;
                    if (scene.lake && std::hypot(scene.position[0] + t * v[0] - scene.lakeCentre[0], scene.position[2] + t * v[2] - scene.lakeCentre[1]) > scene.lakeRadius - reach) continue;
                    ++required;
                    if (!hit)
                    {
                        if (holes < 4)
                        {
                            const double hx = scene.position[0] + t * v[0], hz = scene.position[2] + t * v[2];
                            std::printf("  hole at pixel (%u, %u): still water at (%.3f, %.3f), %.3f m from the camera's foot; drawn blocks over it:", x, y, hx, hz, horizontal);
                            for (size_t e = 0; e < drawnBlocks.size(); e += 4)
                            {
                                const uint32_t lv = drawnBlocks[e];
                                float sp;
                                int32_t o[2];
                                std::memcpy(&sp, &layout.params[36 + 8 * lv + 2], 4);
                                std::memcpy(o, &layout.params[36 + 8 * lv + 4], 8);
                                const double x0 = (o[0] + int32_t(drawnBlocks[e + 1]) * 8 - 1) * double(sp), z0 = (o[1] + int32_t(drawnBlocks[e + 2]) * 8 - 1) * double(sp);
                                if (hx >= x0 && hx <= x0 + 10 * sp && hz >= z0 && hz <= z0 + 10 * sp) std::printf(" (level %u, %d, %d)", lv, int32_t(drawnBlocks[e + 1]), int32_t(drawnBlocks[e + 2]));
                            }
                            std::printf("\n");
                        }
                        ++holes;
                    }
                }
            std::sort(err.begin(), err.end());
            std::sort(moved.begin(), moved.end());
            const double p999 = err.empty() ? 0 : err[std::min(err.size() - 1, size_t(0.999 * err.size()))];
            std::printf("%s (measured R %.2f m, A %.2f m, x 1.25): grid %u x %u + %u near levels (%zu blocks drawn), %u big triangles (largest box %lld px); water pixels %llu (required %llu, holes %llu); CPU raster: coverage mismatches %llu, depth max rel %.2e; polished %.2f %% (moved %.3f px at 99.9 %%); "
                        "output point to the surface <= median %.3f px, 99.9 %% %.3f px, max %.3f px (%llu > 0.5 px)\n",
                        scene.name, used.horizontalBound / 1.25, used.verticalBound / 1.25, layout.columns, layout.rows, layout.nearLevels, drawnBlocks.size() / 4, bigCounts[0], (long long)largestBox, (unsigned long long)water2, (unsigned long long)required, (unsigned long long)holes,
                        (unsigned long long)coverageMismatch, worstDepth, water2 ? 100.0 * polished / water2 : 0.0,
                        moved.empty() ? 0 : moved[std::min(moved.size() - 1, size_t(0.999 * moved.size()))], err.empty() ? 0 : err[err.size() / 2], p999,
                        err.empty() ? 0 : err.back(), (unsigned long long)over);
            W_CHECK(!calm || bigCounts[0] > 0, "%s: no big triangle (the scene exercises that path)", scene.name);
            W_CHECK(required > 0 && holes == 0, "%s: %llu holes of %llu required pixels", scene.name, (unsigned long long)holes, (unsigned long long)required);
            W_CHECK(coverageMismatch == 0, "%s: %llu pixels differ in coverage from the CPU raster", scene.name, (unsigned long long)coverageMismatch);
            W_CHECK(worstDepth <= 1e-5, "%s: depth differs from the CPU raster by %.3g relative", scene.name, worstDepth);
            if (over)
            {
                std::printf("  past 0.5 px by grid: far %llu", (unsigned long long)overBy[0]);
                for (uint32_t l = 0; l < layout.nearLevels; ++l) std::printf(", near %u: %llu", l, (unsigned long long)overBy[1 + l]);
                std::printf("\n");
            }
            W_CHECK(p999 <= 0.5, "%s: output points are up to %.3f px from the continuous surface at the 99.9th percentile", scene.name, p999);
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
