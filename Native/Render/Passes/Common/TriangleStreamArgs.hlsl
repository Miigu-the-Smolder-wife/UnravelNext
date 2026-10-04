// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
[numthreads(32, 1, 1)]
void main(uint lane : SV_GroupIndex)
{
    if (lane >= P[0].y) return;
    const uint4 pair = P[1 + lane / 2];
    const uint descriptor = (lane & 1u) ? pair.z : pair.x;
    const uint capacity = (lane & 1u) ? pair.w : pair.y;
    ByteAddressBuffer draw = ResourceDescriptorHeap[descriptor];
    const uint triangles = min(draw.Load(0) / 3, capacity);
    const uint groups = triangles / 32 + (triangles % 32 != 0 ? 1u : 0u);
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].x];
    output.Store3((P[0].z + lane) * 12, uint3(min(groups, 65535u), max(1u, (groups + 65534u) / 65535u), 1));
}
