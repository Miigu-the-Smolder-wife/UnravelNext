// unx-kernel: cs_6_6 main
// V test: clears a RWTexture2D<uint>. P[0].x UAV, P[0].y width, P[0].z height, P[0].w value
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (p.x >= P[0].y || p.y >= P[0].z) return;
    RWTexture2D<uint> t = ResourceDescriptorHeap[P[0].x];
    t[p] = P[0].w;
}
