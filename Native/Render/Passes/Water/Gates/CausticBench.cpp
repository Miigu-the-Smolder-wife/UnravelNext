// Caustics cost study (FEATURE_STATUS B8): the sun map of a D0-like water surface - a 3.3 m square basin (the D0 fluid's
// domain, so a 2048^2 map and a 1024^2 caustic grid) whose surface carries particle-scale bumps (a sum of random waves of
// 2-6 cm wavelength) - then WaterCaustics.hlsl timed as the renderer runs it and its study variant
// (Gates/WaterCausticsStudy.hlsl) split by term: rays only, raster without atomics, full, each slice alone; one counting
// run (blocks, merged quads, cells by level, raster texel iterations, atomics, SIMD divergence) and the grid's touched
// texels per slice (atomics per touched texel = the contention).
//   unx_gate_water_causticbench [--amp <m>] [--overhead] [--flat] [--frames <n>]   (hardware, GpuLock W)
#include "unx/water/WaterSunMap.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/GpuProfiler.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
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
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "bench buffer");
    return r;
}
double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0 : v[v.size() / 2];
}
constexpr uint32_t kSlices = 5, kWords = 40;
} // namespace

int main(int argc, char** argv)
{
    try
    {
        double amp = 0.003;
        bool overhead = false;
        int frames = 24;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--amp" && i + 1 < argc) amp = std::atof(argv[++i]);
            if (a == "--flat") amp = 0;
            if (a == "--overhead") overhead = true;
            if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        }
        DeviceOptions o;
        o.debugLayer = false;
        Device device(o);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");

        // The surface: y = 0.4 + sum of 16 waves (random directions, wavelengths 2-6 cm, amplitudes amp / 4 each), on a
        // 6 mm vertex grid with the exact normals (the map interpolates them across a triangle).
        struct Wave
        {
            double kx, kz, a, phase;
        };
        std::vector<Wave> waves;
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> u01(0, 1);
        for (int w = 0; w < 16; ++w)
        {
            const double lambda = 0.02 + 0.04 * u01(rng), theta = 2 * 3.14159265358979 * u01(rng), k = 2 * 3.14159265358979 / lambda;
            waves.push_back({ k * std::cos(theta), k * std::sin(theta), amp / 4, 2 * 3.14159265358979 * u01(rng) });
        }
        const double half = 1.65, y0 = 0.4;
        const int cells = 550;
        std::vector<float> vertices;
        vertices.reserve(size_t(cells) * cells * 6 * 8);
        auto vertex = [&](int i, int k) {
            const double x = -half + 2 * half * i / cells, z = -half + 2 * half * k / cells;
            double h = y0, dx = 0, dz = 0;
            for (const Wave& w : waves)
            {
                const double arg = w.kx * x + w.kz * z + w.phase;
                h += w.a * std::sin(arg);
                dx += w.a * w.kx * std::cos(arg), dz += w.a * w.kz * std::cos(arg);
            }
            const double l = std::sqrt(dx * dx + 1 + dz * dz);
            vertices.insert(vertices.end(), { (float)x, (float)h, (float)z, 1, (float)(-dx / l), (float)(1 / l), (float)(-dz / l), 0 });
        };
        for (int k = 0; k < cells; ++k)
            for (int i = 0; i < cells; ++i)
                for (const auto& c : { std::pair{ 0, 0 }, std::pair{ 0, 1 }, std::pair{ 1, 0 }, std::pair{ 1, 0 }, std::pair{ 0, 1 }, std::pair{ 1, 1 } }) vertex(i + c.first, k + c.second);
        const uint64_t vbytes = vertices.size() * 4;
        ComPtr<ID3D12Resource> vstage = hostBuffer(device, vbytes, D3D12_HEAP_TYPE_UPLOAD);
        {
            uint8_t* m = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(vstage->Map(0, &none, reinterpret_cast<void**>(&m)), "map vertices");
            std::memcpy(m, vertices.data(), vbytes);
            vstage->Unmap(0, nullptr);
        }
        const uint32_t args[4] = { uint32_t(vertices.size() / 8), 1, 0, 0 };
        ComPtr<ID3D12Resource> astage = hostBuffer(device, 16, D3D12_HEAP_TYPE_UPLOAD);
        {
            uint8_t* m = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(astage->Map(0, &none, reinterpret_cast<void**>(&m)), "map args");
            std::memcpy(m, args, 16);
            astage->Unmap(0, nullptr);
        }
        const double sl = std::sqrt(0.3 * 0.3 + 0.8 * 0.8 + 0.5 * 0.5);
        const float sun[3] = { overhead ? 0.0f : float(0.3 / sl), overhead ? 1.0f : float(0.8 / sl), overhead ? 0.0f : float(0.5 / sl) };

        ID3D12PipelineState* study = shaders.compute("Passes/Water/Gates/WaterCausticsStudy");
        ID3D12PipelineState* texClear = shaders.compute("Passes/Water/WaterCausticsClear");
        ID3D12PipelineState* wordClear = shaders.compute("Passes/Water/ViewGridClear");
        water::WaterSunMap map(device);
        GpuProfiler profiler(device, 1, 128);
        // Study runs: name -> mode (bits 0-4 slices, 256 rays only, 512 no atomics).
        std::vector<std::pair<std::string, uint32_t>> runs = { { "study full", 31 }, { "study rays only", 256 | 31 }, { "study no atomics", 512 | 31 },
                                                                 { "study no edge integrals", 512 | 2048 | 31 }, { "study cells only", 512 | 4096 | 31 } };
        for (uint32_t s = 0; s < kSlices; ++s) runs.push_back({ "study slice " + std::to_string(s), 1u << s }), runs.push_back({ "study slice no atomics " + std::to_string(s), 512u | (1u << s) });
        std::map<std::string, std::vector<double>> ms;
        uint32_t counts[kWords] = {};
        std::vector<uint64_t> touched(kSlices);
        uint32_t texels = 0;
        for (int f = 0; f < frames + 1; ++f)
        {
            const bool counting = f == 0;
            RenderGraph g(device);
            water::WaterSunStream w;
            w.stream.vertices = g.createBuffer({ "bench vertices", vbytes, 0 });
            w.stream.drawArgs = g.createBuffer({ "bench args", 16, 0 });
            const BufferRef vb = w.stream.vertices, ab = w.stream.drawArgs;
            ID3D12Resource *vs = vstage.Get(), *as = astage.Get();
            g.addPass("bench upload", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(vb, Use::CopyDst);
                          pb.use(ab, Use::CopyDst);
                      },
                      [=](PassContext& c) {
                          c.cmd->CopyBufferRegion(c.resource(vb), 0, vs, 0, vbytes);
                          c.cmd->CopyBufferRegion(c.resource(ab), 0, as, 0, 16);
                      });
            w.stream.maxTriangles = uint32_t(vertices.size() / 24);
            w.stream.boundsMin = { (float)-half, (float)(y0 - amp * 4), (float)-half };
            w.stream.boundsMax = { (float)half, (float)(y0 + amp * 4), (float)half };
            w.transmittance[0] = w.transmittance[1] = w.transmittance[2] = 1.0f;
            w.ior = 1.333f;
            const water::WaterSunMapOutput out = map.record(g, shaders, uint64_t(f), { w }, { sun[0], sun[1], sun[2] });
            if (!out.texels) fail("no sun map");
            texels = out.texels;
            const uint32_t nc = std::min(out.texels, 1024u);
            uint32_t levelWords = 0;
            for (uint32_t side = nc >> 1; side >= 1; side >>= 1) levelWords += side * side;
            const TextureRef tex = g.createTexture(TextureDesc{ "bench caustics", nc, nc, uint16_t(kSlices), 1, DXGI_FORMAT_R32_UINT });
            const BufferRef levels = g.createBuffer({ "bench levels", uint64_t(levelWords) * kSlices * 4, 0 });
            const BufferRef counters = g.createBuffer({ "bench counters", kWords * 4, 0 });
            const TextureRef depth = out.depth, normal = out.normal, medium = out.medium, prod = out.caustics;
            const BufferRef constants = out.constants;
            auto clear = [&]() {
                g.addPass("bench clear", QueueType::Graphics,
                          [&](PassBuilder& pb) {
                              pb.use(tex, Use::UavCompute);
                              pb.use(levels, Use::UavCompute);
                              pb.use(counters, Use::UavCompute);
                          },
                          [=](PassContext& c) {
                              const uint32_t kt[4] = { c.uav(tex), nc, kSlices, 0 };
                              c.cmd->SetPipelineState(texClear);
                              c.computeConstants(kt, 4);
                              c.cmd->Dispatch((nc + 7) / 8, (nc + 7) / 8, kSlices);
                              const uint32_t words = levelWords * kSlices, kl[4] = { 0, 0, c.uav(levels), words };
                              c.cmd->SetPipelineState(wordClear);
                              c.computeConstants(kl, 4);
                              c.cmd->Dispatch((words + 63) / 64, 1, 1);
                              const uint32_t kc[4] = { 0, 0, c.uav(counters), kWords };
                              c.computeConstants(kc, 4);
                              c.cmd->Dispatch(1, 1, 1);
                          });
            };
            auto dispatch = [&](const std::string& name, uint32_t mode) {
                g.addPass(name, QueueType::Graphics,
                          [&](PassBuilder& pb) {
                              pb.use(depth, Use::SrvCompute);
                              pb.use(normal, Use::SrvCompute);
                              pb.use(medium, Use::SrvCompute);
                              pb.use(constants, Use::SrvCompute);
                              pb.use(tex, Use::UavCompute);
                              pb.use(counters, Use::UavCompute);
                              pb.use(levels, Use::UavCompute);
                          },
                          [=, n = out.texels](PassContext& c) {
                              const uint32_t k[8] = { c.srv(depth), c.srv(normal), c.srv(medium), c.srv(constants), c.uav(tex), c.uav(counters), c.uav(levels), mode };
                              c.cmd->SetPipelineState(study);
                              c.computeConstants(k, 8);
                              const uint32_t blocks = (n + 1) / 2;
                              c.cmd->Dispatch((blocks + 7) / 8, (blocks + 7) / 8, 1);
                          });
            };
            ComPtr<ID3D12Resource> rb;
            const uint32_t pitch = (nc * 4 + 255) & ~255u;
            if (counting)
            {
                clear();
                dispatch("study count", 1024u | 31u);
                rb = hostBuffer(device, uint64_t(pitch) * nc * kSlices + 256, D3D12_HEAP_TYPE_READBACK);
                ID3D12Resource* dst = rb.Get();
                g.addPass("bench read", QueueType::Graphics,
                          [&](PassBuilder& pb) {
                              pb.use(counters, Use::CopySrc);
                              pb.use(prod, Use::CopySrc);
                              pb.keep();
                          },
                          [=](PassContext& c) {
                              for (uint32_t s = 0; s < kSlices; ++s)
                              {
                                  D3D12_TEXTURE_COPY_LOCATION to{ dst, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, from{ c.resource(prod), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                                  to.PlacedFootprint.Offset = uint64_t(pitch) * nc * s;
                                  to.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_UINT, nc, nc, 1, pitch };
                                  from.SubresourceIndex = s;
                                  c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                              }
                              c.cmd->CopyBufferRegion(dst, uint64_t(pitch) * nc * kSlices, c.resource(counters), 0, kWords * 4);
                          });
            }
            else
                for (const auto& r : runs)
                {
                    clear();
                    dispatch(r.first, r.second);
                }
            g.addPass("bench keep", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(tex, Use::SrvCompute);
                          pb.use(prod, Use::SrvCompute);
                          pb.keep();
                      },
                      [](PassContext&) {});
            profiler.beginFrame(uint64_t(f));
            g.execute(&profiler);
            for (uint32_t q = 0; q < kQueueTypeCount; ++q) device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
            if (rb)
            {
                const uint8_t* m = nullptr;
                check(rb->Map(0, nullptr, (void**)&m), "map bench readback");
                for (uint32_t s = 0; s < kSlices; ++s)
                    for (uint32_t y = 0; y < nc; ++y)
                        for (uint32_t x = 0; x < nc; ++x)
                        {
                            uint32_t v;
                            std::memcpy(&v, m + uint64_t(pitch) * nc * s + uint64_t(y) * pitch + x * 4, 4);
                            touched[s] += v != 0;
                        }
                std::memcpy(counts, m + uint64_t(pitch) * nc * kSlices, kWords * 4);
                rb->Unmap(0, nullptr);
            }
            if (f >= 4 && profiler.lastCompleted())
                for (const PassTiming& p : profiler.lastCompleted()->passes)
                    if (p.name.rfind("study", 0) == 0 || p.name.rfind("w.sun map", 0) == 0) ms[p.name].push_back(p.durationMs());
        }
        std::printf("surface: 3.3 m basin, bumps %.1f mm (16 waves of 2-6 cm), sun %s; map %u^2, caustic grid %u^2\n", amp * 1e3, overhead ? "overhead" : "(0.3, 0.8, 0.5)", texels,
                    std::min(texels, 1024u));
        for (const auto& [name, v] : ms) std::printf("[performance] %-32s median %.4f ms (%zu frames)\n", name.c_str(), median(v), v.size());
        std::printf("blocks: %u past the early out, %u with all nine rays\n", counts[0], counts[1]);
        for (uint32_t s = 0; s < kSlices; ++s)
            std::printf("slice %u: merged quads %u, cells %u, raster texel iterations %u (%.2f per cell), slice atomics %u, level atomics %u, touched texels %llu (%.2f atomics each)\n", s,
                        counts[2 + s], counts[7 + s], counts[12 + s], counts[7 + s] ? double(counts[12 + s]) / counts[7 + s] : 0.0, counts[17 + s], counts[22 + s],
                        (unsigned long long)touched[s], touched[s] ? double(counts[17 + s]) / touched[s] : 0.0);
        std::printf("cells by level:");
        for (uint32_t L = 0; L <= 10; ++L) std::printf(" L%u %u", L, counts[27 + L]);
        std::printf("\nSIMD: raster iterations %u, per wave max x lanes %u (efficiency %.2f)\n", counts[38], counts[39], counts[39] ? double(counts[38]) / counts[39] : 0.0);
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
