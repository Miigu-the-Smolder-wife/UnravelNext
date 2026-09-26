// FFT ocean (track W, B7). See include/unx/water/Ocean.h and Ocean.hlsli.
#include "unx/water/Ocean.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::water
{
using namespace unx::render;

namespace
{
constexpr uint64_t kBins = uint64_t(Ocean::kCascades) * Ocean::kN * Ocean::kN;
constexpr double kPi = 3.14159265358979323846, kG = 9.81;

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "ocean buffer");
    r->SetName(name);
    return r;
}
void validate(const OceanDesc& d)
{
    for (uint32_t c = 0; c < Ocean::kCascades; ++c)
        if (!(d.lengths[c] > 0) || (c && !(d.lengths[c] < d.lengths[c - 1]))) fail("ocean: tile lengths must be positive and decreasing");
    for (uint32_t b = 0; b < 2; ++b)
        if (!(d.bands[b] > 0) || !(d.bands[b] < kPi * Ocean::kN / d.lengths[b]) || (b && !(d.bands[1] > d.bands[0])))
            fail("ocean: band boundary %u (%g 1/m) must increase and lie below cascade %u's Nyquist %g 1/m", b, d.bands[b], b, kPi * Ocean::kN / d.lengths[b]);
    if (!(d.windSpeed > 0) || !(d.fetch > 0) || !(d.spread >= 0)) fail("ocean: wind speed and fetch must be positive, the spread non-negative");
}
} // namespace

Ocean::Ocean(Device& device, ShaderLibrary& shaders, const OceanDesc& desc) : m_device(device), m_shaders(shaders), m_desc(desc)
{
    validate(desc);
    m_h0 = makeBuffer(device, kBins * 16, D3D12_HEAP_TYPE_DEFAULT, L"ocean h0");
    m_frequencies = makeBuffer(device, kBins * 4, D3D12_HEAP_TYPE_DEFAULT, L"ocean frequencies");
    m_twiddles = makeBuffer(device, kN * 4, D3D12_HEAP_TYPE_DEFAULT, L"ocean twiddles");
}
Ocean::~Ocean()
{
    for (const ComPtr<ID3D12Resource>& r : { m_h0, m_frequencies, m_twiddles, m_upload, m_frequencyUpload })
        if (r) m_device.deferRelease(r);
}

void Ocean::setDesc(const OceanDesc& desc)
{
    validate(desc);
    if (std::memcmp(desc.lengths, m_desc.lengths, sizeof desc.lengths)) m_frequenciesDirty = true;
    m_desc = desc;
    m_dirty = true;
    m_explicit = false;
}
void Ocean::setSpectrum(const std::vector<float>& h0)
{
    if (h0.size() != kBins * 4) fail("ocean: an explicit spectrum has %llu floats, expected %llu", (unsigned long long)h0.size(), (unsigned long long)(kBins * 4));
    if (m_upload) m_device.deferRelease(m_upload);
    m_upload = makeBuffer(m_device, kBins * 16, D3D12_HEAP_TYPE_UPLOAD, L"ocean h0 upload");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(m_upload->Map(0, &none, &mapped), "map ocean h0 upload");
    std::memcpy(mapped, h0.data(), kBins * 16);
    m_upload->Unmap(0, nullptr);
    m_dirty = true;
    m_explicit = true;
}

uint32_t Ocean::phaseClock(double seconds)
{
    const double f = seconds / kRepeat - std::floor(seconds / kRepeat);
    return (uint32_t)std::min(std::floor(f * 4294967296.0), 4294967295.0);
}
uint32_t Ocean::frequencyIndex(double kLength) { return (uint32_t)std::lround(std::sqrt(kG * kLength) * kRepeat / (2 * kPi)); }

OceanOutput Ocean::record(RenderGraph& g, double seconds)
{
    auto import = [&](ID3D12Resource* r, const char* name) { return g.importBuffer(r, { name, r->GetDesc().Width, 0 }); };
    const BufferRef h0 = import(m_h0.Get(), "ocean h0"), frequencies = import(m_frequencies.Get(), "ocean frequencies"), twiddles = import(m_twiddles.Get(), "ocean twiddles");
    if (m_frequenciesDirty)
    {
        std::vector<uint32_t> m(kBins);
        for (uint32_t c = 0; c < kCascades; ++c)
            for (uint32_t z = 0; z < kN; ++z)
                for (uint32_t x = 0; x < kN; ++x)
                {
                    const double dk = 2 * kPi / m_desc.lengths[c], kx = (double(x) - kN / 2.0) * dk, kz = (double(z) - kN / 2.0) * dk;
                    m[(uint64_t(c) * kN + z) * kN + x] = frequencyIndex(std::sqrt(kx * kx + kz * kz));
                }
        if (m_frequencyUpload) m_device.deferRelease(m_frequencyUpload);
        m_frequencyUpload = makeBuffer(m_device, kBins * 4 + kN * 4, D3D12_HEAP_TYPE_UPLOAD, L"ocean frequencies upload");
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(m_frequencyUpload->Map(0, &none, (void**)&mapped), "map ocean frequencies upload");
        std::memcpy(mapped, m.data(), kBins * 4);
        float* twiddle = (float*)(mapped + kBins * 4);
        for (uint32_t j = 0; j < kN / 2; ++j) { twiddle[2 * j] = (float)std::cos(2 * kPi * j / kN); twiddle[2 * j + 1] = (float)std::sin(2 * kPi * j / kN); }
        m_frequencyUpload->Unmap(0, nullptr);
        ID3D12Resource* source = m_frequencyUpload.Get();
        g.addPass("ocean frequencies", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(frequencies, Use::CopyDst); pb.use(twiddles, Use::CopyDst); pb.keep(); },
                  [=](PassContext& c) {
                      c.cmd->CopyBufferRegion(c.resource(frequencies), 0, source, 0, kBins * 4);
                      c.cmd->CopyBufferRegion(c.resource(twiddles), 0, source, kBins * 4, kN * 4);
                  });
        m_frequenciesDirty = false;
    }
    if (m_dirty && m_explicit)
    {
        ID3D12Resource* source = m_upload.Get();
        g.addPass("ocean h0 upload", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(h0, Use::CopyDst); pb.keep(); },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(h0), 0, source, 0, kBins * 16); });
    }
    else if (m_dirty)
    {
        ID3D12PipelineState* pso = m_shaders.compute("Passes/Water/OceanSpectrum");
        const OceanDesc d = m_desc;
        // Spreading normalisation 1 / integral of cos^(2s)(theta / 2) over [-pi, pi] = Gamma(s + 1) / (2 sqrt(pi) Gamma(s + 1/2)).
        const float norm = (float)(std::exp(std::lgamma(d.spread + 1.0) - std::lgamma(d.spread + 0.5)) / (2 * std::sqrt(kPi)));
        g.addPass("ocean spectrum", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(h0, Use::UavCompute); pb.keep(); },
                  [=](PassContext& c) {
                      uint32_t k[16] = { c.uav(h0), 0, d.seed, kCascades };
                      const float wind[2] = { (float)std::cos(double(d.windDirection)), (float)std::sin(double(d.windDirection)) };
                      std::memcpy(&k[4], &d.windSpeed, 4); std::memcpy(&k[5], &d.fetch, 4); std::memcpy(&k[6], wind, 8); std::memcpy(&k[15], &d.spread, 4);
                      std::memcpy(&k[8], d.lengths, 12);
                      std::memcpy(&k[12], d.bands, 8); std::memcpy(&k[14], &norm, 4);
                      c.cmd->SetPipelineState(pso);
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch((uint32_t)((kBins + 255) / 256), 1, 1);
                  });
    }
    m_dirty = false;

    const BufferRef spectrum = g.createBuffer({ "ocean rows", uint64_t(kCascades) * kN * (kN + 1) * 32, 0 });  // row pitch N + 1 (Ocean.hlsli)
    TextureDesc fd{};
    fd.name = "ocean field";
    fd.width = fd.height = kN;
    fd.depthOrArraySize = 2 * kCascades;
    fd.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const TextureRef field = g.createTexture(fd);
    const uint32_t clock = phaseClock(seconds);
    const OceanDesc d = m_desc;
    auto constants = [=](PassContext& c, bool columnsPass) {
        uint32_t k[12] = { c.srv(h0), c.uav(spectrum), columnsPass ? c.uav(field) : 0u, kCascades, clock, c.srv(frequencies), c.srv(twiddles), 0 };
        std::memcpy(&k[8], d.lengths, 12);
        c.computeConstants(k, 12);
    };
    ID3D12PipelineState* rows = m_shaders.compute("Passes/Water/OceanRows");
    ID3D12PipelineState* columns = m_shaders.compute("Passes/Water/OceanColumns");
    g.addPass("ocean rows", QueueType::Graphics,
              [&](PassBuilder& pb) { pb.use(h0, Use::SrvCompute); pb.use(frequencies, Use::SrvCompute); pb.use(twiddles, Use::SrvCompute); pb.use(spectrum, Use::UavCompute); },
              [=](PassContext& c) { c.cmd->SetPipelineState(rows); constants(c, false); c.cmd->Dispatch(kN * kCascades, 1, 1); });
    g.addPass("ocean columns", QueueType::Graphics,
              [&](PassBuilder& pb) { pb.use(h0, Use::SrvCompute); pb.use(frequencies, Use::SrvCompute); pb.use(twiddles, Use::SrvCompute); pb.use(spectrum, Use::UavCompute); pb.use(field, Use::UavCompute); },
              [=](PassContext& c) { c.cmd->SetPipelineState(columns); constants(c, true); c.cmd->Dispatch(kN * kCascades, 1, 1); });
    return { field, h0 };
}
} // namespace unx::water
