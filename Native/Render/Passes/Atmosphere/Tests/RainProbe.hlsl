// unx-kernel: cs_6_6 main
// Test kernel (RainShadowTests): rainExposure (WeatherField.hlsli) at query points.
// P[0] = { weather record SRV, queries SRV (StructuredBuffer<float4>), output UAV (RWStructuredBuffer<float>), count }
#include "Passes/Atmosphere/WeatherField.hlsli"

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= P[0].w) return;
    StructuredBuffer<float4> queries = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<float> output = ResourceDescriptorHeap[P[0].z];
    output[id] = rainExposure(P[0].x, queries[id].xyz);
}
