#include "unx/render/Device.h"

#include <nvapi.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#pragma comment(lib, "version.lib")

namespace unx::render
{
const char* queueName(QueueType q)
{
    switch (q)
    {
    case QueueType::Graphics: return "graphics";
    case QueueType::Compute: return "compute";
    case QueueType::Copy: return "copy";
    }
    return "?";
}

namespace
{
D3D12_COMMAND_LIST_TYPE listType(QueueType q)
{
    switch (q)
    {
    case QueueType::Graphics: return D3D12_COMMAND_LIST_TYPE_DIRECT;
    case QueueType::Compute: return D3D12_COMMAND_LIST_TYPE_COMPUTE;
    case QueueType::Copy: return D3D12_COMMAND_LIST_TYPE_COPY;
    }
    return D3D12_COMMAND_LIST_TYPE_DIRECT;
}

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

void CALLBACK debugMessage(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity, D3D12_MESSAGE_ID id, LPCSTR text, void* context)
{
    if (severity > D3D12_MESSAGE_SEVERITY_WARNING) return;
    const char* level = severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ? "CORRUPTION" : severity == D3D12_MESSAGE_SEVERITY_ERROR ? "ERROR" : "WARNING";
    logf("[d3d12 %s %d] %s\n", level, (int)id, text);
    if (severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++*static_cast<uint32_t*>(context);
}
} // namespace

Queue::Queue(ID3D12Device* device, QueueType type, D3D12_COMMAND_QUEUE_PRIORITY priority) : m_type(type)
{
    D3D12_COMMAND_QUEUE_DESC d{};
    d.Type = listType(type);
    d.Priority = type == QueueType::Copy ? D3D12_COMMAND_QUEUE_PRIORITY_NORMAL : priority;
    check(device->CreateCommandQueue(&d, IID_PPV_ARGS(&m_queue)), "CreateCommandQueue");
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (type != QueueType::Copy) check(m_queue->GetTimestampFrequency(&m_timestampFrequency), "GetTimestampFrequency");
    std::wstring name = type == QueueType::Graphics ? L"unx graphics" : type == QueueType::Compute ? L"unx compute" : L"unx copy";
    m_queue->SetName(name.c_str());
}

Queue::Queue(ID3D12Device* device, QueueType type, ID3D12CommandQueue* external) : m_type(type)
{
    if (external->GetDesc().Type != listType(type)) fail("external queue: type %d does not match queue type %d", (int)external->GetDesc().Type, (int)listType(type));
    m_queue = external;
    check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)), "CreateFence");
    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (type != QueueType::Copy) check(m_queue->GetTimestampFrequency(&m_timestampFrequency), "GetTimestampFrequency");
}

void Queue::setExecuteHook(std::function<void(ID3D12CommandList*)> hook)
{
    std::lock_guard lock(m_mutex);
    m_executeHook = std::move(hook);
}

uint64_t Queue::signal()
{
    std::lock_guard lock(m_mutex);
    ++m_lastSignaled;
    const HRESULT hr = m_queue->Signal(m_fence.Get(), m_lastSignaled);
    if (FAILED(hr))
    {
        if (!isDeviceRemoved(hr)) check(hr, "Signal");
        noteDeviceRemoved("Queue::signal", hr);  // (Throw: returns; waits then return at once)
    }
    return m_lastSignaled;
}

void Queue::waitCpu(uint64_t value)
{
    const uint64_t done = m_fence->GetCompletedValue();
    if (done == UINT64_MAX)  // a removed device's fence
    {
        noteDeviceRemoved("Queue::waitCpu", DXGI_ERROR_DEVICE_REMOVED);
        return;
    }
    if (done >= value || deviceWasRemoved()) return;
    const HRESULT hr = m_fence->SetEventOnCompletion(value, m_event);
    if (FAILED(hr))
    {
        if (!isDeviceRemoved(hr)) check(hr, "SetEventOnCompletion");
        noteDeviceRemoved("Queue::waitCpu", hr);
        return;
    }
    WaitForSingleObject(m_event, INFINITE);
    if (m_fence->GetCompletedValue() == UINT64_MAX) noteDeviceRemoved("Queue::waitCpu", DXGI_ERROR_DEVICE_REMOVED);
}

void Queue::waitGpu(const Queue& other, uint64_t value)
{
    if (value == 0) return;
    check(m_queue->Wait(other.m_fence.Get(), value), "Queue::Wait");
}

void Queue::execute(ID3D12CommandList* list)
{
    std::function<void(ID3D12CommandList*)> hook;
    {
        std::lock_guard lock(m_mutex);
        hook = m_executeHook;
    }
    if (hook) hook(list);
    else m_queue->ExecuteCommandLists(1, &list);
}

namespace
{
ID3D12Device* g_reasonDevice = nullptr;  // the first device created: GetDeviceRemovedReason for deviceRemoved()
DeviceRemovedPolicy g_removedPolicy = DeviceRemovedPolicy::Exit;
bool g_policySet = false;
std::atomic<bool> g_removed{ false };

std::string removedLine(const char* what, HRESULT hr, HRESULT reason)
{
    char b[256];
    std::snprintf(b, sizeof b, "UNX_DEVICE_REMOVED %s hr 0x%08X reason 0x%08X", what, (unsigned)hr, (unsigned)reason);
    return b;
}

[[noreturn]] void exitRemoved(const std::string& line)
{
    std::fflush(stdout);
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fprintf(stdout, "%s\n", line.c_str());
    std::fflush(stderr);
    std::fflush(stdout);
    std::_Exit(kDeviceRemovedExitCode);
}
} // namespace

void setDeviceRemovedPolicy(DeviceRemovedPolicy policy)
{
    g_removedPolicy = policy;
    g_policySet = true;
}
DeviceRemovedPolicy deviceRemovedPolicy() { return g_removedPolicy; }
bool deviceWasRemoved() { return g_removed.load(); }

void deviceRemoved(const char* what, HRESULT hr)
{
    const HRESULT reason = g_reasonDevice ? g_reasonDevice->GetDeviceRemovedReason() : S_OK;
    const std::string line = removedLine(what, hr, reason);
    if (g_removedPolicy == DeviceRemovedPolicy::Exit) exitRemoved(line);
    if (!g_removed.exchange(true)) logf("%s\n", line.c_str());
    throw DeviceRemovedError(line, what, hr, reason);
}

void noteDeviceRemoved(const char* what, HRESULT hr)
{
    const HRESULT reason = g_reasonDevice ? g_reasonDevice->GetDeviceRemovedReason() : S_OK;
    const std::string line = removedLine(what, hr, reason);
    if (g_removedPolicy == DeviceRemovedPolicy::Exit) exitRemoved(line);
    if (!g_removed.exchange(true)) logf("%s\n", line.c_str());
}

Device::Device(const DeviceOptions& options) : m_options(options)
{
    if (m_options.gpuValidation) m_options.debugLayer = true;
    const bool external = m_options.externalDevice != nullptr;
    if (external && m_options.debugLayer) fail("DeviceOptions: the debug layer cannot be enabled on an external (host) device");
    if (m_options.externalGraphicsQueue && !external) fail("DeviceOptions: externalGraphicsQueue needs externalDevice");
    if (m_options.debugLayer)
    {
        ComPtr<ID3D12Debug1> debug;
        check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12GetDebugInterface (debug layer)");
        debug->EnableDebugLayer();
        if (m_options.gpuValidation) debug->SetEnableGPUBasedValidation(TRUE);
    }
    check(CreateDXGIFactory2(m_options.debugLayer ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&m_factory)), "CreateDXGIFactory2");
    auto describeAdapter = [&](IDXGIAdapter4* a) {
        DXGI_ADAPTER_DESC3 d{};
        a->GetDesc3(&d);
        m_caps.adapter = narrow(d.Description);
        m_caps.vramBytes = d.DedicatedVideoMemory;
        LARGE_INTEGER umd{};
        if (SUCCEEDED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd)))
        {
            char v[64];
            std::snprintf(v, sizeof v, "%u.%u.%u.%u", HIWORD(umd.HighPart), LOWORD(umd.HighPart), HIWORD(umd.LowPart), LOWORD(umd.LowPart));
            m_caps.driver = v;
        }
    };
    if (external)
    {
        check(m_options.externalDevice->QueryInterface(IID_PPV_ARGS(&m_device)), "external device: ID3D12Device10");
        check(m_factory->EnumAdapterByLuid(m_device->GetAdapterLuid(), IID_PPV_ARGS(&m_adapter)), "external device: adapter by LUID");
        describeAdapter(m_adapter.Get());
    }
    for (UINT i = 0; !external; ++i)
    {
        ComPtr<IDXGIAdapter4> a;
        if (FAILED(m_factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&a)))) break;
        DXGI_ADAPTER_DESC3 d{};
        a->GetDesc3(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG3_SOFTWARE) continue;
        if (FAILED(D3D12CreateDevice(a.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&m_device)))) continue;
        m_adapter = a;
        describeAdapter(a.Get());
        break;
    }
    if (!m_device) fail("no hardware adapter supports D3D12 feature level 12_2");
    if (!g_reasonDevice)
    {
        g_reasonDevice = m_device.Get();
        if (!g_policySet && m_options.externalDevice) g_removedPolicy = DeviceRemovedPolicy::Throw;  // a host's process
    }

    if (HMODULE core = GetModuleHandleW(L"D3D12Core.dll"))
    {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(core, path, MAX_PATH);
        m_caps.runtimePath = narrow(path);
        m_caps.runtimeVersion = fileVersion(path);
    }

    D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_9 };
    if (FAILED(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm)))
    {
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_8;
        check(m_device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm), "shader model query");
    }
    m_caps.shaderModel = sm.HighestShaderModel;
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    m_caps.bindingTier = o.ResourceBindingTier;
    m_caps.heapTier = o.ResourceHeapTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5{};
    m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof o5);
    m_caps.raytracingTier = o5.RaytracingTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 o7{};
    m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &o7, sizeof o7);
    m_caps.meshShaderTier = o7.MeshShaderTier;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12{};
    m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof o12);
    m_caps.enhancedBarriers = o12.EnhancedBarriersSupported;
    m_caps.relaxedFormatCasting = o12.RelaxedFormatCastingSupported;

    if (m_caps.shaderModel < D3D_SHADER_MODEL_6_6) fail("shader model 6.6 (bindless) is required");
    if (m_caps.meshShaderTier < D3D12_MESH_SHADER_TIER_1) fail("mesh shaders are required");
    if (m_caps.raytracingTier < D3D12_RAYTRACING_TIER_1_1) fail("DXR 1.1 is required");
    if (!m_caps.enhancedBarriers) fail("enhanced barriers are required");
    if (m_caps.bindingTier < D3D12_RESOURCE_BINDING_TIER_3) fail("resource binding tier 3 is required");
    if (m_caps.heapTier < D3D12_RESOURCE_HEAP_TIER_2) fail("resource heap tier 2 is required (transient aliasing of mixed resources)");

    if (m_options.debugLayer && SUCCEEDED(m_device.As(&m_infoQueue)))
    {
        DWORD cookie = 0;
        m_infoQueue->RegisterMessageCallback(debugMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, &m_debugErrors, &cookie);
    }

    if (NvAPI_Initialize() == NVAPI_OK)
    {
        m_caps.nvapi = true;
        NvAPI_ShortString iface = "", branch = "";
        NvU32 drv = 0;
        NvAPI_GetInterfaceVersionString(iface);
        NvAPI_SYS_GetDriverAndBranchVersion(&drv, branch);
        m_caps.nvapiInterface = iface;
        m_caps.nvapiBranch = branch;
        m_caps.nvapiDriver = drv;
        NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAPS omm = NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_NONE;
        NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS ser = NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_NONE;
        if (NvAPI_D3D12_GetRaytracingCaps(m_device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_OPACITY_MICROMAP, &omm, sizeof omm) == NVAPI_OK)
            m_caps.nvapiOpacityMicromap = (omm & NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_STANDARD) != 0;
        if (NvAPI_D3D12_GetRaytracingCaps(m_device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING, &ser, sizeof ser) == NVAPI_OK)
            m_caps.nvapiThreadReordering = (ser & NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD) != 0;
    }

    for (uint32_t q = 0; q < kQueueTypeCount; ++q)
        m_queues[q] = q == (uint32_t)QueueType::Graphics && m_options.externalGraphicsQueue
                          ? std::make_unique<Queue>(m_device.Get(), QueueType::Graphics, m_options.externalGraphicsQueue)
                          : std::make_unique<Queue>(m_device.Get(), (QueueType)q, m_options.queuePriority);
    m_descriptors = std::make_unique<DescriptorHeaps>(m_device.Get());

    D3D12_ROOT_PARAMETER1 params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = { 0, 0, kRootConstantCount };
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor = { 1, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE };
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samplers[6]{};
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
    sampler(5, D3D12_FILTER_ANISOTROPIC, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, 16);
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC rs{};
    rs.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    rs.Desc_1_1.NumParameters = 2;
    rs.Desc_1_1.pParameters = params;
    rs.Desc_1_1.NumStaticSamplers = 6;
    rs.Desc_1_1.pStaticSamplers = samplers;
    rs.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeVersionedRootSignature(&rs, &blob, &error)))
        fail("root signature: %s", error ? (const char*)error->GetBufferPointer() : "?");
    check(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)), "CreateRootSignature");
}

Device::~Device()
{
    waitIdle();
    collectGarbage();
    drainDebugMessages();
}

CommandList Device::acquireCommandList(QueueType type)
{
    CommandList cl;
    {
        std::lock_guard lock(m_poolMutex);
        auto& pool = m_pool[(size_t)type];
        uint64_t done = m_queues[(size_t)type]->completed();
        for (size_t i = 0; i < pool.size(); ++i)
        {
            if (pool[i].retireValue <= done)
            {
                cl = std::move(pool[i]);
                pool.erase(pool.begin() + (ptrdiff_t)i);
                break;
            }
        }
    }
    if (!cl.list)
    {
        cl.queue = type;
        check(m_device->CreateCommandAllocator(listType(type), IID_PPV_ARGS(&cl.allocator)), "CreateCommandAllocator");
        check(m_device->CreateCommandList1(0, listType(type), D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(&cl.list)), "CreateCommandList1");
    }
    check(cl.allocator->Reset(), "CommandAllocator::Reset");
    check(cl.list->Reset(cl.allocator.Get(), nullptr), "CommandList::Reset");
    if (type != QueueType::Copy)
    {
        ID3D12DescriptorHeap* heaps[] = { m_descriptors->resourceHeap(), m_descriptors->samplerHeap() };
        cl.list->SetDescriptorHeaps(2, heaps);
        cl.list->SetComputeRootSignature(m_rootSignature.Get());
        if (type == QueueType::Graphics) cl.list->SetGraphicsRootSignature(m_rootSignature.Get());
    }
    return cl;
}

uint64_t Device::submit(CommandList& list)
{
    check(list.list->Close(), "CommandList::Close");
    Queue& q = *m_queues[(size_t)list.queue];
    q.execute(list.list.Get());
    list.retireValue = q.signal();
    uint64_t v = list.retireValue;
    recycle(std::move(list));
    return v;
}

void Device::recycle(CommandList&& list)
{
    std::lock_guard lock(m_poolMutex);
    m_pool[(size_t)list.queue].push_back(std::move(list));
}

void Device::deferRelease(ComPtr<ID3D12Pageable> object)
{
    if (!object) return;
    Deferred d;
    d.object = std::move(object);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) d.fence[q] = m_queues[q]->lastSignaled();
    std::lock_guard lock(m_garbageMutex);
    m_garbage.push_back(std::move(d));
}

void Device::deferCall(std::function<void()> call)
{
    Deferred d;
    d.call = std::move(call);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) d.fence[q] = m_queues[q]->lastSignaled();
    std::lock_guard lock(m_garbageMutex);
    m_garbage.push_back(std::move(d));
}

void Device::collectGarbage()
{
    std::vector<std::function<void()>> calls;
    {
        std::lock_guard lock(m_garbageMutex);
        while (!m_garbage.empty())
        {
            Deferred& d = m_garbage.front();
            bool done = true;
            for (uint32_t q = 0; q < kQueueTypeCount; ++q) done = done && m_queues[q]->completed() >= d.fence[q];
            if (!done) break;
            if (d.call) calls.push_back(std::move(d.call));
            m_garbage.pop_front();
        }
    }
    for (auto& c : calls) c();
}

void Device::waitIdle()
{
    for (auto& q : m_queues)
    {
        uint64_t v = q->signal();
        q->waitCpu(v);
    }
}

uint32_t Device::drainDebugMessages() { return m_debugErrors; }
} // namespace unx::render
