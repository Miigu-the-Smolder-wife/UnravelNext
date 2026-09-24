// unx-kernel: cs_6_6 main
// Empties the penumbra list of a shadow visibility run (ShadowVisibility.hlsl appends to it).
// P[0].x list UAV (raw)
#include "Bindless.hlsli"

[numthreads(1, 1, 1)]
void main()
{
    RWByteAddressBuffer list = ResourceDescriptorHeap[P[0].x];
    list.Store(0, 0);
}
