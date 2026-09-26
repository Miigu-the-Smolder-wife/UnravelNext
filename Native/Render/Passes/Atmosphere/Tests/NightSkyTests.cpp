// The sky's celestial objects (Celestial.hlsli atmosphereCelestial, B4) against their definitions, without the
// atmosphere (transmittance 1; the atmosphere only multiplies by its transmittance to space):
//  1. Moon: the image around the disk integrates (sum of radiance x pixel solid angle) to the illuminance of a Lambert
//     sphere, E = albedo E_sun sin^2(r) (2 / 3) phi(alpha), phi(alpha) = (sin alpha + (pi - alpha) cos alpha) / pi -- the
//     value Celestial.cpp's directionalLight gives the moonlit frame, so the disk drawn and the light cast agree; phase
//     angles 0, 60, 120 deg. Also the terminator: at 90 deg the lit half is the half towards the sun.
//  2. Stars: the image around a star integrates to its illuminance at sub-pixel offsets (0, 0), (0.3, 0.7), (0.5, 0.5)
//     (the Gaussian spread sums to 1 over the pixel grid), including a star on a cube-cell corner (three cells).
//  3. Airglow: the zenith radiance is the record's value; 60 deg from the zenith van Rhijn's factor.
//  4. Nothing drawn when flags are 0.
#include "../Celestial.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <cmath>
#include <cstring>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
constexpr double kPi = 3.14159265358979323846;

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
// An upload-heap buffer with the data and a raw SRV (the test reads it once).
struct Raw
{
    ComPtr<ID3D12Resource> resource;
    uint32_t srv = 0;
};
Raw rawBuffer(Device& device, const void* data, uint64_t bytes)
{
    Raw b;
    bytes = (bytes + 15) & ~15ull;
    b.resource = makeBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, false);
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(b.resource->Map(0, &none, &mapped), "map");
    std::memset(mapped, 0, (size_t)bytes);
    std::memcpy(mapped, data, (size_t)std::min<uint64_t>(bytes, bytes));
    b.resource->Unmap(0, nullptr);
    b.srv = device.descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.Buffer.NumElements = (UINT)(bytes / 4);
    sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device.d3d()->CreateShaderResourceView(b.resource.Get(), &sd, device.descriptors().resourceCpu(b.srv));
    return b;
}

// The image around 'centre' (W x H pixels of angle a) -> radiance per pixel (RGB).
std::vector<float3> image(Device& device, ShaderLibrary& shaders, uint32_t recordSrv, float3 centre, float a, uint32_t w, uint32_t h, float2 subpixel = { 0, 0 })
{
    const float3 up = std::fabs(centre.y) < 0.9f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    const float3 tx = normalize(cross(up, centre)), ty = cross(centre, tx);
    const float3 c = centre - tx * (subpixel.x * a) - ty * (subpixel.y * a);  // the source moves by +subpixel pixels
    const uint64_t bytes = (uint64_t)w * h * 16;
    ComPtr<ID3D12Resource> out = makeBuffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, true), readback = makeBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK, false);
    {
        RenderGraph graph(device);
        const BufferRef o = graph.importBuffer(out.Get(), { "night sky image", bytes, 16 });
        graph.addPass("test.nightsky", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(o, Use::UavCompute);
                          b.keep();
                      },
                      [&, o](PassContext& ctx) {
                          uint32_t k[16] = { recordSrv, ctx.uav(o), w, h };
                          std::memcpy(&k[4], &c, 12);
                          std::memcpy(&k[7], &a, 4);
                          std::memcpy(&k[8], &tx, 12);
                          std::memcpy(&k[12], &ty, 12);
                          ctx.cmd->SetPipelineState(shaders.compute("Passes/Atmosphere/Tests/NightSkyProbe"));
                          ctx.computeConstants(k, 16);
                          ctx.cmd->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
                      });
        graph.execute(nullptr);
        device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));
    }
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, bytes);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    std::vector<float3> px((size_t)w * h);
    void* mapped = nullptr;
    D3D12_RANGE all{ 0, (SIZE_T)bytes };
    check(readback->Map(0, &all, &mapped), "map readback");
    for (size_t i = 0; i < px.size(); ++i) std::memcpy(&px[i], static_cast<const uint8_t*>(mapped) + i * 16, 12);
    D3D12_RANGE none{ 0, 0 };
    readback->Unmap(0, &none);
    return px;
}
double luminance(float3 c) { return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z; }
} // namespace

int main()
{
    try
    {
        Device device({});
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        bool pass = true;
        // Stars: one on an axis, one on a cube-cell corner (the face edge x = y and the cell line), one elsewhere.
        std::vector<sky::Star> stars(3);
        stars[0].equatorial = { 1, 0, 0 };
        stars[0].illuminance = { 1e-3f, 2e-3f, 3e-3f };
        stars[1].equatorial = normalize(float3{ 1, 1, 0.4f });
        stars[1].illuminance = { 4e-4f, 4e-4f, 4e-4f };
        stars[2].equatorial = normalize(float3{ -0.3f, 0.2f, 0.93f });
        stars[2].illuminance = { 2e-5f, 1e-5f, 5e-6f };
        const std::vector<uint32_t> packed = sky::packStars(stars);
        const Raw starBuffer = rawBuffer(device, packed.data(), packed.size() * 4);

        auto record = [&](const CelestialFrame& f) {
            uint32_t words[32];
            sky::packCelestialFrame(f, (uint32_t)stars.size(), starBuffer.srv, words);
            return rawBuffer(device, words, sizeof words);
        };
        CelestialFrame base;  // identity rotation: world = equatorial
        base.airglowRadiance = 0;
        base.sunIlluminance = 128000;
        base.moonAlbedo = 0.12f;
        base.moonAngularRadius = 0.0045f;
        base.moonDirection = normalize(float3{ 0.3f, 0.8f, 0.2f });

        // 1. Moon at phase angles 0, 60, 120 deg; the sun direction s from the observer with cos(alpha) = s . (-m).
        for (double alphaDeg : { 0.0, 60.0, 120.0 })
        {
            CelestialFrame f = base;
            f.flags = 2;
            const float3 m = f.moonDirection;
            const float3 side = normalize(cross(m, float3{ 0, 0, 1 }));
            const double al = alphaDeg * kPi / 180;
            f.sunDirection = normalize(m * (float)-std::cos(al) + side * (float)std::sin(al));
            const Raw r = record(f);
            const float a = f.moonAngularRadius / 40;  // 80 pixels across the disk
            const uint32_t n = 128;
            const std::vector<float3> px = image(device, shaders, r.srv, m, a, n, n);
            double sum = 0;
            for (const float3& v : px) sum += luminance(v) * a * a;
            const double s = std::sin((double)f.moonAngularRadius);
            const double phi = (std::sin(al) + (kPi - al) * std::cos(al)) / kPi;
            const double expected = f.moonAlbedo * f.sunIlluminance * s * s * (2.0 / 3.0) * phi;
            const bool ok = std::fabs(sum / expected - 1) < 0.01;
            logf("moon, phase angle %5.1f deg: integrated %.6e lux against the Lambert sphere %.6e (%+.3f %%) %s\n", alphaDeg, sum, expected,
                 100 * (sum / expected - 1), ok ? "ok" : "FAIL");
            pass = pass && ok;
            if (alphaDeg == 60.0)
            {
                // Which half is lit: the pixels on the sun's side of the disk centre hold the light.
                const float3 up = std::fabs(m.y) < 0.9f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
                const float3 tx = normalize(cross(up, m)), ty = cross(m, tx);
                double towards = 0, away = 0;
                for (uint32_t j = 0; j < n; ++j)
                    for (uint32_t i = 0; i < n; ++i)
                    {
                        const float3 o = tx * ((i + 0.5f - n / 2.0f) * a) + ty * ((j + 0.5f - n / 2.0f) * a);
                        (dot(o, f.sunDirection) > 0 ? towards : away) += luminance(px[j * n + i]);
                    }
                const bool okSide = towards > 5 * away;
                logf("moon at 60 deg: light on the sun's side %.3e, on the other %.3e %s\n", towards, away, okSide ? "ok" : "FAIL");
                pass = pass && okSide;
            }
        }
        // 2. Stars at sub-pixel offsets.
        {
            CelestialFrame f = base;
            f.flags = 4;
            const Raw r = record(f);
            for (uint32_t k = 0; k < (uint32_t)stars.size(); ++k)
                for (float2 sp : { float2{ 0, 0 }, float2{ 0.3f, 0.7f }, float2{ 0.5f, 0.5f } })
                {
                    const float a = 0.0002f;  // 0.011 deg pixels
                    const std::vector<float3> px = image(device, shaders, r.srv, stars[k].equatorial, a, 24, 24, sp);
                    float3 sum{};
                    for (const float3& v : px) sum = sum + v * (a * a);
                    const double rel = luminance(sum) / luminance(stars[k].illuminance) - 1;
                    const bool ok = std::fabs(rel) < 1e-3;
                    logf("star %u at sub-pixel (%.1f, %.1f): integrated %.6e lux against %.6e (%+.4f %%) %s\n", k, sp.x, sp.y, luminance(sum),
                         luminance(stars[k].illuminance), 100 * rel, ok ? "ok" : "FAIL");
                    pass = pass && ok;
                }
        }
        // 3. Airglow and 4. nothing drawn.
        {
            CelestialFrame f = base;
            f.flags = 0;
            f.airglowRadiance = 2e-4f;
            const Raw r = record(f);
            const std::vector<float3> zenith = image(device, shaders, r.srv, { 0, 1, 0 }, 0.001f, 8, 8);
            const float3 d60 = normalize(float3{ std::sin(1.0471976f), std::cos(1.0471976f), 0 });
            const std::vector<float3> slant = image(device, shaders, r.srv, d60, 0.001f, 8, 8);
            const double k = 6360.0 / 6450.0, expected60 = 2e-4 / std::sqrt(1 - k * k * 0.75);
            const bool ok = std::fabs(luminance(zenith[27]) / 2e-4 - 1) < 1e-3 && std::fabs(luminance(slant[27]) / expected60 - 1) < 1e-2;
            logf("airglow: zenith %.4e (2e-4), 60 deg %.4e (%.4e, van Rhijn) %s\n", luminance(zenith[27]), luminance(slant[27]), expected60, ok ? "ok" : "FAIL");
            pass = pass && ok;
            CelestialFrame none = base;
            none.flags = 0;
            const Raw rn = record(none);
            const std::vector<float3> nothing = image(device, shaders, rn.srv, base.moonDirection, base.moonAngularRadius / 10, 32, 32);
            double total = 0;
            for (const float3& v : nothing) total += luminance(v);
            const bool okNone = total == 0;
            logf("flags 0 and no airglow: %.3e in the moon's image %s\n", total, okNone ? "ok" : "FAIL");
            pass = pass && okNone;
        }
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        device.waitIdle();
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
