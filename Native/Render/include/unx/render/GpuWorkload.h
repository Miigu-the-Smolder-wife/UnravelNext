#pragma once
#include "unx/render/D3D12.h"

namespace unx::render
{
// Recorded commands, not estimates of shader-internal work. Indirect command counts/dimensions
// and inline TraceRay calls are deliberately not inferred from their upper bounds.
struct GpuWorkload
{
    uint64_t dispatches = 0, groups = 0, draws = 0, indirect = 0, rayDispatches = 0, rayLaunches = 0;
    uint64_t computeInvocations = 0, pixelInvocations = 0, rasterPrimitives = 0;
    bool pipelineStatistics = false;
};
inline thread_local GpuWorkload* recordingWorkload = nullptr;
inline thread_local ID3D12GraphicsCommandList* recordingWorkloadCommands = nullptr;
inline GpuWorkload* workloadFor(ID3D12GraphicsCommandList* cmd)
{
    return cmd == recordingWorkloadCommands ? recordingWorkload : nullptr;
}
inline void gpuDispatch(ID3D12GraphicsCommandList* cmd, UINT x, UINT y, UINT z)
{
    if (auto* w = workloadFor(cmd)) { ++w->dispatches; w->groups += uint64_t(x) * y * z; }
    cmd->Dispatch(x, y, z);
}
inline void gpuDispatchMesh(ID3D12GraphicsCommandList6* cmd, UINT x, UINT y, UINT z)
{
    if (auto* w = workloadFor(cmd)) { ++w->draws; w->groups += uint64_t(x) * y * z; }
    cmd->DispatchMesh(x, y, z);
}
inline void gpuDispatchRays(ID3D12GraphicsCommandList4* cmd, const D3D12_DISPATCH_RAYS_DESC* d)
{
    if (auto* w = workloadFor(cmd)) { ++w->rayDispatches; w->rayLaunches += uint64_t(d->Width) * d->Height * d->Depth; }
    cmd->DispatchRays(d);
}
inline void gpuExecuteIndirect(ID3D12GraphicsCommandList* cmd, ID3D12CommandSignature* signature,
    UINT maximum, ID3D12Resource* arguments, UINT64 offset, ID3D12Resource* count, UINT64 countOffset)
{
    if (auto* w = workloadFor(cmd)) ++w->indirect;
    cmd->ExecuteIndirect(signature, maximum, arguments, offset, count, countOffset);
}
inline void gpuDrawInstanced(ID3D12GraphicsCommandList* cmd, UINT vertices, UINT instances, UINT firstVertex, UINT firstInstance)
{
    if (auto* w = workloadFor(cmd)) ++w->draws;
    cmd->DrawInstanced(vertices, instances, firstVertex, firstInstance);
}
inline void gpuDrawIndexedInstanced(ID3D12GraphicsCommandList* cmd, UINT indices, UINT instances, UINT firstIndex, INT baseVertex, UINT firstInstance)
{
    if (auto* w = workloadFor(cmd)) ++w->draws;
    cmd->DrawIndexedInstanced(indices, instances, firstIndex, baseVertex, firstInstance);
}
}
