// unx-kernel: cs_6_6 main
// m.tsr.clear: the closest occluder scatter texture to 0 before m.tsr.dilate's atomic max (Tsr.hlsli).
// P[0] = { scatter UAV (R32_UINT), width, height, 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[0].yz)) return;
    RWTexture2D<uint> scatter = ResourceDescriptorHeap[P[0].x];
    scatter[id] = 0;
}
