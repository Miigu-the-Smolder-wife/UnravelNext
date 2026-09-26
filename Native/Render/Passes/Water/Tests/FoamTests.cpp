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
//   6. automatic threshold: the GPU's displacement-gradient covariances and their rates' equal double sums (1e-4 of the
//      scale); its b, lambda at its J_t equal the CPU's double model (a 721^2 trapezoid rule instead of Gauss-Hermite;
//      relative 2e-3); mean F(J_t) = W(U) (relative 2e-3); an authored J_t still overrides it; and with an instant
//      lifetime the measured breaking fraction at several J_t (level 0, 16 windows) is the model's b (25 % + 2e-4)
//   7. whitecap coverage (--coverage): seas at U = 8/12/16/20 m/s, the default foam in 4 windows 300 m apart run from
//      F = 0 at 60 Hz for S seconds (--coverage-seconds, default 16 = 4 tau: the start's bias below e^-4), then level 0's
//      mean F averaged over the windows and 9 frames of the next 4 s is within a factor 2 of Monahan &
//      O'Muircheartaigh's W = 3.84e-6 U^3.41 (printed with the model's value and the implied active fraction)
//   8. origin rebase (FrameContext::originShift, multiples of 1024 m): after a 600 m window move (an entering band with a
//      history of its own), a run rebased by (1024, -2048) m with its camera moved the same keeps every level stored and
//      read (FoamProbe: the shading reader, place by place) bit-identical to the unrebased run over a still frame, and
//      reads within 2e-3 of it 17 frames later at J_t 0.6 (every level updates and holds foam). The ocean repeats every
//      1024 m, so the stored arrays alone cannot tell a lost storage bias (levels 3 and 4 shift by 512 and 128 texels):
//      the reader does (checked: without the bias the gate fails)
//   unx_test_water_foamtests [--no-debug-layer] [--warp] [--coverage] [--coverage-seconds S]
#include "unx/water/Foam.h"
#include "unx/water/LinearDispatch.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
    double cov[12] = {};         // FoamVariance: Sigma and Sigma' (aa, bb, cc, ab, ac, bc)
    float cal[4] = {};           // FoamCalibrate: J_t, b, lambda, mean F
    std::vector<float> probe;    // FoamProbe (when asked): the reader at 256^2 window-relative places per level
    std::vector<float> h0;       // when asked
    float at(uint32_t level, int32_t wx, int32_t wz) const { return half(foam[(size_t(level) * N + uint32_t(wz & int32_t(N - 1))) * N + uint32_t(wx & int32_t(N - 1))]); }
};
Frame run(Gpu& gpu, Ocean& ocean, Foam& foam, uint64_t frame, double seconds, double cx, double cz, bool withH0 = false, bool read = true, bool withProbe = false)
{
    RenderGraph g(gpu.device);
    const auto fields = ocean.record(g, seconds);
    const auto out = foam.record(g, frame, seconds, fields, ocean.desc(), cx, cz);
    const uint32_t probeCount = 256 * 256 * foam.desc().levels;
    BufferRef probe;
    if (withProbe)
    {
        probe = g.createBuffer({ "foam probe", uint64_t(probeCount) * 4, 0 });
        ID3D12PipelineState* pso = gpu.shaders.compute("Passes/Water/Tests/FoamProbe");
        const TextureRef foamTex = out.foam;
        const uint32_t paramSrv = out.paramSrv, levels = foam.desc().levels;
        g.addPass("foam probe", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(foamTex, Use::SrvCompute); pb.use(probe, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.srv(foamTex), paramSrv, c.uav(probe), levels };
                      c.cmd->SetPipelineState(pso);
                      c.computeConstants(k, 4);
                      unx::water::dispatchLinear(c.cmd, (probeCount + 63) / 64);
                  });
    }
    if (!read)
    {
        g.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
        return {};
    }
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
    const uint64_t probeAt = total + 256 + (withH0 ? h0Bytes : 0);
    ComPtr<ID3D12Resource> rb = buffer(gpu.device, probeAt + (withProbe ? uint64_t(probeCount) * 4 : 0), D3D12_HEAP_TYPE_READBACK);
    ID3D12Resource* r = rb.Get();
    const TextureRef foamTex = out.foam;
    const BufferRef h0 = fields.h0, variance = out.variance;
    g.addPass("foam read", QueueType::Graphics,
              [&](PassBuilder& pb) { pb.use(foamTex, Use::CopySrc); pb.use(variance, Use::CopySrc); if (withH0) pb.use(h0, Use::CopySrc); if (withProbe) pb.use(probe, Use::CopySrc); pb.keep(); },
              [=](PassContext& c) {
                  for (uint32_t s = 0; s < Foam::kMaxLevels; ++s)
                  {
                      D3D12_TEXTURE_COPY_LOCATION dst{ r, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                      dst.PlacedFootprint = fp[s];
                      D3D12_TEXTURE_COPY_LOCATION src{ c.resource(foamTex), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                      src.SubresourceIndex = s;
                      c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                  }
                  c.cmd->CopyBufferRegion(r, total, c.resource(variance), 0, 152);
                  if (withH0) c.cmd->CopyBufferRegion(r, total + 256, c.resource(h0), 0, h0Bytes);
                  if (withProbe) c.cmd->CopyBufferRegion(r, probeAt, c.resource(probe), 0, uint64_t(probeCount) * 4);
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
    for (uint32_t i = 0; i < 12; ++i)
    {
        int64_t v;
        std::memcpy(&v, m + total + 40 + 8 * i, 8);
        f.cov[i] = double(v) / 1099511627776.0;
    }
    std::memcpy(f.cal, m + total + 136, 16);
    if (withH0) f.h0.assign((const float*)(m + total + 256), (const float*)(m + total + 256 + h0Bytes));
    if (withProbe) f.probe.assign((const float*)(m + probeAt), (const float*)(m + probeAt) + probeCount);
    rb->Unmap(0, nullptr);
    return f;
}
// FoamCalibrate.hlsl's model in double with an independent rule (trapezoid over (v, w)'s unit normals on [-9, 9]^2):
// (b, lambda) at threshold t for the covariances cov (Sigma, Sigma').
struct Model
{
    double b = 0, lambda = 0;
    double mean(double tau) const { return b + lambda * tau / (1 + lambda / (1 - b) * tau); }
};
Model model(const double cov[12], double t)
{
    const double aa = cov[0], bb = cov[1], cc = cov[2], ab = cov[3], ac = cov[4], bc = cov[5];
    const double suu = aa + bb + 2 * ab, svv = aa + bb - 2 * ab, sww = cc, suv = aa - bb, suw = ac + bc, svw = ac - bc;
    const double l11 = std::sqrt(svv), l21 = svw / l11, l22 = std::sqrt(sww - l21 * l21), c1 = suv / l11, c2 = (suw - l21 * c1) / l22;
    const double s = std::sqrt(suu - c1 * c1 - c2 * c2);
    auto Phi = [](double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); };
    const int n = 721;
    const double h = 18.0 / (n - 1);
    Model m;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
        {
            const double x1 = -9 + i * h, x2 = -9 + j * h;
            const double weight = (i == 0 || i == n - 1 ? 0.5 : 1.0) * (j == 0 || j == n - 1 ? 0.5 : 1.0) * h * h * std::exp(-0.5 * (x1 * x1 + x2 * x2)) / (2 * kPi);
            const double v = l11 * x1, w = l21 * x1 + l22 * x2, mu = c1 * x1 + c2 * x2, q = 4 * t + v * v + 4 * w * w;
            if (q <= 0) continue;
            const double r = std::sqrt(q);
            m.b += weight * (Phi((r - 2 - mu) / s) - Phi((-r - 2 - mu) / s));
            for (int root = -1; root <= 1; root += 2)
            {
                const double u = -2 + root * r, a = 0.5 * (u + v), bv = 0.5 * (u - v), g0 = 1 + bv, g1 = 1 + a, g2 = -2 * w;
                const double sd2 = g0 * g0 * cov[6] + g1 * g1 * cov[7] + g2 * g2 * cov[8] + 2 * (g0 * g1 * cov[9] + g0 * g2 * cov[10] + g1 * g2 * cov[11]);
                const double e = (u - mu) / s;
                m.lambda += weight * 0.5 * std::exp(-0.5 * e * e) / (std::sqrt(2 * kPi) * s) * std::sqrt(2 / kPi) * std::sqrt(std::max(sd2, 0.0)) / (0.5 * r);
            }
        }
    return m;
}
double monahan(double u) { return std::min(1.0, 3.84e-6 * std::pow(u, 3.41)); }
// One frame of the coverage gate: one ocean record, the foams at their windows; level 0's mean F per foam when read.
std::vector<double> coverageFrame(Gpu& gpu, Ocean& ocean, std::vector<Foam*>& foams, const double (*at)[2], uint64_t frame, double seconds, bool read)
{
    RenderGraph g(gpu.device);
    const auto fields = ocean.record(g, seconds);
    std::vector<TextureRef> textures;
    for (size_t i = 0; i < foams.size(); ++i) textures.push_back(foams[i]->record(g, frame, seconds, fields, ocean.desc(), at[i][0], at[i][1]).foam);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 slice = 0;
    ComPtr<ID3D12Resource> rb;
    if (read)
    {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = td.Height = N;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R16_FLOAT;
        td.SampleDesc.Count = 1;
        gpu.device.d3d()->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &slice);
        slice = (slice + 511) & ~UINT64(511);
        rb = buffer(gpu.device, slice * foams.size(), D3D12_HEAP_TYPE_READBACK);
        ID3D12Resource* r = rb.Get();
        for (size_t i = 0; i < textures.size(); ++i)
        {
            const TextureRef t = textures[i];
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT place = fp;
            place.Offset = slice * i;
            g.addPass("coverage read", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(t, Use::CopySrc); pb.keep(); },
                      [=](PassContext& c) {
                          D3D12_TEXTURE_COPY_LOCATION dst{ r, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                          dst.PlacedFootprint = place;
                          D3D12_TEXTURE_COPY_LOCATION src{ c.resource(t), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                          src.SubresourceIndex = 0;
                          c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                      });
        }
    }
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    std::vector<double> means;
    if (!read) return means;
    const uint8_t* m = nullptr;
    check(rb->Map(0, nullptr, (void**)&m), "map coverage");
    for (size_t i = 0; i < foams.size(); ++i)
    {
        double sum = 0;
        for (uint32_t z = 0; z < N; ++z)
        {
            const uint16_t* row = (const uint16_t*)(m + slice * i + uint64_t(z) * fp.Footprint.RowPitch);
            for (uint32_t x = 0; x < N; ++x) sum += half(row[x]);
        }
        means.push_back(sum / (double(N) * N));
    }
    rb->Unmap(0, nullptr);
    return means;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, warp = false, coverage = false;
        double coverageSeconds = 16;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--warp") warp = true;
            if (std::string(argv[i]) == "--coverage") coverage = true;
            if (std::string(argv[i]) == "--coverage-seconds" && i + 1 < argc) coverageSeconds = std::atof(argv[++i]);
        }
        Gpu gpu(debugLayer, warp);
        OceanDesc od;
        od.windSpeed = 16.0f;  // a rough sea
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
                double expect[Foam::kMaxLevels] = {}, cov[12] = {};
                for (uint32_t c = 0; c < 3; ++c)
                    for (uint32_t z = 0; z < Ocean::kN; ++z)
                        for (uint32_t x = 0; x < Ocean::kN; ++x)
                        {
                            const double L = od.lengths[c], kx = (double(x) - Ocean::kN / 2.0) * 2 * kPi / L, kz = (double(z) - Ocean::kN / 2.0) * 2 * kPi / L;
                            const float* h = &fa.h0[((size_t(c) * Ocean::kN + z) * Ocean::kN + x) * 4];
                            const double k2 = kx * kx + kz * kz, w = k2 * (double(h[0]) * h[0] + double(h[1]) * h[1] + double(h[2]) * h[2] + double(h[3]) * h[3]);
                            for (uint32_t l = 0; l < fd.levels; ++l)
                                if (std::sqrt(k2) > kPi / (fd.s0 * double(1u << (2 * l)))) expect[l] += w;
                            if (k2 > 0)
                            {
                                const double k = std::sqrt(k2), A = kx * kx / k, B = kz * kz / k, C = kx * kz / k, H = w / k2;
                                const double terms[6] = { A * A * H, B * B * H, C * C * H, A * B * H, A * C * H, B * C * H };
                                for (uint32_t t = 0; t < 6; ++t)
                                {
                                    cov[t] += terms[t];
                                    cov[6 + t] += 9.81 * k * terms[t];
                                }
                            }
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

                // 6. automatic threshold: the covariances (scale: sqrt of the two diagonal terms), then the model
                const uint32_t pair[6][2] = { { 0, 0 }, { 1, 1 }, { 2, 2 }, { 0, 1 }, { 0, 2 }, { 1, 2 } };
                double covError = 0;
                for (uint32_t set = 0; set < 2; ++set)
                    for (uint32_t t = 0; t < 6; ++t)
                    {
                        const double scale = std::sqrt(cov[6 * set + pair[t][0]] * cov[6 * set + pair[t][1]]);
                        covError = std::max(covError, std::abs(fa.cov[6 * set + t] - cov[6 * set + t]) / scale);
                    }
                W_CHECK(covError <= 1e-4, "the GPU gradient covariances differ from the CPU sums by %.3g of their scale", covError);
                const double target = monahan(od.windSpeed), sigma = std::sqrt(cov[0] + cov[1] + 2 * cov[3]);
                const Model cpu = model(cov, fa.cal[0]);
                const double bError = std::abs(fa.cal[1] - cpu.b) / cpu.b, lError = std::abs(fa.cal[2] - cpu.lambda) / cpu.lambda, mean = cpu.mean(fd.tau);
                std::printf("automatic threshold: sigma_J %.4f, J_t %.4f (Gaussian z %.3f): b %.3e / %.3e, lambda %.4f / %.4f /s (GPU / CPU), mean F %.5f for W %.5f\n",
                            sigma, fa.cal[0], (fa.cal[0] - 1) / sigma, fa.cal[1], cpu.b, fa.cal[2], cpu.lambda, mean, target);
                W_CHECK(bError <= 2e-3 && lError <= 2e-3, "the GPU model differs from the CPU's: b %.3g, lambda %.3g relative", bError, lError);
                W_CHECK(std::abs(mean - target) <= 2e-3 * target, "the GPU's J_t gives mean F %.6g, target %.6g", mean, target);
            }
        }
        for (uint64_t f = 0; f < 12; ++f) fb = run(gpu, ocean, b, f, 40.0 + f * dt, cx, cz);
        W_CHECK(fa.foam == fb.foam, "two instances differ");
        double covered = 0;
        for (uint32_t i = 0; i < N * N; ++i) covered += half(fa.foam[i]);
        std::printf("determinism: two instances bit-identical after 12 frames; level 0 mean foam %.4f\n", covered / (N * N));
        W_CHECK(covered > 0, "no foam at all on a 16 m/s sea (the source does not fire)");
        {
            // The authored override still applies (FoamUpdate uses p.threshold when it is a number).
            FoamDesc authored = fd;
            authored.threshold = -1e6f;
            Foam o(gpu.device, gpu.shaders, authored);
            const Frame fo = run(gpu, ocean, o, 0, 40.0, cx, cz);
            W_CHECK(std::all_of(fo.foam.begin(), fo.foam.begin() + size_t(N) * N, [](uint16_t h) { return h == 0; }), "an authored J_t below every Jacobian still breaks");
            std::printf("authored override: J_t -1e6 gives no foam\n");
        }

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
        // 8. origin rebase
        {
            FoamDesc rich = fd;
            rich.threshold = 0.6f;  // foam on every level (the coarse levels hold little at the automatic J_t)
            Foam moved(gpu.device, gpu.shaders, rich), fixed(gpu.device, gpu.shaders, rich);
            // The ocean repeats every 1024 m, and so does foam grown from it alone: a 600 m window move first gives the
            // entering band a history of its own (started from the coarser level), so a storage off by a whole period
            // is visible.
            const double px = cx + 600;
            for (uint64_t f = 0; f < 6; ++f)
            {
                run(gpu, ocean, moved, f, 50.0 + f * dt, f < 5 ? cx : px, cz, false, false);
                run(gpu, ocean, fixed, f, 50.0 + f * dt, f < 5 ? cx : px, cz, false, false);
            }
            moved.rebase(1024, -2048);
            const double rx = px - 1024, rz = cz + 2048;
            FoamDesc still = fd;
            still.threshold = -1e6f;
            still.tau = 1e9f;
            moved.setDesc(still);
            fixed.setDesc(still);
            Frame a6 = run(gpu, ocean, moved, 6, 50.0 + 6 * dt, rx, rz, false, true, true), b6 = run(gpu, ocean, fixed, 6, 50.0 + 6 * dt, px, cz, false, true, true);
            W_CHECK(a6.foam == b6.foam, "a rebase changed the stored foam");
            W_CHECK(a6.probe == b6.probe, "the reader sees other foam at the same ground after a rebase");
            moved.setDesc(rich);
            fixed.setDesc(rich);
            for (uint64_t f = 7; f < 24; ++f)  // frames 8 and 16: every level updates
            {
                a6 = run(gpu, ocean, moved, f, 50.0 + f * dt, rx, rz, false, true, f == 23);
                b6 = run(gpu, ocean, fixed, f, 50.0 + f * dt, px, cz, false, true, f == 23);
            }
            double worst = 0;
            std::printf("origin rebase: (1024, -2048) m keeps all %u levels bit-identical (stored and read) over a still frame; 17 frames later, read place by place, per level max |diff| (mean F):", fd.levels);
            for (uint32_t l = 0; l < fd.levels; ++l)
            {
                // Through the reader, place by place (the stored arrays alone cannot tell: the ocean repeats every 1024 m).
                double level = 0, mean = 0;
                for (size_t i = size_t(l) * 65536; i < size_t(l + 1) * 65536; ++i)
                {
                    level = std::max(level, double(std::abs(a6.probe[i] - b6.probe[i])));
                    mean += b6.probe[i];
                }
                std::printf(" %.2e (%.4f)", level, mean / 65536.0);
                worst = std::max(worst, level);
            }
            std::printf("\n");
            W_CHECK(worst <= 2e-3, "the rebased run differs by %.3g after 17 frames", worst);
        }

        // 6 (distribution): an instant lifetime, authored thresholds; level 0's mean breaking fraction against b(t)
        {
            const Frame reference = run(gpu, ocean, a, 20, 40.0, cx, cz);  // the covariances (a's accumulator)
            for (float t : { 0.1f, 0.3f, 0.5f, 0.7f })
            {
                FoamDesc instant = fd;
                instant.tau = 1e-9f;
                instant.threshold = t;
                Foam c(gpu.device, gpu.shaders, instant);
                // 16 windows (positions 150 m apart, times 1.7 s apart); each read on its second frame, where level 0
                // alone updates and its first frame's value (entered texels take the coarser levels' foam) has decayed.
                double mean = 0;
                for (uint32_t j = 0; j < 16; ++j)
                {
                    const double px = cx + 150.0 * (j % 4), pz = cz + 150.0 * (j / 4), time = 40.0 + 1.7 * j;
                    run(gpu, ocean, c, 2 * j, time, px, pz, false, false);
                    const Frame fc = run(gpu, ocean, c, 2 * j + 1, time + dt, px, pz);
                    for (uint32_t i = 0; i < N * N; ++i) mean += half(fc.foam[i]);
                }
                mean /= 16.0 * N * N;
                const Model m = model(reference.cov, t);
                const double sigma = std::sqrt(reference.cov[0] + reference.cov[1] + 2 * reference.cov[3]);
                std::printf("distribution: J_t %.1f: measured breaking fraction %.3e, model b %.3e (%+.1f %%), Gaussian %.3e\n", t, mean, m.b, 100 * (mean / m.b - 1),
                            0.5 * std::erfc(-(t - 1) / sigma / std::sqrt(2.0)));
                W_CHECK(std::abs(mean - m.b) <= 0.25 * m.b + 2e-4, "J_t %.1f: breaking fraction %.4g against the model's %.4g", t, mean, m.b);
            }
        }

        // 7. whitecap coverage
        uint32_t outside = 0;
        if (coverage)
            for (float wind : { 8.0f, 12.0f, 16.0f, 20.0f })
            {
                OceanDesc sd = od;
                sd.windSpeed = wind;
                Ocean sea(gpu.device, gpu.shaders, sd);
                Foam f0(gpu.device, gpu.shaders, fd), f1(gpu.device, gpu.shaders, fd), f2(gpu.device, gpu.shaders, fd), f3(gpu.device, gpu.shaders, fd);
                std::vector<Foam*> foams = { &f0, &f1, &f2, &f3 };
                const double at[4][2] = { { cx, cz }, { cx + 300, cz }, { cx, cz + 300 }, { cx + 300, cz + 300 } };
                const uint64_t warm = uint64_t(coverageSeconds * 60 + 0.5);
                double mean = 0;
                uint32_t samples = 0;
                for (uint64_t i = 0; i <= warm + 240; ++i)
                {
                    const bool read = i >= warm && (i - warm) % 30 == 0;
                    for (double v : coverageFrame(gpu, sea, foams, at, i, 40.0 + double(i) / 60, read)) { mean += v; ++samples; }
                }
                mean /= samples;
                const Frame last = run(gpu, sea, f0, warm + 241, 40.0 + double(warm + 241) / 60, cx, cz);  // the calibration's values
                const double w = monahan(wind);
                std::printf("coverage U %4.1f m/s: mean F %.5f, W %.5f (x%.2f), model %.5f, J_t %.4f, onsets %.4f /s, implied active fraction %.3f\n", wind, mean, w,
                            mean / w, last.cal[3], last.cal[0], last.cal[2], last.cal[1] / last.cal[3]);
                std::fflush(stdout);
                if (!(mean >= 0.5 * w && mean <= 2 * w)) ++outside;
            }
        W_CHECK(outside == 0, "%u sea states outside [W/2, 2W]", outside);
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
