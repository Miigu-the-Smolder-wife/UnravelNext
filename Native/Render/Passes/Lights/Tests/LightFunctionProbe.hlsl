// unx-kernel: cs_6_6 main
// Light function test probe (LightFunctionTests.cpp): lightFunction for queries 0 .. P[0].z - 1.
// P[0] = { queries SRV (3 float4 each: dir, time; forward, footprint; right, light index as uint), results UAV (float4),
// count, table SRV (LIGHT_FUNCTION_NONE = none) }.
#include "Bindless.hlsli"
#include "Passes/Lights/LightFunction.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].z) return;
    StructuredBuffer<float4> queries = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float4> results = ResourceDescriptorHeap[P[0].y];
    const float4 a = queries[3 * id.x], b = queries[3 * id.x + 1], c = queries[3 * id.x + 2];
    results[id.x] = float4(lightFunction(P[0].w, asuint(c.w), b.xyz, c.xyz, a.xyz, b.w, a.w), 0);
}
