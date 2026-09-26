// Wind windAt (WindField.hlsli, B6), C++ and HLSL from the one header:
//  1. Record semantics (the World's FieldSampling.h rules): a global ADD, a unit-sphere REPLACE scaled to 10 m, a box
//     MAX, a later global MIN, at points inside and outside each domain, against the values worked out by hand.
//  2. Turbulence is divergence free: central differences (h = 1e-3 L) at 2,000 points, |div v| against |grad v|.
//  3. Continuity: across the noise lattice's planes (steps of 1e-4 L) the wind changes by O(step).
//  4. GPU = CPU: 4,096 points with every record kind and turbulence, relative difference.
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/core/Math.h"
#include "unx/render/Device.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "../WindField.hlsli"

using namespace unx;
using namespace unx::render;
using wind::WindRecord;

namespace
{
WindRecord record(uint32_t op, uint32_t shape, float3 origin, float scale, float3 value, float amplitude = 0, float length = 1, float timeScale = 1,
                  uint32_t octaves = 1, uint32_t seed = 0)
{
    WindRecord r{};
    r.origin = origin;
    r.opShape = op | (shape << 8);
    const float inv = 1 / scale;
    r.row0 = { inv, 0, 0, value.x };
    r.row1 = { 0, inv, 0, value.y };
    r.row2 = { 0, 0, inv, value.z };
    uint32_t bits = octaves | (seed << 8);
    float fbits;
    std::memcpy(&fbits, &bits, 4);
    r.turbulence = { amplitude, length, timeScale, fbits };
    return r;
}
bool close3(float3 a, float3 b, float eps) { return std::fabs(a.x - b.x) <= eps && std::fabs(a.y - b.y) <= eps && std::fabs(a.z - b.z) <= eps; }

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, bool uav)
{
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (uav) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "buffer");
    return r;
}
uint32_t structuredSrv(Device& device, ID3D12Resource* r, uint32_t stride, uint32_t count)
{
    const uint32_t index = device.descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.Buffer.NumElements = count;
    sd.Buffer.StructureByteStride = stride;
    device.d3d()->CreateShaderResourceView(r, &sd, device.descriptors().resourceCpu(index));
    return index;
}
ComPtr<ID3D12Resource> upload(Device& device, const void* data, uint64_t bytes)
{
    ComPtr<ID3D12Resource> r = makeBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, false);
    void* m = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(r->Map(0, &none, &m), "map");
    std::memcpy(m, data, (size_t)bytes);
    r->Unmap(0, nullptr);
    return r;
}
} // namespace

int main()
{
    try
    {
        bool pass = true;
        auto report = [&](bool ok, const char* what, double value, double limit) {
            logf("  %-62s %.3e (limit %.1e) %s\n", what, value, limit, ok ? "ok" : "FAIL");
            pass = pass && ok;
        };
        // 0. The unit curl noise has RMS speed 1 for 1-4 octaves (wfCurl's constants, measured at seed 7) at another seed and place.
        for (uint32_t oct = 1; oct <= 4; ++oct)
        {
            double sum = 0;
            const int n = 200000;
            for (int i = 0; i < n; ++i)
            {
                const float3 p{ (float)std::fmod(i * 0.7371, 997.0), (float)std::fmod(i * 0.3117, 991.0), (float)std::fmod(i * 0.5933, 983.0) };
                const float3 v = wind::wfCurl(p + float3{ 311.7f, -47.3f, 1033.9f }, 0.029f * i, oct, 23);  // not the constants' sample set (seed 7)
                sum += dot(v, v);
            }
            logf("  unit curl noise, %u octave(s): RMS speed %.5f\n", oct, std::sqrt(sum / n));
            report(std::fabs(std::sqrt(sum / n) - 1) < 0.02, "unit curl noise RMS speed within 2 % of 1 (the amplitude's meaning)", std::fabs(std::sqrt(sum / n) - 1), 0.02);
        }
        // 1. Semantics.
        {
            const std::vector<WindRecord> rs = {
                record(wind::kWindAdd, wind::kWindGlobal, { 0, 0, 0 }, 1, { 5, 0, 0 }),
                record(wind::kWindReplace, wind::kWindSphere, { 20, 0, 0 }, 10, { 0, 0, 3 }),
                record(wind::kWindMaximum, wind::kWindBox, { -20, 0, 0 }, 5, { 8, 1, -2 }),
                record(wind::kWindMinimum, wind::kWindGlobal, { 0, 0, 0 }, 1, { 6, 0.5f, 10 }),
            };
            struct Case
            {
                float3 x, want;
            };
            // outside both local domains: ADD (5,0,0) then MIN with (6,0.5,10) -> (5,0,0)
            // in the sphere: ADD, then REPLACE (0,0,3), then MIN -> (0,0,3)
            // in the box: ADD (5,0,0), MAX with (8,1,-2) -> (8,1,0), MIN -> (6,0.5,0)
            // sphere edge just outside (radius 10.01): as outside
            const Case cases[] = { { { 0, 0, 0 }, { 5, 0, 0 } },       { { 20, 3, 4 }, { 0, 0, 3 } },   { { -22, 4, -4 }, { 6, 0.5f, 0 } },
                                   { { 30.01f, 0, 0 }, { 5, 0, 0 } }, { { -25.01f, 0, 0 }, { 5, 0, 0 } } };
            double worst = 0;
            for (const Case& c : cases)
            {
                const float3 v = wind::windAt(rs.data(), (uint32_t)rs.size(), c.x, 0);
                worst = std::max<double>(worst, std::max({ std::fabs(v.x - c.want.x), std::fabs(v.y - c.want.y), std::fabs(v.z - c.want.z) }));
            }
            report(worst == 0, "record semantics (ADD, REPLACE, MAX, MIN; domains) max |error|", worst, 0);
        }
        // 2-3. Turbulence.
        const WindRecord gust = record(wind::kWindAdd, wind::kWindGlobal, { 0, 0, 0 }, 1, { 3, 0, 0 }, 2.0f, 24.0f, 7.0f, 3, 11);
        {
            double worstRatio = 0, meanGrad = 0;
            uint32_t n = 0;
            for (int i = 0; i < 2000; ++i)
            {
                const float3 x{ (float)std::fmod(i * 37.13, 300.0) - 150, (float)std::fmod(i * 11.7, 60.0), (float)std::fmod(i * 91.3, 300.0) - 150 };
                const float t = 0.37f * i, h = 1e-3f * 24.0f;
                auto at = [&](float3 p) { return wind::windAt(&gust, 1, p, t); };
                const float3 dx = at(x + float3{ h, 0, 0 }) - at(x - float3{ h, 0, 0 }), dy = at(x + float3{ 0, h, 0 }) - at(x - float3{ 0, h, 0 }),
                             dz = at(x + float3{ 0, 0, h }) - at(x - float3{ 0, 0, h });
                const double div = (dx.x + dy.y + dz.z) / (2 * h);
                const double grad = std::sqrt((double)dot(dx, dx) + dot(dy, dy) + dot(dz, dz)) / (2 * h);
                worstRatio = std::max(worstRatio, std::fabs(div) / std::max(grad, 1e-9));
                meanGrad += grad;
                ++n;
            }
            logf("  turbulence: mean |grad v| %.3f 1/s over 2000 points\n", meanGrad / n);
            report(worstRatio < 5e-3, "turbulence divergence |div v| / |grad v| (worst)", worstRatio, 5e-3);
        }
        {
            double worst = 0;
            for (int i = 0; i < 400; ++i)
            {
                // Points on lattice planes of the first octave (x / L integer).
                const float3 x{ 24.0f * (i % 11 - 5), 24.0f * (i % 3) + 0.3f * i, 24.0f * (i % 7 - 3) };
                const float step = 1e-4f * 24.0f;
                const float3 a = wind::windAt(&gust, 1, x - float3{ step, step, step }, 1.0f), b = wind::windAt(&gust, 1, x + float3{ step, step, step }, 1.0f);
                worst = std::max<double>(worst, std::sqrt((double)dot(b - a, b - a)) / (2 * std::sqrt(3.0) * step));
            }
            report(worst < 5, "continuity across lattice planes: |dv| / |dx| (1/s, worst)", worst, 5);
        }
        // 4. GPU = CPU.
        {
            Device device({});
            ShaderLibrary shaders(device, executableDirectory() / "shaders");
            std::vector<WindRecord> rs = {
                record(wind::kWindAdd, wind::kWindGlobal, { 0, 0, 0 }, 1, { 3, 0, 0 }, 2.0f, 24.0f, 7.0f, 3, 11),
                record(wind::kWindReplace, wind::kWindSphere, { 20, 0, 0 }, 10, { 0, 0, 3 }, 1.0f, 6.0f, 2.0f, 2, 5),
                record(wind::kWindMaximum, wind::kWindBox, { -20, 0, 0 }, 5, { 8, 1, -2 }),
            };
            const uint32_t count = 4096;
            std::vector<float> queries(count * 4);
            for (uint32_t i = 0; i < count; ++i)
            {
                queries[4 * i] = (float)std::fmod(i * 7.31, 80.0) - 40;
                queries[4 * i + 1] = (float)std::fmod(i * 3.17, 20.0) - 5;
                queries[4 * i + 2] = (float)std::fmod(i * 5.93, 40.0) - 20;
                queries[4 * i + 3] = 0.013f * i;
            }
            ComPtr<ID3D12Resource> recordBuffer = upload(device, rs.data(), rs.size() * sizeof(WindRecord));
            ComPtr<ID3D12Resource> queryBuffer = upload(device, queries.data(), queries.size() * 4);
            const uint32_t recordSrv = structuredSrv(device, recordBuffer.Get(), sizeof(WindRecord), (uint32_t)rs.size());
            const uint32_t querySrv = structuredSrv(device, queryBuffer.Get(), 16, count);
            ComPtr<ID3D12Resource> out = makeBuffer(device, count * 16ull, D3D12_HEAP_TYPE_DEFAULT, true), readback = makeBuffer(device, count * 16ull, D3D12_HEAP_TYPE_READBACK, false);
            {
                RenderGraph graph(device);
                const BufferRef o = graph.importBuffer(out.Get(), { "wind out", count * 16ull, 16 });
                graph.addPass("test.wind", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  b.use(o, Use::UavCompute);
                                  b.keep();
                              },
                              [&, o](PassContext& c) {
                                  const uint32_t k[8] = { recordSrv, (uint32_t)rs.size(), querySrv, c.uav(o), count, 0, 0, 0 };
                                  c.cmd->SetPipelineState(shaders.compute("Passes/Atmosphere/Tests/WindProbe"));
                                  c.computeConstants(k, 8);
                                  c.cmd->Dispatch((count + 63) / 64, 1, 1);
                              });
                graph.execute(nullptr);
                device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));
            }
            CommandList cl = device.acquireCommandList(QueueType::Graphics);
            cl.list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, count * 16ull);
            device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
            void* m = nullptr;
            D3D12_RANGE all{ 0, (SIZE_T)(count * 16) };
            check(readback->Map(0, &all, &m), "map readback");
            double worst = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                float3 g;
                std::memcpy(&g, static_cast<const uint8_t*>(m) + i * 16, 12);
                const float3 c = wind::windAt(rs.data(), (uint32_t)rs.size(), { queries[4 * i], queries[4 * i + 1], queries[4 * i + 2] }, queries[4 * i + 3]);
                const double scale = std::max(1.0, std::sqrt((double)dot(c, c)));
                worst = std::max(worst, std::sqrt((double)dot(g - c, g - c)) / scale);
            }
            D3D12_RANGE none{ 0, 0 };
            readback->Unmap(0, &none);
            report(worst < 1e-4, "GPU vs CPU windAt, 4096 points (relative, worst)", worst, 1e-4);
            device.waitIdle();
        }
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
