// Host boundary gate (ARCHITECTURE 7.1-5, I track), standalone part. Measures the same frame workload through the four
// boundaries of Probe.h on this machine without Unity: a DIRECT "host queue" stands in for Unity's queue, with the
// host's own work before (one small pass) and after the frame (a full-screen copy of the output, like Unity's
// presentation blit). Reports per boundary the steady-state GPU frame period, the frame's own duration and the GPU gaps
// the boundary adds, at 4K and 1440p. The in-Unity part (Unity's real queue and submission thread) runs in the
// UnravelNext.dll plugin with the same workload.
//
//   GpuLock.ps1 -Track I -- build/I/bin/unx_gate_host_hostboundary.exe --resolution 4K [--frames 600] [--reps 3]
//        [--modes fused_list,host_queue_lists,own_queue,independent_device] [--host-priority high|normal] [--out dir]
#include "Probe/Probe.h"

#include "unx/core/File.h"
#include "unx/render/GpuLock.h"
#include "unx/render/Harness.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host::probe;
using render::check;
using render::ComPtr;
using render::Distribution;

namespace
{
struct Options
{
    std::string resolution = "4K";
    uint32_t frames = 600;
    uint32_t reps = 3;
    double warmupSeconds = 1.5;
    std::vector<Mode> modes = { Mode::FusedList, Mode::HostQueueLists, Mode::OwnQueue, Mode::IndependentDevice };
    D3D12_COMMAND_QUEUE_PRIORITY hostPriority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    std::filesystem::path out;
};

Mode parseMode(const std::string& s)
{
    for (uint32_t m = 0; m < kModeCount; ++m)
        if (s == modeName((Mode)m)) return (Mode)m;
    fail("unknown mode '%s'", s.c_str());
}

// Timestamp slots of one frame in the readback buffer.
enum Stamp : uint32_t
{
    PreBegin,
    PreEnd,
    WorkBegin,
    WorkEnd,
    PostBegin,
    PostEnd,
    kStamps
};
constexpr uint32_t kSlots = 2;  // frames in flight; output textures ring

struct Lists
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList7> list;
    void create(ID3D12Device* device, const wchar_t* name)
    {
        check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
        check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList");
        list->SetName(name);
        list->Close();
    }
    ID3D12GraphicsCommandList7* begin()
    {
        check(allocator->Reset(), "allocator Reset");
        check(list->Reset(allocator.Get(), nullptr), "list Reset");
        return list.Get();
    }
};

// Timestamps written by one device's queues and resolved into one readback buffer per slot.
struct Stamps
{
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    const uint64_t* mapped = nullptr;
    void create(ID3D12Device* device)
    {
        D3D12_QUERY_HEAP_DESC q{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kStamps * kSlots, 0 };
        check(device->CreateQueryHeap(&q, IID_PPV_ARGS(&heap)), "CreateQueryHeap");
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = kStamps * kSlots * 8;
        b.Height = 1;
        b.DepthOrArraySize = 1;
        b.MipLevels = 1;
        b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
        D3D12_RANGE all{ 0, (SIZE_T)b.Width };
        check(readback->Map(0, &all, (void**)&mapped), "Map readback");
    }
    void stamp(ID3D12GraphicsCommandList* cmd, uint32_t slot, Stamp s) const { cmd->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + s); }
    void resolve(ID3D12GraphicsCommandList* cmd, uint32_t slot, Stamp first, uint32_t count) const
    {
        cmd->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * kStamps + first, count, readback.Get(), (slot * kStamps + first) * 8ull);
    }
    uint64_t at(uint32_t slot, Stamp s) const { return mapped[slot * kStamps + s]; }
};

struct Fence
{
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    void create(ID3D12Device* device, D3D12_FENCE_FLAGS flags = D3D12_FENCE_FLAG_NONE)
    {
        check(device->CreateFence(0, flags, IID_PPV_ARGS(&fence)), "CreateFence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }
    void waitCpu(uint64_t value)
    {
        if (fence->GetCompletedValue() >= value) return;
        check(fence->SetEventOnCompletion(value, event), "SetEventOnCompletion");
        WaitForSingleObject(event, INFINITE);
    }
    ~Fence()
    {
        if (event) CloseHandle(event);
    }
};

ComPtr<ID3D12CommandQueue> createQueue(ID3D12Device* device, D3D12_COMMAND_QUEUE_PRIORITY priority, const wchar_t* name)
{
    D3D12_COMMAND_QUEUE_DESC d{};
    d.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    d.Priority = priority;
    ComPtr<ID3D12CommandQueue> q;
    check(device->CreateCommandQueue(&d, IID_PPV_ARGS(&q)), "CreateCommandQueue");
    q->SetName(name);
    return q;
}

void execute(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list)
{
    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
}

struct Samples
{
    std::vector<double> period, work, pre, post, gapBefore, gapAfter, cpuSubmit;
};

// Independent device (ID3D12DeviceFactory): a device that is not the process's singleton for the adapter, from the
// Agility runtime next to the executable. Returns null with the reason when the runtime refuses.
ComPtr<ID3D12Device> createIndependentDevice(IDXGIAdapter* adapter, std::string& note)
{
    ComPtr<ID3D12SDKConfiguration1> config;
    HRESULT hr = D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&config));
    if (FAILED(hr)) { note = format("D3D12GetInterface(SDKConfiguration1) 0x%08X", (unsigned)hr); return nullptr; }
    ComPtr<ID3D12DeviceFactory> factory;
    hr = config->CreateDeviceFactory(D3D12_SDK_VERSION, ".\\D3D12\\", IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { note = format("CreateDeviceFactory(%u, .\\D3D12\\) 0x%08X", (unsigned)D3D12_SDK_VERSION, (unsigned)hr); return nullptr; }
    hr = factory->SetFlags(D3D12_DEVICE_FACTORY_FLAG_DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON);
    if (FAILED(hr)) { note = format("ID3D12DeviceFactory::SetFlags 0x%08X", (unsigned)hr); return nullptr; }
    ComPtr<ID3D12Device> device;
    hr = factory->CreateDevice(adapter, D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&device));
    if (FAILED(hr)) { note = format("ID3D12DeviceFactory::CreateDevice 0x%08X", (unsigned)hr); return nullptr; }
    note = "created (factory flag DISALLOW_STORING_NEW_DEVICE_AS_SINGLETON)";
    return device;
}

class Run
{
public:
    Run(const Options& o, const WorkDesc& desc, ID3D12Device* hostDevice, IDXGIAdapter* adapter, const std::filesystem::path& shaders)
        : m_options(o), m_desc(desc), m_host(hostDevice)
    {
        m_hostQueue = createQueue(m_host, o.hostPriority, L"probe host queue");
        m_ownQueue = createQueue(m_host, D3D12_COMMAND_QUEUE_PRIORITY_HIGH, L"probe renderer queue");
        m_gpu = std::make_unique<ProbeGpu>(m_host, shaders);
        m_work = std::make_unique<ProbeWork>(*m_gpu, desc);
        m_target = createTexture(m_host, desc.width, desc.height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_HEAP_FLAG_NONE,
                                 D3D12_RESOURCE_FLAG_NONE, L"probe host target");
        m_targetUav = m_gpu->uav(m_target.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        m_hostCounter = createBuffer(m_host, 256);
        m_hostCounterUav = m_gpu->rawUav(m_hostCounter.Get(), 256);
        for (uint32_t s = 0; s < kSlots; ++s)
        {
            m_output[s] = createTexture(m_host, desc.width, desc.height, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                        D3D12_HEAP_FLAG_NONE, D3D12_RESOURCE_FLAG_NONE, L"probe output");
            m_outputUav[s] = m_gpu->uav(m_output[s].Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
            m_pre[s].create(m_host, L"probe pre");
            m_workLists[s].create(m_host, L"probe work");
            m_post[s].create(m_host, L"probe post");
        }
        m_stamps.create(m_host);
        m_hostFence.create(m_host, D3D12_FENCE_FLAG_SHARED);
        m_ownFence.create(m_host);

        // Independent device: its own queue, workload, shared output ring and shared fences.
        m_independent = createIndependentDevice(adapter, m_independentNote);
        if (m_independent)
        {
            m_indQueue = createQueue(m_independent.Get(), D3D12_COMMAND_QUEUE_PRIORITY_HIGH, L"probe independent queue");
            m_indGpu = std::make_unique<ProbeGpu>(m_independent.Get(), shaders);
            m_indWork = std::make_unique<ProbeWork>(*m_indGpu, desc);
            m_indStamps.create(m_independent.Get());
            for (uint32_t s = 0; s < kSlots; ++s)
            {
                // Cross-device textures are simultaneous-access (layout COMMON on both devices, no transitions).
                m_indOutput[s] = createTexture(m_independent.Get(), desc.width, desc.height, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_STATE_COMMON,
                                               D3D12_HEAP_FLAG_SHARED, D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, L"probe shared output");
                m_indOutputUav[s] = m_indGpu->uav(m_indOutput[s].Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
                HANDLE h = nullptr;
                check(m_independent->CreateSharedHandle(m_indOutput[s].Get(), nullptr, GENERIC_ALL, nullptr, &h), "CreateSharedHandle (output)");
                check(m_host->OpenSharedHandle(h, IID_PPV_ARGS(&m_indOutputOnHost[s])), "OpenSharedHandle (output)");
                CloseHandle(h);
                m_indOutputOnHostUav[s] = m_gpu->uav(m_indOutputOnHost[s].Get(), DXGI_FORMAT_R10G10B10A2_UNORM);
                m_indLists[s].create(m_independent.Get(), L"probe independent work");
            }
            m_indFence.create(m_independent.Get(), D3D12_FENCE_FLAG_SHARED);
            HANDLE h = nullptr;
            check(m_independent->CreateSharedHandle(m_indFence.fence.Get(), nullptr, GENERIC_ALL, nullptr, &h), "CreateSharedHandle (fence)");
            check(m_host->OpenSharedHandle(h, IID_PPV_ARGS(&m_indFenceOnHost)), "OpenSharedHandle (fence)");
            CloseHandle(h);
            check(m_host->CreateSharedHandle(m_hostFence.fence.Get(), nullptr, GENERIC_ALL, nullptr, &h), "CreateSharedHandle (host fence)");
            check(m_independent->OpenSharedHandle(h, IID_PPV_ARGS(&m_hostFenceOnInd)), "OpenSharedHandle (host fence)");
            CloseHandle(h);
        }
    }

    bool independentAvailable() const { return m_independent != nullptr; }
    const std::string& independentNote() const { return m_independentNote; }

    Samples run(Mode mode)
    {
        Samples out;
        m_hostClock.calibrate(m_hostQueue.Get());
        m_ownClock.calibrate(m_ownQueue.Get());
        if (m_independent) m_indClock.calibrate(m_indQueue.Get());
        const QueueClock& workClock = mode == Mode::IndependentDevice ? m_indClock : mode == Mode::OwnQueue ? m_ownClock : m_hostClock;
        const Stamps& workStamps = mode == Mode::IndependentDevice ? m_indStamps : m_stamps;
        const auto start = std::chrono::steady_clock::now();
        bool warm = false, havePrevious = false;
        double previousPostEnd = 0;
        for (uint64_t f = 0; out.work.size() < m_options.frames; ++f)
        {
            const uint32_t s = (uint32_t)(f % kSlots);
            // The frame that used this slot before: wait for it (frame pacing, kSlots in flight) and read it.
            if (m_slotValue[s])
            {
                m_hostFence.waitCpu(m_slotValue[s]);
                if (m_slotMeasured[s])
                {
                    const double preBegin = m_hostClock.toMs(m_stamps.at(s, PreBegin)), preEnd = m_hostClock.toMs(m_stamps.at(s, PreEnd));
                    const double postBegin = m_hostClock.toMs(m_stamps.at(s, PostBegin)), postEnd = m_hostClock.toMs(m_stamps.at(s, PostEnd));
                    const double workBegin = workClock.toMs(workStamps.at(s, WorkBegin)), workEnd = workClock.toMs(workStamps.at(s, WorkEnd));
                    out.pre.push_back(preEnd - preBegin);
                    out.work.push_back(workEnd - workBegin);
                    out.post.push_back(postEnd - postBegin);
                    out.gapBefore.push_back(workBegin - preEnd);
                    out.gapAfter.push_back(postBegin - workEnd);
                    if (havePrevious) out.period.push_back(postEnd - previousPostEnd);
                    previousPostEnd = postEnd;
                    havePrevious = true;
                }
            }
            if (!warm && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= m_options.warmupSeconds) warm = true;
            const auto cpu0 = std::chrono::steady_clock::now();
            submit(mode, s);
            if (warm) out.cpuSubmit.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cpu0).count());
            m_slotMeasured[s] = warm;
        }
        // Drain before the next boundary reuses the resources: the host queue's last signal follows every queue's work.
        m_hostFence.waitCpu(m_hostValue);
        for (uint32_t s = 0; s < kSlots; ++s) m_slotMeasured[s] = false;
        return out;
    }

private:
    void recordPre(ID3D12GraphicsCommandList7* cmd, uint32_t s)
    {
        // The host's own work before the frame (Unity's culling/shadow passes; here one dependent single-group pass on
        // host-owned memory, so it never touches the renderer's resources).
        m_stamps.stamp(cmd, s, PreBegin);
        m_gpu->bind(cmd);
        cmd->SetPipelineState(m_gpu->tiny());
        const uint32_t c[4] = { m_hostCounterUav, 0, 0, 0 };
        cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
        cmd->Dispatch(1, 1, 1);
        globalUavBarrier(cmd);
        m_stamps.stamp(cmd, s, PreEnd);
    }
    void recordWork(ID3D12GraphicsCommandList7* cmd, const Stamps& stamps, const ProbeWork& work, uint32_t outputUav, uint32_t s)
    {
        stamps.stamp(cmd, s, WorkBegin);
        work.record(cmd, outputUav);
        globalUavBarrier(cmd);
        stamps.stamp(cmd, s, WorkEnd);
    }
    void recordPost(ID3D12GraphicsCommandList7* cmd, uint32_t outputUavOnHost, uint32_t s)
    {
        // The host's presentation copy of the output (Unity's blit into its back buffer).
        m_stamps.stamp(cmd, s, PostBegin);
        m_gpu->bind(cmd);
        cmd->SetPipelineState(m_gpu->present());
        const uint32_t p[4] = { outputUavOnHost, m_targetUav, m_desc.width, m_desc.height };
        cmd->SetComputeRoot32BitConstants(0, 4, p, 0);
        cmd->Dispatch((m_desc.width + 7) / 8, (m_desc.height + 7) / 8, 1);
        globalUavBarrier(cmd);
        m_stamps.stamp(cmd, s, PostEnd);
    }
    static ID3D12GraphicsCommandList7* closed(ID3D12GraphicsCommandList7* cmd)
    {
        check(cmd->Close(), "Close");
        return cmd;
    }

    void submit(Mode mode, uint32_t s)
    {
        ID3D12CommandQueue* host = m_hostQueue.Get();
        const uint64_t reuseAfter = m_slotValue[s];  // host fence value of the last frame that read this output slot
        if (mode == Mode::FusedList)
        {
            ID3D12GraphicsCommandList7* cmd = m_pre[s].begin();
            recordPre(cmd, s);
            recordWork(cmd, m_stamps, *m_work, m_outputUav[s], s);
            recordPost(cmd, m_outputUav[s], s);
            m_stamps.resolve(cmd, s, PreBegin, kStamps);
            execute(host, closed(cmd));
        }
        else
        {
            const bool independent = mode == Mode::IndependentDevice;
            ID3D12GraphicsCommandList7* pre = m_pre[s].begin();
            recordPre(pre, s);
            m_stamps.resolve(pre, s, PreBegin, 2);
            ID3D12GraphicsCommandList7* work = independent ? m_indLists[s].begin() : m_workLists[s].begin();
            const Stamps& workStamps = independent ? m_indStamps : m_stamps;
            recordWork(work, workStamps, independent ? *m_indWork : *m_work, independent ? m_indOutputUav[s] : m_outputUav[s], s);
            workStamps.resolve(work, s, WorkBegin, 2);
            ID3D12GraphicsCommandList7* post = m_post[s].begin();
            recordPost(post, independent ? m_indOutputOnHostUav[s] : m_outputUav[s], s);
            m_stamps.resolve(post, s, PostBegin, 2);
            closed(pre);
            closed(work);
            closed(post);
            execute(host, pre);
            if (mode == Mode::HostQueueLists)
            {
                execute(host, work);
            }
            else
            {
                // The renderer's queue: write-after-read on the output slot waits for the host's copy of the frame that
                // used the slot before; the host's copy of this frame waits for the renderer's signal.
                ID3D12CommandQueue* own = independent ? m_indQueue.Get() : m_ownQueue.Get();
                ID3D12Fence* hostFenceSeenByOwn = independent ? m_hostFenceOnInd.Get() : m_hostFence.fence.Get();
                ID3D12Fence* ownFence = independent ? m_indFence.fence.Get() : m_ownFence.fence.Get();
                ID3D12Fence* ownFenceSeenByHost = independent ? m_indFenceOnHost.Get() : m_ownFence.fence.Get();
                const uint64_t ownValue = ++m_ownValue[independent ? 1 : 0];
                if (reuseAfter) check(own->Wait(hostFenceSeenByOwn, reuseAfter), "renderer queue Wait");
                execute(own, work);
                check(own->Signal(ownFence, ownValue), "renderer queue Signal");
                check(host->Wait(ownFenceSeenByHost, ownValue), "host queue Wait");
            }
            execute(host, post);
        }
        check(host->Signal(m_hostFence.fence.Get(), ++m_hostValue), "host Signal");
        m_slotValue[s] = m_hostValue;
    }

    static ComPtr<ID3D12Resource> createBuffer(ID3D12Device* device, uint32_t bytes)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = bytes;
        b.Height = 1;
        b.DepthOrArraySize = 1;
        b.MipLevels = 1;
        b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        b.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)), "buffer");
        return r;
    }

    const Options& m_options;
    WorkDesc m_desc;
    ID3D12Device* m_host;
    ComPtr<ID3D12CommandQueue> m_hostQueue, m_ownQueue;
    QueueClock m_hostClock, m_ownClock, m_indClock;
    std::unique_ptr<ProbeGpu> m_gpu, m_indGpu;
    std::unique_ptr<ProbeWork> m_work, m_indWork;
    ComPtr<ID3D12Resource> m_target, m_hostCounter;
    uint32_t m_targetUav = 0, m_hostCounterUav = 0;
    ComPtr<ID3D12Resource> m_output[kSlots];
    uint32_t m_outputUav[kSlots] = {};
    Lists m_pre[kSlots], m_workLists[kSlots], m_post[kSlots];
    Stamps m_stamps, m_indStamps;
    Fence m_hostFence, m_ownFence, m_indFence;
    uint64_t m_hostValue = 0, m_ownValue[2] = {};
    uint64_t m_slotValue[kSlots] = {};
    bool m_slotMeasured[kSlots] = {};

    ComPtr<ID3D12Device> m_independent;
    std::string m_independentNote;
    ComPtr<ID3D12CommandQueue> m_indQueue;
    ComPtr<ID3D12Resource> m_indOutput[kSlots], m_indOutputOnHost[kSlots];
    uint32_t m_indOutputUav[kSlots] = {}, m_indOutputOnHostUav[kSlots] = {};
    Lists m_indLists[kSlots];
    ComPtr<ID3D12Fence> m_indFenceOnHost, m_hostFenceOnInd;
};

std::string distJson(const std::vector<double>& v)
{
    const Distribution d = Distribution::of(v);
    return format("{\"count\": %u, \"median\": %.5f, \"p95\": %.5f, \"p99\": %.5f, \"mean\": %.5f, \"min\": %.5f, \"max\": %.5f}", d.count, d.median, d.p95,
                  d.p99, d.mean, d.min, d.max);
}

std::string timestamp()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[32];
    std::strftime(b, sizeof b, "%Y%m%d_%H%M%S", &tm);
    return b;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        Options o;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("%s needs a value", a.c_str());
                return argv[++i];
            };
            if (a == "--resolution") o.resolution = next();
            else if (a == "--frames") o.frames = (uint32_t)std::stoul(next());
            else if (a == "--reps") o.reps = (uint32_t)std::stoul(next());
            else if (a == "--out") o.out = next();
            else if (a == "--host-priority")
            {
                const std::string p = next();
                o.hostPriority = p == "normal" ? D3D12_COMMAND_QUEUE_PRIORITY_NORMAL : D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
            }
            else if (a == "--modes")
            {
                o.modes.clear();
                std::string list = next();
                size_t at = 0;
                while (at <= list.size())
                {
                    size_t comma = list.find(',', at);
                    if (comma == std::string::npos) comma = list.size();
                    o.modes.push_back(parseMode(list.substr(at, comma - at)));
                    at = comma + 1;
                }
            }
            else fail("unknown argument %s", a.c_str());
        }
        WorkDesc desc;
        if (o.resolution == "4K") { desc.width = 3840; desc.height = 2160; }
        else if (o.resolution == "1440p") { desc.width = 2560; desc.height = 1440; }
        else fail("measurements run at 4K or 1440p only (ARCHITECTURE 6), not '%s'", o.resolution.c_str());
        const std::string lockHolder = render::requireGpuLock("host boundary gate");
        if (o.out.empty()) o.out = std::filesystem::path(UNX_SOURCE_DIR) / "Results/I/HostBoundary";

        ComPtr<IDXGIFactory6> factory;
        check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
        ComPtr<IDXGIAdapter4> adapter;
        check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)), "EnumAdapterByGpuPreference");
        ComPtr<ID3D12Device> host, again;
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&host)), "D3D12CreateDevice");
        // D3D12 devices are per-adapter singletons: a second D3D12CreateDevice in the same process returns the same
        // device. Inside Unity this means the engine's Device attaches to Unity's device (the own-queue boundary).
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&again)), "D3D12CreateDevice (second)");
        const bool singleton = host.Get() == again.Get();
        again.Reset();
        const DeviceFacts facts = readDeviceFacts(host.Get());

        Run run(o, desc, host.Get(), adapter.Get(), executableDirectory() / "shaders/Host");
        logf("host boundary %s: %ux%u, %u heavy + %u tiny passes + output; independent device: %s\n", o.resolution.c_str(), desc.width, desc.height,
             desc.heavyPasses, desc.tinyPasses, run.independentNote().c_str());

        std::map<Mode, std::vector<Samples>> results;
        for (uint32_t r = 0; r < o.reps; ++r)
        {
            // Alternate the order every repetition so drift and clock ramps do not favour one boundary.
            std::vector<Mode> order = o.modes;
            if (r % 2) std::reverse(order.begin(), order.end());
            for (Mode m : order)
            {
                if (m == Mode::IndependentDevice && !run.independentAvailable()) continue;
                Samples s = run.run(m);
                const Distribution p = Distribution::of(s.period), w = Distribution::of(s.work);
                logf("  rep %u %-20s period %.4f ms (p95 %.4f, p99 %.4f)  work %.4f  gaps %.4f + %.4f  host %.4f + %.4f ms\n", r, modeName(m), p.median, p.p95,
                     p.p99, w.median, Distribution::of(s.gapBefore).median, Distribution::of(s.gapAfter).median, Distribution::of(s.pre).median,
                     Distribution::of(s.post).median);
                results[m].push_back(std::move(s));
            }
        }

        std::string json = "{\n";
        json += format("  \"gate\": \"host_boundary_standalone\",\n  \"resolution\": \"%s\",\n  \"width\": %u,\n  \"height\": %u,\n", o.resolution.c_str(), desc.width,
                       desc.height);
        json += format("  \"workload\": {\"heavyPasses\": %u, \"tinyPasses\": %u, \"outputPass\": 1},\n", desc.heavyPasses, desc.tinyPasses);
        json += format("  \"framesPerRun\": %u,\n  \"reps\": %u,\n  \"warmupSeconds\": %.2f,\n  \"framesInFlight\": %u,\n", o.frames, o.reps, o.warmupSeconds, kSlots);
        json += format("  \"hostQueuePriority\": \"%s\",\n  \"rendererQueuePriority\": \"high\",\n",
                       o.hostPriority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH ? "high" : "normal");
        json += format("  \"gpuLock\": \"%s\",\n  \"deviceSingleton\": %s,\n  \"independentDevice\": \"%s\",\n", jsonEscape(lockHolder).c_str(),
                       singleton ? "true" : "false", jsonEscape(run.independentNote()).c_str());
        json += "  \"device\": " + facts.toJson() + ",\n  \"modes\": {\n";
        bool firstMode = true;
        for (auto& [m, runs] : results)
        {
            Samples all;
            std::string perRep;
            for (const Samples& s : runs)
            {
                all.period.insert(all.period.end(), s.period.begin(), s.period.end());
                all.work.insert(all.work.end(), s.work.begin(), s.work.end());
                all.pre.insert(all.pre.end(), s.pre.begin(), s.pre.end());
                all.post.insert(all.post.end(), s.post.begin(), s.post.end());
                all.gapBefore.insert(all.gapBefore.end(), s.gapBefore.begin(), s.gapBefore.end());
                all.gapAfter.insert(all.gapAfter.end(), s.gapAfter.begin(), s.gapAfter.end());
                all.cpuSubmit.insert(all.cpuSubmit.end(), s.cpuSubmit.begin(), s.cpuSubmit.end());
                perRep += format("%s%.5f", perRep.empty() ? "" : ", ", Distribution::of(s.period).median);
            }
            json += format("%s    \"%s\": {\n", firstMode ? "" : ",\n", modeName(m));
            json += "      \"periodMs\": " + distJson(all.period) + ",\n";
            json += "      \"periodMedianPerRepMs\": [" + perRep + "],\n";
            json += "      \"workMs\": " + distJson(all.work) + ",\n";
            json += "      \"hostPreMs\": " + distJson(all.pre) + ",\n";
            json += "      \"hostPostMs\": " + distJson(all.post) + ",\n";
            json += "      \"gapBeforeWorkMs\": " + distJson(all.gapBefore) + ",\n";
            json += "      \"gapAfterWorkMs\": " + distJson(all.gapAfter) + ",\n";
            json += "      \"cpuSubmitMs\": " + distJson(all.cpuSubmit) + "\n    }";
            firstMode = false;
        }
        json += "\n  }\n}\n";
        std::filesystem::create_directories(o.out);
        const std::filesystem::path file = o.out / format("standalone_%s_%s.json", o.resolution.c_str(), timestamp().c_str());
        writeTextFile(file, json);
        logf("wrote %s\n", file.string().c_str());
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "host boundary gate failed: %s\n", e.what());
        return 1;
    }
}
