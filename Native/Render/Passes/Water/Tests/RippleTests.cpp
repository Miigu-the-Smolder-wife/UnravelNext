// Track W local ripples correctness (B7; Ripple.hlsli; the first run of new kernels under GpuLock -Kind correctness):
//   1. dispersion: a single standing mode eta = A cos(k.x), phi = 0 evolves as A cos(k.x) cos(w t) with the exact
//      w^2 = k tanh(k d) (g + sigma k^2 / rho) after 300 frames (|d| <= 1e-4 A): a long gravity wave, a short wave
//      where capillarity moves w by 1.5 %, and the same on 0.3 m of water (tanh)
//   2. the whole frame (sources, sponge, window shift, viscous decay, slopes) equals a double-precision reference of
//      the same algorithm over 4 frames (|d| <= 1e-4 of the field's peak)
//   2b. a displaced-volume source changes the water volume by exactly -V (|d| <= 1e-8 m^3 for 1 litre)
//   3. open boundary: a splash at the centre leaves the window through the sponge: after 90 s (the slowest resolved
//      ripple, 10 cm, needs 64 s to reach the edge) the peak |eta| is below 1e-3 of its early peak (no wrap-around)
//   4. two runs are bit-identical
//   unx_test_water_rippletests [--no-debug-layer] [--time]
#include "unx/water/Ripple.h"

#include "unx/render/GpuProfiler.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;
using unx::water::Ripples;
using unx::water::RippleDesc;
using unx::water::RippleSource;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
constexpr uint32_t N = Ripples::kN;
constexpr size_t kTexels = size_t(N) * N;
constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

struct Gpu
{
    Device device;
    ShaderLibrary shaders;
    explicit Gpu(bool debugLayer)
        : device([&] { DeviceOptions o; o.debugLayer = debugLayer; return o; }()), shaders(device, executableDirectory() / "shaders") {}
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

// One frame on the GPU; the field (eta, eta_x, eta_z, phi) read back when `read`.
std::vector<float> step(Gpu& gpu, Ripples& ripples, uint64_t frame, double fx, double fz, float dt, const std::vector<RippleSource>& sources, bool read,
                        GpuProfiler* profiler = nullptr)
{
    RenderGraph g(gpu.device);
    const auto out = ripples.record(g, frame, fx, fz, dt, sources);
    ComPtr<ID3D12Resource> rb;
    const uint64_t pitch = N * 16;
    if (read)
    {
        rb = buffer(gpu.device, pitch * N, D3D12_HEAP_TYPE_READBACK);
        ID3D12Resource* r = rb.Get();
        const auto field = out.field;
        g.addPass("ripple read", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(field, Use::CopySrc); pb.keep(); },
                  [=](PassContext& c) {
                      D3D12_TEXTURE_COPY_LOCATION dst{ r, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                      dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, N, N, 1, UINT(pitch) };
                      D3D12_TEXTURE_COPY_LOCATION src{ c.resource(field), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                      c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                  });
    }
    else
    {
        const auto field = out.field;
        g.addPass("ripple keep", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(field, Use::SrvCompute); pb.keep(); }, [](PassContext&) {});
    }
    if (profiler) profiler->beginFrame(frame);
    g.execute(profiler);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    std::vector<float> f;
    if (read)
    {
        const float* p = nullptr;
        check(rb->Map(0, nullptr, (void**)&p), "map ripple field");
        f.assign(p, p + kTexels * 4);
        rb->Unmap(0, nullptr);
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
struct Reference
{
    RippleDesc d;
    std::vector<double> eta, phi;
    int32_t origin[2] = {};
    bool placed = false;
    explicit Reference(const RippleDesc& desc) : d(desc), eta(kTexels, 0), phi(kTexels, 0) {}
    double k1(uint32_t m) const { const int i = m >= N / 2 ? int(m) - int(N) : int(m); return double(i) * 2 * kPi / (double(N) * d.texel); }
    std::vector<double> frame(double fx, double fz, float dt, const std::vector<RippleSource>& sources)
    {
        const int32_t o[2] = { Ripples::windowOrigin(fx, d.texel), Ripples::windowOrigin(fz, d.texel) };
        int32_t shift[2] = { 0, 0 };
        if (placed) { shift[0] = o[0] - origin[0]; shift[1] = o[1] - origin[1]; }
        origin[0] = o[0]; origin[1] = o[1]; placed = true;
        std::vector<double> e(kTexels, 0), p(kTexels, 0);
        for (uint32_t z = 0; z < N; ++z)
            for (uint32_t x = 0; x < N; ++x)
            {
                const int ox = int(x) + shift[0], oz = int(z) + shift[1];
                if (ox >= 0 && oz >= 0 && ox < int(N) && oz < int(N)) { e[z * N + x] = eta[oz * N + ox]; p[z * N + x] = phi[oz * N + ox]; }
            }
        std::vector<int64_t> accum(kTexels, 0), lower(kTexels, 0);
        for (const auto& s : sources)
        {
            const float px = float(s.x / d.texel - o[0]), pz = float(s.z / d.texel - o[1]);
            const float sigmaF = std::max(s.radius, d.texel);  // the kernel's float arithmetic for the footprint's extent
            const double sigma = sigmaF;
            const int reach = std::min(int(std::ceil(4.0f * sigmaF / d.texel)), 64), width = 2 * reach + 1;
            const int cx = int(std::floor(px + 0.5f)), cz = int(std::floor(pz + 0.5f));
            double sum = 0;  // the footprint's own discrete sum: the impulse is exact
            for (int i = 0; i < width * width; ++i)
            {
                const double dx = (cx + i % width - reach - double(px)) * d.texel, dz = (cz + i / width - reach - double(pz)) * d.texel;
                sum += std::exp(-(dx * dx + dz * dz) / (2 * sigma * sigma));
            }
            const double amplitude = -(double(s.impulse) / 1000.0) / (sum * double(d.texel) * d.texel), lowering = -double(s.volume) / (sum * double(d.texel) * d.texel);
            for (int i = 0; i < width * width; ++i)
            {
                const int tx = cx + i % width - reach, tz = cz + i / width - reach;
                if (tx < 0 || tz < 0 || tx >= int(N) || tz >= int(N)) continue;
                const double dx = (tx - double(px)) * d.texel, dz = (tz - double(pz)) * d.texel;
                const double w = std::exp(-(dx * dx + dz * dz) / (2 * sigma * sigma));
                if (s.impulse != 0) accum[size_t(tz) * N + tx] += std::llround(amplitude * w * 16777216.0);
                if (s.volume != 0) lower[size_t(tz) * N + tx] += std::llround(lowering * w * 16777216.0);
            }
        }
        std::vector<cd> Z(kTexels);
        for (uint32_t z = 0; z < N; ++z)
            for (uint32_t x = 0; x < N; ++x)
            {
                const double edge = double(std::min(std::min(x, N - 1 - x), std::min(z, N - 1 - z)));
                double m = 1;
                if (edge < Ripples::kSponge) { const double ramp = 1 - edge / Ripples::kSponge; m = std::exp(-double(d.spongeRate) * dt * ramp * ramp); }
                const size_t i = size_t(z) * N + x;
                Z[i] = std::conj(cd(e[i] + double(lower[i]) / 16777216.0, p[i] + double(accum[i]) / 16777216.0) * m);
            }
        ifft2(Z);
        for (auto& v : Z) v = std::conj(v) / double(kTexels);
        std::vector<cd> S(kTexels);
        for (uint32_t z = 0; z < N; ++z)
            for (uint32_t x = 0; x < N; ++x)
            {
                const uint32_t mx = (N - x) % N, mz = (N - z) % N;
                const size_t i = size_t(z) * N + x, j = size_t(mz) * N + mx;
                if (j < i) continue;
                if (x == N / 2 || z == N / 2) { Z[i] = Z[j] = S[i] = S[j] = 0; continue; }
                cd H = 0.5 * (Z[i] + std::conj(Z[j])), Ph = (Z[i] - std::conj(Z[j])) / cd(0, 2);
                const double kx = k1(x), kz = k1(z), k = std::sqrt(kx * kx + kz * kz);
                if (k > 0)
                {
                    const double K = d.depth > 0 ? k * std::tanh(k * d.depth) : k, G = d.gravity + double(d.tensionOverDensity) * k * k, w = std::sqrt(K * G);
                    const double c = std::cos(w * dt), s = std::sin(w * dt), decay = std::exp(-2 * double(d.viscosity) * k * k * dt);
                    const cd h2 = (H * c + Ph * (K / w * s)) * decay, p2 = (Ph * c - H * (w / K * s)) * decay;
                    H = h2; Ph = p2;
                }
                else Ph = 0;
                const cd I(0, 1);
                Z[i] = H + I * Ph; S[i] = I * kx * H - kz * H;
                if (j != i) { const cd Hm = std::conj(H), Pm = std::conj(Ph); Z[j] = Hm + I * Pm; S[j] = -I * kx * Hm + kz * Hm; }
            }
        ifft2(Z);
        ifft2(S);
        std::vector<double> out(kTexels * 4);
        for (size_t i = 0; i < kTexels; ++i)
        {
            eta[i] = Z[i].real(); phi[i] = Z[i].imag();
            out[4 * i] = Z[i].real(); out[4 * i + 1] = S[i].real(); out[4 * i + 2] = S[i].imag(); out[4 * i + 3] = Z[i].imag();
        }
        return out;
    }
};

double omega(const RippleDesc& d, double k)
{
    const double K = d.depth > 0 ? k * std::tanh(k * d.depth) : k;
    return std::sqrt(K * (double(d.gravity) + double(d.tensionOverDensity) * k * k));
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, time = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--time") time = true;
        }
        Gpu gpu(debugLayer);
        const float dt = 1.0f / 165;

        if (time)
        {
            Ripples ripples(gpu.device, gpu.shaders, RippleDesc{});
            GpuProfiler profiler(gpu.device, 1, 64);
            std::vector<RippleSource> sources(64);
            for (size_t i = 0; i < sources.size(); ++i) { sources[i].x = float(-3 + 0.1 * double(i)); sources[i].z = 1.5f; sources[i].impulse = 0.5f; }
            std::vector<double> ms;
            for (uint64_t f = 0; f < 72; ++f)
            {
                step(gpu, ripples, f, 0.02 * f, 0, dt, sources, false, &profiler);
                if (f >= 8 && profiler.lastCompleted())
                {
                    double t = 0;
                    for (const auto& p : profiler.lastCompleted()->passes)
                        if (p.name.rfind("ripple ", 0) == 0 && p.name != "ripple keep") t += p.durationMs();
                    ms.push_back(t);
                }
            }
            std::sort(ms.begin(), ms.end());
            std::printf("ripple frame passes (512^2, 64 sources, moving window): median %.4f ms, min %.4f, max %.4f over %zu frames\n", ms[ms.size() / 2], ms.front(), ms.back(), ms.size());
            return 0;
        }

        // 1. exact dispersion of single modes (no sponge, no viscosity)
        auto mode = [&](const char* what, float depth, uint32_t ix, uint32_t iz) {
            RippleDesc d;
            d.spongeRate = 0;
            d.viscosity = 0;
            d.depth = depth;
            Ripples ripples(gpu.device, gpu.shaders, d);
            const double A = 0.02, kx = 2 * kPi * ix / (N * double(d.texel)), kz = 2 * kPi * iz / (N * double(d.texel));
            std::vector<float> state(kTexels * 2, 0.0f);
            for (uint32_t z = 0; z < N; ++z)
                for (uint32_t x = 0; x < N; ++x) state[2 * (z * N + x)] = float(A * std::cos(kx * x * d.texel + kz * z * d.texel));
            ripples.setState(state);
            std::vector<float> f;
            const int frames = 300;
            for (int n = 0; n < frames; ++n) f = step(gpu, ripples, uint64_t(n), 0, 0, dt, {}, n == frames - 1);
            const double w = omega(d, std::sqrt(kx * kx + kz * kz)), wGravity = std::sqrt(std::sqrt(kx * kx + kz * kz) * d.gravity);
            double worst = 0;
            for (uint32_t z = 0; z < N; ++z)
                for (uint32_t x = 0; x < N; ++x)
                    worst = std::max(worst, std::abs(f[4 * (z * N + x)] - A * std::cos(kx * x * d.texel + kz * z * d.texel) * std::cos(w * frames * double(dt))));
            W_CHECK(worst <= 1e-4 * A, "%s: eta differs from A cos(k.x) cos(w t) by %.3g A", what, worst / A);
            std::printf("dispersion %s: |k| %.2f 1/m, w %.4f rad/s (gravity only %.4f, %+.2f %%), after %d frames within %.2e A\n", what, std::sqrt(kx * kx + kz * kz), w, wGravity,
                        100 * (w / wGravity - 1), frames, worst / A);
        };
        mode("long gravity wave", 0, 8, 3);
        mode("short wave (capillarity)", 0, 200, 40);
        mode("0.3 m depth", 0.3f, 6, 0);

        // 2. the whole frame against the double reference
        {
            RippleDesc d;
            Ripples ripples(gpu.device, gpu.shaders, d);
            Reference ref(d);
            std::vector<RippleSource> sources = { { 0.3f, -0.4f, 0.12f, 2.0f, 0 }, { -1.1f, 0.7f, 0.02f, 0.5f, 0 }, { 2.0f, 1.0f, 0.3f, -1.0f, 0 }, { -0.5f, -1.2f, 0.15f, 0, 2e-4f } };
            const double focus[4][2] = { { 0, 0 }, { 0.12, -0.08 }, { 0.37, 0.2 }, { 0.37, 0.2 } };
            double worst = 0, peak = 0;
            for (int n = 0; n < 4; ++n)
            {
                const auto used = n < 2 ? sources : std::vector<RippleSource>{};
                const auto f = step(gpu, ripples, uint64_t(n), focus[n][0], focus[n][1], dt, used, true);
                const auto r = ref.frame(focus[n][0], focus[n][1], dt, used);
                double e = 0, p = 0, ce[4] = {}, cp[4] = {};
                size_t at[4] = {};
                for (size_t i = 0; i < kTexels * 4; ++i)
                {
                    const double err = std::abs(double(f[i]) - r[i]);
                    e = std::max(e, err); p = std::max(p, std::abs(r[i]));
                    if (err > ce[i % 4]) { ce[i % 4] = err; at[i % 4] = i / 4; }
                    cp[i % 4] = std::max(cp[i % 4], std::abs(r[i]));
                }
                for (int c = 0; c < 4; ++c)
                    std::printf("  frame %d channel %d: max error %.3g at (%zu, %zu) (GPU %.6g, reference %.6g), channel peak %.3g\n", n, c, ce[c], at[c] % N, at[c] / N, double(f[4 * at[c] + c]),
                                r[4 * at[c] + c], cp[c]);
                W_CHECK(e <= 1e-4 * p, "frame %d: GPU differs from the double reference by %.3g (peak %.3g)", n, e, p);
                worst = std::max(worst, e / p);
                peak = std::max(peak, p);
            }
            std::printf("frames: 4 frames (3 impulse sources and a volume source, window moved twice, sponge, viscosity) equal the double reference within %.2e of the peak\n", worst);
        }

        // 2b. displaced volume: a body pushing V = 1 litre out of its footprint lowers the mean surface by exactly V
        {
            Ripples ripples(gpu.device, gpu.shaders, RippleDesc{});
            const auto f = step(gpu, ripples, 0, 0, 0, dt, { { 0.4f, -0.3f, 0.1f, 0, 1e-3f } }, true);
            double volume = 0;
            for (size_t i = 0; i < kTexels; ++i) volume += double(f[4 * i]) * 0.05 * 0.05;
            W_CHECK(std::abs(volume + 1e-3) <= 1e-8, "a 1 litre source changed the water volume by %.9g m^3", volume);
            std::printf("volume: a 1 litre source lowers the surface by %.9g m^3 in total (exact: -0.001)\n", volume);
        }

        // 3. open boundary
        {
            Ripples ripples(gpu.device, gpu.shaders, RippleDesc{});
            double early = 0, late = 0;
            // The slowest resolved ripples (wavelength 2 h = 10 cm) travel at their group velocity, about 0.2 m/s: 64 s
            // to the edge from the centre, so the check waits 90 s.
            const int frames = 165 * 90;
            for (int n = 0; n < frames; ++n)
            {
                const bool read = n == 40 || n == frames - 1;
                const auto f = step(gpu, ripples, uint64_t(n), 0, 0, dt, n == 0 ? std::vector<RippleSource>{ { 0, 0, 0.08f, 5.0f } } : std::vector<RippleSource>{}, read);
                if (!read) continue;
                double m = 0;
                for (size_t i = 0; i < kTexels; ++i) m = std::max(m, double(std::abs(f[4 * i])));
                (n == 40 ? early : late) = m;
            }
            W_CHECK(late <= 1e-3 * early, "the splash did not leave the window: peak %.3g m after 90 s, %.3g m at 0.24 s", late, early);
            std::printf("open boundary: peak |eta| %.3g m at 0.24 s, %.3g m after 90 s (%.2e)\n", early, late, late / early);
        }

        // 4. determinism
        {
            std::vector<float> a, b;
            for (int run = 0; run < 2; ++run)
            {
                Ripples ripples(gpu.device, gpu.shaders, RippleDesc{});
                std::vector<float> f;
                for (int n = 0; n < 20; ++n) f = step(gpu, ripples, uint64_t(n), 0.01 * n, 0, dt, { { 0.2f, 0.1f, 0.05f, 1.0f } }, n == 19);
                (run ? b : a) = f;
            }
            W_CHECK(std::memcmp(a.data(), b.data(), a.size() * 4) == 0, "two runs differ");
            std::printf("determinism: two 20-frame runs are bit-identical\n");
        }
        const uint32_t errors = gpu.device.drainDebugMessages();
        W_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("ripple tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
