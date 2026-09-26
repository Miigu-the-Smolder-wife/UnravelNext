// Track W FFT ocean correctness (B7; Ocean.hlsli; the first run of new kernels under GpuLock -Kind correctness):
//   1. single mode: an explicit spectrum with one wave (k along +x, cascade 0) gives h = A cos(k x - w t + phi),
//      Dx = -A sin(...) (the deep-water orbit: crests sharpen), dh/dx, dDx/dx and J = 1 + dDx/dx in closed form at
//      t = 0 and t = 37.3 s (|d| <= 1e-5 A), zero on the other channels and cascades; the crest travels along +k
//   2. sea state: the GPU initial spectrum equals a double-precision replica of the same hash, Box-Muller and JONSWAP
//      spreading per bin; each pair holds conj(h0(-k)) bit-exactly (the field is real); the expected surface variance
//      of the band (sum of E|h(k, t)|^2, with E|h0|^2 = E(k) dk^2 / 2) equals the integral of S(w) over the band
//      (E(k) = S(w) D (dw/dk) / k, within 2 %), and the realised surface's mean h^2 equals it within 5 standard
//      deviations of the realisation spread (a factor-2 amplitude convention error fails this)
//   3. fields: all 8 fields of all 3 cascades equal a double-precision CPU FFT of the same spectrum (independent code;
//      each field's inverse transform is real to 1e-9 of its rms: the packing's Hermitian pairs), max |d| <= 4e-5 rms
//      (float FFT with the host's exact twiddles; hardware sin/cos twiddles measured 6.4e-5)
//   4. time: the field at t and t + T (T = 4096 s) is equal (the integer phase clock), and at t = 1 h the fields still
//      equal the double reference (no float phase loss); two runs are bit-identical; each record's previous
//      displacement is the last record's (every mip, bit for bit), invalid after a new sea state
//   5. mips: every level of both textures is the mean of its 2 x 2 children (within the roundings of the two 4-term sums)
//   unx_test_water_oceantests [--no-debug-layer] [--time]   (--time: 64 frames, median GPU time of the frame passes)
#include "unx/water/Ocean.h"

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
using unx::water::Ocean;
using unx::water::OceanDesc;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
constexpr uint32_t N = Ocean::kN, C = Ocean::kCascades;
constexpr uint64_t kBins = uint64_t(C) * N * N;
constexpr double kPi = 3.14159265358979323846, kG = 9.81;
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

// The module's three textures read back with every mip: 0 displacement (Dx, h, Dz, dDx/dz), 1 slopes (dh/dx, dh/dz,
// dDx/dx, dDz/dz), 2 the previous displacement. Channel ch (0..7) of cascade c at mip 0 = texture ch / 4, component ch % 4.
constexpr uint32_t kMips = Ocean::kMips;
uint64_t texelIndex(uint32_t mip, uint32_t c, uint32_t x, uint32_t z)
{
    uint64_t base = 0;
    for (uint32_t m = 0; m < mip; ++m) base += uint64_t(C) * (N >> m) * (N >> m);
    const uint32_t n = N >> mip;
    return (base + (uint64_t(c) * n + z) * n + x) * 4;
}
struct Frame
{
    std::vector<float> tex[3];
    std::vector<float> h0;  // [bin][4]
    bool previousValid = false;
    float at(uint32_t c, uint32_t ch, uint32_t x, uint32_t z) const { return tex[ch / 4][texelIndex(0, c, x, z) + ch % 4]; }
};

Frame run(Gpu& gpu, Ocean& ocean, double seconds, GpuProfiler* profiler = nullptr, uint64_t frame = 0)
{
    RenderGraph g(gpu.device);
    const auto out = ocean.record(g, seconds);
    Frame f;
    f.previousValid = out.previousValid;
    constexpr uint32_t kSubresources = kMips * C;
    ComPtr<ID3D12Resource> readTex[3], readH0 = buffer(gpu.device, kBins * 16, D3D12_HEAP_TYPE_READBACK);
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp(kSubresources);
    const TextureRef textures[3] = { out.displacement, out.slopes, out.previousDisplacement };
    if (!profiler)
    {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = td.Height = N;
        td.DepthOrArraySize = C;
        td.MipLevels = kMips;
        td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.SampleDesc.Count = 1;
        std::vector<UINT> rows(kSubresources);
        std::vector<UINT64> rowBytes(kSubresources);
        UINT64 total = 0;
        gpu.device.d3d()->GetCopyableFootprints(&td, 0, kSubresources, 0, fp.data(), rows.data(), rowBytes.data(), &total);
        ID3D12Resource* rt[3];
        for (uint32_t t = 0; t < 3; ++t) { readTex[t] = buffer(gpu.device, total, D3D12_HEAP_TYPE_READBACK); rt[t] = readTex[t].Get(); }
        ID3D12Resource* rh = readH0.Get();
        const auto h0 = out.h0;
        const bool withPrevious = out.previousValid;
        g.addPass("ocean read", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      for (uint32_t t = 0; t < (withPrevious ? 3u : 2u); ++t) pb.use(textures[t], Use::CopySrc);
                      pb.use(h0, Use::CopySrc);
                      pb.keep();
                  },
                  [=](PassContext& c) {
                      for (uint32_t t = 0; t < (withPrevious ? 3u : 2u); ++t)
                          for (uint32_t s = 0; s < kSubresources; ++s)
                          {
                              D3D12_TEXTURE_COPY_LOCATION dst{ rt[t], D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                              dst.PlacedFootprint = fp[s];
                              D3D12_TEXTURE_COPY_LOCATION src{ c.resource(textures[t]), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                              src.SubresourceIndex = s;
                              c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                          }
                      c.cmd->CopyBufferRegion(rh, 0, c.resource(h0), 0, kBins * 16);
                  });
    }
    else
    {
        g.addPass("ocean keep", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(textures[0], Use::SrvCompute); pb.use(textures[1], Use::SrvCompute); pb.keep(); }, [](PassContext&) {});
        profiler->beginFrame(frame);
    }
    g.execute(profiler);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    if (profiler) return f;
    for (uint32_t t = 0; t < (out.previousValid ? 3u : 2u); ++t)
    {
        const uint8_t* p = nullptr;
        check(readTex[t]->Map(0, nullptr, (void**)&p), "map ocean texture");
        f.tex[t].resize(texelIndex(kMips, 0, 0, 0));
        // Subresource index = mip + slice x mips.
        for (uint32_t c = 0; c < C; ++c)
            for (uint32_t m = 0; m < kMips; ++m)
            {
                const auto& footprint = fp[m + c * kMips];
                const uint32_t n = N >> m;
                for (uint32_t z = 0; z < n; ++z) std::memcpy(&f.tex[t][texelIndex(m, c, 0, z)], p + footprint.Offset + uint64_t(z) * footprint.Footprint.RowPitch, n * 16);
            }
        readTex[t]->Unmap(0, nullptr);
    }
    const float* h = nullptr;
    check(readH0->Map(0, nullptr, (void**)&h), "map h0");
    f.h0.assign(h, h + kBins * 4);
    readH0->Unmap(0, nullptr);
    return f;
}

// Every mip level of both textures is the mean of its 2 x 2 children (within the rounding of a 4-term float sum).
uint64_t checkMips(const Frame& f)
{
    uint64_t checked = 0;
    for (uint32_t t = 0; t < 2; ++t)
        for (uint32_t m = 1; m < kMips; ++m)
            for (uint32_t c = 0; c < C; ++c)
                for (uint32_t z = 0; z < (N >> m); ++z)
                    for (uint32_t x = 0; x < (N >> m); ++x)
                        for (uint32_t k = 0; k < 4; ++k)
                        {
                            auto child = [&](uint32_t dx, uint32_t dz) { return f.tex[t][texelIndex(m - 1, c, 2 * x + dx, 2 * z + dz) + k]; };
                            const float expect = ((child(0, 0) + child(1, 0)) + (child(0, 1) + child(1, 1))) * 0.25f;
                            const float got = f.tex[t][texelIndex(m, c, x, z) + k];
                            // HLSL leaves the order of the float additions to the compiler: 3 roundings of each of the two sums
                            const double bound = 6 * 5.97e-8 * 0.25 * (std::abs(child(0, 0)) + std::abs(child(1, 0)) + std::abs(child(0, 1)) + std::abs(child(1, 1)));
                            W_CHECK(std::abs(double(got) - double(expect)) <= bound, "texture %u mip %u cascade %u texel (%u, %u).%u: %.9g, mean of children %.9g", t, m, c, x, z, k, got, expect);
                            ++checked;
                        }
    return checked;
}

// Double-precision replica of OceanSpectrum.hlsl.
uint32_t hashU(uint32_t v)
{
    const uint32_t s = v * 747796405u + 2891336453u;
    const uint32_t w = ((s >> ((s >> 28u) + 4u)) ^ s) * 277803737u;
    return (w >> 22u) ^ w;
}
double uniformU(uint32_t h) { return (double(h >> 9) + 0.5) / 8388608.0; }
cd gaussian(uint32_t seed, uint32_t cascade, uint32_t bin)
{
    const uint32_t a = hashU(seed ^ hashU(cascade * 0x9E3779B9u ^ hashU(bin))), b = hashU(a ^ 0x68E31DA4u);
    return std::polar(std::sqrt(-2.0 * std::log(uniformU(a))), 2.0 * kPi * uniformU(b));
}
double jonswap(const OceanDesc& d, double omega)
{
    const double U = d.windSpeed, F = d.fetch, g = kG;
    const double alpha = 0.076 * std::pow(U * U / (F * g), 0.22), omegaP = 22.0 * std::pow(g * g / (U * F), 1.0 / 3.0);
    const double sigma = omega <= omegaP ? 0.07 : 0.09, e = (omega - omegaP) / (sigma * omegaP);
    return alpha * g * g / std::pow(omega, 5) * std::exp(-1.25 * std::pow(omegaP / omega, 4)) * std::pow(3.3, std::exp(-0.5 * e * e));
}
double variance(const OceanDesc& d, double kx, double kz, uint32_t cascade)
{
    const double k = std::sqrt(kx * kx + kz * kz);
    const double lo = cascade == 0 ? 0 : d.bands[cascade - 1], hi = cascade == 2 ? 1e30 : d.bands[cascade];
    if (!(k > 0) || k < lo || k >= hi) return 0;
    const double omega = std::sqrt(kG * k), s = d.spread;
    const double norm = std::exp(std::lgamma(s + 1) - std::lgamma(s + 0.5)) / (2 * std::sqrt(kPi));
    const double hx = kx / k + (float)std::cos(double(d.windDirection)), hz = kz / k + (float)std::sin(double(d.windDirection));  // the host's float wind vector
    const double D = norm * std::pow(0.25 * (hx * hx + hz * hz), s);
    const double dk = 2 * kPi / d.lengths[cascade];
    return jonswap(d, omega) * D * (kG / (2 * omega)) / k * dk * dk;
}
// cos^2(theta / 2) of the spreading factor for wavevector (kx, kz).
double halfCos2(const OceanDesc& d, double kx, double kz)
{
    const double k = std::sqrt(kx * kx + kz * kz), hx = kx / k + (float)std::cos(double(d.windDirection)), hz = kz / k + (float)std::sin(double(d.windDirection));
    return 0.25 * (hx * hx + hz * hz);
}
double kOf(uint32_t i, double L) { return (double(i) - N / 2.0) * 2 * kPi / L; }

// In-place inverse FFT (e^{+i}, unnormalised) of a power-of-two array, recursive: independent of the GPU's iterative
// bit-reversed butterflies.
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
// Real field of spectrum s (centred storage, [z][x]): sum_k s(k) e^{i k.x} at texel (x, z); returns the max |imag|.
double transform(std::vector<cd>& s, std::vector<double>& out)
{
    std::vector<cd> line(N);
    for (uint32_t z = 0; z < N; ++z)
    {
        for (uint32_t x = 0; x < N; ++x) line[x] = s[z * N + x];
        ifft(line);
        for (uint32_t x = 0; x < N; ++x) s[z * N + x] = line[x];
    }
    for (uint32_t x = 0; x < N; ++x)
    {
        for (uint32_t z = 0; z < N; ++z) line[z] = s[z * N + x];
        ifft(line);
        for (uint32_t z = 0; z < N; ++z) s[z * N + x] = line[z];
    }
    double imag = 0;
    out.resize(N * N);
    for (uint32_t z = 0; z < N; ++z)
        for (uint32_t x = 0; x < N; ++x)
        {
            const cd v = s[z * N + x] * (((x + z) & 1) ? -1.0 : 1.0);
            out[z * N + x] = v.real();
            imag = std::max(imag, std::abs(v.imag()));
        }
    return imag;
}
// The 8 fields of cascade c at time t from the GPU's h0, in double (channel order of Frame::at).
std::vector<std::vector<double>> referenceFields(const Frame& f, const OceanDesc& d, uint32_t c, double seconds, double* worstImag)
{
    const uint32_t clock = Ocean::phaseClock(seconds);
    std::vector<std::vector<cd>> spectra(8, std::vector<cd>(N * N));
    for (uint32_t z = 0; z < N; ++z)
        for (uint32_t x = 0; x < N; ++x)
        {
            const uint64_t bin = (uint64_t(c) * N + z) * N + x;
            const double kx = kOf(x, d.lengths[c]), kz = kOf(z, d.lengths[c]), k = std::sqrt(kx * kx + kz * kz);
            const uint32_t m = Ocean::frequencyIndex(k);
            const double phase = 2 * kPi * double(uint32_t(m * clock)) / 4294967296.0;
            const cd a(f.h0[4 * bin], f.h0[4 * bin + 1]), b(f.h0[4 * bin + 2], f.h0[4 * bin + 3]);
            const cd h = a * std::polar(1.0, -phase) + b * std::polar(1.0, phase), I(0, 1);
            if (!(k > 0)) continue;
            const cd v[8] = { I * kx / k * h, h, I * kz / k * h, 0.0, I * kx * h, I * kz * h, -kx * kx / k * h, -kz * kz / k * h };
            for (int ch = 0; ch < 8; ++ch) spectra[ch][z * N + x] = v[ch];
            spectra[3][z * N + x] = -kx * kz / k * h;  // dDx/dz
        }
    std::vector<std::vector<double>> fields(8);
    for (int ch = 0; ch < 8; ++ch) *worstImag = std::max(*worstImag, transform(spectra[ch], fields[ch]) / 1.0);
    return fields;
}
double rms(const std::vector<double>& v)
{
    double s = 0;
    for (double x : v) s += x * x;
    return std::sqrt(s / double(v.size()));
}
double compareFields(const Frame& f, const OceanDesc& d, double seconds, const char* what)
{
    double worst = 0;
    for (uint32_t c = 0; c < C; ++c)
    {
        double imag = 0;
        const auto ref = referenceFields(f, d, c, seconds, &imag);
        for (uint32_t ch = 0; ch < 8; ++ch)
        {
            const double r = rms(ref[ch]);
            W_CHECK(r > 0, "%s: cascade %u channel %u is empty", what, c, ch);
            double e = 0;
            for (uint32_t z = 0; z < N; ++z)
                for (uint32_t x = 0; x < N; ++x) e = std::max(e, std::abs(double(f.at(c, ch, x, z)) - ref[ch][z * N + x]));
            const double scale = r;
            W_CHECK(e <= 4e-5 * scale, "%s: cascade %u channel %u differs from the double FFT by %.3g (rms %.3g)", what, c, ch, e, scale);
            worst = std::max(worst, e / scale);
        }
        W_CHECK(imag <= 1e-9 * std::max(1.0, rms(ref[1])), "%s: cascade %u's reference transform is not real (%.3g): the spectrum pairs are not Hermitian", what, c, imag);
    }
    return worst;
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
        OceanDesc desc;
        desc.windDirection = 0.4f;
        desc.seed = 7;
        Ocean ocean(gpu.device, gpu.shaders, desc);

        if (time)
        {
            GpuProfiler profiler(gpu.device, 1, 64);
            std::vector<double> ms, rowsMs, columnsMs, mipsMs;
            for (uint64_t i = 0; i < 64; ++i)
            {
                run(gpu, ocean, 0.01 * double(i), &profiler, i);
                if (i >= 8 && profiler.lastCompleted())
                {
                    double r = 0, c = 0, mp = 0;
                    for (const auto& p : profiler.lastCompleted()->passes)
                    {
                        if (p.name == "ocean rows") r += p.durationMs();
                        if (p.name == "ocean columns") c += p.durationMs();
                        if (p.name == "ocean mip") mp += p.durationMs();
                    }
                    ms.push_back(r + c + mp);
                    mipsMs.push_back(mp);
                    rowsMs.push_back(r);
                    columnsMs.push_back(c);
                }
            }
            auto median = [](std::vector<double> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };
            std::sort(ms.begin(), ms.end());
            std::printf("ocean frame passes (3 cascades 512^2): rows + columns + mips median %.4f ms (min %.4f, max %.4f; rows %.4f, columns %.4f, 9 mips %.4f) over %zu frames\n", ms[ms.size() / 2],
                        ms.front(), ms.back(), median(rowsMs), median(columnsMs), median(mipsMs), ms.size());
            return 0;
        }

        // 1. single mode
        {
            std::vector<float> h0(kBins * 4, 0.0f);
            const uint32_t kx = N / 2 + 8, kz = N / 2, mirror = N / 2 - 8;
            const float ar = 0.3f, ai = 0.2f;
            h0[4 * (kz * N + kx) + 0] = ar;
            h0[4 * (kz * N + kx) + 1] = ai;
            h0[4 * (kz * N + mirror) + 2] = ar;
            h0[4 * (kz * N + mirror) + 3] = -ai;
            ocean.setSpectrum(h0);
            const double L = desc.lengths[0], k = kOf(kx, L), A = 2 * std::hypot(ar, ai), phi = std::atan2(ai, ar);
            const double omega = Ocean::frequencyIndex(k) * 2 * kPi / Ocean::kRepeat;
            double worst = 0, crest[2] = {};
            for (int step = 0; step < 2; ++step)
            {
                const double t = step ? 37.3 : 0.0;
                const Frame f = run(gpu, ocean, t);
                double best = -1e30;
                for (uint32_t z = 0; z < N; z += 37)
                    for (uint32_t x = 0; x < N; ++x)
                    {
                        const double X = double(x) * L / N, th = k * X - omega * t + phi;
                        const double expect[8] = { -A * std::sin(th), A * std::cos(th), 0, 0, -A * k * std::sin(th), 0, -A * k * std::cos(th), 0 };
                        for (uint32_t ch = 0; ch < 8; ++ch) worst = std::max(worst, std::abs(f.at(0, ch, x, z) - expect[ch]) / (ch == 1 || ch == 0 ? A : A * k));
                        for (uint32_t c = 1; c < C; ++c)
                            for (uint32_t ch = 0; ch < 8; ++ch) W_CHECK(f.at(c, ch, x, z) == 0.0f, "single mode: cascade %u channel %u not empty", c, ch);
                        if (z == 0 && f.at(0, 1, x, 0) > best) { best = f.at(0, 1, x, 0); crest[step] = X; }
                    }
            }
            W_CHECK(worst <= 1e-5, "single mode: fields differ from the closed form by %.3g of the amplitude", worst);
            // The crest nearest the first one moves by c t = (w / k) 37.3 s along +x (modulo the tile).
            const double wavelength = 2 * kPi / k, texel = L / N;
            const double moved = std::fmod(crest[1] - crest[0] + 10 * L, wavelength), expected = std::fmod(omega / k * 37.3, wavelength);
            double miss = std::fmod(moved - expected + 1.5 * wavelength, wavelength) - 0.5 * wavelength;  // wrapped to [-l/2, l/2)
            W_CHECK(std::abs(miss) <= texel, "single mode: the crest moved %.3f m, expected %.3f m along +k", moved, expected);
            std::printf("single mode: closed form within %.2e of the amplitude; crest travelled %.2f m (expected %.2f) along +x\n", worst, moved, expected);
        }

        // 2. sea state spectrum
        ocean.setDesc(desc);
        const Frame f0 = run(gpu, ocean, 3.7);
        W_CHECK(!f0.previousValid, "a new sea state must not report a valid previous displacement");
        const uint64_t mipValues = checkMips(f0);
        std::printf("mips: %llu values of levels 1..9 equal the mean of their children (float rounding)\n", (unsigned long long)mipValues);
        {
            double worst = 0, bandSum = 0, pairSquares = 0;
            uint32_t mismatched = 0;
            for (uint32_t c = 0; c < C; ++c)
                for (uint32_t z = 0; z < N; ++z)
                    for (uint32_t x = 0; x < N; ++x)
                    {
                        const uint64_t bin = (uint64_t(c) * N + z) * N + x;
                        const uint32_t mx = (N - x) % N, mz = (N - z) % N;
                        const bool nyquist = x == 0 || z == 0;
                        const double L = desc.lengths[c];
                        const double var = nyquist ? 0 : variance(desc, kOf(x, L), kOf(z, L), c), varM = nyquist ? 0 : variance(desc, kOf(mx, L), kOf(mz, L), c);
                        // Expected surface variance: sum over bins of E|h(k, t)|^2 = E|h0(k)|^2 + E|h0(-k)|^2 = (var(k) + var(-k)) / 2.
                        if (c == 1) bandSum += 0.5 * (var + varM);
                        // Realisation spread: a pair (k, -k) carries X = |h(k)|^2 + |h(-k)|^2 with std(X) = E(X) = var(k) + var(-k).
                        if (c == 1 && (uint64_t(z) * N + x) < (uint64_t(mz) * N + mx)) pairSquares += (var + varM) * (var + varM);
                        const cd h = gaussian(desc.seed, c, uint32_t(z * N + x)) * std::sqrt(0.25 * var);
                        const cd hm = std::conj(gaussian(desc.seed, c, uint32_t(mz * N + mx)) * std::sqrt(0.25 * varM));
                        const double sigma = std::sqrt(0.25 * var), sigmaM = std::sqrt(0.25 * varM);
                        const double e0 = std::abs(cd(f0.h0[4 * bin], f0.h0[4 * bin + 1]) - h), e1 = std::abs(cd(f0.h0[4 * bin + 2], f0.h0[4 * bin + 3]) - hm);
                        // Float holds variances down to 1.2e-38 m^2 (amplitude 1e-19 m): bins below it (the upwind tail of
                        // the spreading lobe) are zero on the GPU. The 1e-18 m floor admits exactly those. Upwind, the
                        // float sum k^ + w^ (components within 2^-23) leaves cos^2(theta / 2) a relative error
                        // 2^-22 / cos^2, raised to s and halved by the square root.
                        auto spreadTol = [&](uint32_t ix, uint32_t iz) { return nyquist ? 0.0 : 0.5 * desc.spread * 2.4e-7 / std::max(halfCos2(desc, kOf(ix, L), kOf(iz, L)), 1e-30); };
                        const double tol0 = (2e-4 + spreadTol(x, z)) * std::abs(h) + 1e-5 * sigma + 1e-18, tol1 = (2e-4 + spreadTol(mx, mz)) * std::abs(hm) + 1e-5 * sigmaM + 1e-18;
                        if ((e0 > tol0 || e1 > tol1) && ++mismatched <= 8)
                            std::printf("  bin c%u (%u, %u) |k| %.5g: var %.4g (mirror %.4g); GPU (%.6g, %.6g | %.6g, %.6g), replica (%.6g, %.6g | %.6g, %.6g)\n", c, x, z,
                                        std::hypot(kOf(x, L), kOf(z, L)), var, varM, f0.h0[4 * bin], f0.h0[4 * bin + 1], f0.h0[4 * bin + 2], f0.h0[4 * bin + 3], h.real(), h.imag(), hm.real(), hm.imag());
                        if (e0 > 1e-18) worst = std::max(worst, e0 / (std::abs(h) + sigma));
                        if (e1 > 1e-18) worst = std::max(worst, e1 / (std::abs(hm) + sigmaM));
                        if (!nyquist)
                        {
                            const uint64_t mbin = (uint64_t(c) * N + mz) * N + mx;
                            W_CHECK(f0.h0[4 * bin + 2] == f0.h0[4 * mbin] && f0.h0[4 * bin + 3] == -f0.h0[4 * mbin + 1], "cascade %u bin (%u, %u): the pair is not conj(h0(-k))", c, x, z);
                        }
                    }
            W_CHECK(mismatched == 0, "%u spectrum bins differ from the double replica (worst %.3g relative)", mismatched, worst);
            // Band integral: cascade 1 holds bands[0] <= |k| < bands[1]: integral of S(w) dw over w = sqrt(g k).
            const double w0 = std::sqrt(kG * desc.bands[0]), w1 = std::sqrt(kG * desc.bands[1]);
            double integral = 0;
            const int steps = 200000;
            for (int i = 0; i < steps; ++i) integral += jonswap(desc, w0 + (w1 - w0) * (i + 0.5) / steps) * (w1 - w0) / steps;
            W_CHECK(std::abs(bandSum / integral - 1) <= 0.02, "cascade 1 expected surface variance %.6g m^2, integral of S %.6g m^2", bandSum, integral);
            // The realised surface: the spatial mean of h^2 of cascade 1 (Parseval: the realisation's sum of |h(k, t)|^2)
            // against the expected variance, within 5 standard deviations of the realisation spread.
            double realised = 0;
            for (uint32_t z = 0; z < N; ++z)
                for (uint32_t x = 0; x < N; ++x) realised += double(f0.at(1, 1, x, z)) * f0.at(1, 1, x, z);
            realised /= double(N) * N;
            const double spread = std::sqrt(pairSquares) / bandSum;
            W_CHECK(std::abs(realised / bandSum - 1) <= 5 * spread, "cascade 1 realised height variance %.6g m^2, expected %.6g m^2 (spread %.3g)", realised, bandSum, spread);
            std::printf("spectrum: every bin equals the replica (worst %.2e relative); pairs Hermitian; expected band variance %.6g m^2 vs integral of S %.6g (%.2f %%); realised %.6g (%+.2f %%, spread %.2f %%)\n",
                        worst, bandSum, integral, 100 * (bandSum / integral - 1), realised, 100 * (realised / bandSum - 1), 100 * spread);
        }

        // 3. fields
        const double e3 = compareFields(f0, desc, 3.7, "t = 3.7 s");
        std::printf("fields at t = 3.7 s: 3 cascades x 8 fields equal the double FFT within %.2e of each field's rms\n", e3);

        // 4. time
        {
            const Frame a = run(gpu, ocean, 5.0), b = run(gpu, ocean, 5.0 + Ocean::kRepeat), again = run(gpu, ocean, 5.0);
            for (uint32_t t = 0; t < 2; ++t) W_CHECK(std::memcmp(a.tex[t].data(), again.tex[t].data(), a.tex[t].size() * 4) == 0, "two runs at the same time differ (texture %u)", t);
            // The previous displacement of each record is the last record's displacement, every mip, bit for bit.
            W_CHECK(b.previousValid && again.previousValid, "previous displacement not valid after the first record");
            W_CHECK(b.tex[2] == a.tex[0] && again.tex[2] == b.tex[0], "the previous displacement is not the last record's");
            double worst = 0, scale = 0;
            for (uint32_t t = 0; t < 2; ++t)
                for (size_t i = 0; i < a.tex[t].size(); ++i) { worst = std::max(worst, (double)std::abs(a.tex[t][i] - b.tex[t][i])); scale = std::max(scale, (double)std::abs(a.tex[t][i])); }
            W_CHECK(worst <= 1e-5 * scale, "t and t + T differ by %.3g (max field %.3g)", worst, scale);
            const Frame hour = run(gpu, ocean, 3600.0);
            const double eh = compareFields(hour, desc, 3600.0, "t = 1 h");
            std::printf("time: bit-identical reruns; t + T equal within %.2e; at t = 1 h the fields equal the double FFT within %.2e rms\n", worst / scale, eh);
        }
        const uint32_t errors = gpu.device.drainDebugMessages();
        W_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("ocean tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
