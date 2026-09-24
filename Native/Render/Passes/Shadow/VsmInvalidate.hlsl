// unx-kernel: cs_6_6 main
// Dirty rule (a) of ARCHITECTURE 2.3, second half: for each moved instance (VsmMoved) and clipmap level, every resident
// page under the instance's old or new bounds becomes stale. One group per (instance, level); its 64 threads split the
// page rectangle, so a large instance near the camera costs a few iterations per thread, not one thread's serial walk.
// Skinned bounds: the bind-pose sphere (see S_STATUS_KO.md). The wind rule (b) is per page in VsmRelease.
// P[0].x page table UAV (raw), P[0].y moved list SRV (raw), P[0].z VSM constants CBV, P[0].w unused.
// Frame constants of the main view (scene buffers, wind).
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmCommon.hlsli"

void markRect(ConstantBuffer<VsmConstants> c, RWByteAddressBuffer table, float3 centre, float radius, uint k, uint lane)
{
    const float3 ls = vsmLightSpace(c, centre);
    const float pageSize = vsmTexel(k) * VSM_PAGE;
    const int2 lo = max(int2(floor((ls.xy - radius) / pageSize)), vsmOrigin(c, k));
    const int2 hi = min(int2(floor((ls.xy + radius) / pageSize)), vsmOrigin(c, k) + (int)VSM_TABLE - 1);
    if (any(lo > hi)) return;
    const uint2 size = uint2(hi - lo + 1);
    [loop] for (uint i = lane; i < size.x * size.y; i += 64)
    {
        const int2 page = lo + int2(i % size.x, i / size.x);
        const uint slot = vsmSlot(page, k);
        const uint2 e = table.Load2(slot * 8);
        if ((e.x & VSM_FLAG_RESIDENT) != 0 && e.y == vsmTag(page) && (e.x & VSM_FLAG_STALE) == 0) table.InterlockedOr(slot * 8, VSM_FLAG_STALE);
    }
}

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer moved = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer table = ResourceDescriptorHeap[P[0].x];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[0].z];
    const GpuInstance inst = loadInstance(moved.Load(4 + group.x * 4));
    const GpuMesh mesh = loadMesh(inst.mesh);
    const float scale = length(inst.objectToWorld[0].xyz);
    // Deformed extent: the bind-pose sphere grown by the largest wind displacement.
    const float radius = (mesh.boundsSphere.w + windOffsetBound(inst, mesh.boundsSphere.xyz, mesh.boundsSphere.w)) * scale;
    markRect(c, table, transformPoint(inst.objectToWorld, mesh.boundsSphere.xyz), radius, group.y, lane);
    markRect(c, table, transformPoint(inst.prevObjectToWorld, mesh.boundsSphere.xyz), radius, group.y, lane);
}
