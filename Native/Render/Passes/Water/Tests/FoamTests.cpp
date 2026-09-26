// Track W water foam F correctness (FEATURES_GAME 1.3 (f)-F gates; Foam.hlsli; the first run of new kernels on WARP):
//   1. variances: the GPU's per-level sigma_J^2 (64-bit fixed point) equals a double sum over the read-back spectrum
//      (relative 1e-4)
//   2. decay law: with the source off (J_t far below any Jacobian) every texel of level 0 becomes F exp(-dt / tau)
//      (R16F rounding: relative 2e-3 where F > 1e-3)
//   3. window shift: after the camera moves 64 level-0 texels (no source, no decay) the texels both windows hold are
//      bit-identical and the entering ones equal the next coarser level's bilinear foam there (1e-3)
//   4. determinism: two instances fed the same frames are bit-identical
//   5. level consistency: with an instant lifetime (F = C) the mean breaking fraction over level 0's window and over the
//      same area on level 1 agree within the model's statistical tolerance (printed; 25 % + 0.002)
//   unx_test_water_foamtests [--no-debug-layer] [--warp]
#include "unx/water/Foam.h"

#include "unx/core/File.h"
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
using unx::water::Foam;
using unx::water::FoamDesc;
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
constexpr uint32_t N = Foam::kN;

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
float half(uint16_t h)
{
    const uint32_t sign = uint32_t(h >> 15) << 31, exponent = (h >> 10) & 31, mantissa = h & 1023;
    uint32_t bits;
    if (exponent == 0) { const float f = std::ldexp(float(mantissa), -24); return (h >> 15) ? -f : f; }
    if (exponent == 31) bits = sign | 0x7F800000u | (mantissa << 13);
    else bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
struct Frame
{
    std::vector<uint16_t> foam;  // [level][storage z][storage x]
    uint64_t variance[Foam::kMaxLevels] = {};
    std::vector<float> h0;       // when asked
    float at(uint32_t level, int32_t wx, int32_t wz) const { return half(foam[(size_t(level) * N + uint32_t(wz & int32_t(N - 1))) * N + uint32_t(wx & int32_t(N - 1))]); }
};
Frame run(Gpu& gpu, Ocean& ocean, Foam& foam, uint64_t frame, double seconds, double cx, double cz, bool withH0 = false)
{
    RenderGraph g(gpu.device);
    const auto fields = ocean.record(g, seconds);
    const auto out = foam.record(g, frame, seconds, fields, ocean.desc().lengths, cx, cz);
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = td.Height = N;
    td.DepthOrArraySize = Foam::kMaxLevels;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16_FLOAT;
    td.SampleDesc.Count = 1;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(Foam::kMaxLevels);
    UINT64 total = 0;
    gpu.device.d3d()->GetCopyableFootprints(&td, 0, Foam::kMaxLevels, 0, fp.data(), nullptr, nullptr, &total);
    const uint64_t h0Bytes = uint64_t(Ocean::kN) * Ocean::kN * 3 * 16;
    ComPtr<ID3D12Resource> rb = buffer(gpu.device, total + 256 + (withH0 ? h0Bytes : 0), D3D12_HEAP_TYPE_READBACK);
    ID3D12Resource* r = rb.Get();
    const TextureRef foamTex = out.foam;
    const BufferRef h0 = fields.h0, variance = out.variance;
    g.addPass("foam read", QueueType::Graphics,
              [&](PassBuilder& pb) { pb.use(foamTex, Use::CopySrc); pb.use(variance, Use::CopySrc); if (withH0) pb.use(h0, Use::CopySrc); pb.keep(); },
              [=](PassContext& c) {
                  for (uint32_t s = 0; s < Foam::kMaxLevels; ++s)
                  {
                      D3D12_TEXTURE_COPY_LOCATION dst{ r, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                      dst.PlacedFootprint = fp[s];
                      D3D12_TEXTURE_COPY_LOCATION src{ c.resource(foamTex), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                      src.SubresourceIndex = s;
                      c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                  }
                  c.cmd->CopyBufferRegion(r, total, c.resource(variance), 0, 8 * Foam::kMaxLevels);
                  if (withH0) c.cmd->CopyBufferRegion(r, total + 256, c.resource(h0), 0, h0Bytes);
              });
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    Frame f;
    const uint8_t* m = nullptr;
    check(rb->Map(0, nullptr, (void**)&m), "map foam");
    f.foam.resize(size_t(Foam::kMaxLevels) * N * N);
    for (uint32_t s = 0; s < Foam::kMaxLevels; ++s)
        for (uint32_t z = 0; z < N; ++z) std::memcpy(&f.foam[(size_t(s) * N + z) * N], m + fp[s].Offset + uint64_t(z) * fp[s].Footprint.RowPitch, N * 2);
    std::memcpy(f.variance, m + total, 8 * Foam::kMaxLevels);
    if (withH0) f.h0.assign((const float*)(m + total + 256), (const float*)(m + total + 256 + h0Bytes));
    rb->Unmap(0, nullptr);
    return f;
}
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
        OceanDesc od;
        od.windSpeed = 16.0f;  // a rough sea: breaking crests at J_t = 0.3
        od.windDirection = 0.4f;
        od.seed = 5;
        Ocean ocean(gpu.device, gpu.shaders, od);
        const double dt = 1.0 / 60;
        const double cx = 3.7, cz = -5.3;

        // 4. determinism, with 1 (the variances and h0) on the way
        FoamDesc fd;
        Foam a(gpu.device, gpu.shaders, fd), b(gpu.device, gpu.shaders, fd);
        Frame fa, fb;
        for (uint64_t f = 0; f < 12; ++f)
        {
            fa = run(gpu, ocean, a, f, 40.0 + f * dt, cx, cz, f == 0);
            if (f == 0)
            {
                // 1. variances: sum |k|^2 (|h0(k)|^2 + |h0(-k)|^2) over |k| > pi / s_l, in double.
                double expect[Foam::kMaxLevels] = {};
                for (uint32_t c = 0; c < 3; ++c)
                    for (uint32_t z = 0; z < Ocean::kN; ++z)
                        for (uint32_t x = 0; x < Ocean::kN; ++x)
                        {
                            const double L = od.lengths[c], kx = (double(x) - Ocean::kN / 2.0) * 2 * kPi / L, kz = (double(z) - Ocean::kN / 2.0) * 2 * kPi / L;
                            const float* h = &fa.h0[((size_t(c) * Ocean::kN + z) * Ocean::kN + x) * 4];
                            const double k2 = kx * kx + kz * kz, w = k2 * (double(h[0]) * h[0] + double(h[1]) * h[1] + double(h[2]) * h[2] + double(h[3]) * h[3]);
                            for (uint32_t l = 0; l < fd.levels; ++l)
                                if (std::sqrt(k2) > kPi / (fd.s0 * double(1u << (2 * l)))) expect[l] += w;
                        }
                double worst = 0;
                std::printf("variances: sigma_J per level (GPU / CPU double):");
                for (uint32_t l = 0; l < fd.levels; ++l)
                {
                    const double gpuValue = double(fa.variance[l]) / 1099511627776.0;
                    worst = std::max(worst, std::abs(gpuValue - expect[l]) / std::max(expect[l], 1e-30));
                    std::printf(" %.4g / %.4g", std::sqrt(gpuValue), std::sqrt(expect[l]));
                }
                std::printf("\n");
                W_CHECK(worst <= 1e-4, "the GPU variances differ from the CPU sums by %.3g relative", worst);
            }
        }
        for (uint64_t f = 0; f < 12; ++f) fb = run(gpu, ocean, b, f, 40.0 + f * dt, cx, cz);
        W_CHECK(fa.foam == fb.foam, "two instances differ");
        double covered = 0;
        for (uint32_t i = 0; i < N * N; ++i) covered += half(fa.foam[i]);
        std::printf("determinism: two instances bit-identical after 12 frames; level 0 mean foam %.4f\n", covered / (N * N));
        W_CHECK(covered > 0, "no foam at all on a 16 m/s sea (the source does not fire)");

        // 2. decay law: the source off, one more frame
        {
            FoamDesc off = fd;
            off.threshold = -1e6f;
            a.setDesc(off);
            const Frame next = run(gpu, ocean, a, 12, 40.0 + 12 * dt, cx, cz);
            const double factor = std::exp(-dt / fd.tau);
            double worst = 0;
            for (uint32_t i = 0; i < N * N; ++i)
            {
                const double before = half(fa.foam[i]), after = half(next.foam[i]);
                if (before > 1e-3) worst = std::max(worst, std::abs(after - before * factor) / (before * factor));
            }
            W_CHECK(worst <= 2e-3, "decay: level 0 differs from F exp(-dt / tau) by %.3g relative", worst);
            std::printf("decay law: F exp(-dt / tau) within %.2e relative (R16F)\n", worst);
            fa = next;
        }

        // 3. window shift: no source, no decay; the camera moves 64 level-0 texels in x
        {
            FoamDesc still = fd;
            still.threshold = -1e6f;
            still.tau = 1e9f;
            a.setDesc(still);
            const double shift = 64 * fd.s0;
            const Frame moved = run(gpu, ocean, a, 13, 40.0 + 13 * dt, cx + shift, cz);
            const int32_t o0[2] = { Foam::origin(cx, fd.s0), Foam::origin(cz, fd.s0) }, o1[2] = { Foam::origin(cx + shift, fd.s0), Foam::origin(cz, fd.s0) };
            uint64_t same = 0, entered = 0;
            double worstEntered = 0;
            const float s1 = fd.s0 * 4;
            const int32_t coarse[2] = { Foam::origin(cx + shift, s1), Foam::origin(cz, s1) };
            for (int32_t z = o1[1]; z < o1[1] + int32_t(N); ++z)
                for (int32_t x = o1[0]; x < o1[0] + int32_t(N); ++x)
                {
                    const bool inOld = x >= o0[0] && x < o0[0] + int32_t(N) && z >= o0[1] && z < o0[1] + int32_t(N);
                    const float v = moved.at(0, x, z);
                    if (inOld)
                    {
                        W_CHECK(v == fa.at(0, x, z), "texel (%d, %d) changed across the shift", x, z);
                        ++same;
                        continue;
                    }
                    // The coarser level's bilinear foam at the texel centre (texel centres (I + 0.5) s_1).
                    const double ux = (x + 0.5) * fd.s0 / s1 - 0.5, uz = (z + 0.5) * fd.s0 / s1 - 0.5;
                    const int32_t ix = int32_t(std::floor(ux)), iz = int32_t(std::floor(uz));
                    const float fx = float(ux - ix), fz = float(uz - iz);
                    W_CHECK(ix >= coarse[0] && iz >= coarse[1] && ix + 1 < coarse[0] + int32_t(N) && iz + 1 < coarse[1] + int32_t(N), "entering texel outside level 1");
                    const float c00 = moved.at(1, ix, iz), c10 = moved.at(1, ix + 1, iz), c01 = moved.at(1, ix, iz + 1), c11 = moved.at(1, ix + 1, iz + 1);
                    const float expect = (c00 + (c10 - c00) * fx) + ((c01 + (c11 - c01) * fx) - (c00 + (c10 - c00) * fx)) * fz;
                    worstEntered = std::max(worstEntered, double(std::abs(v - expect)));
                    ++entered;
                }
            W_CHECK(entered == 64u * N && same == uint64_t(N - 64) * N, "entered %llu, kept %llu", (unsigned long long)entered, (unsigned long long)same);
            W_CHECK(worstEntered <= 1e-3, "entering texels differ from level 1's bilinear foam by %.3g", worstEntered);
            std::printf("window shift: %llu texels bit-identical, %llu entered from level 1 within %.2e\n", (unsigned long long)same, (unsigned long long)entered, worstEntered);
        }

        // 5. level consistency: an instant lifetime (F = C)
        {
            FoamDesc instant = fd;
            instant.tau = 1e-9f;
            Foam c(gpu.device, gpu.shaders, instant);
            Frame fc;
            for (uint64_t f = 0; f < 4; ++f) fc = run(gpu, ocean, c, f * 2, 40.0 + f * dt, cx, cz);  // even frames: level 1 updates too
            const int32_t o0[2] = { Foam::origin(cx, fd.s0), Foam::origin(cz, fd.s0) };
            double mean0 = 0, mean1 = 0;
            uint64_t n1 = 0;
            for (int32_t z = o0[1]; z < o0[1] + int32_t(N); ++z)
                for (int32_t x = o0[0]; x < o0[0] + int32_t(N); ++x) mean0 += fc.at(0, x, z);
            mean0 /= double(N) * N;
            // Level 1 texels whose area lies inside level 0's window.
            auto floorDiv = [](int32_t v, int32_t d) { return int32_t(std::floor(double(v) / d)); };
            auto ceilDiv = [](int32_t v, int32_t d) { return int32_t(std::ceil(double(v) / d)); };
            for (int32_t z = ceilDiv(o0[1], 4); z < floorDiv(o0[1] + int32_t(N), 4); ++z)
                for (int32_t x = ceilDiv(o0[0], 4); x < floorDiv(o0[0] + int32_t(N), 4); ++x) { mean1 += fc.at(1, x, z); ++n1; }
            mean1 /= double(n1);
            std::printf("level consistency: mean breaking fraction level 0 %.5f, level 1 over the same area %.5f (%+.1f %%)\n", mean0, mean1, 100 * (mean1 / std::max(mean0, 1e-12) - 1));
            W_CHECK(std::abs(mean1 - mean0) <= 0.25 * mean0 + 0.002, "levels 0 and 1 disagree: %.5f vs %.5f", mean0, mean1);
        }
        if (!warp)
        {
            const uint32_t errors = gpu.device.drainDebugMessages();
            W_CHECK(errors == 0, "%u debug-layer errors", errors);
        }
        std::printf("foam tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
