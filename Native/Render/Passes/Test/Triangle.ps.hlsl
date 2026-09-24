// unx-kernel: ps_6_6 main
// unx-variants: OUT=0,1,2
// Render-graph test pixel shader. OUT=0: float colour target, OUT=1: uint identity target (visibility buffer shape),
// OUT=2: no target, appends to a raw UAV (coverage-layer shape; P[1].x = UAV index).
#include "Bindless.hlsli"

#if OUT == 0
float4 main(float4 position : SV_Position) : SV_Target0
{
    return float4(0.25, 0.5, 0.75, 1);
}
#elif OUT == 1
uint main(float4 position : SV_Position) : SV_Target0
{
    return P[0].x;
}
#else
void main(float4 position : SV_Position)
{
    RWByteAddressBuffer fragments = ResourceDescriptorHeap[P[1].x];
    uint slot;
    fragments.InterlockedAdd(0, 1, slot);
    fragments.Store4(16 + (slot & 1023) * 16, uint4(position.xy, asuint(position.z), P[0].x));
}
#endif
