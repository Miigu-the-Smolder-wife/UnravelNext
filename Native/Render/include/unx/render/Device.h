#pragma once
#include "unx/render/D3D12.h"
#include "unx/render/Descriptors.h"

#include <array>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace unx::render
{
struct DeviceOptions
{
    bool debugLayer = false;     // D3D12 debug layer (from the Agility package's d3d12SDKLayers.dll)
    bool gpuValidation = false;  // GPU-based validation; implies debugLayer
    // Graphics/compute queue priority. HIGH needs no privilege; it asks the GPU scheduler to prefer this process's
    // work over other applications', which otherwise time-slice in and stall ~5 % of frames by 0.16-0.47 ms
    // (P0b empty-frame gate: P99 0.33-0.44 ms at NORMAL, 0.176-0.178 ms at HIGH in 4 of 5 runs [measured]).
    D3D12_COMMAND_QUEUE_PRIORITY queuePriority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
    // Host integration (I track, INTERFACES_KO.md 4.1, v1.9): build on the host's device instead of creating one. The
    // Device then neither enables the debug layer (debugLayer must be false: enabling it in a process that has a device
    // removes that device) nor enumerates adapters (the adapter comes from the device's LUID). Feature checks still run.
    ID3D12Device* externalDevice = nullptr;
    // The host's DIRECT queue (Unity's). The graphics Queue executes and signals on it with the Device's own fence, so a
    // frame is one list on the host's queue with no cross-queue synchronisation. Compute and copy queues are still
    // created on the device; queuePriority applies to them only (the host queue's priority is not changed).
    ID3D12CommandQueue* externalGraphicsQueue = nullptr;
};

struct DeviceCaps
{
    std::string adapter;
    std::string driver;  // UMD version a.b.c.d
    uint64_t vramBytes = 0;
    std::string runtimePath;  // loaded D3D12Core.dll
    std::string runtimeVersion;
    D3D_SHADER_MODEL shaderModel = D3D_SHADER_MODEL_5_1;
    D3D12_RAYTRACING_TIER raytracingTier = D3D12_RAYTRACING_TIER_NOT_SUPPORTED;
    D3D12_MESH_SHADER_TIER meshShaderTier = D3D12_MESH_SHADER_TIER_NOT_SUPPORTED;
    D3D12_RESOURCE_BINDING_TIER bindingTier = D3D12_RESOURCE_BINDING_TIER_1;
    D3D12_RESOURCE_HEAP_TIER heapTier = D3D12_RESOURCE_HEAP_TIER_1;
    bool enhancedBarriers = false;
    // NVAPI (R590 SDK): opacity micromaps and thread reordering on drivers without DXR 1.2 (ARCHITECTURE 7.1-6).
    bool nvapi = false;
    bool nvapiOpacityMicromap = false;
    bool nvapiThreadReordering = false;
    std::string nvapiInterface;
    std::string nvapiBranch;
    uint32_t nvapiDriver = 0;  // e.g. 59186
};

class Queue
{
public:
    Queue(ID3D12Device* device, QueueType type, D3D12_COMMAND_QUEUE_PRIORITY priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL);
    // Wraps a queue the host owns (DeviceOptions::externalGraphicsQueue): its own fence, the host queue's name and
    // priority unchanged.
    Queue(ID3D12Device* device, QueueType type, ID3D12CommandQueue* external);
    ID3D12CommandQueue* get() const { return m_queue.Get(); }
    QueueType type() const { return m_type; }
    ID3D12Fence* fence() const { return m_fence.Get(); }
    uint64_t signal();
    void waitCpu(uint64_t value);
    void waitGpu(const Queue& other, uint64_t value);
    uint64_t completed() const { return m_fence->GetCompletedValue(); }
    uint64_t lastSignaled() const { return m_lastSignaled; }
    uint64_t timestampFrequency() const { return m_timestampFrequency; }
    void execute(ID3D12CommandList* list);
    // Routes execute() through the host (Unity: IUnityGraphicsD3D12v8::ExecuteCommandList, which also declares the
    // states of host-owned textures the list touches). signal() and waitGpu() stay direct on the queue. Empty (default):
    // ExecuteCommandLists on the queue. Set only while the host allows access to its queue (its render event).
    void setExecuteHook(std::function<void(ID3D12CommandList*)> hook);

private:
    QueueType m_type;
    std::function<void(ID3D12CommandList*)> m_executeHook;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_event = nullptr;
    uint64_t m_lastSignaled = 0;
    uint64_t m_timestampFrequency = 0;
    std::mutex m_mutex;
};

// A recorded command list and the allocator it came from. Returned to the pool with the fence value that retires it.
struct CommandList
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList7> list;
    QueueType queue = QueueType::Graphics;
    uint64_t retireValue = 0;
};

class Device
{
public:
    explicit Device(const DeviceOptions& options);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    ID3D12Device10* d3d() const { return m_device.Get(); }
    IDXGIAdapter4* adapter() const { return m_adapter.Get(); }
    const DeviceCaps& caps() const { return m_caps; }
    const DeviceOptions& options() const { return m_options; }
    Queue& queue(QueueType type) { return *m_queues[(size_t)type]; }
    DescriptorHeaps& descriptors() { return *m_descriptors; }
    // The single bindless root signature: 32 root constants (b0), a root CBV (b1), static samplers s0-s5,
    // heaps directly indexed. Every compute and mesh pipeline uses it.
    ID3D12RootSignature* rootSignature() const { return m_rootSignature.Get(); }
    static constexpr uint32_t kRootConstantCount = 32;

    // Command lists come from a pool; submit() executes and recycles them once the queue fence passes.
    CommandList acquireCommandList(QueueType type);
    uint64_t submit(CommandList& list);  // Close + Execute + Signal; returns the fence value
    void recycle(CommandList&& list);    // list executed elsewhere; retireValue must be set

    // Releases the object (or runs the call) once every queue has passed its current fence value.
    void deferRelease(ComPtr<ID3D12Pageable> object);
    void deferCall(std::function<void()> call);
    void collectGarbage();
    void waitIdle();
    // Drops D3D12 debug-layer messages to the log; returns the number of errors/corruptions seen so far.
    uint32_t drainDebugMessages();

private:
    DeviceOptions m_options;
    ComPtr<IDXGIFactory6> m_factory;
    ComPtr<IDXGIAdapter4> m_adapter;
    ComPtr<ID3D12Device10> m_device;
    ComPtr<ID3D12InfoQueue1> m_infoQueue;
    DeviceCaps m_caps;
    std::array<std::unique_ptr<Queue>, kQueueTypeCount> m_queues;
    std::unique_ptr<DescriptorHeaps> m_descriptors;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    std::mutex m_poolMutex;
    std::array<std::vector<CommandList>, kQueueTypeCount> m_pool;
    struct Deferred
    {
        ComPtr<ID3D12Pageable> object;
        std::function<void()> call;
        std::array<uint64_t, kQueueTypeCount> fence;
    };
    std::mutex m_garbageMutex;
    std::deque<Deferred> m_garbage;
    uint32_t m_debugErrors = 0;
};
} // namespace unx::render
