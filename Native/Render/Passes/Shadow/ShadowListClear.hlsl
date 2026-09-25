// unx-kernel: cs_6_6 main
// Empties the penumbra list of a shadow visibility run (ShadowVisibility.hlsl appends to it) and, in the main view, the
// overflow tile list and the fallback tile list (count 0, dispatch args (0, 1, 1)).
// P[0].x list UAV (raw), P[0].y overflow tile list UAV (raw), P[0].z fallback tile list UAV (raw), P[0].w overflow
// allocation counter UAV (raw; 0xFFFFFFFF: none of the three)
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    list.Store(0, 0);
    if (P[0].y != 0xFFFFFFFFu)
    {
        RWByteAddressBuffer tiles = ResourceDescriptorHeap[P[0].y];
        tiles.Store4(0, uint4(0, 0, 1, 1));
    }
    if (P[0].z != 0xFFFFFFFFu)
    {
        RWByteAddressBuffer fallback = ResourceDescriptorHeap[P[0].z];
        fallback.Store4(0, uint4(0, 0, 1, 1));
    }
    if (P[0].w != 0xFFFFFFFFu)
    {
        RWByteAddressBuffer counter = ResourceDescriptorHeap[P[0].w];
        counter.Store(0, 0);
    }
}
