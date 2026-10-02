// unx-kernel: ms_6_6 main
// r.card.capture (CardCapture.hlsli): one group per 32 source triangles of a submesh. A triangle of a one-sided material
// whose front does not face the card's side is culled (the capture sees the mesh as a camera on that side would).
#include "Passes/SurfaceCache/CardCapture.hlsli"

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices CardVertex verts[96], out primitives CardPrimitive prims[32],
          out indices uint3 tris[32])
{
    const uint triangles = P[0].z;
    const uint first = (group.x + group.y * 65535u) * 32u;
    const uint count = first < triangles ? min(32u, triangles - first) : 0u;  // uniform over the group
    SetMeshOutputCounts(3 * count, count);
    if (lane >= count) return;
    const GpuInstance inst = loadInstance(P[0].x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    StructuredBuffer<uint> indices = ResourceDescriptorHeap[g_indices];
    const uint base = mesh.indexOffset + P[0].y + 3u * (first + lane);
    float3 ax, ay, az;
    ccAxes(P[2].w & 7u, ax, ay, az);
    const float handedness = dot(cross(ax, ay), az);  // +1 / -1: the card basis against the mesh's
    const float3 origin = asfloat(P[1].xyz), extent = max(asfloat(P[2].xyz), 1e-6);
    const float scale = asfloat(P[1].w);
    const float4 rect = asfloat(P[3]);
    float3 scaled[3];
    [unroll] for (uint j = 0; j < 3; ++j)
    {
        const VertexData v = loadVertex(mesh, indices[base + j]);
        scaled[j] = v.position * scale;
        const float3 d = scaled[j] - origin;
        const float3 c = float3(dot(d, ax), dot(d, ay), dot(d, az));
        const float2 uv = c.xy / extent.xy * 0.5 + 0.5;
        const float2 page = (uv - rect.xy) / max(rect.zw - rect.xy, 1e-8);
        CardVertex o;
        o.position = float4(page.x * 2 - 1, 1 - page.y * 2, 0.5 - c.z / extent.z * 0.5, 1);
        o.normal = float3(dot(v.normal, ax), dot(v.normal, ay), dot(v.normal, az));
        o.tangent = float4(dot(v.tangent, ax), dot(v.tangent, ay), dot(v.tangent, az), v.tangentSign * handedness);
        o.uv = v.uv;
        o.scaled = scaled[j];
        verts[3 * lane + j] = o;
    }
    tris[lane] = uint3(3 * lane, 3 * lane + 1, 3 * lane + 2);
    const bool twoSided = ((P[2].w >> 8) & 1u) != 0;
    const float3 normal = cross(scaled[1] - scaled[0], scaled[2] - scaled[0]);  // counter-clockwise front (mesh space)
    prims[lane].cull = !twoSided && !(dot(normal, az) > 0);
}
