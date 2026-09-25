// unx-kernel: cs_6_6 main
// M edge tests: edgeTriangleArea (Edge.hlsli) and V's coverageTriangleArea for query rows (a.xy, b.xy), (c.xy, pixel.xy)
// in screen pixels. P[0] = { queries SRV (float4), out UAV (float2 per row: M, V), count }
#include "Bindless.hlsli"
#include "Passes/Shading/Edge.hlsli"
#include "Passes/Visibility/Coverage.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    StructuredBuffer<float4> q = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float2> o = ResourceDescriptorHeap[P[0].y];
    const float4 r0 = q[2 * i], r1 = q[2 * i + 1];
    o[i] = float2(edgeTriangleArea(r0.xy, r0.zw, r1.xy, r1.zw), coverageTriangleArea(r0.xy, r0.zw, r1.xy, r1.zw));
}
