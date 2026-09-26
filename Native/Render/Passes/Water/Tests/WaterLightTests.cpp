// Track W water stage 2 (FEATURES_GAME 1.9; WaterSunMap.ms / .ps, WaterLight.hlsli): the sun-space water map of two
// streams - a flat pool (2 x 2 m at y = 0.4, pure water, IOR 1.333) and a tilted plate (20 degrees, its own medium and
// IOR 1.5) - and waterSunLight at points under, above and beside them, against the exact geometry in double:
//   1. lit exactly where the straight sun ray from the point meets a water surface above it (points within 1 cm of a
//      surface's edge along its plane are boundary cases);
//   2. the light direction = -(the exact Snell refraction of the sunlight at that surface's normal) (within 2e-3; the
//      map stores the normal as octahedral fp16);
//   3. the transmittance = (1 - F(theta_s)) T^d, F the exact unpolarised Fresnel, d the path from the point to the surface
//      along the light direction (relative 3e-3; the medium is fp16);
//   4. with the map absent (constants UNX_NONE) nothing is lit.
//   unx_test_water_waterlighttests [--no-debug-layer] [--warp]
#include "unx/water/LinearDispatch.h"
#include "unx/water/WaterSunMap.h"

#include "unx/core/File.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/core/Log.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
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
ComPtr<ID3D12Resource> hostBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type)
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
struct V3
{
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
V3 operator*(V3 a, double k) { return { a.x * k, a.y * k, a.z * k }; }
double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) { return a * (1 / std::sqrt(dot(a, a))); }
double fresnel(double cosI, double eta)
{
    cosI = std::clamp(cosI, 0.0, 1.0);
    const double s2 = eta * eta * (1 - cosI * cosI);
    if (s2 >= 1) return 1;
    const double ct = std::sqrt(1 - s2), rs = (eta * cosI - ct) / (eta * cosI + ct), rp = (eta * ct - cosI) / (eta * ct + cosI);
    return 0.5 * (rs * rs + rp * rp);
}
// A water plate: centre, in-plane axes (unit), half sizes, normal (towards the sky), medium.
struct Plate
{
    V3 centre, a, b, n;
    double ha, hb;
    float T[3];
    float ior;
};
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, warp = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--warp") warp = true;
        }
        Gpu gpu(debugLayer, warp);
        const double tilt = 20.0 * 3.14159265358979323846 / 180.0;
        const Plate plates[2] = {
            { { 0, 0.4, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1.0, 1.0, { 0.712f, 0.945f, 0.991f }, 1.333f },
            { { 3, 0.4, 0 }, { std::cos(tilt), std::sin(tilt), 0 }, { 0, 0, 1 }, { -std::sin(tilt), std::cos(tilt), 0 }, 0.5, 0.5, { 0.6f, 0.8f, 0.9f }, 1.5f },
        };
        const V3 sun = norm({ 0.3, 0.8, 0.5 });
        // Each plate as two triangles (32 B vertices, normal = the plate's).
        std::vector<std::vector<float>> vertices(2);
        for (int p = 0; p < 2; ++p)
        {
            const Plate& q = plates[p];
            V3 c[4];
            for (int k = 0; k < 4; ++k) c[k] = q.centre + q.a * ((k & 1) ? q.ha : -q.ha) + q.b * ((k & 2) ? q.hb : -q.hb);
            for (int k : { 0, 1, 2, 1, 3, 2 })
                vertices[p].insert(vertices[p].end(), { (float)c[k].x, (float)c[k].y, (float)c[k].z, 1, (float)q.n.x, (float)q.n.y, (float)q.n.z, 0 });
        }
        // Probe points: a grid under and around both plates at two depths, and above the water.
        std::vector<float> points;
        for (double y : { 0.0, 0.25, 0.9 })
            for (double z = -1.3; z <= 1.3; z += 0.05)
                for (double x = -1.5; x <= 3.8; x += 0.05) points.insert(points.end(), { (float)x, (float)y, (float)z, 0 });
        const uint32_t count = uint32_t(points.size() / 4);

        std::vector<uint8_t> result[2];
        for (int run = 0; run < 2; ++run)
        {
            RenderGraph g(gpu.device);
            std::vector<ComPtr<ID3D12Resource>> keep;
            auto upload = [&](const char* name, const void* data, uint64_t bytes, uint32_t stride = 0) {
                const BufferRef b = g.createBuffer({ name, bytes, stride });
                ComPtr<ID3D12Resource> staging = hostBuffer(gpu.device, bytes, D3D12_HEAP_TYPE_UPLOAD);
                uint8_t* m = nullptr;
                D3D12_RANGE none{ 0, 0 };
                check(staging->Map(0, &none, reinterpret_cast<void**>(&m)), "map");
                std::memcpy(m, data, bytes);
                staging->Unmap(0, nullptr);
                ID3D12Resource* src = staging.Get();
                keep.push_back(staging);
                g.addPass("w.test.upload", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(b, Use::CopyDst); },
                          [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(b), 0, src, 0, bytes); });
                return b;
            };
            std::vector<water::WaterSunStream> streams;
            for (int p = 0; p < 2; ++p)
            {
                water::WaterSunStream w;
                w.stream.vertices = upload("w.test.plate vertices", vertices[p].data(), vertices[p].size() * 4);
                const uint32_t args[4] = { 6, 1, 0, 0 };
                w.stream.drawArgs = upload("w.test.plate args", args, 16);
                w.stream.maxTriangles = 2;
                float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
                for (size_t v = 0; v < vertices[p].size(); v += 8)
                    for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], vertices[p][v + a]), hi[a] = std::max(hi[a], vertices[p][v + a]);
                w.stream.boundsMin = { lo[0], lo[1], lo[2] };
                w.stream.boundsMax = { hi[0], hi[1], hi[2] };
                std::copy(plates[p].T, plates[p].T + 3, w.transmittance);
                w.ior = plates[p].ior;
                streams.push_back(w);
            }
            water::WaterSunMap map(gpu.device);
            const water::WaterSunMapOutput out = map.record(g, gpu.shaders, 0, streams, { (float)sun.x, (float)sun.y, (float)sun.z });
            W_CHECK(out.texels >= 256, "no sun map");
            const BufferRef pts = upload("w.test.points", points.data(), points.size() * 4, 16);
            const BufferRef res = g.createBuffer({ "w.test.light", uint64_t(count) * 32, 0 });
            ComPtr<ID3D12Resource> rb = hostBuffer(gpu.device, uint64_t(count) * 32, D3D12_HEAP_TYPE_READBACK);
            ID3D12Resource* rbp = rb.Get();
            ID3D12PipelineState* probe = gpu.shaders.compute("Passes/Water/Tests/WaterLightProbe");
            const TextureRef depth = out.depth, normal = out.normal, medium = out.medium;
            const BufferRef constants = out.constants;
            const bool absent = run == 1;
            const float sunF[3] = { (float)sun.x, (float)sun.y, (float)sun.z };
            g.addPass("w.test.probe", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(pts, Use::SrvCompute);
                          pb.use(res, Use::UavCompute);
                          pb.use(depth, Use::SrvCompute);
                          pb.use(normal, Use::SrvCompute);
                          pb.use(medium, Use::SrvCompute);
                          pb.use(constants, Use::SrvCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k[12] = { c.srv(pts), c.uav(res), count, 0, c.srv(depth), c.srv(normal), c.srv(medium), absent ? gpu::kNone : c.srv(constants) };
                          std::memcpy(&k[8], sunF, 12);
                          c.cmd->SetPipelineState(probe);
                          c.computeConstants(k, 12);
                          water::dispatchLinear(c.cmd, (count + 63) / 64);
                      });
            g.addPass("w.test.read", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(res, Use::CopySrc);
                          pb.keep();
                      },
                      [=](PassContext& c) { c.cmd->CopyBufferRegion(rbp, 0, c.resource(res), 0, uint64_t(count) * 32); });
            g.execute(nullptr);
            for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
            const uint8_t* m = nullptr;
            check(rb->Map(0, nullptr, (void**)&m), "map result");
            result[run].assign(m, m + uint64_t(count) * 32);
            rb->Unmap(0, nullptr);
        }

        uint32_t lit = 0, unlit = 0, boundary = 0, wrongLit = 0, wrongUnlit = 0;
        double worstDir = 0, worstT = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const V3 X{ points[4 * i], points[4 * i + 1], points[4 * i + 2] };
            // The exact first water surface along the sun ray from X (the one nearest the sun).
            int hit = -1;
            double bestU = -1;
            bool nearEdge = false;
            for (int p = 0; p < 2; ++p)
            {
                const Plate& q = plates[p];
                const double den = dot(sun, q.n);
                if (std::fabs(den) < 1e-12) continue;
                const double u = dot(q.centre - X, q.n) / den;
                if (u <= 0) continue;
                const V3 S = X + sun * u, rel = S - q.centre;
                const double ea = q.ha - std::fabs(dot(rel, q.a)), eb = q.hb - std::fabs(dot(rel, q.b));
                if (std::fabs(ea) < 0.01 || std::fabs(eb) < 0.01) nearEdge = true;
                if (ea >= 0 && eb >= 0 && u > bestU) bestU = u, hit = p;
            }
            float gl[4], gt[4];
            std::memcpy(gl, &result[0][32 * i], 16);
            std::memcpy(gt, &result[0][32 * i + 16], 16);
            uint32_t absentFlag;
            std::memcpy(&absentFlag, &result[1][32 * i + 12], 4);
            W_CHECK(absentFlag == 0, "point %u lit without a map", i);
            uint32_t litWord;
            std::memcpy(&litWord, &result[0][32 * i + 12], 4);
            const bool gpuLit = litWord == 1;
            if (nearEdge) { ++boundary; continue; }
            if ((hit >= 0) != gpuLit)
            {
                (gpuLit ? wrongLit : wrongUnlit)++;
                if (wrongLit + wrongUnlit <= 5) logf("  point (%.2f, %.2f, %.2f): CPU %s, GPU %s\n", X.x, X.y, X.z, hit >= 0 ? "lit" : "unlit", gpuLit ? "lit" : "unlit");
                continue;
            }
            if (hit < 0) { ++unlit; continue; }
            ++lit;
            const Plate& q = plates[hit];
            const double eta = 1.0 / q.ior, cosS = dot(sun, q.n), s2 = eta * eta * (1 - cosS * cosS);
            const V3 t = norm(sun * -eta + q.n * (eta * cosS - std::sqrt(1 - s2)));
            const V3 L = t * -1.0;
            const V3 S = X + sun * bestU;
            const double path = dot(S - X, q.n) / dot(L, q.n), F = fresnel(cosS, eta);
            worstDir = std::max({ worstDir, std::fabs(gl[0] - L.x), std::fabs(gl[1] - L.y), std::fabs(gl[2] - L.z) });
            for (int c = 0; c < 3; ++c)
            {
                const double want = (1 - F) * std::pow((double)q.T[c], path);
                worstT = std::max(worstT, std::fabs(gt[c] - want) / want);
            }
        }
        std::printf("sun map: %u points lit, %u unlit, %u boundary; misclassified %u lit / %u unlit; light direction within %.2e, transmittance within %.2e (relative)\n", lit,
                    unlit, boundary, wrongLit, wrongUnlit, worstDir, worstT);
        W_CHECK(lit > 1000 && unlit > 1000, "too few points on either side: %u lit, %u unlit", lit, unlit);
        W_CHECK(wrongLit == 0 && wrongUnlit == 0, "%u / %u points misclassified", wrongLit, wrongUnlit);
        W_CHECK(worstDir <= 2e-3, "light direction off by %.3g", worstDir);
        W_CHECK(worstT <= 3e-3, "transmittance off by %.3g relative", worstT);
        if (!warp)
        {
            const uint32_t errors = gpu.device.drainDebugMessages();
            W_CHECK(errors == 0, "%u debug-layer errors", errors);
        }
        std::printf("water light tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
