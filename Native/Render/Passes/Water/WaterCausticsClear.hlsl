// unx-kernel: cs_6_6 main
// Water stage 2 caustics: zeroes the caustic slices (WaterCaustics.hlsl) before the splat.
// P[0] caustics UAV (RWTexture2DArray<uint>), grid texels per side, slices
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].y || id.y >= P[0].y || id.z >= P[0].z) return;
    RWTexture2DArray<uint> caustics = ResourceDescriptorHeap[P[0].x];
    caustics[id] = 0;
}
