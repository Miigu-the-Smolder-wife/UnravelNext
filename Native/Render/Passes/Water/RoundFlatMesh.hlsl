// unx-kernel: cs_6_6 main
// Exact zero-state surface with the original 512-segment outer boundary.
// Internal coplanar rings contribute no shape and need no triangles.
// Same constants as RoundMesh; three vertices per boundary segment.
#include "Bindless.hlsli"
[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= 512u * 3u) return;
    const uint corner = id % 3, segment = id / 3;
    const float3 ax = float3(asfloat(P[3].x), 0, asfloat(P[3].y));
    const float3 az = float3(asfloat(P[3].z), 0, asfloat(P[3].w));
    const float3 origin = float3(asfloat(P[2].x), asfloat(P[1].z), asfloat(P[2].y));
    float3 position = origin;
    if (corner != 0)
    {
        const float theta = float(segment + (corner == 2 ? 1u : 0u)) * (2 * 3.14159265358979323846 / 512.0);
        position += (ax * cos(theta) + az * sin(theta)) * asfloat(P[2].z);
    }
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer velocities = ResourceDescriptorHeap[P[0].w];
    vertices.Store4(32 * id, asuint(float4(position, 1)));
    vertices.Store4(32 * id + 16, asuint(float4(0, 1, 0, 0)));
    velocities.Store4(16 * id, uint4(0, 0, 0, 0));
    if (id == 0)
    {
        RWByteAddressBuffer draw = ResourceDescriptorHeap[P[1].x];
        draw.Store4(0, uint4(512 * 3, 1, 0, 0));
    }
}
