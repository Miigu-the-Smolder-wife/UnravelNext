// unx-kernel: cs_6_6 main
// Decal query boxes (RayScene::recordDecals, HitDecals.hlsli): one procedural AABB per decal of the main view, world space
// (the camera-relative frame record's centre + camera, +- |axisX| + |axisY| + |axisZ| per component: the box of the
// oriented box). Decal i is AABB i, so a candidate's PrimitiveIndex is its frame record.
// P[0] = { frames SRV (StructuredBuffer<DecalFrame>), AABBs UAV (raw, D3D12_RAYTRACING_AABB: 24 B), count, 0 }.
// Frame constants: the main view (the frames' camera).
#include "Passes/Decal/Decal.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].z) return;
    StructuredBuffer<DecalFrame> frames = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer boxes = ResourceDescriptorHeap[P[0].y];
    const DecalFrame d = frames[id.x];
    const float3 c = d.centre + g_cameraPosition, e = abs(d.axisX) + abs(d.axisY) + abs(d.axisZ);
    boxes.Store3(id.x * 24, asuint(c - e));
    boxes.Store3(id.x * 24 + 12, asuint(c + e));
}
