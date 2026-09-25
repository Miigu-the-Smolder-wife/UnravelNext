// UnravelNext.dll: Unity native plugin entry points (I track). Unity calls UnityPluginLoad when it loads the DLL; the
// plugin keeps Unity's D3D12 interface (IUnityGraphicsD3D12v8) and serves the C ABI of UnravelNextHost.h.
//
// This file currently carries the host boundary probe (ARCHITECTURE 7.1-5) inside Unity's process: the same workload
// as the standalone gate (Probe.h), run on Unity's device either as a list on Unity's queue or on the renderer's own
// queue with fences, with timestamps on Unity's queue before and after the frame.
#include "unx/host/UnravelNextHost.h"

#include "Probe/Probe.h"
#include "Unity/PluginState.h"
#include "unx/render/D3D12.h"
#include "unx/render/Harness.h"

#include "IUnityGraphics.h"
#include "IUnityGraphicsD3D12.h"
#include "IUnityInterface.h"

#include <shellapi.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host::probe;
using render::check;
using render::ComPtr;

namespace
{
std::mutex g_gate;
IUnityInterfaces* g_interfaces = nullptr;
IUnityGraphics* g_graphics = nullptr;
IUnityGraphicsD3D12v8* g_d3d = nullptr;
int32_t g_eventBase = -1;
thread_local std::string g_lastError;
std::string g_facts;
} // namespace

namespace unx::host::plugin
{
int32_t failWith(const char* message, int32_t code)
{
    g_lastError = message;
    std::string line = std::string("UnravelNext: ") + message + "\n";
    OutputDebugStringA(line.c_str());
    std::fwrite(line.data(), 1, line.size(), stderr);
    std::fflush(stderr);
    return code;
}
const std::string& lastError() { return g_lastError; }
IUnityGraphicsD3D12v8* unityD3D12() { return g_d3d; }
} // namespace unx::host::plugin

namespace
{
using unx::host::plugin::failWith;

template <typename F>
int32_t guarded(F&& f)
{
    try
    {
        return f();
    }
    catch (const std::exception& e)
    {
        return failWith(e.what());
    }
}

int32_t copyOut(const std::string& text, char* out, uint32_t capacity, uint32_t* required)
{
    if (required) *required = (uint32_t)text.size() + 1;
    if (!out || capacity < text.size() + 1) return UNX_ERROR_BUFFER;
    std::memcpy(out, text.c_str(), text.size() + 1);
    return UNX_OK;
}

std::filesystem::path moduleDirectory()
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&moduleDirectory), &self);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

// ---- Probe inside Unity ---------------------------------------------------------------------------------------------

struct Samples
{
    std::vector<double> period, work, gapBefore, afterWork, cpuWork;
};

class UnityProbe
{
public:
    static constexpr uint32_t kSlots = 3;   // list sets in flight (Unity queues up to 2 frames + the one recording)
    static constexpr uint32_t kOutputs = 2;
    static constexpr uint32_t kPrimeFrames = 4;  // first frames always use Unity's queue so Unity's tracked output state
                                                 // is its own read state before the own-queue protocol takes over

    explicit UnityProbe(const UnxProbeSettings& s) : m_settings(s)
    {
        ID3D12Device* device = g_d3d->GetDevice();
        m_unityQueue = g_d3d->GetCommandQueue();
        if (!m_unityQueue) fail("Unity's command queue is not accessible (plugin event without queue access)");
        const std::u8string shaderDirectory(reinterpret_cast<const char8_t*>(s.shaderDirectory));
        m_gpu = std::make_unique<ProbeGpu>(device, std::filesystem::path(shaderDirectory));
        WorkDesc desc;
        desc.width = s.width;
        desc.height = s.height;
        m_work = std::make_unique<ProbeWork>(*m_gpu, desc);
        for (uint32_t o = 0; o < kOutputs; ++o)
        {
            m_outputs[o] = reinterpret_cast<ID3D12Resource*>(s.outputs[o]);
            const D3D12_RESOURCE_DESC d = m_outputs[o]->GetDesc();
            if (d.Width != s.width || d.Height != s.height || !(d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
                fail("probe output %u is %llux%u flags 0x%x; needs %ux%u with random write", o, (unsigned long long)d.Width, d.Height, (unsigned)d.Flags, s.width, s.height);
            m_outputUav[o] = m_gpu->uav(m_outputs[o], DXGI_FORMAT_R10G10B10A2_UNORM);
        }
        for (uint32_t i = 0; i < kSlots; ++i)
            for (uint32_t k = 0; k < 3; ++k)
            {
                check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_alloc[i][k])), "probe allocator");
                check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_alloc[i][k].Get(), nullptr, IID_PPV_ARGS(&m_list[i][k])), "probe list");
                m_list[i][k]->Close();
            }
        D3D12_QUERY_HEAP_DESC q{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 4 * kSlots, 0 };
        check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&m_queries)), "probe query heap");
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = 4 * kSlots * 8;
        b.Height = 1;
        b.DepthOrArraySize = 1;
        b.MipLevels = 1;
        b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&m_readback)), "probe readback");
        D3D12_RANGE all{ 0, (SIZE_T)b.Width };
        check(m_readback->Map(0, &all, (void**)&m_mapped), "probe readback map");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_unityFence)), "probe fence");
        check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_ownFence)), "probe own fence");
        m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (s.mode == UNX_PROBE_OWN_QUEUE)
        {
            D3D12_COMMAND_QUEUE_DESC d{};
            d.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            d.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
            check(device->CreateCommandQueue(&d, IID_PPV_ARGS(&m_ownQueue)), "probe own queue");
            m_ownQueue->SetName(L"UnravelNext probe queue");
            m_ownClock.calibrate(m_ownQueue.Get());
        }
        m_unityClock.calibrate(m_unityQueue);
        if (s.raiseUnityQueuePriority)
        {
            ComPtr<ID3D12CommandQueue1> q1;
            if (FAILED(m_unityQueue->QueryInterface(IID_PPV_ARGS(&q1)))) fail("Unity's queue has no ID3D12CommandQueue1 (priority cannot be raised)");
            q1->GetProcessPriority(&m_previousPriority);
            check(q1->SetProcessPriority(D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_HIGH), "SetProcessPriority(HIGH) on Unity's queue");
            m_raised = true;
        }
        m_start = std::chrono::steady_clock::now();
    }

    ~UnityProbe()
    {
        waitCpu(m_unityValue);
        if (m_raised)
        {
            ComPtr<ID3D12CommandQueue1> q1;
            if (SUCCEEDED(m_unityQueue->QueryInterface(IID_PPV_ARGS(&q1)))) q1->SetProcessPriority(m_previousPriority);
        }
        if (m_event) CloseHandle(m_event);
    }

    uint32_t measured() const { return (uint32_t)m_samples.work.size(); }
    bool finished() const { return m_samples.work.size() >= m_settings.frames; }

    void before(uint64_t frame)
    {
        const uint32_t slot = (uint32_t)(frame % kSlots);
        collect(slot);
        m_frame = frame;
        m_slotFrame[slot] = frame;
        if (!m_warm && std::chrono::duration<double>(std::chrono::steady_clock::now() - m_start).count() >= m_settings.warmupSeconds) m_warm = true;
        ID3D12GraphicsCommandList7* cmd = begin(slot, 0);
        cmd->EndQuery(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 4 + 0);
        check(cmd->Close(), "Close");
        executeOnUnityQueue(cmd);
    }

    void work(uint64_t frame)
    {
        if (frame != m_frame) fail("probe work event for frame %llu after before event of %llu", (unsigned long long)frame, (unsigned long long)m_frame);
        const auto cpu0 = std::chrono::steady_clock::now();
        const uint32_t slot = (uint32_t)(frame % kSlots), o = (uint32_t)(frame % kOutputs);
        const bool own = m_settings.mode == UNX_PROBE_OWN_QUEUE && m_primed >= kPrimeFrames;
        ID3D12GraphicsCommandList7* cmd = begin(slot, 1);
        if (own) transition(cmd, m_outputs[o], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd->EndQuery(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 4 + 1);
        m_work->record(cmd, m_outputUav[o]);
        globalUavBarrier(cmd);
        cmd->EndQuery(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 4 + 2);
        if (own) transition(cmd, m_outputs[o], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        check(cmd->Close(), "Close");
        if (own)
        {
            // Write-after-read: Unity's blit of the frame that used this output before must be done.
            if (m_outputDone[o]) check(m_ownQueue->Wait(m_unityFence.Get(), m_outputDone[o]), "own queue Wait");
            ID3D12CommandList* lists[] = { cmd };
            m_ownQueue->ExecuteCommandLists(1, lists);
            check(m_ownQueue->Signal(m_ownFence.Get(), ++m_ownValue), "own queue Signal");
            check(m_unityQueue->Wait(m_ownFence.Get(), m_ownValue), "Unity queue Wait");
        }
        else
        {
            UnityGraphicsD3D12ResourceState state{ m_outputs[o], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
            g_d3d->ExecuteCommandList(cmd, 1, &state);
            ++m_primed;
        }
        m_slotOwn[slot] = own;
        m_slotMeasured[slot] = m_warm && (m_settings.mode != UNX_PROBE_OWN_QUEUE || own);
        m_cpuWork = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpu0).count();
    }

    void after(uint64_t frame)
    {
        if (frame != m_frame) fail("probe after event for frame %llu after before event of %llu", (unsigned long long)frame, (unsigned long long)m_frame);
        const uint32_t slot = (uint32_t)(frame % kSlots), o = (uint32_t)(frame % kOutputs);
        ID3D12GraphicsCommandList7* cmd = begin(slot, 2);
        cmd->EndQuery(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 4 + 3);
        cmd->ResolveQueryData(m_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 4, 4, m_readback.Get(), slot * 32ull);
        check(cmd->Close(), "Close");
        executeOnUnityQueue(cmd);
        check(m_unityQueue->Signal(m_unityFence.Get(), ++m_unityValue), "Unity queue Signal");
        m_slotDone[slot] = m_unityValue;
        m_outputDone[o] = m_unityValue;
        m_slotCpuWork[slot] = m_cpuWork;
    }

    std::string results() const
    {
        std::string j = "{";
        j += format("\"mode\": \"%s\", \"width\": %u, \"height\": %u, \"frames\": %u, \"warmupSeconds\": %.2f, \"unityQueuePriorityRaised\": %s, ",
                    m_settings.mode == UNX_PROBE_OWN_QUEUE ? "own_queue" : "unity_queue", m_settings.width, m_settings.height, m_settings.frames,
                    m_settings.warmupSeconds, m_raised ? "true" : "false");
        j += "\"periodMs\": " + dist(m_samples.period) + ", \"workMs\": " + dist(m_samples.work) + ", \"gapBeforeWorkMs\": " + dist(m_samples.gapBefore) +
             ", \"afterWorkToAfterMs\": " + dist(m_samples.afterWork) + ", \"cpuWorkEventMs\": " + dist(m_samples.cpuWork) + "}";
        return j;
    }

private:
    static std::string dist(const std::vector<double>& v)
    {
        const render::Distribution d = render::Distribution::of(v);
        return format("{\"count\": %u, \"median\": %.5f, \"p95\": %.5f, \"p99\": %.5f, \"mean\": %.5f, \"min\": %.5f, \"max\": %.5f}", d.count, d.median, d.p95,
                      d.p99, d.mean, d.min, d.max);
    }

    ID3D12GraphicsCommandList7* begin(uint32_t slot, uint32_t k)
    {
        check(m_alloc[slot][k]->Reset(), "probe allocator Reset");
        check(m_list[slot][k]->Reset(m_alloc[slot][k].Get(), nullptr), "probe list Reset");
        return m_list[slot][k].Get();
    }

    void executeOnUnityQueue(ID3D12GraphicsCommandList* cmd)
    {
        // Timestamp-only lists touch no Unity resource, so they go straight to Unity's queue (this event has queue
        // access and runs on Unity's submission thread after Unity flushed its own command buffers).
        ID3D12CommandList* lists[] = { cmd };
        m_unityQueue->ExecuteCommandLists(1, lists);
    }

    static void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = from;
        b.Transition.StateAfter = to;
        cmd->ResourceBarrier(1, &b);
    }

    void waitCpu(uint64_t value)
    {
        if (!value || m_unityFence->GetCompletedValue() >= value) return;
        check(m_unityFence->SetEventOnCompletion(value, m_event), "SetEventOnCompletion");
        WaitForSingleObject(m_event, 10000);
    }

    // Reads the timestamps of the frame that last used this slot (its lists are about to be reset).
    void collect(uint32_t slot)
    {
        if (!m_slotDone[slot]) return;
        waitCpu(m_slotDone[slot]);
        if (m_slotMeasured[slot] && !finished())
        {
            const uint64_t* t = m_mapped + slot * 4;
            const QueueClock& workClock = m_slotOwn[slot] ? m_ownClock : m_unityClock;
            const double before = m_unityClock.toMs(t[0]), workBegin = workClock.toMs(t[1]), workEnd = workClock.toMs(t[2]), after = m_unityClock.toMs(t[3]);
            m_samples.work.push_back(workEnd - workBegin);
            m_samples.gapBefore.push_back(workBegin - before);
            m_samples.afterWork.push_back(after - workEnd);
            m_samples.cpuWork.push_back(m_slotCpuWork[slot]);
            if (m_haveAfter && m_slotFrame[slot] == m_lastFrame + 1) m_samples.period.push_back(after - m_lastAfter);
            m_lastAfter = after;
            m_lastFrame = m_slotFrame[slot];
            m_haveAfter = true;
        }
        m_slotDone[slot] = 0;
    }

    UnxProbeSettings m_settings;
    ID3D12CommandQueue* m_unityQueue = nullptr;
    std::unique_ptr<ProbeGpu> m_gpu;
    std::unique_ptr<ProbeWork> m_work;
    ID3D12Resource* m_outputs[kOutputs] = {};
    uint32_t m_outputUav[kOutputs] = {};
    ComPtr<ID3D12CommandAllocator> m_alloc[kSlots][3];
    ComPtr<ID3D12GraphicsCommandList7> m_list[kSlots][3];
    ComPtr<ID3D12QueryHeap> m_queries;
    ComPtr<ID3D12Resource> m_readback;
    const uint64_t* m_mapped = nullptr;
    ComPtr<ID3D12Fence> m_unityFence, m_ownFence;
    ComPtr<ID3D12CommandQueue> m_ownQueue;
    HANDLE m_event = nullptr;
    uint64_t m_unityValue = 0, m_ownValue = 0;
    uint64_t m_slotDone[kSlots] = {}, m_slotFrame[kSlots] = {}, m_outputDone[kOutputs] = {};
    bool m_slotMeasured[kSlots] = {}, m_slotOwn[kSlots] = {};
    double m_slotCpuWork[kSlots] = {}, m_cpuWork = 0;
    QueueClock m_unityClock, m_ownClock;
    uint64_t m_frame = UINT64_MAX, m_lastFrame = 0;
    double m_lastAfter = 0;
    bool m_haveAfter = false, m_warm = false, m_raised = false;
    uint32_t m_primed = 0;
    D3D12_COMMAND_QUEUE_PROCESS_PRIORITY m_previousPriority = D3D12_COMMAND_QUEUE_PROCESS_PRIORITY_NORMAL;
    std::chrono::steady_clock::time_point m_start;
    Samples m_samples;
};

std::unique_ptr<UnityProbe> g_probe;
std::unique_ptr<UnxProbeSettings> g_pendingProbe;  // created on the submission thread at the next event
std::string g_probeResults;

// Facts about Unity's device and queue, collected on the submission thread (queue access).
std::string collectFacts()
{
    ID3D12Device* device = g_d3d->GetDevice();
    ID3D12CommandQueue* queue = g_d3d->GetCommandQueue();
    std::string j = "{";
    j += "\"device\": " + readDeviceFacts(device).toJson();
    if (queue)
    {
        const D3D12_COMMAND_QUEUE_DESC d = queue->GetDesc();
        j += format(", \"unityQueue\": {\"type\": %d, \"priority\": %d, \"flags\": %d", (int)d.Type, (int)d.Priority, (int)d.Flags);
        ComPtr<ID3D12CommandQueue1> q1;
        if (SUCCEEDED(queue->QueryInterface(IID_PPV_ARGS(&q1))))
        {
            D3D12_COMMAND_QUEUE_PROCESS_PRIORITY pp{};
            D3D12_COMMAND_QUEUE_GLOBAL_PRIORITY gp{};
            const HRESULT hp = q1->GetProcessPriority(&pp), hg = q1->GetGlobalPriority(&gp);
            j += format(", \"commandQueue1\": true, \"processPriority\": %d, \"processPriorityHr\": \"0x%08X\", \"globalPriority\": %d, \"globalPriorityHr\": \"0x%08X\"}",
                        (int)pp, (unsigned)hp, (int)gp, (unsigned)hg);
        }
        else
            j += ", \"commandQueue1\": false}";
        uint64_t freq = 0;
        queue->GetTimestampFrequency(&freq);
        j += format(", \"unityQueueTimestampHz\": %llu", (unsigned long long)freq);
    }
    else
        j += ", \"unityQueue\": null";
    // Singleton: a D3D12CreateDevice on Unity's adapter returns Unity's device (so an engine Device made with
    // D3D12CreateDevice inside Unity attaches to Unity's device).
    const LUID luid = device->GetAdapterLuid();
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
    {
        ComPtr<IUnknown> unityUnknown;
        device->QueryInterface(IID_PPV_ARGS(&unityUnknown));
        for (D3D_FEATURE_LEVEL fl : { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_12_2 })
        {
            ComPtr<ID3D12Device> again;
            const HRESULT hr = D3D12CreateDevice(adapter.Get(), fl, IID_PPV_ARGS(&again));
            ComPtr<IUnknown> againUnknown;
            if (again) again->QueryInterface(IID_PPV_ARGS(&againUnknown));
            j += format(", \"createDeviceFl%s\": {\"hr\": \"0x%08X\", \"sameAsUnity\": %s}", fl == D3D_FEATURE_LEVEL_11_0 ? "11_0" : "12_2", (unsigned)hr,
                        againUnknown && againUnknown.Get() == unityUnknown.Get() ? "true" : "false");
        }
        // Independent device (ID3D12DeviceFactory) with the runtime Unity loaded, and with the engine's own runtime if
        // it is deployed next to this DLL.
        ComPtr<ID3D12SDKConfiguration1> config;
        HRESULT hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&config));
        j += format(", \"sdkConfiguration1Hr\": \"0x%08X\"", (unsigned)hr);
        if (config)
        {
            const std::string own = (moduleDirectory() / "D3D12").string() + "\\";
            for (const std::string& path : { std::string(".\\D3D12\\"), own })
            {
                ComPtr<ID3D12DeviceFactory> df;
                HRESULT h1 = config->CreateDeviceFactory(D3D12_SDK_VERSION, path.c_str(), IID_PPV_ARGS(&df));
                HRESULT h2 = E_FAIL;
                if (df)
                {
                    df->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON);
                    ComPtr<ID3D12Device> independent;
                    h2 = df->CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&independent));
                }
                j += format(", \"independentDevice%s\": {\"path\": \"%s\", \"factoryHr\": \"0x%08X\", \"createHr\": \"0x%08X\"}",
                            path == own ? "Own" : "Unity", jsonEscape(path).c_str(), (unsigned)h1, (unsigned)h2);
            }
        }
    }
    // Extra queues on Unity's device (async/copy ownership).
    for (auto [type, name] : { std::pair{ D3D12_COMMAND_LIST_TYPE_DIRECT, "direct" }, std::pair{ D3D12_COMMAND_LIST_TYPE_COMPUTE, "compute" },
                               std::pair{ D3D12_COMMAND_LIST_TYPE_COPY, "copy" } })
    {
        D3D12_COMMAND_QUEUE_DESC d{};
        d.Type = type;
        d.Priority = type == D3D12_COMMAND_LIST_TYPE_COPY ? D3D12_COMMAND_QUEUE_PRIORITY_NORMAL : D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        ComPtr<ID3D12CommandQueue> q;
        const HRESULT hr = device->CreateCommandQueue(&d, IID_PPV_ARGS(&q));
        j += format(", \"ownQueue_%s\": \"0x%08X\"", name, (unsigned)hr);
    }
    // Swap chain (Player only; the editor returns null).
    if (IDXGISwapChain* sc = g_d3d->GetSwapChain())
    {
        ComPtr<IDXGISwapChain1> sc1;
        DXGI_SWAP_CHAIN_DESC1 d{};
        if (SUCCEEDED(sc->QueryInterface(IID_PPV_ARGS(&sc1)))) sc1->GetDesc1(&d);
        j += format(", \"swapChain\": {\"width\": %u, \"height\": %u, \"format\": %d, \"buffers\": %u, \"swapEffect\": %d, \"flags\": %u, \"bufferUsage\": %u, "
                    "\"syncInterval\": %u, \"presentFlags\": %u}",
                    d.Width, d.Height, (int)d.Format, d.BufferCount, (int)d.SwapEffect, d.Flags, (unsigned)d.BufferUsage, g_d3d->GetSyncInterval(),
                    g_d3d->GetPresentFlags());
    }
    else
        j += ", \"swapChain\": null";
    j += "}";
    return j;
}

void UNITY_INTERFACE_API onDeviceEvent(UnityGfxDeviceEventType type)
{
    std::lock_guard lock(g_gate);
    if (type == kUnityGfxDeviceEventInitialize || type == kUnityGfxDeviceEventAfterReset)
    {
        g_d3d = g_graphics && g_graphics->GetRenderer() == kUnityGfxRendererD3D12 ? g_interfaces->Get<IUnityGraphicsD3D12v8>() : nullptr;
        if (g_d3d)
        {
            // Queue access on Unity's submission thread, after Unity flushed its command buffers: the probe (and later
            // the renderer) records its own lists and executes them on Unity's queue in submission order.
            UnityD3D12PluginEventConfig config{};
            config.graphicsQueueAccess = kUnityD3D12GraphicsQueueAccess_Allow;
            config.flags = kUnityD3D12EventConfigFlag_FlushCommandBuffers;
            config.ensureActiveRenderTextureIsBound = false;
            for (int32_t e = 0; e < UNX_EVENT_COUNT; ++e) g_d3d->ConfigureEvent(g_eventBase + e, &config);
        }
    }
    else if (type == kUnityGfxDeviceEventShutdown || type == kUnityGfxDeviceEventBeforeReset)
    {
        g_probe.reset();
        unx::host::plugin::destroyDeviceRenderers();
        g_d3d = nullptr;
    }
}

void UNITY_INTERFACE_API onRenderEvent(int eventId, void* data)
{
    std::lock_guard lock(g_gate);
    const int32_t e = eventId - g_eventBase;
    const uint64_t frame = (uint64_t)(uintptr_t)data;
    try
    {
        if (!g_d3d) return;
        if (e == UNX_EVENT_RENDER)
        {
            unx::host::plugin::renderEvent(frame);
            return;
        }
        if (e == UNX_EVENT_PROBE_FACTS)
        {
            g_facts = collectFacts();
            return;
        }
        if (g_pendingProbe && e == UNX_EVENT_PROBE_BEFORE)
        {
            g_probe = std::make_unique<UnityProbe>(*g_pendingProbe);
            g_pendingProbe.reset();
        }
        if (!g_probe) return;
        if (e == UNX_EVENT_PROBE_BEFORE) g_probe->before(frame);
        else if (e == UNX_EVENT_PROBE_WORK) g_probe->work(frame);
        else if (e == UNX_EVENT_PROBE_AFTER)
        {
            g_probe->after(frame);
            if (g_probe->finished() && g_probeResults.empty()) g_probeResults = g_probe->results();
        }
    }
    catch (const std::exception& ex)
    {
        g_probeResults = std::string("{\"error\": \"") + jsonEscape(ex.what()) + "\"}";
        g_probe.reset();
        failWith(ex.what());
    }
}
} // namespace

namespace
{
// "-unxLogFile <path>" on the host's command line (Player or editor): the renderer's log (unx::logf; stderr is not
// captured by Unity) also goes to that file, e.g. track statistics lines (reflection.stats_log_frames).
void openLogFileFromCommandLine()
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return;
    for (int i = 0; i + 1 < argc; ++i)
    {
        if (std::wstring(argv[i]) != L"-unxLogFile") continue;
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, nullptr, 0, nullptr, nullptr);
        std::string path(bytes > 0 ? (size_t)bytes - 1 : 0, '\0');
        if (bytes > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i + 1], -1, path.data(), bytes, nullptr, nullptr);
        if (!path.empty()) unx::logOpenFile(path);
        break;
    }
    LocalFree(argv);
}
} // namespace

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* interfaces)
{
    openLogFileFromCommandLine();
    // Inside Unity a device removal must not end the process (v1.27): renderer calls report UNX_DEVICE_REMOVED instead.
    unx::render::setDeviceRemovedPolicy(unx::render::DeviceRemovedPolicy::Throw);
    g_interfaces = interfaces;
    g_graphics = interfaces->Get<IUnityGraphics>();
    g_eventBase = g_graphics->ReserveEventIDRange(UNX_EVENT_COUNT);
    g_graphics->RegisterDeviceEventCallback(onDeviceEvent);
    onDeviceEvent(kUnityGfxDeviceEventInitialize);
}

extern "C" void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
    if (g_graphics) g_graphics->UnregisterDeviceEventCallback(onDeviceEvent);
    std::lock_guard lock(g_gate);
    g_probe.reset();
    unx::host::plugin::destroyDeviceRenderers();
    g_d3d = nullptr;
    g_graphics = nullptr;
    g_interfaces = nullptr;
}

UNX_API uint32_t UNX_CALL UnxAbiVersion(void) { return UNX_ABI_VERSION; }
UNX_API const char* UNX_CALL UnxLastError(void) { return g_lastError.c_str(); }
UNX_API void* UNX_CALL UnxRenderEventFunc(void) { return reinterpret_cast<void*>(&onRenderEvent); }
UNX_API int32_t UNX_CALL UnxEventBase(void) { return g_eventBase; }

UNX_API int32_t UNX_CALL UnxProbeFacts(char* json, uint32_t capacity, uint32_t* required)
{
    std::lock_guard lock(g_gate);
    if (!g_d3d) return failWith("Unity's D3D12 device is not available", UNX_ERROR_NO_DEVICE);
    if (g_facts.empty()) return failWith("no facts yet: issue UNX_EVENT_PROBE_FACTS and render one frame");
    return copyOut(g_facts, json, capacity, required);
}

UNX_API int32_t UNX_CALL UnxProbeStart(const UnxProbeSettings* settings)
{
    std::lock_guard lock(g_gate);
    return guarded([&] {
        if (!settings || settings->size != sizeof(UnxProbeSettings) || settings->version != 1) return failWith("UnxProbeSettings ABI mismatch", UNX_ERROR_ABI);
        if (!g_d3d) return failWith("Unity's D3D12 device is not available", UNX_ERROR_NO_DEVICE);
        if (settings->mode != UNX_PROBE_UNITY_QUEUE && settings->mode != UNX_PROBE_OWN_QUEUE) return failWith("unknown probe mode");
        if (!((settings->width == 3840 && settings->height == 2160) || (settings->width == 2560 && settings->height == 1440)))
            return failWith("probe measures at 4K or 1440p only (ARCHITECTURE 6)");
        if (!settings->outputs[0] || !settings->outputs[1]) return failWith("probe needs two output textures");
        g_probe.reset();
        g_probeResults.clear();
        g_pendingProbe = std::make_unique<UnxProbeSettings>(*settings);
        return (int32_t)UNX_OK;
    });
}

UNX_API int32_t UNX_CALL UnxProbeProgress(uint32_t* measured, uint32_t* finished)
{
    std::lock_guard lock(g_gate);
    if (measured) *measured = g_probe ? g_probe->measured() : 0;
    if (finished) *finished = !g_probeResults.empty() ? 1 : 0;
    return UNX_OK;
}

UNX_API int32_t UNX_CALL UnxProbeResults(char* json, uint32_t capacity, uint32_t* required)
{
    std::lock_guard lock(g_gate);
    if (g_probeResults.empty()) return failWith("probe has no results yet");
    const int32_t r = copyOut(g_probeResults, json, capacity, required);
    return r;
}

UNX_API int32_t UNX_CALL UnxProbeStop(void)
{
    std::lock_guard lock(g_gate);
    return guarded([&] {
        g_probe.reset();
        g_pendingProbe.reset();
        g_probeResults.clear();
        return (int32_t)UNX_OK;
    });
}
