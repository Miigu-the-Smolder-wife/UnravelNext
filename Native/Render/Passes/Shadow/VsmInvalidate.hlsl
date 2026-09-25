// unx-kernel: cs_6_6 main
// Dirty rule (a) of ARCHITECTURE 2.3, second half: for each moved instance (VsmMoved) and each clipmap level of its mask,
// every resident page under the instance's bounds where the level last rendered it (the level's anchor) or where it is
// now becomes stale, and the anchor moves here. One group per (instance, level); its 64 threads split the page
// rectangle, so a large instance near the camera costs a few iterations per thread, not one thread's serial walk.
// Skinned bounds: the bind-pose sphere (see S_STATUS_KO.md). The wind rule (b) is per page in VsmRelease.
// P[0].x page table UAV (raw), P[0].y moved list SRV (raw: count, then (instance, level mask) pairs), P[0].z VSM
// constants CBV, P[0].w motion state UAV (VsmMoved).
// Frame constants of the main view (scene buffers, wind).
#include "Deformation.hlsli"
#include "Passes/Shadow/VsmCommon.hlsli"

void markRect(ConstantBuffer<VsmConstants> c, RWByteAddressBuffer table, float3 centre, float radius, uint k, uint lane)
{
    const float3 ls = vsmLightSpaceAt(c, centre, k);
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
    const uint2 entry = moved.Load2(4 + group.x * 8);
    const uint k = group.y;
    if ((entry.y >> k & 1u) == 0) return;
    const GpuInstance inst = loadInstance(entry.x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const float scale = length(inst.objectToWorld[0].xyz);
    // Deformed extent: the bind-pose sphere grown by the largest wind displacement.
    const float radius = (mesh.boundsSphere.w + windOffsetBound(inst, mesh.boundsSphere.xyz, mesh.boundsSphere.w)) * scale;
    RWStructuredBuffer<float4> motion = ResourceDescriptorHeap[P[0].w];
    const float3 centre = transformPoint(inst.objectToWorld, mesh.boundsSphere.xyz), anchor = motion[entry.x * VSM_LEVELS + k].xyz;
    markRect(c, table, centre, radius, k, lane);
    markRect(c, table, anchor, radius, k, lane);
    if (lane == 0) motion[entry.x * VSM_LEVELS + k] = float4(centre, 0);
}
