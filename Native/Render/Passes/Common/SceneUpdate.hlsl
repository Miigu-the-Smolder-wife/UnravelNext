// unx-kernel: cs_6_6 main
// Core: per-frame GPU scene updates (GpuScene::flushUpdates): scatters 16-byte elements from the frame's upload slot
// into the scene buffers. Upload layout: count headers (target << 28 | destination element), padded to 16 B, then
// count uint4 payloads. Targets: 0 instances, 1 bone palette, 2 previous bone palette, 3 morph records (C4).
//   P[0] upload SRV (raw), element count, instances UAV (raw), bone palette UAV (raw); P[1].x previous palette UAV (raw),
//   P[1].y morph records UAV (raw); C2b runtime pool (targets 4-14: meshes, submeshes, vertices, indices, clusters,
//   cluster vertex indices, cluster triangles, V's nodes, mesh roots, LOD spheres, sheets): P[1].zw, P[2], P[3], P[4].x;
//   C5 terrain patch slots (target 15): P[4].y
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    // Large scene publications span rows without exceeding D3D12's 65535
    // groups per dimension. Small updates retain the original one-row layout.
    const uint i = (group.y * 65535u + group.x) * 64u + lane;
    const uint count = P[0].y;
    if (i >= count) return;
    ByteAddressBuffer upload = ResourceDescriptorHeap[P[0].x];
    const uint header = upload.Load(4 * i);
    const uint4 payload = upload.Load4(((count * 4 + 15) & ~15u) + 16 * i);
    const uint target = header >> 28, element = header & 0x0FFFFFFFu;
    // Raw UAV of each target: P[0].zw, P[1], P[2], P[3], P[4].xy = targets 0 .. 15 (4 .. 14: C2b runtime pool buffers,
    // 15: C5 terrain patch slots).
    const uint uavs[16] = { P[0].z, P[0].w, P[1].x, P[1].y, P[1].z, P[1].w, P[2].x, P[2].y, P[2].z, P[2].w, P[3].x, P[3].y, P[3].z, P[3].w, P[4].x, P[4].y };
    // Adjacent upload elements can belong to different scene streams.
    RWByteAddressBuffer destination = ResourceDescriptorHeap[NonUniformResourceIndex(uavs[target])];
    destination.Store4(16 * element, payload);
}
