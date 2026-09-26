// Track W closed basins correctness (W2; Pool.hlsli; the first run of new kernels under GpuLock -Kind correctness):
//   1. basin modes: eta = A cos(m pi x / Lx) cos(n pi z / Lz), phi = 0 evolves as the same mode times cos(w t) with the
//      exact w^2 = k tanh(k d) (g + sigma k^2 / rho), k = pi |(m / Lx, n / Lz)| (the walls' no-flux condition holds
//      for every mode) after 300 frames: eta within 1e-4 A, slopes within 1e-4 A k: the sloshing mode (1, 0) on 0.6 m
//      of water, a short oblique mode (37, 23) where capillarity matters, and the shortest (256, 0) (Nyquist)
//   2. the whole frame (sources near walls and corners folded onto their images, impulse and volume, the uniform
//      term, viscosity, slopes) equals a double-precision reference of the same algorithm over 4 frames (1e-4 of the peak)
//   3. volume: sources that push water out of their footprints leave the basin's water volume (trapezoid sum x hx hz)
//      unchanged (|d| <= 1e-7 m^3 for 50 litres) and lower the surface under them
//   3b. damping: the sloshing mode advanced by 600 s in one step (clean surface) and a short mode by 20 s (clean and
//      inextensible film) keep the mode's shape with amplitude exp(-delta t) cos(w t), delta the boundary-layer formula
//      (Pool.hlsli) in double (1e-3 A)
//   4. lazy evolution: a basin recorded at frame 0 and then at frame 120 equals the one recorded every frame (1e-4 of
//      the peak), and its stream's velocities (the last frame interval) agree likewise
//   5. the triangle stream: 6 x 256^2 vertices, positions and normals from the field on the basin's rotated frame,
//      counter-clockwise seen from above, velocity = (eta - previous eta) / dt
//   6. two runs are bit-identical
//   unx_test_water_pooltests [--no-debug-layer] [--time | --warp]
#include "unx/water/Pool.h"

#include "unx/render/GpuProfiler.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;
using unx::water::Pool;
using unx::water::PoolDesc;
using unx::water::PoolPlacement;
using unx::water::PoolSource;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
constexpr uint32_t N = Pool::kN, Q = Pool::kQ, C = Pool::kCells;
constexpr size_t kSamples = size_t(Q) * Q, kMirror = size_t(N) * N;
constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

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

struct Frame
{
    std::vector<float> field;              // (eta, eta_x, eta_z, phi) per sample, row-major z then x
    std::vector<float> vertices, velocities;  // when the stream is read
    uint32_t drawArgs[4] = {};
};
// One frame on the GPU; the field read back when `read`, the stream too when `stream`.
Frame step(Gpu& gpu, Pool& pool, uint64_t frame, const PoolPlacement& at, double time, float frameDt, const std::vector<PoolSource>& sources, bool read, bool stream = false,
           GpuProfiler* profiler = nullptr)
{
    RenderGraph g(gpu.device);
    const auto out = pool.record(g, frame, at, time, frameDt, sources);
    const uint64_t pitch = (Q * 16 + 255) / 256 * 256;
    const uint64_t vertexBytes = uint64_t(out.stream.maxTriangles) * 3 * 32, velocityBytes = uint64_t(out.stream.maxTriangles) * 3 * 16;
    ComPtr<ID3D12Resource> rb = read ? buffer(gpu.device, pitch * Q, D3D12_HEAP_TYPE_READBACK) : nullptr;
    ComPtr<ID3D12Resource> rbStream = stream ? buffer(gpu.device, vertexBytes + velocityBytes + 16, D3D12_HEAP_TYPE_READBACK) : nullptr;
    ID3D12Resource* r = rb.Get();
    ID3D12Resource* rs = rbStream.Get();
    const auto field = out.field;
    const auto s = out.stream;
    g.addPass("pool read", QueueType::Graphics,
              [&](PassBuilder& pb) {
                  pb.use(field, read ? Use::CopySrc : Use::SrvCompute);
                  for (BufferRef b : { s.vertices, s.velocities, s.drawArgs }) pb.use(b, stream ? Use::CopySrc : Use::SrvCompute);
                  pb.keep();
              },
              [=](PassContext& c) {
                  if (r)
                  {
                      D3D12_TEXTURE_COPY_LOCATION dst{ r, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                      dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, Q, Q, 1, UINT(pitch) };
                      D3D12_TEXTURE_COPY_LOCATION src{ c.resource(field), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                      c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                  }
                  if (rs)
                  {
                      c.cmd->CopyBufferRegion(rs, 0, c.resource(s.vertices), 0, vertexBytes);
                      c.cmd->CopyBufferRegion(rs, vertexBytes, c.resource(s.velocities), 0, velocityBytes);
                      c.cmd->CopyBufferRegion(rs, vertexBytes + velocityBytes, c.resource(s.drawArgs), 0, 16);
                  }
              });
    if (profiler) profiler->beginFrame(frame);
    g.execute(profiler);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    Frame f;
    if (read)
    {
        const uint8_t* p = nullptr;
        check(rb->Map(0, nullptr, (void**)&p), "map pool field");
        f.field.resize(kSamples * 4);
        for (uint32_t z = 0; z < Q; ++z) std::memcpy(&f.field[size_t(z) * Q * 4], p + z * pitch, Q * 16);
        rb->Unmap(0, nullptr);
    }
    if (stream)
    {
        const uint8_t* p = nullptr;
        check(rbStream->Map(0, nullptr, (void**)&p), "map pool stream");
        f.vertices.assign((const float*)p, (const float*)(p + vertexBytes));
        f.velocities.assign((const float*)(p + vertexBytes), (const float*)(p + vertexBytes + velocityBytes));
        std::memcpy(f.drawArgs, p + vertexBytes + velocityBytes, 16);
        rbStream->Unmap(0, nullptr);
    }
    return f;
}

// ---- double-precision reference of the same algorithm ----
void ifft(std::vector<cd>& a)
{
    const size_t n = a.size();
    if (n == 1) return;
    std::vector<cd> even(n / 2), odd(n / 2);
    for (size_t i = 0; i < n / 2; ++i) { even[i] = a[2 * i]; odd[i] = a[2 * i + 1]; }
    ifft(even);
    ifft(odd);
    for (size_t k = 0; k < n / 2; ++k)
    {
        const cd t = std::polar(1.0, 2 * kPi * double(k) / double(n)) * odd[k];
        a[k] = even[k] + t;
        a[k + n / 2] = even[k] - t;
    }
}
void ifft2(std::vector<cd>& a)
{
    std::vector<cd> line(N);
    for (uint32_t z = 0; z < N; ++z) { for (uint32_t x = 0; x < N; ++x) line[x] = a[z * N + x]; ifft(line); for (uint32_t x = 0; x < N; ++x) a[z * N + x] = line[x]; }
    for (uint32_t x = 0; x < N; ++x) { for (uint32_t z = 0; z < N; ++z) line[z] = a[z * N + x]; ifft(line); for (uint32_t z = 0; z < N; ++z) a[z * N + x] = line[z]; }
}
double omegaOf(const PoolDesc& d, double k)
{
    const double K = d.depth > 0 ? k * std::tanh(k * d.depth) : k;
    return std::sqrt(K * (double(d.gravity) + double(d.tensionOverDensity) * k * k));
}
// Amplitude damping rate of the basin mode (kx, kz): bulk and boundary layers (Pool.hlsli), in double.
double damping(const PoolDesc& d, double kx, double kz)
{
    const double k = std::sqrt(kx * kx + kz * kz);
    if (k == 0) return 0;
    const double nu = d.viscosity, D = d.depth, w = omegaOf(d, k), half2k = 0.5 / k;
    const double s = D > 0 ? std::sinh(2 * k * D) : 0, dOverS = D > 0 ? D / s : 0;
    const double ax = (kx != 0 ? 4.0 : 2.0) / d.sizeX, az = (kz != 0 ? 4.0 : 2.0) / d.sizeZ;
    const double layers = (D > 0 ? 2 * k * k / s : 0) + ax * (kz * kz * (dOverS + half2k) + k * k * (half2k - dOverS)) +
                          az * (kx * kx * (dOverS + half2k) + k * k * (half2k - dOverS)) + d.surfaceFilm * k * k * (D > 0 ? 1 / std::tanh(k * D) : 1.0);
    return 2 * nu * k * k + std::sqrt(0.5 * nu * w) * half2k * layers;
}
uint32_t fold(uint32_t i) { return i <= N / 2 ? i : N - i; }
struct Reference
{
    PoolDesc d;
    PoolPlacement at;
    std::vector<double> eta, phi;
    Reference(const PoolDesc& desc, const PoolPlacement& placement) : d(desc), at(placement), eta(kSamples, 0), phi(kSamples, 0) {}
    double hx() const { return double(d.sizeX / C); }
    double hz() const { return double(d.sizeZ / C); }
    double k1(uint32_t m, double h) const { const int i = m >= N / 2 ? int(m) - int(N) : int(m); return double(i) * 2 * kPi / (double(N) * h); }
    std::vector<double> frame(float dt, const std::vector<PoolSource>& sources)
    {
        const float hxF = d.sizeX / C, hzF = d.sizeZ / C;
        std::vector<int64_t> lower(kSamples, 0), accum(kSamples, 0);
        double volumeSum = 0;
        for (const auto& s : sources)
        {
            float px, pz;
            Pool::toSamples(d, at, s.x, s.z, px, pz);
            const float sigmaF = std::max(s.radius, std::max(hxF, hzF));
            const double sigma = sigmaF;
            const int rx = std::min(int(std::ceil(4.0f * sigmaF / hxF)), 64), rz = std::min(int(std::ceil(4.0f * sigmaF / hzF)), 64);
            const int cx = int(std::floor(px + 0.5f)), cz = int(std::floor(pz + 0.5f));
            double sum = 0;
            for (int j = -rz; j <= rz; ++j)
                for (int i = -rx; i <= rx; ++i)
                {
                    const double dx = (cx + i - double(px)) * hxF, dz = (cz + j - double(pz)) * hzF;
                    sum += std::exp(-(dx * dx + dz * dz) / (2 * sigma * sigma));
                }
            const double area = double(hxF) * hzF;
            const double amplitude = -(double(s.impulse) / 1000.0) / (sum * area), lowering = -double(s.volume) / (sum * area);
            const int half = int(N / 2);
            for (int j = -rz; j <= rz; ++j)
                for (int i = -rx; i <= rx; ++i)
                {
                    const int ux = cx + i, uz = cz + j;
                    const double dx = (ux - double(px)) * hxF, dz = (uz - double(pz)) * hzF;
                    double w = std::exp(-(dx * dx + dz * dz) / (2 * sigma * sigma));
                    const int qx = ux < 0 ? -ux : (ux > half ? 2 * half - ux : ux), qz = uz < 0 ? -uz : (uz > half ? 2 * half - uz : uz);
                    if (qx < 0 || qz < 0 || qx > half || qz > half) continue;
                    w *= (qx == 0 || qx == half ? 2.0 : 1.0) * (qz == 0 || qz == half ? 2.0 : 1.0);
                    const size_t q = size_t(qz) * Q + qx;
                    if (s.volume != 0) lower[q] += std::llround(lowering * w * 16777216.0);
                    if (s.impulse != 0) accum[q] += std::llround(amplitude * w * 16777216.0);
                }
            volumeSum += s.volume;
        }
        const double meanShift = double(float(volumeSum / (double(d.sizeX) * d.sizeZ)));  // the host's float root constant
        std::vector<cd> Z(kMirror);
        for (uint32_t z = 0; z < N; ++z)
            for (uint32_t x = 0; x < N; ++x)
            {
                const size_t q = size_t(fold(z)) * Q + fold(x);
                Z[size_t(z) * N + x] = std::conj(cd(eta[q] + double(lower[q]) / 16777216.0 + meanShift, phi[q] + double(accum[q]) / 16777216.0));
            }
        ifft2(Z);
        for (auto& v : Z) v = std::conj(v) / double(kMirror);
        std::vector<cd> S(kMirror);
        for (uint32_t z = 0; z < N; ++z)
            for (uint32_t x = 0; x < N; ++x)
            {
                const uint32_t mx = (N - x) % N, mz = (N - z) % N;
                const size_t i = size_t(z) * N + x, j = size_t(mz) * N + mx;
                if (j < i) continue;
                cd H = 0.5 * (Z[i] + std::conj(Z[j])), Ph = (Z[i] - std::conj(Z[j])) / cd(0, 2);
                const double kx = k1(x, hx()), kz = k1(z, hz()), k = std::sqrt(kx * kx + kz * kz);
                if (k > 0)
                {
                    const double K = d.depth > 0 ? k * std::tanh(k * d.depth) : k, G = d.gravity + double(d.tensionOverDensity) * k * k, w = std::sqrt(K * G);
                    const double c = std::cos(w * dt), s = std::sin(w * dt), decay = std::exp(-damping(d, kx, kz) * dt);
                    const cd h2 = (H * c + Ph * (K / w * s)) * decay, p2 = (Ph * c - H * (w / K * s)) * decay;
                    H = h2; Ph = p2;
                }
                else Ph = 0;
                const double ksx = x == N / 2 ? 0 : kx, ksz = z == N / 2 ? 0 : kz;
                const cd I(0, 1);
                Z[i] = H + I * Ph; S[i] = I * ksx * H - ksz * H;
                if (j != i) { const cd Hm = std::conj(H), Pm = std::conj(Ph); Z[j] = Hm + I * Pm; S[j] = -I * ksx * Hm + ksz * Hm; }
            }
        ifft2(Z);
        ifft2(S);
        std::vector<double> out(kSamples * 4);
        for (uint32_t z = 0; z < Q; ++z)
            for (uint32_t x = 0; x < Q; ++x)
            {
                const size_t m = size_t(z) * N + x, q = size_t(z) * Q + x;
                eta[q] = Z[m].real(); phi[q] = Z[m].imag();
                out[4 * q] = Z[m].real(); out[4 * q + 1] = S[m].real(); out[4 * q + 2] = S[m].imag(); out[4 * q + 3] = Z[m].imag();
            }
        return out;
    }
};

double omega(const PoolDesc& d, double k)
{
    const double K = d.depth > 0 ? k * std::tanh(k * d.depth) : k;
    return std::sqrt(K * (double(d.gravity) + double(d.tensionOverDensity) * k * k));
}
double trapezoid(const std::vector<float>& field, uint32_t channel)
{
    double sum = 0;
    for (uint32_t z = 0; z < Q; ++z)
        for (uint32_t x = 0; x < Q; ++x)
            sum += double(field[4 * (size_t(z) * Q + x) + channel]) * (x == 0 || x == C ? 0.5 : 1.0) * (z == 0 || z == C ? 0.5 : 1.0);
    return sum;
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
        const float dt = 1.0f / 165;
        PoolPlacement at;  // a 4 x 3 m bath turned by 30 degrees, still level 0.45 m
        at.centre[0] = 2.5; at.centre[1] = 0.45; at.centre[2] = -1.0;
        at.yaw = float(kPi / 6);
        PoolDesc bath;
        bath.sizeX = 4.0f; bath.sizeZ = 3.0f; bath.depth = 0.6f;

        if (time)
        {
            Pool pool(gpu.device, gpu.shaders, bath);
            GpuProfiler profiler(gpu.device, 1, 64);
            std::vector<PoolSource> sources;
            for (int i = 0; i < 64; ++i)
            {
                PoolSource s;
                s.x = at.centre[0] + 0.02 * (i - 32); s.z = at.centre[2] + 0.3; s.radius = 0.06f; s.impulse = 0.5f; s.volume = 1e-4f;
                sources.push_back(s);
            }
            std::vector<double> ms, msMesh, msQuiet;
            for (uint64_t f = 0; f < 136; ++f)
            {
                const bool quiet = f >= 72;  // the second half: frames without sources (no forward transform)
                step(gpu, pool, f, at, f * double(dt), dt, quiet ? std::vector<PoolSource>{} : sources, false, false, &profiler);
                if (f >= 8 && profiler.lastCompleted())
                {
                    double t = 0, m = 0;
                    for (const auto& p : profiler.lastCompleted()->passes)
                    {
                        if (p.name == "pool surface") m += p.durationMs();
                        else if (p.name.rfind("pool ", 0) == 0 && p.name != "pool read") t += p.durationMs();
                    }
                    if (f >= 80) msQuiet.push_back(t);
                    else if (f < 72) { ms.push_back(t); msMesh.push_back(m); }
                }
            }
            std::sort(ms.begin(), ms.end());
            std::sort(msMesh.begin(), msMesh.end());
            std::sort(msQuiet.begin(), msQuiet.end());
            std::printf("pool evolution passes without sources (evolve, inverse): median %.4f ms, min %.4f, max %.4f over %zu frames\n", msQuiet[msQuiet.size() / 2], msQuiet.front(),
                        msQuiet.back(), msQuiet.size());
            std::printf("pool evolution passes (257^2 basin, 512^2 mirrored FFT, 64 sources): median %.4f ms, min %.4f, max %.4f over %zu frames\n", ms[ms.size() / 2], ms.front(),
                        ms.back(), ms.size());
            std::printf("pool surface stream (393,216 vertices): median %.4f ms, min %.4f, max %.4f\n", msMesh[msMesh.size() / 2], msMesh.front(), msMesh.back());
            return 0;
        }

        // 1. basin modes (no viscosity). The slopes are checked by their projection on the mode's slope shape (the
        // mode's amplitude); the residual (every other mode: float rounding of the round trip, which the derivative
        // multiplies by up to the Nyquist wavenumber) is reported against the frame count.
        auto mode = [&](const char* what, float depth, uint32_t m, uint32_t n, double A, int frames) {
            PoolDesc d = bath;
            d.viscosity = 0;
            d.depth = depth;
            Pool pool(gpu.device, gpu.shaders, d);
            const double hx = double(d.sizeX / C), hz = double(d.sizeZ / C);
            const double kx = kPi * m / d.sizeX, kz = kPi * n / d.sizeZ;
            std::vector<float> state(kSamples * 2, 0.0f);
            for (uint32_t z = 0; z < Q; ++z)
                for (uint32_t x = 0; x < Q; ++x) state[2 * (size_t(z) * Q + x)] = float(A * std::cos(kx * x * hx) * std::cos(kz * z * hz));
            pool.setState(state);
            Frame f;
            for (int i = 0; i < frames; ++i) f = step(gpu, pool, uint64_t(i), at, i * double(dt), dt, {}, i == frames - 1);
            const double k = std::sqrt(kx * kx + kz * kz), w = omega(d, k), T = (frames - 1) * double(dt), ct = std::cos(w * T);
            double worst = 0, fs[2] = {}, ss[2] = {};
            for (uint32_t z = 0; z < Q; ++z)
                for (uint32_t x = 0; x < Q; ++x)
                {
                    const size_t q = size_t(z) * Q + x;
                    const double cx = std::cos(kx * x * hx), cz = std::cos(kz * z * hz), sx = std::sin(kx * x * hx), sz = std::sin(kz * z * hz);
                    worst = std::max(worst, std::abs(f.field[4 * q] - A * cx * cz * ct));
                    const double shape[2] = { -kx * sx * cz, -kz * cx * sz };
                    for (int a = 0; a < 2; ++a) { fs[a] += f.field[4 * q + 1 + a] * shape[a]; ss[a] += shape[a] * shape[a]; }
                }
            double amp[2] = {}, residual = 0;
            for (int a = 0; a < 2; ++a) amp[a] = ss[a] > 1e-12 ? fs[a] / ss[a] : A * ct;  // no slope along an axis at m = 0 or Nyquist
            for (uint32_t z = 0; z < Q; ++z)
                for (uint32_t x = 0; x < Q; ++x)
                {
                    const size_t q = size_t(z) * Q + x;
                    const double cx = std::cos(kx * x * hx), cz = std::cos(kz * z * hz), sx = std::sin(kx * x * hx), sz = std::sin(kz * z * hz);
                    residual = std::max({ residual, std::abs(f.field[4 * q + 1] - amp[0] * -kx * sx * cz), std::abs(f.field[4 * q + 2] - amp[1] * -kz * cx * sz) });
                }
            const double ampErr = std::max(std::abs(amp[0] - A * ct), std::abs(amp[1] - A * ct));
            W_CHECK(worst <= 1e-4 * A, "%s: eta differs from the mode by %.3g A", what, worst / A);
            W_CHECK(ampErr <= 1e-4 * A, "%s: the slopes' mode amplitude differs by %.3g A", what, ampErr / A);
            const double kNyquist = kPi * C * std::sqrt(1 / (double(d.sizeX) * d.sizeX) + 1 / (double(d.sizeZ) * d.sizeZ));
            std::printf("mode %s (%u, %u): k %.2f 1/m, w %.4f rad/s, period %.4f s; after %d frames eta within %.2e A, slope amplitude within %.2e A, slope residual %.2e A k "
                        "(= %.2e A k_Nyquist)\n",
                        what, m, n, k, w, 2 * kPi / w, frames, worst / A, ampErr / A, residual / (A * k), residual / (A * kNyquist));
            return residual / (A * kNyquist);
        };
        const double r10 = mode("sloshing", 0.6f, 1, 0, 0.02, 10), r300 = mode("sloshing", 0.6f, 1, 0, 0.02, 300);
        W_CHECK(r300 <= 4 * r10, "the slope residual grows with the frame count (%.2e after 10, %.2e after 300 frames): rounding feeds back into the state", r10, r300);
        std::printf("  slope residual after 10 and 300 frames: %.2e, %.2e A k_Nyquist (the output transform's rounding; the spectral state does not accumulate it)\n", r10, r300);
        mode("short oblique (capillarity)", 0.6f, 37, 23, 0.002, 300);
        mode("shortest (Nyquist)", 0.6f, 256, 0, 1e-4, 300);

        // 2. the whole frame against the double reference
        {
            Pool pool(gpu.device, gpu.shaders, bath);
            Reference ref(bath, at);
            // local (u, v) in samples -> world
            auto world = [&](double u, double v, double& x, double& z) {
                const double lx = u * bath.sizeX / C - 0.5 * bath.sizeX, lz = v * bath.sizeZ / C - 0.5 * bath.sizeZ;
                const double c = std::cos(double(at.yaw)), s = std::sin(double(at.yaw));
                x = at.centre[0] + lx * c + lz * s;
                z = at.centre[2] - lx * s + lz * c;
            };
            std::vector<PoolSource> sources;
            const double spots[5][5] = { { 128.3, 97.6, 0.12, 2.0, 0 },       // centre, impulse
                                         { 3.2, 140.1, 0.05, 0.8, 3e-4 },      // near the x = 0 wall: folded
                                         { 251.7, 4.4, 0.08, -1.0, 2e-4 },     // near a corner: folded on both axes
                                         { 60.0, 256.0, 0.03, 0.5, 0 },        // on the z = Lz wall
                                         { 180.0, 30.0, 0.2, 0, 5e-3 } };      // a wide volume source
            for (const auto& p : spots)
            {
                PoolSource s;
                world(p[0], p[1], s.x, s.z);
                s.radius = float(p[2]); s.impulse = float(p[3]); s.volume = float(p[4]);
                sources.push_back(s);
            }
            double worst = 0;
            for (int n = 0; n < 4; ++n)
            {
                const auto used = n < 2 ? sources : std::vector<PoolSource>{};
                const auto f = step(gpu, pool, uint64_t(n), at, n * double(dt), dt, used, true);
                const auto r = ref.frame(n == 0 ? 0.0f : dt, used);
                double e = 0, p = 0, ce[4] = {}, cp[4] = {};
                size_t where[4] = {};
                for (size_t i = 0; i < kSamples * 4; ++i)
                {
                    const double err = std::abs(double(f.field[i]) - r[i]);
                    e = std::max(e, err); p = std::max(p, std::abs(r[i]));
                    if (err > ce[i % 4]) { ce[i % 4] = err; where[i % 4] = i / 4; }
                    cp[i % 4] = std::max(cp[i % 4], std::abs(r[i]));
                }
                for (int c = 0; c < 4; ++c)
                    std::printf("  frame %d channel %d: max error %.3g at (%zu, %zu) (GPU %.6g, reference %.6g), channel peak %.3g\n", n, c, ce[c], where[c] % Q, where[c] / Q,
                                double(f.field[4 * where[c] + c]), r[4 * where[c] + c], cp[c]);
                W_CHECK(e <= 1e-4 * p, "frame %d: GPU differs from the double reference by %.3g (peak %.3g)", n, e, p);
                worst = std::max(worst, e / p);
            }
            std::printf("frames: 4 frames (5 sources: centre, wall, corner, on a wall, wide volume; viscosity) equal the double reference within %.2e of the peak\n", worst);
        }

        // 3. volume conservation
        {
            Pool pool(gpu.device, gpu.shaders, bath);
            std::vector<PoolSource> sources;
            for (int i = 0; i < 10; ++i)
            {
                PoolSource s;
                s.x = at.centre[0] + 0.15 * (i - 5); s.z = at.centre[2] + 0.1 * (i % 3); s.radius = 0.15f; s.volume = 5e-3f;  // 10 x 5 litres
                sources.push_back(s);
            }
            const auto f0 = step(gpu, pool, 0, at, 0, dt, sources, true);
            Frame f;
            for (int n = 1; n <= 200; ++n) f = step(gpu, pool, uint64_t(n), at, n * double(dt), dt, {}, n == 200);
            const double area = double(bath.sizeX / C) * double(bath.sizeZ / C);
            const double v0 = trapezoid(f0.field, 0) * area, v1 = trapezoid(f.field, 0) * area;
            float u, v;
            Pool::toSamples(bath, at, sources[5].x, sources[5].z, u, v);
            const double under = f0.field[4 * (size_t(std::lround(v)) * Q + size_t(std::lround(u)))];
            W_CHECK(std::abs(v0) <= 1e-7 && std::abs(v1) <= 1e-7, "50 litres of displaced water changed the basin's volume by %.3g m^3 (frame 0) and %.3g m^3 (frame 200)", v0, v1);
            W_CHECK(under < 0, "the surface under a body is not lowered (%.3g m)", under);
            std::printf("volume: 50 litres pushed out of 10 footprints: basin volume change %.3g m^3 at once, %.3g m^3 after 200 frames; surface under a body %.4g m\n", v0, v1, under);
        }

        // 3b. damping
        {
            auto decayCase = [&](const char* what, float film, uint32_t m, uint32_t n, double A, double seconds) {
                PoolDesc d = bath;
                d.surfaceFilm = film;
                Pool pool(gpu.device, gpu.shaders, d);
                const double hx = double(d.sizeX / C), hz = double(d.sizeZ / C), kx = kPi * m / d.sizeX, kz = kPi * n / d.sizeZ;
                std::vector<float> state(kSamples * 2, 0.0f);
                for (uint32_t z = 0; z < Q; ++z)
                    for (uint32_t x = 0; x < Q; ++x) state[2 * (size_t(z) * Q + x)] = float(A * std::cos(kx * x * hx) * std::cos(kz * z * hz));
                pool.setState(state);
                step(gpu, pool, 0, at, 0, dt, {}, false);
                const Frame f = step(gpu, pool, 1, at, seconds, dt, {}, true);  // one exact step (lazy evolution: seconds - dt, then dt)
                const double k = std::sqrt(kx * kx + kz * kz), delta = damping(d, kx, kz), w = omegaOf(d, k);
                // The GPU's phase: float(seconds - dt) and float(dt) steps of w (float).
                const double phase = double(float(w) * float(seconds - double(dt))) + double(float(w) * dt);
                const double amp = A * std::exp(-delta * seconds) * std::cos(phase);
                double num = 0, den = 0, worst = 0;
                for (uint32_t z = 0; z < Q; ++z)
                    for (uint32_t x = 0; x < Q; ++x)
                    {
                        const double shape = std::cos(kx * x * hx) * std::cos(kz * z * hz), v = f.field[4 * (size_t(z) * Q + x)];
                        num += v * shape; den += shape * shape;
                        worst = std::max(worst, std::abs(v - amp * shape));
                    }
                W_CHECK(worst <= 1e-3 * A, "%s: after %.0f s the mode differs from A exp(-delta t) cos(w t) by %.3g A (amplitude %.6g, expected %.6g)", what, seconds, worst / A, num / den / A,
                        amp / A);
                std::printf("damping %s (%u, %u): delta %.4g 1/s (bulk alone %.3g), after %.0f s amplitude %.6f A (exp(-delta t) %.6f, cos %.6f), within %.2e A\n", what, m, n, delta,
                            2 * double(d.viscosity) * k * k, seconds, num / den / A, std::exp(-delta * seconds), std::cos(phase), worst / A);
            };
            decayCase("sloshing, clean", 0, 1, 0, 0.02, 600.0);
            decayCase("short ripple, clean", 0, 60, 40, 0.001, 20.0);
            decayCase("short ripple, inextensible film", 1, 60, 40, 0.001, 20.0);
        }

        // 4. lazy evolution
        {
            PoolSource s;
            s.x = at.centre[0] + 0.4; s.z = at.centre[2] - 0.2; s.radius = 0.08f; s.impulse = 3.0f; s.volume = 1e-3f;
            Pool every(gpu.device, gpu.shaders, bath), lazy(gpu.device, gpu.shaders, bath);
            Frame a, b;
            const int last = 120;
            for (int n = 0; n <= last; ++n) a = step(gpu, every, uint64_t(n), at, n * double(dt), dt, n == 0 ? std::vector<PoolSource>{ s } : std::vector<PoolSource>{}, n == last, n == last);
            step(gpu, lazy, 0, at, 0, dt, { s }, false);
            b = step(gpu, lazy, 1, at, last * double(dt), dt, {}, true, true);
            double e = 0, p = 0, ev = 0, pv = 0;
            for (size_t i = 0; i < kSamples * 4; ++i) { e = std::max(e, double(std::abs(a.field[i] - b.field[i]))); p = std::max(p, double(std::abs(a.field[i]))); }
            for (size_t i = 1; i < a.velocities.size(); i += 4) { ev = std::max(ev, double(std::abs(a.velocities[i] - b.velocities[i]))); pv = std::max(pv, double(std::abs(a.velocities[i]))); }
            W_CHECK(e <= 1e-4 * p, "lazy evolution differs from per-frame evolution by %.3g (peak %.3g)", e, p);
            W_CHECK(ev <= 1e-3 * pv, "lazy evolution's velocities differ by %.3g (peak %.3g m/s)", ev, pv);
            std::printf("lazy: recorded at frames 0 and %d equals every frame within %.2e of the peak; velocities within %.2e of %.3g m/s\n", last, e / p, ev / pv, pv);
        }

        // 5. the triangle stream
        {
            Pool pool(gpu.device, gpu.shaders, bath);
            PoolSource s;
            s.x = at.centre[0]; s.z = at.centre[2]; s.radius = 0.1f; s.impulse = 4.0f;
            Frame prev, f;
            for (int n = 0; n < 30; ++n) { prev = f; f = step(gpu, pool, uint64_t(n), at, n * double(dt), dt, n == 0 ? std::vector<PoolSource>{ s } : std::vector<PoolSource>{}, n >= 28, n == 29); }
            W_CHECK(f.drawArgs[0] == 6 * C * C && f.drawArgs[1] == 1, "draw arguments %u x %u", f.drawArgs[0], f.drawArgs[1]);
            const double c = std::cos(double(at.yaw)), sn = std::sin(double(at.yaw)), hx = double(bath.sizeX / C), hz = double(bath.sizeZ / C);
            double worstP = 0, worstN = 0, worstV = 0, peakV = 0;
            uint32_t wrongWinding = 0;
            for (uint32_t cell = 0; cell < C * C; ++cell)
            {
                const uint32_t i = cell % C, j = cell / C;
                const uint32_t corner[6][2] = { { i, j }, { i, j + 1 }, { i + 1, j }, { i + 1, j }, { i, j + 1 }, { i + 1, j + 1 } };
                for (int v = 0; v < 6; ++v)
                {
                    const size_t at6 = size_t(6) * cell + v, q = size_t(corner[v][1]) * Q + corner[v][0];
                    const float* vert = &f.vertices[8 * at6];
                    const double lx = corner[v][0] * hx - 0.5 * bath.sizeX, lz = corner[v][1] * hz - 0.5 * bath.sizeZ;
                    const double px = at.centre[0] + lx * c + lz * sn, pz = at.centre[2] - lx * sn + lz * c, py = at.centre[1] + f.field[4 * q];
                    worstP = std::max({ worstP, std::abs(vert[0] - px), std::abs(vert[1] - py), std::abs(vert[2] - pz) });
                    const double ex = f.field[4 * q + 1], ez = f.field[4 * q + 2];
                    double n[3] = { -ex * c - ez * sn, 1, ex * sn - ez * c };
                    const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    for (int a = 0; a < 3; ++a) worstN = std::max(worstN, std::abs(vert[4 + a] - n[a] / len));
                    W_CHECK(vert[3] == 1.0f && vert[7] == 0.0f, "vertex %zu: w components %g, %g", at6, vert[3], vert[7]);
                    const double vy = (double(f.field[4 * q]) - double(prev.field[4 * q])) / dt;
                    worstV = std::max(worstV, std::abs(f.velocities[4 * at6 + 1] - vy));
                    peakV = std::max(peakV, std::abs(vy));
                }
                for (int t = 0; t < 2; ++t)
                {
                    const float* a = &f.vertices[8 * (6 * size_t(cell) + 3 * t)];
                    const float* b = a + 8;
                    const float* d = a + 16;
                    const double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] };
                    if (e1[2] * e2[0] - e1[0] * e2[2] <= 0) ++wrongWinding;  // (e1 x e2).y
                }
            }
            W_CHECK(worstP <= 1e-5, "vertex positions differ from the field by %.3g m", worstP);
            W_CHECK(worstN <= 1e-5, "vertex normals differ from the field's by %.3g", worstN);
            W_CHECK(wrongWinding == 0, "%u triangles are not counter-clockwise seen from above", wrongWinding);
            W_CHECK(worstV <= 1e-5 * std::max(1.0, peakV) + 1e-6, "velocities differ from (eta - previous) / dt by %.3g (peak %.3g m/s)", worstV, peakV);
            std::printf("stream: %u vertices; positions within %.2e m, normals within %.2e, all triangles counter-clockwise from above, velocities within %.2e (peak %.3g m/s)\n",
                        f.drawArgs[0], worstP, worstN, worstV, peakV);
        }

        // 6. determinism
        {
            std::vector<float> a, b;
            for (int run = 0; run < 2; ++run)
            {
                Pool pool(gpu.device, gpu.shaders, bath);
                PoolSource s;
                s.x = at.centre[0] + 0.3; s.z = at.centre[2]; s.radius = 0.05f; s.impulse = 1.0f; s.volume = 2e-4f;
                Frame f;
                for (int n = 0; n < 20; ++n) f = step(gpu, pool, uint64_t(n), at, n * double(dt), dt, { s }, n == 19);
                (run ? b : a) = f.field;
            }
            W_CHECK(std::memcmp(a.data(), b.data(), a.size() * 4) == 0, "two runs differ");
            std::printf("determinism: two 20-frame runs are bit-identical\n");
        }
        std::printf("pool tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
}
