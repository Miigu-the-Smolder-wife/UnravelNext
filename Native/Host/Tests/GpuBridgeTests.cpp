// Shared GPU bridge correctness (GpuBridge.h; engine 1): the NRC_GpuBridge callback table on the renderer's device.
//   1. reserve + bind charge the allocation ledger exactly (reserved = bound = the resources' allocation sizes)
//   2. a graphics read, a copy-queue upload and a compute-queue readback of the same buffer are ordered by the
//      dependency graph without the caller naming any fence: the graphics queue is held behind a CPU gate, so the
//      copy (write after the graphics read) and the compute read (after the copy write) must each wait on the GPU
//      (2 queue waits, deterministic); the graphics read sees the old contents and the compute read the new ones
//   3. a graphics use (two-phase: prepare, the renderer's own list, commit with its fence) reads the module's buffer
//      after the module's write, with the graph's queue wait on the graphics queue
//   4. released leases retire only after the GPU passed their last use; dropping the COM references returns the ledger
//      to zero
//   5. memory pressure denies an allocation beyond the OS budget (NRC_GPU_PRESSURE); a second binding of a lease and a
//      foreign completion token are rejected
//   unx_test_host_gpubridgetests [--no-debug-layer]
#include "GpuBridge/GpuBridge.h"

#include "unx/render/Device.h"
#include "unx/core/Log.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;
using unx::host::GpuBridgeHost;

#define B_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
ComPtr<ID3D12Resource> committed(ID3D12Device* device, uint64_t bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES properties{ heap };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r)), "bridge test resource");
    return r;
}
uint64_t allocationBytes(ID3D12Device* device, ID3D12Resource* r)
{
    const auto d = r->GetDesc();
    return device->GetResourceAllocationInfo(0, 1, &d).SizeInBytes;
}
struct Recorder
{
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Recorder(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type)
    {
        check(device->CreateCommandAllocator(type, IID_PPV_ARGS(&allocator)), "allocator");
        check(device->CreateCommandList(0, type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
    }
};
NRC_GpuWorldStamp stamp() { NRC_GpuWorldStamp s{}; s.world = 7; s.world_generation = 1; s.epoch = 1; s.tick = 1; s.branch = 1; s.phase = 1; return s; }
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true;
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
        Device device([&] { DeviceOptions o; o.debugLayer = debugLayer; return o; }());
        ID3D12Device* d3d = device.d3d();
        auto& graphics = device.queue(QueueType::Graphics);
        auto host = std::make_shared<GpuBridgeHost>(d3d, graphics.get(), graphics.fence(), 11);
        NRC_GpuBridge bridge = host->acquire();
        B_CHECK(bridge.size == sizeof(NRC_GpuBridge) && bridge.version == 1 && bridge.generation == 11 && bridge.device == d3d, "bridge table header");

        // 1. reserve + bind
        const uint64_t bytes = 1 << 20;
        auto target = committed(d3d, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
        auto upload = committed(d3d, bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        auto readback = committed(d3d, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        auto graphicsReadback = committed(d3d, bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        uint64_t ids[3] = {};
        ID3D12Resource* resources[3] = { target.Get(), upload.Get(), readback.Get() };
        const uint32_t memories[3] = { NRC_GPU_PERSISTENT, NRC_GPU_UPLOAD_MEMORY, NRC_GPU_READBACK };
        const uint32_t states[3] = { D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST };
        D3D12_FEATURE_DATA_ARCHITECTURE architecture{};
        d3d->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &architecture, sizeof(architecture));
        uint64_t charged = 0;
        for (int i = 0; i < 3; ++i)
        {
            NRC_GpuAllocation a{};
            a.size = sizeof(a); a.version = 1; a.bytes = allocationBytes(d3d, resources[i]); a.owner = 99; a.domain = NRC_GPU_PHYSICS_DOMAIN; a.memory = memories[i];
            a.nonlocal = (i > 0 && !architecture.UMA) ? 1u : 0u;
            B_CHECK(bridge.reserve(bridge.context, &a, &ids[i]) == NRC_GPU_OK && ids[i], "reserve %d", i);
            B_CHECK(bridge.bind(bridge.context, ids[i], resources[i], states[i]) == NRC_GPU_OK, "bind %d", i);
            charged += a.bytes;
        }
        NRC_GpuStatistics s{};
        s.size = sizeof(s); s.version = 1;
        B_CHECK(bridge.statistics(bridge.context, &s) == NRC_GPU_OK && s.reserved_bytes == charged && s.bound_bytes == charged && s.resources == 3,
                "ledger: reserved %llu bound %llu, charged %llu", (unsigned long long)s.reserved_bytes, (unsigned long long)s.bound_bytes, (unsigned long long)charged);
        B_CHECK(s.domain_bytes[NRC_GPU_PHYSICS_DOMAIN] == charged, "domain accounting");
        B_CHECK(bridge.bind(bridge.context, ids[0], target.Get(), 0) == NRC_GPU_STALE, "a lease bound twice must be stale");

        // 2. graphics read (held behind a CPU gate), copy-queue upload, compute-queue readback: the graph orders them
        std::vector<uint32_t> pattern(bytes / 4);
        for (size_t i = 0; i < pattern.size(); ++i) pattern[i] = uint32_t(i * 2654435761u) | 1u;
        void* mapped = nullptr;
        check(upload->Map(0, nullptr, &mapped), "map upload");
        std::memcpy(mapped, pattern.data(), bytes);
        upload->Unmap(0, nullptr);
        ComPtr<ID3D12Fence> gate;
        check(d3d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)), "gate fence");
        check(graphics.get()->Wait(gate.Get(), 1), "hold the graphics queue");
        uint64_t heldRead = 0;
        {
            const uint64_t ticket = host->prepareGraphics({ { target.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, NRC_GPU_READ } }, stamp());
            auto list = device.acquireCommandList(QueueType::Graphics);
            list.list->CopyBufferRegion(graphicsReadback.Get(), 0, target.Get(), 0, bytes);
            heldRead = device.submit(list);
            host->commitGraphics(ticket, heldRead);
        }
        Recorder copy(d3d, D3D12_COMMAND_LIST_TYPE_COPY);
        copy.list->CopyBufferRegion(target.Get(), 0, upload.Get(), 0, bytes);
        check(copy.list->Close(), "close copy");
        NRC_GpuAccess copyAccess[2] = { { ids[0], 2, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, NRC_GPU_WRITE, 0 },
                                        { ids[1], 1, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_GENERIC_READ, NRC_GPU_READ, 0 } };
        NRC_GpuSubmission sub{};
        sub.size = sizeof(sub); sub.version = 1; sub.queue = NRC_GPU_COPY; sub.stage = NRC_GPU_PHYSICS; sub.source = stamp();
        sub.commands = copy.list.Get(); sub.command_allocator = copy.allocator.Get(); sub.accesses = copyAccess; sub.access_count = 2;
        NRC_GpuFence copied{};
        B_CHECK(bridge.submit(bridge.context, &sub, &copied) == NRC_GPU_OK && copied.queue == NRC_GPU_COPY, "copy submission");
        Recorder compute(d3d, D3D12_COMMAND_LIST_TYPE_COMPUTE);
        compute.list->CopyBufferRegion(readback.Get(), 0, target.Get(), 0, bytes);
        check(compute.list->Close(), "close compute");
        NRC_GpuAccess readAccess[2] = { { ids[0], 2, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, NRC_GPU_READ, 0 },
                                        { ids[2], 2, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_DEST, NRC_GPU_WRITE, 0 } };
        sub.queue = NRC_GPU_COMPUTE; sub.commands = compute.list.Get(); sub.command_allocator = compute.allocator.Get(); sub.accesses = readAccess;
        NRC_GpuFence read{};
        B_CHECK(bridge.submit(bridge.context, &sub, &read) == NRC_GPU_OK, "compute submission");
        NRC_GpuStatistics held{};
        held.size = sizeof(held); held.version = 1;
        B_CHECK(bridge.statistics(bridge.context, &held) == NRC_GPU_OK && held.queue_waits == 2,
                "the copy must wait for the held graphics read and the compute read for the copy: %llu queue waits", (unsigned long long)held.queue_waits);
        uint32_t ready = 0;
        B_CHECK(bridge.poll(bridge.context, read, 0, &ready) == NRC_GPU_OK && !ready, "the compute read completed while the graphics queue was held");
        check(gate->Signal(1), "release the graphics queue");
        B_CHECK(bridge.poll(bridge.context, read, 10000, &ready) == NRC_GPU_OK && ready, "compute completion");
        check(readback->Map(0, nullptr, &mapped), "map readback");
        B_CHECK(std::memcmp(mapped, pattern.data(), bytes) == 0, "the compute queue read the buffer before the copy queue wrote it");
        readback->Unmap(0, nullptr);
        graphics.waitCpu(heldRead);
        check(graphicsReadback->Map(0, nullptr, &mapped), "map graphics readback");
        B_CHECK(std::memcmp(mapped, pattern.data(), bytes) != 0, "the graphics read saw the copy's data (the copy did not wait for it)");
        graphicsReadback->Unmap(0, nullptr);
        B_CHECK(bridge.statistics(bridge.context, &s) == NRC_GPU_OK && s.queue_waits == 2, "queue waits after release: %llu", (unsigned long long)s.queue_waits);

        // 3. graphics use of the module's buffer (two-phase)
        const uint64_t ticket = host->prepareGraphics({ { target.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, NRC_GPU_READ } }, stamp());
        auto list = device.acquireCommandList(QueueType::Graphics);
        list.list->CopyBufferRegion(graphicsReadback.Get(), 0, target.Get(), 0, bytes);
        const uint64_t frameFence = device.submit(list);
        host->commitGraphics(ticket, frameFence);
        graphics.waitCpu(frameFence);
        check(graphicsReadback->Map(0, nullptr, &mapped), "map graphics readback");
        B_CHECK(std::memcmp(mapped, pattern.data(), bytes) == 0, "the graphics frame read wrong data");
        graphicsReadback->Unmap(0, nullptr);

        // 4. retirement: leases released, then the COM references; the ledger returns to zero
        for (uint64_t id : ids) B_CHECK(bridge.release_resource(bridge.context, id) == NRC_GPU_OK, "release lease");
        host->collect();
        target.Reset(); upload.Reset(); readback.Reset();
        host->collect();
        B_CHECK(bridge.statistics(bridge.context, &s) == NRC_GPU_OK && s.resources == 0 && s.reserved_bytes == 0 && s.bound_bytes == 0,
                "retirement: %llu resources, %llu reserved bytes left", (unsigned long long)s.resources, (unsigned long long)s.reserved_bytes);

        // 5. pressure and foreign tokens
        NRC_GpuAllocation huge{};
        huge.size = sizeof(huge); huge.version = 1; huge.bytes = uint64_t(1) << 44; huge.owner = 99; huge.domain = NRC_GPU_PHYSICS_DOMAIN; huge.memory = NRC_GPU_PERSISTENT;
        uint64_t hugeId = 0;
        B_CHECK(bridge.reserve(bridge.context, &huge, &hugeId) == NRC_GPU_PRESSURE && hugeId == 0, "a 16 TB allocation must be denied");
        NRC_GpuFence foreign = read;
        foreign.generation = 12;
        B_CHECK(bridge.poll(bridge.context, foreign, 0, &ready) == NRC_GPU_STALE, "a foreign token must be stale");
        B_CHECK(bridge.statistics(bridge.context, &s) == NRC_GPU_OK && s.denied_allocations == 1, "denial count");

        bridge.release(bridge.context);
        host->quiesce();
        host.reset();
        const uint32_t errors = device.drainDebugMessages();
        B_CHECK(errors == 0, "%u debug-layer errors", errors);
        std::printf("gpu bridge: ledger exact (%llu B charged and returned), graphics read -> copy -> compute ordered by the graph (%llu queue waits while the graphics queue was held), graphics use after the "
                    "module write, pressure denial, stale binding and foreign token rejected\n",
                    (unsigned long long)charged, (unsigned long long)s.queue_waits);
        std::printf("gpu bridge tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
