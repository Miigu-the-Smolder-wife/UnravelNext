// unx-kernel: cs_6_6 main
// Dense second stage of local overflow visibility. One unique destination byte
// per item; OR combines disjoint bytes after the producer wrote settled values.
// P0 = { queue SRV, overflow UAV, depth SRV, gbuffer SRV }
// P1 = { table SRV, atlas SRV, blocks SRV, constants CBV }, P2.x = local lights SRV.
#include "Passes/Shadow/ShadowVisibility.hlsli"
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer queue = ResourceDescriptorHeap[P[0].x];
    const uint item = (group.y * 65535u + group.x) * 64u + lane;
    if (item >= min(queue.Load(0), queue.Load(4))) return;
    const uint address = 16 + item * 48;
    const uint4 head = queue.Load4(address), body = queue.Load4(address + 16), tail = queue.Load4(address + 32);
    const uint2 pixel = uint2(head.x & 0xFFFFu, head.x >> 16);
    const ShadowPixelReceiver rc = shadowPixelReceiver(pixel, P[0].z, P[0].w);
    VsmLocalFilter filter;
    filter.centre = asfloat(body.xyz); filter.radius = asfloat(body.w);
    filter.tolerance = asfloat(tail.x); filter.mip = tail.y; filter.face = tail.z;
    VsmLocalResources r;
    r.table = ResourceDescriptorHeap[P[1].x]; r.pool = ResourceDescriptorHeap[P[1].y]; r.blocks = ResourceDescriptorHeap[P[1].z];
    StructuredBuffer<VsmLocalLight> lights = ResourceDescriptorHeap[P[2].x];
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[P[1].w];
    const float v = vsmLocalFilterVisibility(r, lights[head.y], head.y, rc.world, rc.normal, filter, c.filterTaps);
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].y];
    output.InterlockedOr(head.z, (uint)round(saturate(v) * 255.0) << head.w);
}
