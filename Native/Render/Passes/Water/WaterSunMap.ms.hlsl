// unx-kernel: ms_6_6 main
// Water stage 2 sun-space map (WaterLight.hlsli): one of W's triangle streams, orthographic along the sun, into the
// map's depth (1 = nearest the sun; the depth test keeps the greatest: the surface the sunlight meets first) with its
// interpolated normal and the stream's medium (WaterSunMap.ps). One group per 32 triangles of the stream's capacity; the
// live count is the draw arguments' vertex count / 3 (written on the GPU), groups past it emit nothing. Both faces.
//   P[0].x vertices SRV (raw, 32 B per vertex: (xyz, 1), (normal, 0)), P[0].y draw arguments SRV (raw), P[0].z capacity
//   (triangles), P[0].w constants SRV (waterSunConstants); P[1] the medium: 1 m transmittance RGB (float), IOR (float)
#include "Bindless.hlsli"

struct VertexOut
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
};

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[96], out indices uint3 tris[32])
{
    ByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    const uint live = min(args.Load(0) / 3, P[0].z);
    const uint first = (group.x + group.y * 65535) * 32;
    const uint count = first < live ? min(32u, live - first) : 0u;  // uniform over the group
    SetMeshOutputCounts(3 * count, count);
    if (lane >= count) return;
    const uint t = first + lane;
    ByteAddressBuffer c = ResourceDescriptorHeap[P[0].w];
    const float4 r = asfloat(c.Load4(16)), u = asfloat(c.Load4(32)), s = asfloat(c.Load4(48)), k = asfloat(c.Load4(64));
    ByteAddressBuffer vertexData = ResourceDescriptorHeap[P[0].x];
    [unroll] for (uint j = 0; j < 3; ++j)
    {
        const uint at = 32 * (3 * t + j);
        const float3 p = asfloat(vertexData.Load3(at));
        VertexOut o;
        o.position = float4(((dot(p, r.xyz) - r.w) * k.x) * 2 - 1, ((dot(p, u.xyz) - u.w) * k.y) * 2 - 1, saturate((dot(p, s.xyz) - s.w) / k.z), 1);
        o.normal = asfloat(vertexData.Load3(at + 16));
        verts[3 * lane + j] = o;
    }
    tris[lane] = uint3(3 * lane, 3 * lane + 1, 3 * lane + 2);
}
