// Frozen GPU particle publications. Authoritative fluid buffers may advance on
// the World worker as soon as these copies have read them; frames retain their
// own immutable publication independently of the next physics tick.
#include "Renderer/HostRenderer.h"
#include "GpuBridge/GpuBridge.h"
#include "unx/render/Device.h"
#include <algorithm>
#include <atomic>
#include <limits>

namespace unx::host
{
using namespace render;

struct FluidPresentation
{
    NRC_GpuBridge bridge{};
    uint64_t owner = 0;
    std::vector<ComPtr<ID3D12Resource>> buffers, sources;
    std::vector<uint64_t> ids, capacities, key;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commands;
    NRC_GpuFence ready{};

    explicit FluidPresentation(GpuBridgeHost& host) : bridge(host.acquire())
    {
        static std::atomic<uint64_t> serial{1};
        const uint64_t id = serial.fetch_add(1);
        if (id >= (1ull << 40)) { bridge.release(bridge.context); bridge = {}; fail("fluid presentation identities exhausted"); }
        owner = (uint64_t(0x465052) << 40) | id;
    }
    ~FluidPresentation()
    {
        for (uint64_t id : ids) if (id) bridge.release_resource(bridge.context, id);
        if (bridge.context) bridge.release(bridge.context);
    }
    void size(Device& device, size_t slot, uint64_t bytes)
    {
        if (slot >= buffers.size())
        {
            buffers.resize(slot + 1); ids.resize(slot + 1); capacities.resize(slot + 1);
        }
        if (buffers[slot] && capacities[slot] >= bytes) return;
        uint64_t capacity = 256;
        while (capacity < bytes)
        {
            if (capacity > UINT64_MAX / 2) fail("fluid presentation capacity overflow");
            capacity *= 2;
        }
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = capacity;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        NRC_GpuAllocation allocation{};
        allocation.size = sizeof allocation; allocation.version = 1;
        allocation.bytes = device.d3d()->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        allocation.owner = owner; allocation.domain = NRC_GPU_RENDERER; allocation.memory = NRC_GPU_HISTORY;
        uint64_t id = 0;
        if (bridge.reserve(bridge.context, &allocation, &id) != NRC_GPU_OK) fail("fluid presentation GPU allocation denied");
        ComPtr<ID3D12Resource> next;
        try
        {
            check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                  nullptr, IID_PPV_ARGS(&next)), "fluid presentation buffer");
            if (bridge.bind(bridge.context, id, next.Get(), D3D12_RESOURCE_STATE_COMMON) != NRC_GPU_OK)
                fail("fluid presentation GPU binding failed");
        }
        catch (...) { bridge.release_resource(bridge.context, id); throw; }
        if (ids[slot]) bridge.release_resource(bridge.context, ids[slot]);
        buffers[slot] = std::move(next); ids[slot] = id; capacities[slot] = capacity;
    }
};

void HostRenderer::freezeFluidPresentation(std::vector<FramePacket::Fluid>& fluids, const uint64_t (&stamp)[6])
{
    // The caller is at a completed World publication. Validate the byte ranges
    // before admitting any copy; alpha/material/origin changes reuse the same
    // immutable particle bytes and do not create another GPU publication.
    std::vector<uint64_t> key(std::begin(stamp), std::end(stamp));
    std::vector<ID3D12Resource*> sources;
    std::vector<uint64_t> sourceIds, bytes;
    for (const auto& f : fluids)
    {
        const auto& v = f.frame;
        key.insert(key.end(), {f.currentResource, f.startResource, uint64_t(reinterpret_cast<uintptr_t>(v.current)),
                              uint64_t(reinterpret_cast<uintptr_t>(v.start)), v.tick, v.count, v.startCount, v.stride, v.startValid});
        for (uint32_t side = 0; side < 2; ++side)
        {
            ID3D12Resource* resource = side ? (v.startValid ? v.start : nullptr) : v.current;
            const uint64_t count = side ? v.startCount : v.count;
            if (resource)
            {
                const auto desc = resource->GetDesc();
                if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || count > desc.Width / v.stride)
                    fail("fluid presentation particle range exceeds its source buffer");
            }
            sources.push_back(resource);
            sourceIds.push_back(side ? f.startResource : f.currentResource);
            bytes.push_back(resource ? count * v.stride : 0);
        }
    }
    std::shared_ptr<FluidPresentation> snapshot;
    for (const auto& candidate : m_fluidPresentations)
        if (candidate->key == key) { snapshot = candidate; break; }
    if (!snapshot)
    {
        for (const auto& candidate : m_fluidPresentations)
            if (candidate.use_count() == 1 && gpuBridge().resourcesIdle(candidate->ids)) { snapshot = candidate; break; }
        if (!snapshot)
        {
            snapshot = std::make_shared<FluidPresentation>(gpuBridge());
            m_fluidPresentations.push_back(snapshot);
        }
        // No publication may refer to this entry until the entire batch of
        // copies has been admitted. Failed recording never relabels old bytes.
        snapshot->key.clear();
        for (size_t i = 0; i < sources.size(); ++i)
        {
            if (sources[i] && (!sourceIds[i] || gpuBridge().resourceId(sources[i]) != sourceIds[i]))
                fail("fluid presentation source identity is stale");
            if (sources[i]) snapshot->size(*m_device, i, std::max<uint64_t>(bytes[i], 4));
        }
        if (!snapshot->allocator)
        {
            check(m_device->d3d()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&snapshot->allocator)), "fluid presentation allocator");
            check(m_device->d3d()->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, snapshot->allocator.Get(), nullptr,
                  IID_PPV_ARGS(&snapshot->commands)), "fluid presentation list");
        }
        else
        {
            check(snapshot->allocator->Reset(), "fluid presentation allocator reset");
            check(snapshot->commands->Reset(snapshot->allocator.Get(), nullptr), "fluid presentation list reset");
        }
        std::vector<GpuBridgeHost::BufferCopy> copies;
        snapshot->sources.clear();
        try
        {
            for (size_t i = 0; i < sources.size(); ++i)
            {
                if (!sources[i]) continue;
                snapshot->sources.emplace_back(sources[i]);
                if (!bytes[i]) continue;
                snapshot->commands->CopyBufferRegion(snapshot->buffers[i].Get(), 0, sources[i], 0, bytes[i]);
                copies.push_back({sourceIds[i], snapshot->ids[i], bytes[i]});
            }
            check(snapshot->commands->Close(), "fluid presentation list close");
            NRC_GpuWorldStamp source{};
            source.world = stamp[0]; source.world_generation = stamp[1]; source.epoch = stamp[2]; source.tick = stamp[3];
            source.branch = stamp[4]; source.phase = (uint32_t)stamp[5];
            if (!source.world) { source.world = 1; source.world_generation = snapshot->bridge.generation; source.branch = 1; source.phase = NRC_GPU_PRESENTATION; }
            snapshot->ready = gpuBridge().submitPresentationCopy(snapshot->commands.Get(), snapshot->allocator.Get(), copies, source);
        }
        catch (...)
        {
            // A failed submission can already own the list: discard this cache
            // entry instead of ever resetting its allocator speculatively.
            m_fluidPresentations.erase(std::remove(m_fluidPresentations.begin(), m_fluidPresentations.end(), snapshot), m_fluidPresentations.end());
            throw;
        }
        snapshot->key = std::move(key);
    }
    for (size_t i = 0; i < fluids.size(); ++i)
    {
        auto& f = fluids[i];
        f.presentation = snapshot;
        f.frame.current = snapshot->buffers[2 * i].Get(); f.currentResource = snapshot->ids[2 * i];
        if (f.frame.startValid)
        {
            f.frame.start = snapshot->buffers[2 * i + 1].Get(); f.startResource = snapshot->ids[2 * i + 1];
        }
        else { f.frame.start = nullptr; f.startResource = 0; }
    }
}
} // namespace unx::host
