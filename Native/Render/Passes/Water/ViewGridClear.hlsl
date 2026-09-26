// unx-kernel: cs_6_6 main
// Water view grid: the frame's pixel keys to ~0 (no water) and its counters to zero.
// P[0] key UAV (raw, 8 B per pixel), pixels, counter UAV (raw), counter words
#include "Bindless.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i < P[0].y)
    {
        RWByteAddressBuffer keys = ResourceDescriptorHeap[P[0].x];
        keys.Store2(i * 8, uint2(0xFFFFFFFFu, 0xFFFFFFFFu));
    }
    if (i < P[0].w)
    {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[P[0].z];
        counters.Store(i * 4, 0u);
    }
}
