// unx-kernel: cs_6_6 main
// Host boundary probe, standalone host only: stands in for the host's presentation blit (Unity copies the renderer's
// output into its back buffer). Reads the RGB10A2 output and writes an RGBA8 target of the same size.
// P[0].x output UAV (RGB10A2, typed UAV load), P[0].y target UAV (RGBA8 unorm), P[0].zw size in pixels
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= P[0].zw)) return;
    RWTexture2D<unorm float4> output = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<unorm float4> target = ResourceDescriptorHeap[P[0].y];
    target[p] = output[p];
}
