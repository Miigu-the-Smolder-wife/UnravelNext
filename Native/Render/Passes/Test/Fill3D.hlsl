// unx-kernel: cs_6_6 main
// Render-graph test kernel: writes (x, y, z, 1) into every texel of a 3D float4 UAV (aliasing tests read it back).
// P[0].x RWTexture3D<float4> UAV, P[0].yzw size
#include "Bindless.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 p : SV_DispatchThreadID)
{
    if (any(p >= P[0].yzw)) return;
    RWTexture3D<float4> t = ResourceDescriptorHeap[P[0].x];
    t[p] = float4(p, 1);
}
