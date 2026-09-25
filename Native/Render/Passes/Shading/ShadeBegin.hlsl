// unx-kernel: cs_6_6 main
// Start of the shading passes: resets the edge args (composite dispatch (0, 1, 1), pixel count 0; Edge.hlsli) and bounds
// every class's tile count in the resolve's dispatch arguments by the view's tile count. A count cannot exceed it by
// construction; the bound keeps an indirect dispatch finite if the arguments were ever clobbered (e.g. by a transient
// alias written late) instead of launching billions of groups and hanging the GPU.
// It also turns S's shadow overflow fallback list (INTERFACES 7.3: word 0 = tile count) into M's own dispatch arguments for
// the fallback shading kernel, bounded the same way (the list is that kernel's SRV, which the graph cannot also bind as
// its indirect arguments).
// P[0] = { edge args UAV (raw, 24 B), class tile args UAV (raw, classes x 12 B), classes, tiles in the view }
// P[1] = { S's fallback tile list SRV (raw) or UNX_NONE, M's fallback args UAV (raw, 12 B) or UNX_NONE }
#include "Bindless.hlsli"
#include "Frame.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer edge = ResourceDescriptorHeap[P[0].x];
    edge.Store3(0, uint3(0, 1, 1));
    edge.Store3(12, uint3(0, 1, 1));
    RWByteAddressBuffer tiles = ResourceDescriptorHeap[P[0].y];
    for (uint i = 0; i < P[0].z; ++i) tiles.Store(12 * i, min(tiles.Load(12 * i), P[0].w));
    if (P[1].y != UNX_NONE)
    {
        uint n = 0;
        if (P[1].x != UNX_NONE)
        {
            ByteAddressBuffer fallback = ResourceDescriptorHeap[P[1].x];
            n = min(fallback.Load(0), P[0].w);
        }
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].y];
        args.Store3(0, uint3(n, 1, 1));
    }
}
