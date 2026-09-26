// unx-kernel: cs_6_6 main
// Test kernel (WindCacheTests): windSample and windExact (WindCache.hlsli) at query points.
// P[0] = { wind header SRV, queries SRV (StructuredBuffer<float4>), output UAV (2 float4 per query), count }
#include "Passes/Atmosphere/WindCache.hlsli"

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= P[0].w) return;
    StructuredBuffer<float4> queries = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[0].z];
    const float3 x = queries[id].xyz;
    output[2 * id] = float4(windSample(P[0].x, x), 0);
    output[2 * id + 1] = float4(windExact(P[0].x, x), 0);
}
