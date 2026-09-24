// unx-kernel: cs_6_6 main
// Zeroes the reflection distance history after it is (re)created (no distance known: G spacing 1 until measured).
// P[0] = { history UAV, width, height, 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    if (pixel.x >= P[0].y || pixel.y >= P[0].z) return;
    RWTexture2D<float> history = ResourceDescriptorHeap[P[0].x];
    history[pixel] = 0;
}
