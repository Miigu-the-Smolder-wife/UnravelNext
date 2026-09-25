#include "Probe/Probe.h"

#include "unx/core/File.h"

#include <cstdio>

#pragma comment(lib, "version.lib")

namespace unx::host::probe
{
using render::check;

const char* modeName(Mode mode)
{
    switch (mode)
    {
    case Mode::FusedList: return "fused_list";
    case Mode::HostQueueLists: return "host_queue_lists";
    case Mode::OwnQueue: return "own_queue";
    case Mode::IndependentDevice: return "independent_device";
    }
    return "?";
}

ProbeGpu::ProbeGpu(ID3D12Device* device, const std::filesystem::path& shaderDirectory) : m_device(device)
{
    D3D12_ROOT_PARAMETER1 params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, 32 };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = { 1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samplers[5]{};
    auto sampler = [&](uint32_t reg, D3D12_FILTER filter, D3D12_TEXTURE_ADDRESS_MODE mode, uint32_t aniso = 1, D3D12_COMPARISON_FUNC cmp = D3D12_COMPARISON_FUNC_NONE) {
        D3D12_STATIC_SAMPLER_DESC& s = samplers[reg];
        s.Filter = filter;
        s.AddressU = s.AddressV = s.AddressW = mode;
        s.MaxAnisotropy = aniso;
        s.ComparisonFunc = cmp;
        s.MaxLOD = D3D12_FLOAT32_MAX;
        s.ShaderRegister = reg;
        s.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    };
    sampler(0, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    sampler(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    sampler(3, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_WRAP, 16);
    sampler(4, D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 1, D3D12_COMPARISON_FUNC_GREATER_EQUAL);
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = 2;
    rs.Desc_1_1.pParameters = params;
    rs.Desc_1_1.NumStaticSamplers = 5;
    rs.Desc_1_1.pStaticSamplers = samplers;
    rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rs, &blob, &error)))
        fail("probe root signature: %s", error ? (const char*)error->GetBufferPointer() : "?");
    check(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)), "probe CreateRootSignature");

    D3D12_DESCRIPTOR_HEAP_DESC h{};
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    h.NumDescriptors = 256;
    h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&m_heap)), "probe CreateDescriptorHeap");
    m_descriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    m_heavy = load(shaderDirectory / "Probe/Heavy.dxil");
    m_tiny = load(shaderDirectory / "Probe/Tiny.dxil");
    m_output = load(shaderDirectory / "Probe/Output.dxil");
    m_present = load(shaderDirectory / "Probe/Present.dxil");
}

ComPtr<ID3D12PipelineState> ProbeGpu::load(const std::filesystem::path& file)
{
    const std::vector<uint8_t> dxil = readBinaryFile(file);
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = m_rootSignature.Get();
    d.CS = { dxil.data(), dxil.size() };
    ComPtr<ID3D12PipelineState> pso;
    check(m_device->CreateComputePipelineState(&d, IID_PPV_ARGS(&pso)), "probe CreateComputePipelineState");
    return pso;
}

uint32_t ProbeGpu::uav(ID3D12Resource* resource, DXGI_FORMAT format)
{
    if (m_next >= 256) fail("probe descriptor heap full");
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = format;
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)m_next * m_descriptorSize;
    m_device->CreateUnorderedAccessView(resource, nullptr, &d, cpu);
    return m_next++;
}

uint32_t ProbeGpu::rawUav(ID3D12Resource* buffer, uint32_t bytes)
{
    if (m_next >= 256) fail("probe descriptor heap full");
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT_R32_TYPELESS;
    d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    d.Buffer.NumElements = bytes / 4;
    d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)m_next * m_descriptorSize;
    m_device->CreateUnorderedAccessView(buffer, nullptr, &d, cpu);
    return m_next++;
}

void ProbeGpu::bind(ID3D12GraphicsCommandList* cmd) const
{
    ID3D12DescriptorHeap* heaps[] = { m_heap.Get() };
    cmd->SetDescriptorHeaps(1, heaps);
    cmd->SetComputeRootSignature(m_rootSignature.Get());
}

ComPtr<ID3D12Resource> createTexture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                                     D3D12_RESOURCE_STATES state, D3D12_HEAP_FLAGS heapFlags, D3D12_RESOURCE_FLAGS flags, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = flags | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&heap, heapFlags, &d, state, nullptr, IID_PPV_ARGS(&r)), "probe CreateCommittedResource (texture)");
    if (name) r->SetName(name);
    return r;
}

void globalUavBarrier(ID3D12GraphicsCommandList7* cmd)
{
    D3D12_GLOBAL_BARRIER g{};
    g.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    g.SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
    g.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    g.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_GLOBAL;
    group.NumBarriers = 1;
    group.pGlobalBarriers = &g;
    cmd->Barrier(1, &group);
}

ProbeWork::ProbeWork(ProbeGpu& gpu, const WorkDesc& desc) : m_gpu(gpu), m_desc(desc)
{
    ID3D12Device* device = gpu.device();
    m_ping = createTexture(device, desc.width, desc.height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_FLAG_NONE, L"probe ping");
    m_pong = createTexture(device, desc.width, desc.height, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_FLAG_NONE, L"probe pong");
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC b{};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = 256;
    b.Height = 1;
    b.DepthOrArraySize = 1;
    b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    b.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_counter)), "probe counter");
    m_pingUav = gpu.uav(m_ping.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_pongUav = gpu.uav(m_pong.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
    m_counterUav = gpu.rawUav(m_counter.Get(), 256);
}

void ProbeWork::record(ID3D12GraphicsCommandList7* cmd, uint32_t outputUav) const
{
    m_gpu.bind(cmd);
    const uint32_t gx = (m_desc.width + 7) / 8, gy = (m_desc.height + 7) / 8;
    const uint32_t total = m_desc.heavyPasses + m_desc.tinyPasses;
    // Spread the heavy passes evenly among the tiny ones.
    uint32_t heavyDone = 0;
    bool pingIsSource = true;
    for (uint32_t i = 0; i < total; ++i)
    {
        const bool heavy = heavyDone < m_desc.heavyPasses && (uint64_t)(i + 1) * m_desc.heavyPasses >= (uint64_t)(heavyDone + 1) * total;
        if (heavy)
        {
            const uint32_t c[8] = { pingIsSource ? m_pingUav : m_pongUav, pingIsSource ? m_pongUav : m_pingUav, m_desc.width, m_desc.height, heavyDone, 0, 0, 0 };
            cmd->SetPipelineState(m_gpu.heavy());
            cmd->SetComputeRoot32BitConstants(0, 8, c, 0);
            cmd->Dispatch(gx, gy, 1);
            pingIsSource = !pingIsSource;
            ++heavyDone;
        }
        else
        {
            const uint32_t c[4] = { m_counterUav, i, 0, 0 };
            cmd->SetPipelineState(m_gpu.tiny());
            cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
            cmd->Dispatch(1, 1, 1);
        }
        globalUavBarrier(cmd);
    }
    const uint32_t c[4] = { pingIsSource ? m_pingUav : m_pongUav, outputUav, m_desc.width, m_desc.height };
    cmd->SetPipelineState(m_gpu.output());
    cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
    cmd->Dispatch(gx, gy, 1);
}

void QueueClock::calibrate(ID3D12CommandQueue* queue)
{
    check(queue->GetTimestampFrequency(&gpuFrequency), "GetTimestampFrequency");
    check(queue->GetClockCalibration(&gpuAtCalibration, &cpuAtCalibration), "GetClockCalibration");
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    cpuFrequency = (uint64_t)f.QuadPart;
}

double QueueClock::toMs(uint64_t gpuTicks) const
{
    const double gpuSeconds = ((double)(int64_t)(gpuTicks - gpuAtCalibration)) / (double)gpuFrequency;
    return (double)cpuAtCalibration * 1000.0 / (double)cpuFrequency + gpuSeconds * 1000.0;
}

namespace
{
std::string narrow(const wchar_t* w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::string fileVersion(const wchar_t* path)
{
    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeW(path, &handle);
    if (!size) return "unknown";
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return "unknown";
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return "unknown";
    char buf[64];
    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
    return buf;
}
} // namespace

DeviceFacts readDeviceFacts(ID3D12Device* device)
{
    DeviceFacts f;
    const LUID luid = device->GetAdapterLuid();
    f.adapterLuid = ((uint64_t)(uint32_t)luid.HighPart << 32) | luid.LowPart;
    ComPtr<IDXGIFactory4> factory;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
    {
        ComPtr<IDXGIAdapter1> adapter;
        if (SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
        {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            f.adapter = narrow(d.Description);
        }
    }
    if (HMODULE core = GetModuleHandleW(L"D3D12Core.dll"))
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(core, path, MAX_PATH);
        f.runtimePath = narrow(path);
        f.runtimeVersion = fileVersion(path);
    }
    D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_9 };
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)))
    {
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_8;
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) sm.HighestShaderModel = D3D_SHADER_MODEL_5_1;
    }
    char smText[16];
    std::snprintf(smText, sizeof smText, "%u.%u", (unsigned)sm.HighestShaderModel >> 4, (unsigned)sm.HighestShaderModel & 0xF);
    f.shaderModel = smText;
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    f.bindingTier = (uint32_t)o.ResourceBindingTier;
    f.heapTier = (uint32_t)o.ResourceHeapTier;
    f.typedUavLoadAdditionalFormats = o.TypedUAVLoadAdditionalFormats;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
    f.raytracingTier = (uint32_t)o5.RaytracingTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7);
    f.meshShaderTier = (uint32_t)o7.MeshShaderTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
    device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12);
    f.enhancedBarriers = o12.EnhancedBarriersSupported;
    return f;
}

std::string jsonEscape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); out += b; }
        else out += c;
    }
    return out;
}

std::string DeviceFacts::toJson() const
{
    return format("{\"adapter\": \"%s\", \"adapterLuid\": \"%016llx\", \"runtimePath\": \"%s\", \"runtimeVersion\": \"%s\", "
                  "\"shaderModel\": \"%s\", \"meshShaderTier\": %u, \"raytracingTier\": %u, \"resourceBindingTier\": %u, "
                  "\"resourceHeapTier\": %u, \"enhancedBarriers\": %s, \"typedUavLoadAdditionalFormats\": %s}",
                  jsonEscape(adapter).c_str(), (unsigned long long)adapterLuid, jsonEscape(runtimePath).c_str(), runtimeVersion.c_str(),
                  shaderModel.c_str(), meshShaderTier, raytracingTier, bindingTier, heapTier, enhancedBarriers ? "true" : "false",
                  typedUavLoadAdditionalFormats ? "true" : "false");
}
} // namespace unx::host::probe
