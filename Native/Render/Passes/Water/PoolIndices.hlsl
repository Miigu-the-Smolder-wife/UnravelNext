// unx-kernel: cs_6_6 main
// Immutable grid topology, identical corner and primitive order to the original PoolMesh triangle soup.
#include "Bindless.hlsli"
[numthreads(256, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= 6u * 256u * 256u) return;
    const uint2 offset[6] = {uint2(0,0), uint2(0,1), uint2(1,0), uint2(1,0), uint2(0,1), uint2(1,1)};
    const uint cell = id / 6;
    const uint2 at = uint2(cell % 256u, cell / 256u) + offset[id % 6];
    RWByteAddressBuffer indices = ResourceDescriptorHeap[P[0].x];
    indices.Store(id * 4u, at.y * 257u + at.x);
}
