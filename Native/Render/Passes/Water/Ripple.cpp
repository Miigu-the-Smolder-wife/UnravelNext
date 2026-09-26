// Local ripples W (track W, B7). See include/unx/water/Ripple.h and Ripple.hlsli.
#include "unx/water/Ripple.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::water
{
using namespace unx::render;

namespace
{
constexpr uint64_t kTexels = uint64_t(Ripples::kN) * Ripples::kN;
constexpr double kPi = 3.14159265358979323846;

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
          "ripple buffer");
    r->SetName(name);
    return r;
}
} // namespace

Ripples::Ripples(Device& device, ShaderLibrary& shaders, const RippleDesc& desc) : m_device(device), m_shaders(shaders), m_desc(desc)
{
    if (!(desc.texel > 0) || !(desc.depth >= 0) || !(desc.gravity > 0) || !(desc.tensionOverDensity >= 0) || !(desc.viscosity >= 0) || !(desc.spongeRate >= 0) ||
        !desc.maxSources || !desc.framesInFlight)
        fail("ripples: invalid description");
    m_state = makeBuffer(device, kTexels * 8, D3D12_HEAP_TYPE_DEFAULT, L"ripple state");
    m_accum = makeBuffer(device, kTexels * 8, D3D12_HEAP_TYPE_DEFAULT, L"ripple sources");
    m_twiddles = makeBuffer(device, kN * 4, D3D12_HEAP_TYPE_DEFAULT, L"ripple twiddles");
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 t{};
    t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width = t.Height = kN;
    t.DepthOrArraySize = t.MipLevels = 1;
    t.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    t.SampleDesc.Count = 1;
    t.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_output)), "ripple output");
    m_output->SetName(L"ripple field");
    for (uint32_t s = 0; s < desc.framesInFlight; ++s)
    {
        m_sourceUpload.push_back(makeBuffer(device, uint64_t(desc.maxSources) * 32 + kN * 4, D3D12_HEAP_TYPE_UPLOAD, L"ripple upload"));
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(m_sourceUpload.back()->Map(0, &none, (void**)&mapped), "map ripple upload");
        m_sourceMapped.push_back(mapped);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = UINT(desc.maxSources) * 8;
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_sourceSrv.push_back(device.descriptors().allocateResource());
        device.d3d()->CreateShaderResourceView(m_sourceUpload.back().Get(), &sd, device.descriptors().resourceCpu(m_sourceSrv.back()));
    }
}
Ripples::~Ripples()
{
    for (auto& u : m_sourceUpload) u->Unmap(0, nullptr);
    for (const ComPtr<ID3D12Resource>& r : { m_state, m_accum, m_twiddles, m_output, m_stateUpload })
        if (r) m_device.deferRelease(r);
    for (auto& u : m_sourceUpload) m_device.deferRelease(u);
    for (uint32_t srv : m_sourceSrv) m_device.descriptors().freeResource(srv);
}

int32_t Ripples::windowOrigin(double focus, float texel) { return int32_t(std::llround(focus / double(texel))) - int32_t(kN / 2); }

void Ripples::setState(const std::vector<float>& etaPhi)
{
    if (etaPhi.size() != kTexels * 2) fail("ripples: a state has %llu floats, expected %llu", (unsigned long long)etaPhi.size(), (unsigned long long)(kTexels * 2));
    if (m_stateUpload) m_device.deferRelease(m_stateUpload);
    m_stateUpload = makeBuffer(m_device, kTexels * 8, D3D12_HEAP_TYPE_UPLOAD, L"ripple state upload");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(m_stateUpload->Map(0, &none, &mapped), "map ripple state upload");
    std::memcpy(mapped, etaPhi.data(), kTexels * 8);
    m_stateUpload->Unmap(0, nullptr);
    m_stateDirty = true;
}

RippleOutput Ripples::record(RenderGraph& g, uint64_t frame, double focusX, double focusZ, float dt, const std::vector<RippleSource>& sources)
{
    if (!(dt >= 0)) fail("ripples: negative frame time");
    if (sources.size() > m_desc.maxSources) fail("ripples: %zu sources exceed the capacity %u", sources.size(), m_desc.maxSources);
    const int32_t origin[2] = { windowOrigin(focusX, m_desc.texel), windowOrigin(focusZ, m_desc.texel) };
    int32_t shift[2] = { 0, 0 };
    if (m_placed) { shift[0] = origin[0] - m_origin[0]; shift[1] = origin[1] - m_origin[1]; }
    m_origin[0] = origin[0];
    m_origin[1] = origin[1];
    m_placed = true;

    const uint32_t slot = uint32_t(frame % m_desc.framesInFlight);
    uint8_t* mapped = m_sourceMapped[slot];
    for (size_t i = 0; i < sources.size(); ++i)
    {
        const float s[8] = { float(sources[i].x / m_desc.texel - origin[0]), float(sources[i].z / m_desc.texel - origin[1]), sources[i].radius, sources[i].impulse, sources[i].volume, 0, 0, 0 };
        std::memcpy(mapped + 32 * i, s, 32);
    }
    auto import = [&](ID3D12Resource* r, const char* name) { return g.importBuffer(r, { name, r->GetDesc().Width, 0 }); };
    const BufferRef state = import(m_state.Get(), "ripple state"), accum = import(m_accum.Get(), "ripple sources"), twiddles = import(m_twiddles.Get(), "ripple twiddles");
    const TextureRef field = g.importTexture(m_output.Get(), TextureDesc{ "ripple field", kN, kN, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT }, D3D12_BARRIER_LAYOUT_COMMON);
    ID3D12Resource* upload = m_sourceUpload[slot].Get();
    if (!m_twiddlesUploaded)
    {
        float* tw = (float*)(mapped + uint64_t(m_desc.maxSources) * 32);
        for (uint32_t j = 0; j < kN / 2; ++j) { tw[2 * j] = float(std::cos(2 * kPi * j / kN)); tw[2 * j + 1] = float(std::sin(2 * kPi * j / kN)); }
        const uint64_t at = uint64_t(m_desc.maxSources) * 32;
        g.addPass("ripple twiddles", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(twiddles, Use::CopyDst); pb.keep(); },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(twiddles), 0, upload, at, kN * 4); });
        ID3D12PipelineState* clear = m_shaders.compute("Passes/Water/RippleClear");  // a calm surface
        g.addPass("ripple clear", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(accum, Use::UavCompute); pb.use(state, Use::UavCompute); pb.keep(); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(state), 0, 0, c.uav(accum) };
                      c.cmd->SetPipelineState(clear);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(uint32_t((kTexels + 255) / 256), 1, 1);
                  });
        m_twiddlesUploaded = true;
    }
    if (m_stateDirty)
    {
        ID3D12Resource* source = m_stateUpload.Get();
        g.addPass("ripple state upload", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(state, Use::CopyDst); pb.keep(); },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(state), 0, source, 0, kTexels * 8); });
        m_stateDirty = false;
    }
    const uint32_t sourceSrv = m_sourceSrv[slot];  // the upload-heap list, read in place (not a graph resource)
    const BufferRef spectrum = g.createBuffer({ "ripple spectrum", uint64_t(kN) * (kN + 1) * 16, 0 });
    const RippleDesc d = m_desc;
    const uint32_t count = uint32_t(sources.size());
    auto constants = [=](PassContext& c) {
        uint32_t k[16] = { c.uav(state), c.uav(spectrum), c.uav(field), c.uav(accum), 0, 0, c.srv(twiddles), 0, sourceSrv, count, uint32_t(shift[0]), uint32_t(shift[1]) };
        std::memcpy(&k[4], &dt, 4);
        std::memcpy(&k[5], &d.texel, 4);
        std::memcpy(&k[7], &d.depth, 4);
        std::memcpy(&k[12], &d.gravity, 4);
        std::memcpy(&k[13], &d.tensionOverDensity, 4);
        std::memcpy(&k[14], &d.viscosity, 4);
        std::memcpy(&k[15], &d.spongeRate, 4);
        c.computeConstants(k, 16);
    };
    auto uses = [&](PassBuilder& pb) {
        pb.use(state, Use::UavCompute); pb.use(spectrum, Use::UavCompute); pb.use(field, Use::UavCompute); pb.use(accum, Use::UavCompute);
        pb.use(twiddles, Use::SrvCompute);
    };
    auto pass = [&](const char* name, const char* kernel, uint32_t groups) {
        ID3D12PipelineState* pso = m_shaders.compute(kernel);
        g.addPass(name, QueueType::Graphics, uses, [=](PassContext& c) { c.cmd->SetPipelineState(pso); constants(c); c.cmd->Dispatch(groups, 1, 1); });
    };
    if (count) pass("ripple sources", "Passes/Water/RippleSplat", count);
    pass("ripple forward rows", "Passes/Water/RippleForwardRows", kN);
    pass("ripple forward columns", "Passes/Water/RippleForwardColumns", kN);
    pass("ripple evolve", "Passes/Water/RippleEvolve", uint32_t((kTexels + 255) / 256));
    pass("ripple inverse rows", "Passes/Water/RippleInverseRows", kN);
    pass("ripple inverse columns", "Passes/Water/RippleInverseColumns", kN);
    RippleOutput out;
    out.field = field;
    out.originTexel[0] = origin[0];
    out.originTexel[1] = origin[1];
    out.texel = m_desc.texel;
    return out;
}
} // namespace unx::water
