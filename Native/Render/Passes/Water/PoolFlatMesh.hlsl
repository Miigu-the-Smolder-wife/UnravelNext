// unx-kernel: cs_6_6 main
// The exact zero-mode rectangular surface: four vertices and two triangles.
// PoolMesh constants, with P[4].y = index UAV. No approximation threshold is
// used: the host selects this only before any source or explicit state exists.
#include "Bindless.hlsli"
[numthreads(4, 1, 1)]
void main(uint id : SV_GroupThreadID)
{
    const uint2 at = uint2(id & 1u, id >> 1) * 256u;
    const float3 ax = float3(asfloat(P[3].x), 0, asfloat(P[3].y));
    const float3 az = float3(asfloat(P[3].z), 0, asfloat(P[3].w));
    const float3 origin = float3(asfloat(P[2].x), asfloat(P[1].z), asfloat(P[2].z));
    const float3 position = origin + ax * (float(at.x) * asfloat(P[2].w)) + az * (float(at.y) * asfloat(P[4].x));
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer velocities = ResourceDescriptorHeap[P[0].w];
    vertices.Store4(32 * id, asuint(float4(position, 1)));
    vertices.Store4(32 * id + 16, asuint(float4(0, 1, 0, 0)));
    velocities.Store4(16 * id, uint4(0, 0, 0, 0));
    if (id == 0)
    {
        RWByteAddressBuffer draw = ResourceDescriptorHeap[P[1].x];
        RWByteAddressBuffer indices = ResourceDescriptorHeap[P[4].y];
        draw.Store4(0, uint4(6, 1, 0, 0));
        indices.Store3(0, uint3(0, 2, 1));
        indices.Store3(12, uint3(1, 2, 3));
    }
}
