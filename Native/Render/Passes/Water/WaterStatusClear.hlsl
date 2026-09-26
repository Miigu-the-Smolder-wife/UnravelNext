// unx-kernel: cs_6_6 main
// Water surface tests: zeroes the per-pixel status image (WaterSurface.h WaterSurfaceDebug).
// P[0] status UAV (R8_UINT), width, height
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].y || id.y >= P[0].z) return;
    RWTexture2D<uint> status = ResourceDescriptorHeap[P[0].x];
    status[id.xy] = 0;
}
