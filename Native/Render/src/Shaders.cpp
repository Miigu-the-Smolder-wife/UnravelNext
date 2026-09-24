#include "unx/render/Shaders.h"

#include "unx/core/File.h"
#include "unx/core/Jobs.h"

#include <chrono>

namespace unx::render
{
namespace
{
double msSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

template <typename T, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE Type>
struct alignas(void*) Subobject
{
    D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = Type;
    T value{};
};
} // namespace

ShaderLibrary::ShaderLibrary(Device& device, std::filesystem::path directory) : m_device(device), m_directory(std::move(directory))
{
    if (!std::filesystem::exists(m_directory)) fail("shader directory %s does not exist (build the shader targets)", m_directory.string().c_str());
}

std::vector<uint8_t> ShaderLibrary::load(const std::string& kernel) const
{
    return readBinaryFile(m_directory / (kernel + ".dxil"));
}

ComPtr<ID3D12PipelineState> ShaderLibrary::createCompute(const std::string& kernel)
{
    std::vector<uint8_t> dxil = load(kernel);
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = m_device.rootSignature();
    d.CS = { dxil.data(), dxil.size() };
    ComPtr<ID3D12PipelineState> pso;
    auto t0 = std::chrono::steady_clock::now();
    check(m_device.d3d()->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)), ("CreateComputePipelineState " + kernel).c_str());
    double ms = msSince(t0);
    std::wstring wname(kernel.begin(), kernel.end());
    pso->SetName(wname.c_str());
    std::lock_guard lock(m_mutex);
    ++m_stats.created;
    m_stats.totalCpuMs += ms;
    if (ms > m_stats.slowestMs)
    {
        m_stats.slowestMs = ms;
        m_stats.slowest = kernel;
    }
    return pso;
}

ID3D12PipelineState* ShaderLibrary::compute(const std::string& kernel)
{
    {
        std::lock_guard lock(m_mutex);
        auto it = m_pipelines.find(kernel);
        if (it != m_pipelines.end()) return it->second.Get();
    }
    ComPtr<ID3D12PipelineState> pso = createCompute(kernel);
    std::lock_guard lock(m_mutex);
    auto& slot = m_pipelines[kernel];
    if (!slot) slot = pso;
    return slot.Get();
}

void ShaderLibrary::createAll(const std::vector<std::string>& computeKernels)
{
    auto t0 = std::chrono::steady_clock::now();
    Jobs::instance().parallelFor((uint32_t)computeKernels.size(), [&](uint32_t i) { compute(computeKernels[i]); });
    m_stats.wallMs = msSince(t0);
}

ID3D12PipelineState* ShaderLibrary::mesh(const std::string& name, const MeshPipelineDesc& desc)
{
    {
        std::lock_guard lock(m_mutex);
        auto it = m_pipelines.find(name);
        if (it != m_pipelines.end()) return it->second.Get();
    }
    std::vector<uint8_t> ms = load(desc.meshShader);
    std::vector<uint8_t> ps;
    if (!desc.pixelShader.empty()) ps = load(desc.pixelShader);

    struct Stream
    {
        Subobject<ID3D12RootSignature*, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE> root;
        Subobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS> ms;
        Subobject<D3D12_SHADER_BYTECODE, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS> ps;
        Subobject<D3D12_RASTERIZER_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER> raster;
        Subobject<D3D12_DEPTH_STENCIL_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL> depth;
        Subobject<D3D12_RT_FORMAT_ARRAY, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RENDER_TARGET_FORMATS> rts;
        Subobject<DXGI_FORMAT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL_FORMAT> dsFormat;
        Subobject<D3D12_BLEND_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_BLEND> blend;
        Subobject<DXGI_SAMPLE_DESC, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC> sample;
        Subobject<UINT, D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_MASK> mask;
    } s;
    s.root.value = m_device.rootSignature();
    s.ms.value = { ms.data(), ms.size() };
    s.ps.value = { ps.empty() ? nullptr : ps.data(), ps.size() };
    s.raster.value.FillMode = D3D12_FILL_MODE_SOLID;
    s.raster.value.CullMode = desc.cull;
    s.raster.value.FrontCounterClockwise = desc.frontCounterClockwise ? TRUE : FALSE;
    s.raster.value.DepthClipEnable = TRUE;
    s.raster.value.ConservativeRaster = desc.conservative ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    s.depth.value.DepthEnable = desc.depthFormat != DXGI_FORMAT_UNKNOWN;
    s.depth.value.DepthWriteMask = desc.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    s.depth.value.DepthFunc = desc.depthFunc;
    s.rts.value.NumRenderTargets = (UINT)desc.renderTargets.size();
    for (size_t i = 0; i < desc.renderTargets.size(); ++i) s.rts.value.RTFormats[i] = desc.renderTargets[i];
    s.dsFormat.value = desc.depthFormat;
    for (auto& rt : s.blend.value.RenderTarget) rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    s.sample.value = { 1, 0 };
    s.mask.value = UINT_MAX;
    D3D12_PIPELINE_STATE_STREAM_DESC sd{ sizeof s, &s };
    ComPtr<ID3D12PipelineState> pso;
    auto t0 = std::chrono::steady_clock::now();
    check(m_device.d3d()->CreatePipelineState(&sd, IID_PPV_ARGS(&pso)), ("CreatePipelineState (mesh) " + name).c_str());
    double t = msSince(t0);
    std::lock_guard lock(m_mutex);
    ++m_stats.created;
    m_stats.totalCpuMs += t;
    auto& slot = m_pipelines[name];
    if (!slot) slot = pso;
    return slot.Get();
}
} // namespace unx::render
