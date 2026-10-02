// unx-kernel: cs_6_6 main
// m.sss.clear (shading.subsurface_scatter; SubsurfaceScatter.hlsli): the Subsurface class's diffuse texture to 0 before the
// class's kernels add to it - alpha 0 marks the pixels that are not the class's, which the scatter pass's samples skip.
// P[0] = { diffuse UAV (RGBA16F), width, height, 0 }
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[0].yz)) return;
    RWTexture2D<float4> diffuse = ResourceDescriptorHeap[P[0].x];
    diffuse[id] = 0;
}
