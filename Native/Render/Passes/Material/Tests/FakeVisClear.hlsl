// unx-kernel: cs_6_6 main
// M tests: clears the stand-in vis id target to VIS_NONE (0xFFFFFFFF). P[0] = { visId UAV, width, height, 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= P[0].yz)) return;
    RWTexture2D<uint> t = ResourceDescriptorHeap[P[0].x];
    t[p] = 0xFFFFFFFFu;
}
