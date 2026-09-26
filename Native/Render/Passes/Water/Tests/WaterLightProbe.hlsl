// unx-kernel: cs_6_6 main
// Water stage 2 test probe: waterSunLight (WaterLight.hlsli) at each point, as band A shading calls it.
// P[0] points SRV (StructuredBuffer<float4>: xyz), output UAV (raw, 32 B per point: lightDir, valid; transmittance, 0),
// count, 0; P[1] depth SRV, normal SRV, medium SRV, constants SRV (UNX_NONE: no map); P[2].xyz the sun direction
#include "../WaterLight.hlsli"
#include "../WaterLinear.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint i = waterLinear(group, thread, 64);
    if (i >= P[0].z) return;
    StructuredBuffer<float4> points = ResourceDescriptorHeap[P[0].x];
    float3 lightDir, transmittance;
    const bool lit = waterSunLight(P[1].x, P[1].y, P[1].z, P[1].w, points[i].xyz, asfloat(P[2].xyz), 0, lightDir, transmittance);
    RWByteAddressBuffer output = ResourceDescriptorHeap[P[0].y];
    output.Store4(32 * i, uint4(asuint(lightDir), lit ? 1u : 0u));
    output.Store4(32 * i + 16, uint4(asuint(transmittance), 0u));
}
