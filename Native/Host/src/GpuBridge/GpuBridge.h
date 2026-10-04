#pragma once
// Shared GPU bridge (engine 1, B8 host decision (a), 2026-09-26): one device for the renderer and the engine modules
// (physics fluid, VFX). The host hands NativePhysics / NativeVfx an NRC_GpuBridge (GpuExecutionAbi.h, the old engine's
// runtime ABI, byte-identical to Unravel's Native/RuntimeCommon copy) on the renderer's own device: their compute and
// copy work runs on the bridge's queues, their buffers are charged to one allocation ledger against the OS budget, and a
// dependency graph orders every producer and consumer by resource version and queue fence (port of TitanNative's
// SharedGpuExecution, which the old renderer owned).
// Graphics use of module resources (the renderer reading the fluid particles) is two-phase: prepareGraphics() admits the
// frame's reads, issues the queue waits the graph requires on the renderer's graphics queue and returns a ticket;
// commitGraphics() records the frame's graphics fence value once its lists are submitted, so the module's buffers stay
// alive until that frame completes.
#include "GpuExecutionAbi.h"
#include "GpuAllocationLedger.h"
#include "GpuDependencyGraph.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <memory>
#include <vector>

namespace unx::host
{
class GpuBridgeHost : public std::enable_shared_from_this<GpuBridgeHost>
{
    struct Impl;
    std::unique_ptr<Impl> m_impl;

public:
    struct GraphicsUse
    {
        ID3D12Resource* resource = nullptr;
        uint32_t before = 0, after = 0;  // D3D12_RESOURCE_STATES at the frame's entry and exit (buffers: COMMON)
        uint32_t access = NRC_GPU_READ;
    };
    // graphicsQueue/graphicsFence: the renderer's graphics queue and its completion fence (its values increase with each
    // frame). generation: this device's identity for fences (nonzero; a new device is a new generation).
    GpuBridgeHost(ID3D12Device* device, ID3D12CommandQueue* graphicsQueue, ID3D12Fence* graphicsFence, uint64_t generation);
    ~GpuBridgeHost();
    GpuBridgeHost(const GpuBridgeHost&) = delete;
    GpuBridgeHost& operator=(const GpuBridgeHost&) = delete;

    // A callback table leased to one module (released by the module through bridge.release).
    NRC_GpuBridge acquire();
    NRC_GpuStatistics statistics();
    void collect();
    // Stops new work and waits until every submitted list completed (device teardown).
    void quiesce();

    // Phase 1: admits the frame's graphics reads/writes of module resources and makes the graphics queue wait for
    // their producers (queue-to-queue waits, no CPU wait). Returns a ticket for commitGraphics.
    uint64_t prepareGraphics(const std::vector<GraphicsUse>& uses, const NRC_GpuWorldStamp& source);
    // Phase 2: the frame's lists were submitted; `fenceValue` is the graphics fence value signalled after them.
    void commitGraphics(uint64_t ticket, uint64_t fenceValue);
    // The module-side id of a bound resource (0 if the bridge does not know it).
    uint64_t resourceId(ID3D12Resource* resource);
    // Host-owned presentation copies use the same resource-version admission
    // as modules, atomically reading their versions under the bridge mutex.
    struct BufferCopy { uint64_t source = 0, destination = 0, bytes = 0; };
    NRC_GpuFence submitPresentationCopy(ID3D12GraphicsCommandList* commands, ID3D12CommandAllocator* allocator,
                                        const std::vector<BufferCopy>& copies, const NRC_GpuWorldStamp& source);
    bool resourcesIdle(const std::vector<uint64_t>& resources);  // no CPU GPU wait
};
} // namespace unx::host
