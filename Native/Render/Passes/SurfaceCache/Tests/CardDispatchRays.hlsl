// unx-kernel: lib_6_6 main
#include "Bindless.hlsli"
[shader("raygeneration")]
void CardDispatchTestGen()
{
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].x];
    const uint thread = P[0].z + DispatchRaysIndex().x;
    output.Store((P[0].y + thread) * 4, thread + 1);
}
