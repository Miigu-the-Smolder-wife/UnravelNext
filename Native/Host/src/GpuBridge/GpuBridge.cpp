// Shared GPU bridge. See GpuBridge.h. Port of Unravel's Native/TitanNative/src/SharedGpuExecution.cpp and
// SharedGpuAllocation.cpp (the old renderer's host of the same ABI); the residency scheduler is replaced by the
// allocation ledger alone (Native/RuntimeCommon/GpuAllocationLedger.h), and graphics use is two-phase.
#include "GpuBridge.h"

#include <windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <utility>

namespace unx::host
{
using Microsoft::WRL::ComPtr;
using native_runtime::GpuContractError;
using native_runtime::gpuRequire;
using Ledger = native_runtime::GpuAllocationLedger;

namespace
{
const GUID kResourceInitialState = { 0x20b8c06e, 0x96c8, 0x47b5, { 0x81, 0xd7, 0x32, 0x0c, 0x86, 0x9d, 0x54, 0x72 } };
const GUID kAllocationOwner = { 0xddc60b37, 0xd71f, 0x4d69, { 0xaf, 0xca, 0x38, 0x65, 0x09, 0x2b, 0x8a, 0x96 } };
// Private data on a bound resource: its ledger reservation is released when the resource's COM lifetime ends (after
// every consumer and pending submission let go), not at a frame number.
class AllocationOwner final : public IUnknown
{
    std::atomic<ULONG> m_references{ 1 };
    std::shared_ptr<Ledger> m_ledger;
    uint64_t m_id;
    bool m_active = false;

public:
    AllocationOwner(std::shared_ptr<Ledger> ledger, uint64_t id) : m_ledger(std::move(ledger)), m_id(id) {}
    ~AllocationOwner()
    {
        if (m_active) try { m_ledger->release(m_id); } catch (...) { std::terminate(); }
    }
    void activate() noexcept { m_active = true; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override
    {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id != IID_IUnknown) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_references; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const auto left = --m_references;
        if (!left) delete this;
        return left;
    }
};
HRESULT attachAllocation(ID3D12Resource* resource, std::shared_ptr<Ledger> ledger, uint64_t reservation, D3D12_RESOURCE_STATES initial) noexcept
{
    if (!resource || !ledger || !reservation) return E_INVALIDARG;
    IUnknown* existing = nullptr;
    UINT size = sizeof(existing);
    if (SUCCEEDED(resource->GetPrivateData(kAllocationOwner, &size, &existing)))
    {
        if (existing) existing->Release();
        return E_INVALIDARG;
    }
    AllocationOwner* owner = nullptr;
    try
    {
        HRESULT hr = resource->SetPrivateData(kResourceInitialState, sizeof(initial), &initial);
        if (FAILED(hr)) return hr;
        owner = new AllocationOwner(ledger, reservation);
        hr = resource->SetPrivateDataInterface(kAllocationOwner, owner);
        if (FAILED(hr)) { owner->Release(); return hr; }
        try { ledger->bind(reservation); }
        catch (...) { resource->SetPrivateDataInterface(kAllocationOwner, nullptr); owner->Release(); return E_INVALIDARG; }
        owner->activate();
        owner->Release();
        return S_OK;
    }
    catch (...)
    {
        if (owner) owner->Release();
        return E_OUTOFMEMORY;
    }
}
} // namespace

struct GpuBridgeHost::Impl
{
    struct Resource
    {
        ComPtr<ID3D12Resource> value;
        uint64_t allocation = 0, references = 1, version = 1;
        NRC_GpuAllocation request{};
        D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;
        bool buffer = false, acceleration = false, simultaneous = false;
    };
    struct Submission
    {
        NRC_GpuFence fence{};
        native_runtime::GpuDependencyGraph::Plan plan;
        std::vector<ComPtr<IUnknown>> lifetimes;
        std::vector<uint64_t> borrowed;  // graphics uses: leases held until commit
        NRC_GpuWorldStamp source{};
        uint32_t stage = 0;
        bool committed = false;
    };
    struct WaitEvent
    {
        HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ComPtr<ID3D12Fence> fence;
        uint64_t value = 0;
        bool inUse = false;
        ~WaitEvent() { if (handle) CloseHandle(handle); }
    };
    struct Lease;
    std::mutex mutex;
    std::unique_lock<std::mutex> graphicsLock;  // held between prepareGraphics and commitGraphics (same thread)
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGIAdapter3> adapter;
    std::array<ComPtr<ID3D12CommandQueue>, 3> queues;
    std::array<ComPtr<ID3D12Fence>, 3> fences;
    std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
    native_runtime::GpuDependencyGraph graph;
    std::map<uint64_t, Resource> resources;
    std::map<ID3D12Resource*, uint64_t> pointers;
    std::list<Submission> submissions;
    Submission* pendingGraphics = nullptr;
    std::vector<std::shared_ptr<WaitEvent>> waitEvents;
    uint64_t nextResource = 0, queueWaits = 0, cpuWaits = 0, graphicsTicket = 0;
    std::array<uint64_t, 3> counts{};
    bool accepting = true, faulted = false, removed = false, uma = false;
    // Diagnostic only: isolate cross-queue GPU contention without dropping any work.
    ComPtr<ID3D12Fence> diagnosticSerialFence;
    uint64_t diagnosticSerialValue = 0;
    HRESULT lastError = S_OK;

    Impl(ID3D12Device* d, ID3D12CommandQueue* graphicsQueue, ID3D12Fence* graphicsFence, uint64_t generation) : device(d), graph(generation)
    {
        gpuRequire(device && graphicsQueue && graphicsFence, NRC_GPU_INVALID, "The GPU bridge needs the renderer's device, graphics queue and fence");
        sameDevice(graphicsQueue);
        sameDevice(graphicsFence);
        queues[0] = graphicsQueue;
        fences[0] = graphicsFence;
        wchar_t diagnosticSerial[8]{};
        if (GetEnvironmentVariableW(L"UNX_DIAGNOSTIC_SERIAL_BRIDGE", diagnosticSerial, 8) && diagnosticSerial[0] == L'1')
            check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&diagnosticSerialFence)), "Create diagnostic serial fence");
        ComPtr<IDXGIFactory4> factory;
        check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "Create the GPU budget query");
        check(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)), "Find the GPU adapter");
        D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture)))) uma = architecture.UMA != 0;
        for (uint32_t q = 1; q < 3; ++q)
        {
            D3D12_COMMAND_QUEUE_DESC description{};
            description.Type = q == 1 ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_COPY;
            check(device->CreateCommandQueue(&description, IID_PPV_ARGS(&queues[q])), "Create a shared GPU queue");
            queues[q]->SetName(q == 1 ? L"unx.bridge.compute" : L"unx.bridge.copy");
            check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fences[q])), "Create a shared GPU fence");
        }
        // The graphics timeline starts where the renderer's fence already is: earlier values are not bridge work.
        graph.complete(0, fences[0]->GetCompletedValue());
        refreshBudget(0);
        if (!uma) refreshBudget(1);
        else ledger->budget(1, 0, 0);
    }
    void check(HRESULT hr, const char* message)
    {
        if (FAILED(hr))
        {
            lastError = hr;
            faulted = true;
            accepting = false;
            removed = FAILED(device->GetDeviceRemovedReason());
            throw GpuContractError(removed ? NRC_GPU_REMOVED : NRC_GPU_INTERNAL, message);
        }
    }
    void healthy()
    {
        gpuRequire(accepting && !faulted, NRC_GPU_STALE, "The shared GPU generation is quiescing or faulted");
        check(device->GetDeviceRemovedReason(), "The shared GPU device was removed");
    }
    void sameDevice(ID3D12DeviceChild* child)
    {
        ComPtr<ID3D12Device> other;
        gpuRequire(child && SUCCEEDED(child->GetDevice(IID_PPV_ARGS(&other))), NRC_GPU_INVALID, "GPU object has no device");
        ComPtr<IUnknown> a, b;
        check(device.As(&a), "Read the GPU device identity");
        check(other.As(&b), "Read the GPU object's device identity");
        gpuRequire(a.Get() == b.Get(), NRC_GPU_INVALID, "GPU object belongs to another device");
    }
    void refreshBudget(uint32_t segment)
    {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        check(adapter->QueryVideoMemoryInfo(0, segment ? DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL : DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info), "Refresh the GPU memory budget");
        ledger->budget(segment, info.Budget, info.CurrentUsage);
    }
    static bool hasUnfenced(const std::list<Submission>& values)
    {
        for (const auto& value : values) if (!value.committed) return true;
        return false;
    }
    void collectUnlocked()
    {
        if (FAILED(device->GetDeviceRemovedReason())) { lastError = device->GetDeviceRemovedReason(); faulted = removed = true; accepting = false; }
        if (!removed)
            for (uint32_t q = 0; q < 3; ++q)
            {
                const auto value = fences[q]->GetCompletedValue();
                if (value == UINT64_MAX) { lastError = DXGI_ERROR_DEVICE_REMOVED; faulted = removed = true; accepting = false; break; }
                graph.complete(q, value);
            }
        for (auto at = submissions.begin(); at != submissions.end();)
            if (removed || (at->committed && graph.completed(at->fence.queue) >= at->fence.value)) at = submissions.erase(at);
            else ++at;
        const bool unfenced = hasUnfenced(submissions);
        for (auto at = resources.begin(); at != resources.end();)
        {
            auto& value = at->second;
            if (value.references || (!removed && (unfenced || (value.value && !graph.retired(at->first))))) { ++at; continue; }
            if (value.value) { pointers.erase(value.value.Get()); if (!removed) graph.remove(at->first); }
            if (value.allocation && !value.value) ledger->release(value.allocation);
            at = resources.erase(at);
        }
    }
    uint64_t reserve(const NRC_GpuAllocation& request)
    {
        std::lock_guard lock(mutex);
        healthy();
        collectUnlocked();
        gpuRequire(request.nonlocal < 2, NRC_GPU_INVALID, "GPU allocation segment is invalid");
        refreshBudget(request.nonlocal);
        const auto allocation = ledger->reserve(request);
        if (!allocation) throw GpuContractError(NRC_GPU_PRESSURE, "The GPU allocation was deferred by memory pressure");
        try
        {
            gpuRequire(nextResource != UINT64_MAX, NRC_GPU_INTERNAL, "GPU resource identity exhausted");
            const auto id = nextResource + 1;
            Resource resource;
            resource.allocation = allocation;
            resource.request = request;
            resources.emplace(id, std::move(resource));
            nextResource = id;
            return id;
        }
        catch (...) { ledger->release(allocation); throw; }
    }
    void describe(Resource& target, ID3D12Resource* value)
    {
        sameDevice(value);
        D3D12_HEAP_PROPERTIES heap{};
        D3D12_HEAP_FLAGS flags{};
        gpuRequire(SUCCEEDED(value->GetHeapProperties(&heap, &flags)), NRC_GPU_INVALID, "GPU allocation does not expose its heap");
        const auto desc = value->GetDesc();
        target.heap = heap.Type;
        target.buffer = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
        target.simultaneous = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
        target.value = value;
    }
    void bind(uint64_t id, ID3D12Resource* value, uint32_t state)
    {
        std::lock_guard lock(mutex);
        healthy();
        auto found = resources.find(id);
        gpuRequire(found != resources.end() && found->second.references && !found->second.value && value, NRC_GPU_STALE, "Stale GPU allocation binding");
        gpuRequire(pointers.find(value) == pointers.end(), NRC_GPU_INVALID, "A GPU allocation cannot be charged twice");
        Resource prepared = found->second;
        describe(prepared, value);
        const auto desc = value->GetDesc();
        const auto bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        gpuRequire(bytes == prepared.request.bytes, NRC_GPU_INVALID, "The bound GPU allocation differs from its admitted bytes");
        const uint32_t segment = !uma && (prepared.heap == D3D12_HEAP_TYPE_UPLOAD || prepared.heap == D3D12_HEAP_TYPE_READBACK) ? 1u : 0u;
        gpuRequire(segment == prepared.request.nonlocal, NRC_GPU_INVALID, "The bound GPU allocation's memory segment differs");
        prepared.acceleration = state == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        graph.add(id, state);
        try { pointers.emplace(value, id); } catch (...) { graph.remove(id); throw; }
        const auto attached = attachAllocation(value, ledger, prepared.allocation, D3D12_RESOURCE_STATES(state));
        if (FAILED(attached)) { pointers.erase(value); graph.remove(id); throw GpuContractError(NRC_GPU_INVALID, "Cannot attach the shared GPU allocation's lifetime"); }
        found->second = std::move(prepared);
    }
    void retainResource(uint64_t id)
    {
        std::lock_guard lock(mutex);
        auto at = resources.find(id);
        gpuRequire(at != resources.end() && at->second.references && at->second.references != UINT64_MAX, NRC_GPU_STALE, "Retaining a retired GPU resource");
        ++at->second.references;
    }
    void releaseResource(uint64_t id)
    {
        std::lock_guard lock(mutex);
        auto at = resources.find(id);
        gpuRequire(at != resources.end() && at->second.references, NRC_GPU_STALE, "A GPU resource lease was released twice");
        --at->second.references;
        collectUnlocked();
    }
    // One boundary convention for every queue: buffer (and simultaneous-access) states decay to COMMON after
    // ExecuteCommandLists; callers own the transitions inside their lists.
    static uint32_t boundaryState(const Resource& resource, uint32_t state)
    {
        if (resource.heap == D3D12_HEAP_TYPE_UPLOAD) return uint32_t(D3D12_RESOURCE_STATE_GENERIC_READ);
        if (resource.heap == D3D12_HEAP_TYPE_READBACK) return uint32_t(D3D12_RESOURCE_STATE_COPY_DEST);
        if (resource.acceleration) return uint32_t(D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        return (resource.buffer || resource.simultaneous) ? uint32_t(D3D12_RESOURCE_STATE_COMMON) : state;
    }
    void validateAccess(const NRC_GpuAccess* accesses, uint64_t count)
    {
        gpuRequire((!count || accesses) && count <= SIZE_MAX / sizeof(NRC_GpuAccess), NRC_GPU_INVALID, "Missing or oversized GPU resource access table");
        for (uint64_t n = 0; n < count; ++n)
        {
            auto at = resources.find(accesses[n].resource);
            gpuRequire(at != resources.end() && at->second.references && at->second.value, NRC_GPU_STALE, "GPU resource has no live bound lease");
        }
    }
    void waits(ID3D12CommandQueue* queue, const native_runtime::GpuDependencyGraph::Plan& plan)
    {
        for (uint32_t q = 0; q < 3; ++q)
            if (plan.waits[q]) { check(queue->Wait(fences[q].Get(), plan.waits[q]), "Queue-to-queue GPU dependency failed"); ++queueWaits; }
    }
    NRC_GpuFence submit(const NRC_GpuSubmission& request)
    {
        std::lock_guard lock(mutex);
        return submitLocked(request);
    }
    NRC_GpuFence submitLocked(const NRC_GpuSubmission& request)
    {
        healthy();
        collectUnlocked();
        gpuRequire(request.size == sizeof(request) && request.version == 1 && request.queue > 0 && request.queue < 3 && request.stage <= NRC_GPU_PRESENTATION && request.source.world &&
                       request.source.world_generation && request.source.branch && request.source.phase < 8 && !request.source.reserved && request.commands &&
                       request.command_allocator && (!request.lifetime_count || request.lifetime_objects),
                   NRC_GPU_INVALID, "Invalid shared GPU work packet");
        gpuRequire(request.lifetime_count <= SIZE_MAX / sizeof(ComPtr<IUnknown>) - 2, NRC_GPU_INVALID, "GPU submission lifetime extent overflow");
        auto* commands = static_cast<ID3D12GraphicsCommandList*>(request.commands);
        sameDevice(commands);
        sameDevice(static_cast<ID3D12CommandAllocator*>(request.command_allocator));
        gpuRequire(commands->GetType() == queues[request.queue]->GetDesc().Type, NRC_GPU_INVALID, "Command list type differs from its shared queue");
        validateAccess(request.accesses, request.access_count);
        std::vector<NRC_GpuAccess> boundaries;
        if (request.access_count) boundaries.assign(request.accesses, request.accesses + request.access_count);
        for (auto& access : boundaries) access.after_state = boundaryState(resources.at(access.resource), access.after_state);
        Submission pending;
        pending.plan = graph.prepare(request.queue, boundaries.data(), boundaries.size(), request.waits, request.wait_count);
        const auto last = graph.submitted(request.queue);
        gpuRequire(last < UINT64_MAX - 1, NRC_GPU_INTERNAL, "GPU submission timeline exhausted");
        pending.fence = { graph.generation(), request.queue, 0, last + 1 };
        pending.source = request.source;
        pending.stage = request.stage;
        pending.lifetimes.reserve(size_t(request.lifetime_count) + 2);
        pending.lifetimes.emplace_back(static_cast<IUnknown*>(commands));
        pending.lifetimes.emplace_back(static_cast<IUnknown*>(static_cast<ID3D12CommandAllocator*>(request.command_allocator)));
        for (uint64_t n = 0; n < request.lifetime_count; ++n)
        {
            gpuRequire(request.lifetime_objects[n], NRC_GPU_INVALID, "Null GPU submission lifetime object");
            pending.lifetimes.emplace_back(static_cast<IUnknown*>(request.lifetime_objects[n]));
        }
        waits(queues[request.queue].Get(), pending.plan);
        if (diagnosticSerialFence)
        {
            // Submission admission holds mutex, so this orders all previously
            // submitted graphics work before this module's compute/copy work.
            check(queues[0]->Signal(diagnosticSerialFence.Get(), ++diagnosticSerialValue), "Diagnostic graphics serial signal");
            check(queues[request.queue]->Wait(diagnosticSerialFence.Get(), diagnosticSerialValue), "Diagnostic module serial wait");
        }
        submissions.push_back(std::move(pending));
        auto& tracked = submissions.back();
        ID3D12CommandList* lists[]{ commands };
        queues[request.queue]->ExecuteCommandLists(1, lists);
        // A submission whose signal fails is kept: nothing is released until a repaired fence or device removal.
        check(queues[request.queue]->Signal(fences[request.queue].Get(), tracked.fence.value), "Shared GPU completion signal failed");
        tracked.fence = graph.commit(tracked.plan, tracked.fence.value);
        tracked.committed = true;
        ++counts[request.queue];
        for (const auto& access : tracked.plan.accesses) resources.at(access.resource).version = access.version;
        return tracked.fence;
    }
    bool poll(NRC_GpuFence token, uint32_t timeout)
    {
        std::shared_ptr<WaitEvent> event;
        {
            std::lock_guard lock(mutex);
            collectUnlocked();
            gpuRequire(!removed, NRC_GPU_REMOVED, "The shared GPU device was removed during a wait");
            gpuRequire(token.generation == graph.generation() && token.queue < 3 && !token.reserved && token.value <= graph.submitted(token.queue), NRC_GPU_STALE,
                       "Foreign or unsubmitted completion token");
            if (graph.completed(token.queue) >= token.value) return true;
            if (!timeout) return false;
            for (const auto& existing : waitEvents)
                if (!existing->inUse && (!existing->fence || existing->fence->GetCompletedValue() >= existing->value)) { event = existing; break; }
            if (!event)
            {
                event = std::make_shared<WaitEvent>();
                gpuRequire(event->handle != nullptr, NRC_GPU_INTERNAL, "GPU completion event allocation failed");
                waitEvents.push_back(event);
            }
            event->fence = fences[token.queue];
            event->value = token.value;
            ResetEvent(event->handle);
            check(event->fence->SetEventOnCompletion(token.value, event->handle), "Register the GPU completion event");
            event->inUse = true;
            ++cpuWaits;
        }
        const auto result = WaitForSingleObject(event->handle, timeout);
        std::lock_guard lock(mutex);
        event->inUse = false;
        collectUnlocked();
        gpuRequire(!removed, NRC_GPU_REMOVED, "The shared GPU device was removed during a wait");
        gpuRequire(result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT, NRC_GPU_INTERNAL, "GPU completion event wait failed");
        return graph.completed(token.queue) >= token.value;
    }
    NRC_GpuStatistics statistics()
    {
        std::lock_guard lock(mutex);
        collectUnlocked();
        NRC_GpuStatistics result{};
        result.size = sizeof(result);
        result.version = 1;
        result.generation = graph.generation();
        for (uint32_t q = 0; q < 3; ++q) { result.submitted[q] = graph.submitted(q); result.completed[q] = graph.completed(q); result.submissions[q] = counts[q]; }
        result.queue_waits = queueWaits;
        result.cpu_waits = cpuWaits;
        result.resources = resources.size();
        result.faulted = (faulted ? 1u : 0u) | (!accepting ? 2u : 0u) | (removed ? 4u : 0u);
        result.last_error = lastError;
        const auto memory = ledger->totals();
        result.reserved_bytes = memory.bytes;
        result.bound_bytes = memory.bound;
        result.denied_allocations = memory.denied;
        std::copy(memory.domains.begin(), memory.domains.end(), result.domain_bytes);
        std::copy(memory.memory.begin(), memory.memory.end(), result.memory_bytes);
        for (const auto& entry : resources) if (!entry.second.references && entry.second.allocation) result.retiring_bytes += entry.second.request.bytes;
        return result;
    }
    void quiesce()
    {
        std::array<NRC_GpuFence, 3> targets{};
        {
            std::lock_guard lock(mutex);
            accepting = false;
            collectUnlocked();
            if (removed) return;
            for (auto& value : submissions)
                if (!value.committed)
                {
                    // A graphics use whose frame was never committed: its reads are fenced by the renderer's own
                    // completion; only compute/copy markers can be repaired here.
                    gpuRequire(value.fence.queue != NRC_GPU_GRAPHICS, NRC_GPU_PENDING, "The renderer must commit its graphics use before the bridge retires");
                    check(queues[value.fence.queue]->Signal(fences[value.fence.queue].Get(), value.fence.value), "Repair a GPU retirement marker");
                    value.fence = graph.commit(value.plan, value.fence.value);
                    value.committed = true;
                    ++counts[value.fence.queue];
                }
            for (uint32_t q = 0; q < 3; ++q) targets[q] = { graph.generation(), q, 0, graph.submitted(q) };
        }
        for (const auto& token : targets) gpuRequire(poll(token, 30000), NRC_GPU_PENDING, "GPU bridge retirement has not reached completion");
    }
    uint64_t prepareGraphics(const std::vector<GpuBridgeHost::GraphicsUse>& uses, const NRC_GpuWorldStamp& source)
    {
        std::unique_lock lock(mutex);
        healthy();
        collectUnlocked();
        gpuRequire(!pendingGraphics, NRC_GPU_INVALID, "A graphics use is already prepared");
        gpuRequire(!source.reserved && source.phase < 8 && (!source.world || (source.world_generation && source.branch)), NRC_GPU_INVALID, "Invalid graphics source stamp");
        std::vector<uint64_t> borrowed;
        std::vector<NRC_GpuAccess> accesses;
        std::set<ID3D12Resource*> seen;
        auto releaseBorrowed = [&] { for (uint64_t id : borrowed) --resources.at(id).references; };
        try
        {
            for (const auto& use : uses)
            {
                gpuRequire(use.resource && seen.insert(use.resource).second, NRC_GPU_INVALID, "Invalid or duplicate graphics resource use");
                auto known = pointers.find(use.resource);
                gpuRequire(known != pointers.end(), NRC_GPU_INVALID, "Graphics use of a resource the bridge does not know");
                const uint64_t id = known->second;
                auto& value = resources.at(id);
                gpuRequire(value.references && value.references != UINT64_MAX, NRC_GPU_STALE, "The renderer cannot use a retired module allocation");
                ++value.references;
                borrowed.push_back(id);
                accesses.push_back({ id, value.version, boundaryState(value, use.before), boundaryState(value, use.after), use.access, 0 });
            }
            Submission pending;
            pending.plan = graph.prepare(0, accesses.data(), accesses.size());
            pending.source = source;
            pending.stage = NRC_GPU_PRESENTATION;
            pending.fence = { graph.generation(), 0, 0, 0 };
            pending.borrowed = borrowed;
            waits(queues[0].Get(), pending.plan);
            if (diagnosticSerialFence)
                for (uint32_t q = 1; q < 3; ++q)
                    if (const auto value = graph.submitted(q))
                        check(queues[0]->Wait(fences[q].Get(), value), "Diagnostic graphics serial wait");
            submissions.push_back(std::move(pending));
            pendingGraphics = &submissions.back();
            graphicsLock = std::move(lock);  // held until commitGraphics: no other submission may interleave with this plan
            return ++graphicsTicket;
        }
        catch (...) { releaseBorrowed(); collectUnlocked(); throw; }
    }
    void commitGraphics(uint64_t ticket, uint64_t fenceValue)
    {
        gpuRequire(pendingGraphics && graphicsLock.owns_lock() && ticket == graphicsTicket, NRC_GPU_INVALID, "No prepared graphics use for this ticket");
        auto& tracked = *pendingGraphics;
        pendingGraphics = nullptr;
        try
        {
            tracked.fence = graph.commit(tracked.plan, fenceValue);
            tracked.committed = true;
            ++counts[0];
            for (const auto& access : tracked.plan.accesses) resources.at(access.resource).version = access.version;
        }
        catch (...) { faulted = true; accepting = false; for (uint64_t id : tracked.borrowed) --resources.at(id).references; graphicsLock.unlock(); throw; }
        for (uint64_t id : tracked.borrowed) --resources.at(id).references;
        tracked.borrowed.clear();
        graphicsLock.unlock();
    }
};

struct GpuBridgeHost::Impl::Lease
{
    std::atomic<uint64_t> references{ 1 };
    std::shared_ptr<GpuBridgeHost> owner;
    explicit Lease(std::shared_ptr<GpuBridgeHost> value) : owner(std::move(value)) {}
    template <class F> static int32_t call(void* context, F&& action) noexcept
    {
        if (!context) return NRC_GPU_INVALID;
        try { action(*static_cast<Lease*>(context)->owner->m_impl); return NRC_GPU_OK; }
        catch (const GpuContractError& error) { return error.code; }
        catch (const std::bad_alloc&) { return NRC_GPU_PRESSURE; }
        catch (const std::invalid_argument&) { return NRC_GPU_INVALID; }
        catch (...) { return NRC_GPU_INTERNAL; }
    }
    static int32_t NRC_GPU_CALL retain(void* context) noexcept
    {
        if (!context) return NRC_GPU_INVALID;
        auto& count = static_cast<Lease*>(context)->references;
        auto current = count.load();
        for (;;)
        {
            if (!current || current == UINT64_MAX) return NRC_GPU_STALE;
            if (count.compare_exchange_weak(current, current + 1)) return NRC_GPU_OK;
        }
    }
    static void NRC_GPU_CALL release(void* context) noexcept
    {
        if (context) { auto* lease = static_cast<Lease*>(context); if (--lease->references == 0) delete lease; }
    }
    static int32_t NRC_GPU_CALL reserve(void* context, const NRC_GpuAllocation* request, uint64_t* output) noexcept
    {
        if (!request || !output) return NRC_GPU_INVALID;
        return call(context, [&](Impl& self) { *output = self.reserve(*request); });
    }
    static int32_t NRC_GPU_CALL bind(void* context, uint64_t id, void* resource, uint32_t state) noexcept
    {
        return call(context, [&](Impl& self) { self.bind(id, static_cast<ID3D12Resource*>(resource), state); });
    }
    static int32_t NRC_GPU_CALL retainResource(void* context, uint64_t id) noexcept { return call(context, [&](Impl& self) { self.retainResource(id); }); }
    static int32_t NRC_GPU_CALL releaseResource(void* context, uint64_t id) noexcept { return call(context, [&](Impl& self) { self.releaseResource(id); }); }
    static int32_t NRC_GPU_CALL submit(void* context, const NRC_GpuSubmission* request, NRC_GpuFence* output) noexcept
    {
        if (!request || !output) return NRC_GPU_INVALID;
        return call(context, [&](Impl& self) { *output = self.submit(*request); });
    }
    static int32_t NRC_GPU_CALL poll(void* context, NRC_GpuFence token, uint32_t timeout, uint32_t* ready) noexcept
    {
        if (!ready) return NRC_GPU_INVALID;
        return call(context, [&](Impl& self) { *ready = self.poll(token, timeout) ? 1u : 0u; });
    }
    static int32_t NRC_GPU_CALL statistics(void* context, NRC_GpuStatistics* output) noexcept
    {
        if (!output || output->size != sizeof(*output) || output->version != 1) return NRC_GPU_INVALID;
        return call(context, [&](Impl& self) { *output = self.statistics(); });
    }
};

GpuBridgeHost::GpuBridgeHost(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue, ID3D12Fence* graphicsFence, uint64_t generation)
    : m_impl(std::make_unique<Impl>(device, graphicsQueue, graphicsFence, generation))
{
}
GpuBridgeHost::~GpuBridgeHost()
{
    try
    {
        m_impl->quiesce();
        for (auto& entry : m_impl->resources)
            if (entry.second.allocation && !entry.second.value) m_impl->ledger->release(entry.second.allocation);
        m_impl->resources.clear();
    }
    catch (...)
    {
        // No safe early free for unproven GPU completion: the faulted generation is quarantined until process exit, and
        // this module stays loaded while COM lifetime callbacks may still run.
        OutputDebugStringA("UnravelNext GPU bridge quarantined: completion could not be established during teardown.\n");
        HMODULE pinned = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&GpuBridgeHost::Impl::Lease::retain), &pinned);
        (void)m_impl.release();
    }
}
NRC_GpuBridge GpuBridgeHost::acquire()
{
    std::lock_guard lock(m_impl->mutex);
    m_impl->healthy();
    auto* lease = new Impl::Lease(shared_from_this());
    NRC_GpuBridge result{};
    result.size = sizeof(result);
    result.version = 1;
    result.generation = m_impl->graph.generation();
    result.context = lease;
    result.device = m_impl->device.Get();
    result.compute_queue = m_impl->queues[1].Get();
    result.compute_fence = m_impl->fences[1].Get();
    result.copy_queue = m_impl->queues[2].Get();
    result.copy_fence = m_impl->fences[2].Get();
    result.retain = Impl::Lease::retain;
    result.release = Impl::Lease::release;
    result.reserve = Impl::Lease::reserve;
    result.bind = Impl::Lease::bind;
    result.retain_resource = Impl::Lease::retainResource;
    result.release_resource = Impl::Lease::releaseResource;
    result.submit = Impl::Lease::submit;
    result.poll = Impl::Lease::poll;
    result.statistics = Impl::Lease::statistics;
    return result;
}
NRC_GpuStatistics GpuBridgeHost::statistics() { return m_impl->statistics(); }
void GpuBridgeHost::collect() { std::lock_guard lock(m_impl->mutex); m_impl->collectUnlocked(); }
void GpuBridgeHost::quiesce() { m_impl->quiesce(); }
uint64_t GpuBridgeHost::prepareGraphics(const std::vector<GraphicsUse>& uses, const NRC_GpuWorldStamp& source) { return m_impl->prepareGraphics(uses, source); }
void GpuBridgeHost::commitGraphics(uint64_t ticket, uint64_t fenceValue) { m_impl->commitGraphics(ticket, fenceValue); }
uint64_t GpuBridgeHost::resourceId(ID3D12Resource* resource)
{
    std::lock_guard lock(m_impl->mutex);
    auto at = m_impl->pointers.find(resource);
    return at == m_impl->pointers.end() ? 0 : at->second;
}
NRC_GpuFence GpuBridgeHost::submitPresentationCopy(ID3D12GraphicsCommandList* commands, ID3D12CommandAllocator* allocator,
                                                     const std::vector<BufferCopy>& copies, const NRC_GpuWorldStamp& source)
{
    std::lock_guard lock(m_impl->mutex);
    m_impl->healthy();
    std::map<uint64_t, NRC_GpuAccess> byId;
    for (const BufferCopy& copy : copies)
    {
        gpuRequire(copy.source && copy.destination && copy.source != copy.destination && copy.bytes,
                   NRC_GPU_INVALID, "Invalid presentation copy");
        for (const auto [id, write] : { std::pair{ copy.source, false }, std::pair{ copy.destination, true } })
        {
            const auto found = m_impl->resources.find(id);
            gpuRequire(found != m_impl->resources.end() && found->second.references && found->second.value && found->second.buffer,
                       NRC_GPU_STALE, "Presentation copy of an unbound or retired buffer");
            const auto& resource = found->second;
            gpuRequire(copy.bytes <= resource.value->GetDesc().Width && (!write || resource.version < UINT64_MAX),
                       NRC_GPU_INVALID, "Presentation copy exceeds its resource or version range");
            const uint32_t state = Impl::boundaryState(resource, 0);
            NRC_GpuAccess access{ id, resource.version + (write ? 1 : 0), state, state, uint32_t(write ? NRC_GPU_WRITE : NRC_GPU_READ), 0 };
            auto [at, added] = byId.emplace(id, access);
            gpuRequire(added || (at->second.mode == access.mode && !write), NRC_GPU_INVALID, "Aliased presentation copy target");
        }
    }
    std::vector<NRC_GpuAccess> accesses;
    for (const auto& [id, access] : byId) { (void)id; accesses.push_back(access); }
    NRC_GpuSubmission request{};
    request.size = sizeof request; request.version = 1;
    request.queue = NRC_GPU_COPY; request.stage = NRC_GPU_PRESENTATION; request.source = source;
    request.commands = commands; request.command_allocator = allocator;
    request.accesses = accesses.data(); request.access_count = accesses.size();
    return m_impl->submitLocked(request);
}

bool GpuBridgeHost::resourcesIdle(const std::vector<uint64_t>& resources)
{
    std::lock_guard lock(m_impl->mutex);
    m_impl->healthy();
    m_impl->collectUnlocked();
    for (uint64_t id : resources)
        if (id && (m_impl->resources.find(id) == m_impl->resources.end() || !m_impl->graph.retired(id))) return false;
    return true;
}

} // namespace unx::host
