// unx-kernel: cs_6_6 main
// Hair density volume test probe (HairTests.cpp): one thread per probe evaluates HairDensity.hlsli's readers.
// Probe k (float4 x 3): (p relative to the volume's origin camera, reach in metres), (d unit, u), (body, 0, 0, 0).
// Result (float4 x 2): (hairFibreCount, hairFibreCountWithin(reach), hairTransmittance(reach), hairFirstFibre(reach, u)),
// (the probability that the ray meets a fibre, 0, 0, 0).
// P[0] = { probes SRV, results UAV, density parameters (raw), probes }, P[1] = { march steps, 0, 0, 0 }.
#include "Bindless.hlsli"
#include "Passes/Hair/HairDensity.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint k = id.x;
    if (k >= P[0].w) return;
    StructuredBuffer<float4> probes = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float4> results = ResourceDescriptorHeap[P[0].y];
    const float4 c0 = probes[3 * k], c1 = probes[3 * k + 1], c2 = probes[3 * k + 2];
    const uint body = (uint)c2.x;
    float met;
    const float first = hairFirstFibre(P[0].z, body, c0.xyz, c1.xyz, c0.w, c1.w, met);
    results[2 * k] = float4(hairFibreCount(P[0].z, body, c0.xyz, c1.xyz, P[1].x), hairFibreCountWithin(P[0].z, body, c0.xyz, c1.xyz, c0.w, P[1].x),
                            hairTransmittance(P[0].z, c0.xyz, c1.xyz, c0.w, P[1].x), first);
    results[2 * k + 1] = float4(met, 0, 0, 0);
}
