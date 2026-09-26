// unx-kernel: cs_6_6 main
// Volume test probe: the tent-mass line integral and the haze gradient (VolumeCommon.hlsli) for a list of queries.
// P[0].x queries (StructuredBuffer<float4>, 3 per query: (d.xyz, a), (c.xyz, b), (r, haze b, haze radius, 0), then
// (amplitude, width fraction, profile, 0) as a 4th), P[0].y results (RWStructuredBuffer<float2>: line, gradient), P[0].z count.
#include "Passes/Volume/VolumeCommon.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].z) return;
    StructuredBuffer<float4> q = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float2> results = ResourceDescriptorHeap[P[0].y];
    const float4 q0 = q[4 * id.x], q1 = q[4 * id.x + 1], q2 = q[4 * id.x + 2], q3 = q[4 * id.x + 3];
    results[id.x] = float2(volumeTentLine(float3(0, 0, 0), q0.xyz, q0.w, q1.w, q1.xyz, q2.x), volumeHazeGradient(q2.y, q2.z, q3.xyz));
}
