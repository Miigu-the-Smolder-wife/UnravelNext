// B5 volumetric clouds, GPU part (S_STATUS_KO.md 9 step b): the sun's deep opacity map (CloudShadow.hlsl) and the march
// (CloudMarch.hlsl, single scattering of the sun) against the CPU reference (CloudModel.cpp: exact transmittance toward
// the camera and the sun, 20 m steps, double precision) on the same rays; mode 4 (the APPROXIMATE multiple scattering and
// sky light the frame uses, not exact) against its CPU twin referenceApproximate (the implementation, not the model's
// error: that is Tests/CloudMsFit.cpp). The view is 96 x 54 pixels with the pixel angle
// of a quarter-resolution 4K view (2.1e-3 rad), so the march takes the step lengths it takes in a frame.
//   unx_test_atmosphere_cloudtests [--warp] [--no-debug-layer] [--texels N] [--size W H] [--time N [--time-mode M]]
// --time N repeats the shadow map + march N more times and reports the median wall time of a submission and wait (an
// upper bound of the GPU time: includes the submission; run under GpuLock -Kind timing); --time-mode M times the shadow map
// and march mode M alone (1: single scattering, 4: the approximate multiple scattering and sky light).
#include "../CloudGpu.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/render/ViewDesc.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/scene/SceneData.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;
using namespace unx::render::clouds;

namespace
{
ComPtr<ID3D12Device> warpDevice()
{
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "WARP device");
    return device;
}
ComPtr<ID3D12Resource> buffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, bool uav)
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
uint32_t rawSrv(Device& device, ID3D12Resource* r, uint32_t bytes)
{
    const uint32_t index = device.descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.Buffer.NumElements = bytes / 4;
    sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device.d3d()->CreateShaderResourceView(r, &sd, device.descriptors().resourceCpu(index));
    return index;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool warp = false, debugLayer = true;
        uint32_t texelsArg = 0;  // --texels N: the deep opacity map's size (attribution runs)
        uint32_t sizeArg[2] = { 96, 54 }, timeRuns = 0, timeMode = 0;  // --size W H (the pixel angle stays 2.1e-3); --time N: N timed frames
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--warp") warp = true;
            else if (a == "--no-debug-layer") debugLayer = false;
            else if (a == "--texels" && i + 1 < argc) texelsArg = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--size" && i + 2 < argc) sizeArg[0] = (uint32_t)std::stoul(argv[++i]), sizeArg[1] = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--time" && i + 1 < argc) timeRuns = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--time-mode" && i + 1 < argc) timeMode = (uint32_t)std::stoul(argv[++i]);
            else fail("unknown argument %s", a.c_str());
        }
        bool pass = true;
        auto report = [&](bool ok, const char* what, double value, double limit) {
            logf("  %-66s %.3e (limit %.1e) %s\n", what, value, limit, ok ? "ok" : "FAIL");
            pass = pass && ok;
        };
        ComPtr<ID3D12Device> external = warp ? warpDevice() : nullptr;
        DeviceOptions options;
        options.debugLayer = debugLayer && !warp;
        options.externalDevice = external.Get();
        Device device(options);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        logf("clouds on %s\n", warp ? "WARP" : "the hardware device");

        const double R = 6360e3;
        CloudLayer layer;
        layer.coverage = 0.6f;
        const double origin[3] = { 0, 0, 0 };
        const CloudOffsets offsets = offsetsFor(layer, origin, 0);
        const CloudNoise noise = generateNoise(1);
        CloudTextures textures = uploadTextures(device, noise);

        // The view: from the ground toward a cloudy azimuth, 12 deg up; pixel angle 2.1e-3 rad (quarter-resolution 4K).
        const uint32_t W = sizeArg[0], H = sizeArg[1];
        scene::Camera camera;
        camera.position = { 0, 1.8f, 0 };
        const float elevation = 0.21f;
        float azimuth = 0;
        {
            // The first azimuth whose central ray crosses cloud without saturating (0.05 < T < 0.9; CPU, 40 m steps).
            const float sd[3] = { -0.53f, 0.42f, 0.736f };
            const double n = std::sqrt((double)sd[0] * sd[0] + (double)sd[1] * sd[1] + (double)sd[2] * sd[2]);
            const double s[3] = { sd[0] / n, sd[1] / n, sd[2] / n }, o[3] = { 0, 1.8, 0 };
            for (uint32_t a = 0; a < 720; ++a)
            {
                const double az = a * 3.141592653589793 / 360;
                const double e[3] = { std::cos(az) * std::cos(elevation), std::sin(elevation), std::sin(az) * std::cos(elevation) };
                const double t = referenceSingleScattering(noise, layer, offsets, R, o, e, s, 1.0, 0.0, 20000, 40).transmittance;
                if (t < 0.9 && t > 0.05)
                {
                    azimuth = (float)az;
                    break;
                }
            }
        }
        camera.forward = normalize(float3{ std::cos(azimuth) * std::cos(elevation), std::sin(elevation), std::sin(azimuth) * std::cos(elevation) });
        camera.verticalFov = 2.1e-3f * H;
        const ViewDesc view = ViewDesc::fromCamera(camera, W, H, float4x4{});
        gpu::FrameConstants fc{};
        fc.viewProj = view.viewProj, fc.invViewProj = view.invViewProj, fc.view = view.view, fc.proj = view.proj;
        fc.cameraPosition = view.position, fc.nearPlane = view.nearPlane;
        fc.viewWidth = W, fc.viewHeight = H;
        const float sunDir[3] = { -0.53f, 0.42f, 0.736f };  // normalised below
        const float sl = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
        const float sun[3] = { sunDir[0] / sl, sunDir[1] / sl, sunDir[2] / sl }, illuminance[3] = { 1, 1, 1 };
        // Shadow map over the view's cloud area: centred 8 km out along the view at the layer's middle, +-12 km.
        const uint32_t texels = texelsArg ? texelsArg : warp ? 256u : 512u;
        const float centre[3] = { std::cos(azimuth) * 8000, 2750, std::sin(azimuth) * 8000 };
        CloudRecord rec = makeRecord(layer, offsets, textures, R, sun, illuminance, centre, 12000, texels);
        const float skyTest = 0.5f;  // mode 4's sky radiance (relative to the sun's illuminance 1)
        rec.skyRadianceTest = skyTest;

        ComPtr<ID3D12Resource> constants = buffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD, false);
        ComPtr<ID3D12Resource> recordBuffer = buffer(device, 256, D3D12_HEAP_TYPE_UPLOAD, false);
        // Three runs: the march (mode 1), the deep opacity map alone (mode 2, attribution), the approximate model (mode 4).
        const uint64_t outBytes = (uint64_t)W * H * 32;
        ComPtr<ID3D12Resource> outs[3] = { buffer(device, outBytes, D3D12_HEAP_TYPE_DEFAULT, true), buffer(device, outBytes, D3D12_HEAP_TYPE_DEFAULT, true),
                                           buffer(device, outBytes, D3D12_HEAP_TYPE_DEFAULT, true) };
        ComPtr<ID3D12Resource> readback = buffer(device, 3 * outBytes, D3D12_HEAP_TYPE_READBACK, false);
        {
            void* m = nullptr;
            D3D12_RANGE none{ 0, 0 };
            check(constants->Map(0, &none, &m), "map constants");
            std::memcpy(m, &fc, sizeof fc);
            constants->Unmap(0, nullptr);
        }
        const uint32_t recordSrv = rawSrv(device, recordBuffer.Get(), 256);
        std::vector<double> times;
        for (uint32_t iter = 0; iter <= timeRuns; ++iter)
        {
            const auto t0 = std::chrono::steady_clock::now();
            RenderGraph graph(device);
            TextureDesc sd{ "cloud shadow map", texels * 2, texels, 1, 1, DXGI_FORMAT_R32G32B32A32_UINT };
            const TextureRef shadow = graph.createTexture(sd);
            const BufferRef os[3] = { graph.importBuffer(outs[0].Get(), { "cloud march out", outBytes, 0 }), graph.importBuffer(outs[1].Get(), { "cloud march exact sun", outBytes, 0 }),
                                      graph.importBuffer(outs[2].Get(), { "cloud march approximate", outBytes, 0 }) };
            const D3D12_GPU_VIRTUAL_ADDRESS cb = constants->GetGPUVirtualAddress();
            graph.addPass("test.cloud.shadow", QueueType::Graphics, [&](PassBuilder& b) { b.use(shadow, Use::UavCompute); },
                          [&, shadow](PassContext& c) {
                              const uint32_t k[4] = { recordSrv, c.uav(shadow), 0, 0 };
                              c.cmd->SetPipelineState(shaders.compute("Passes/Atmosphere/CloudShadow"));
                              c.computeConstants(k, 4);
                              c.cmd->Dispatch((texels + 7) / 8, (texels + 7) / 8, 1);
                          });
            for (uint32_t mode : { 1u, 2u, 4u })
            {
            if (iter > 0 && timeMode && mode != timeMode) continue;
            const BufferRef o = os[mode == 4 ? 2 : mode - 1];
            graph.addPass(mode == 1 ? "test.cloud.march" : mode == 2 ? "test.cloud.march.exact" : "test.cloud.march.approximate", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              b.use(shadow, Use::SrvCompute);
                              b.use(o, Use::UavCompute);
                              b.keep();
                          },
                          [&, o, cb, shadow, mode](PassContext& c) {
                              // The record names the shadow map's SRV, known only while recording; both passes read the record
                              // on the GPU after this write (one submission).
                              rec.shadow = c.srv(shadow);
                              void* m = nullptr;
                              D3D12_RANGE none{ 0, 0 };
                              check(recordBuffer->Map(0, &none, &m), "map record");
                              std::memcpy(m, &rec, sizeof rec);
                              recordBuffer->Unmap(0, nullptr);
                              float maxDistance = 20000;
                              uint32_t md;
                              std::memcpy(&md, &maxDistance, 4);
                              const uint32_t k[8] = { recordSrv, c.uav(o), 0xFFFFFFFFu, 1, W, H, mode, md };
                              c.cmd->SetPipelineState(shaders.compute("Passes/Atmosphere/CloudMarch"));
                              c.computeConstants(k, 8);
                              c.bindFrameConstants(cb);
                              c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                          });
            }
            graph.execute(nullptr);
            device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));
            if (iter > 0) times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        if (!times.empty())
        {
            std::sort(times.begin(), times.end());
            logf("  %ux%u, map %u^2: shadow map + march %s, median %.3f ms over %zu (submission included)\n", W, H, texels,
                 timeMode == 1 ? "mode 1 (single scattering)" : timeMode == 4 ? "mode 4 (approximate multiple scattering + sky)" : timeMode ? "(one mode)" : "modes 1, 2, 4",
                 times[times.size() / 2], times.size());
        }
        CommandList cl = device.acquireCommandList(QueueType::Graphics);
        cl.list->CopyBufferRegion(readback.Get(), 0, outs[0].Get(), 0, outBytes);
        cl.list->CopyBufferRegion(readback.Get(), outBytes, outs[1].Get(), 0, outBytes);
        cl.list->CopyBufferRegion(readback.Get(), 2 * outBytes, outs[2].Get(), 0, outBytes);
        device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
        std::vector<float> v(W * H * 24);
        {
            void* m = nullptr;
            D3D12_RANGE all{ 0, (SIZE_T)(3 * outBytes) }, none{ 0, 0 };
            check(readback->Map(0, &all, &m), "map readback");
            std::memcpy(v.data(), m, (size_t)(3 * outBytes));
            readback->Unmap(0, &none);
        }
        // The CPU reference on every 6th pixel of every 6th row (the kernel's own directions).
        double maxL = 0;
        struct Pair
        {
            double gpuL, cpuL, gpuT, cpuT, exactL, approxGpu, approxCpu;
        };
        std::vector<Pair> pairs;
        const double o3[3] = { camera.position.x, camera.position.y, camera.position.z }, s3[3] = { sun[0], sun[1], sun[2] };
        const uint32_t stride = std::max(6u, W / 16);
        for (uint32_t y = stride / 2; y < H; y += stride)
            for (uint32_t x = stride / 2; x < W; x += stride)
            {
                const float* p = &v[(y * W + x) * 8];
                const double d[3] = { p[4], p[5], p[6] };
                const RayResult r = referenceSingleScattering(noise, layer, offsets, R, o3, d, s3, 1.0, 0.0, 20000, 20);
                const RayResult ra = referenceApproximate(noise, layer, offsets, R, o3, d, s3, 1.0, skyTest, 20000, 20);
                pairs.push_back({ p[0], r.radiance, p[3], r.transmittance, v[(size_t)W * H * 8 + (y * W + x) * 8], v[(size_t)W * H * 16 + (y * W + x) * 8],
                                  ra.radiance });
                maxL = std::max(maxL, r.radiance);
            }
        double worstT = 0, sumRel = 0, worstRel = 0, sumExact = 0, worstExact = 0, sumApprox = 0, worstApprox = 0;
        uint32_t cloudy = 0;
        for (const Pair& q : pairs)
        {
            worstT = std::max(worstT, std::abs(q.gpuT - q.cpuT));
            if (q.cpuL > 0.05 * maxL)
            {
                const double rel = std::abs(q.gpuL - q.cpuL) / q.cpuL;
                sumRel += rel;
                worstRel = std::max(worstRel, rel);
                const double relExact = std::abs(q.exactL - q.cpuL) / q.cpuL;
                sumExact += relExact;
                worstExact = std::max(worstExact, relExact);
                const double relApprox = std::abs(q.approxGpu - q.approxCpu) / q.approxCpu;
                sumApprox += relApprox;
                worstApprox = std::max(worstApprox, relApprox);
                ++cloudy;
            }
        }
        logf("  %zu rays, %u with radiance above 5 %% of the brightest (%.4e)\n", pairs.size(), cloudy, maxL);
        report(cloudy >= 20, "the view shows lit cloud (rays)", cloudy, 20);
        report(worstT < 0.03, "transmittance |GPU - CPU| (worst)", worstT, 0.03);
        report(cloudy && sumRel / cloudy < 0.02, "radiance |GPU - CPU| / CPU (mean)", cloudy ? sumRel / cloudy : 1, 0.02);
        report(worstRel < 0.1, "radiance |GPU - CPU| / CPU (worst)", worstRel, 0.1);
        report(cloudy && sumApprox / cloudy < 0.02, "approximate model (mode 4) |GPU - CPU twin| / CPU (mean)", cloudy ? sumApprox / cloudy : 1, 0.02);
        report(worstApprox < 0.1, "approximate model (mode 4) |GPU - CPU twin| / CPU (worst)", worstApprox, 0.1);
        logf("  attribution: the deep opacity map alone (mode 2): mean %.4f, worst %.3f\n", cloudy ? sumExact / cloudy : 0.0, worstExact);
        releaseTextures(device, textures);
        device.waitIdle();
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
