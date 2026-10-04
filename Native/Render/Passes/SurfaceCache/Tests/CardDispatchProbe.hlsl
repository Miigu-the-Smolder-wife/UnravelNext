// unx-kernel: cs_6_6 main
#include "Bindless.hlsli"
[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].x];
    output.Store((P[0].y + id.x) * 4, id.x + 1);
}
