// unx-kernel: cs_6_6 main
// Host boundary probe: the frame's last pass writes the display output (RGB10A2, INTERFACES 7.5) through a UAV, the
// way the shading kernel does, into the texture the host presents.
// P[0].x source UAV (RGBA16F), P[0].y output UAV (RGB10A2 unorm), P[0].zw size in pixels
#include "Bindless.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 p : SV_DispatchThreadID)
{
    if (any(p >= P[0].zw)) return;
    RWTexture2D<float4> source = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<unorm float4> output = ResourceDescriptorHeap[P[0].y];
    const float4 v = source[p];
    output[p] = float4(frac(v.rgb), 1);
}
