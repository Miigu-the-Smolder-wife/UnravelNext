// unx-kernel: ms_6_6 main
// Depth raster service (FrameServices::rasterizeDepth, INTERFACES 5.3): one mesh-shader group per visible cluster of a
// draw list, any number of views (the visible entry carries the view). Outputs match struct DepthRasterPixel
// (DepthRaster.hlsli) for the requester's pixel kernel: position, uv (alpha test), userData, material, instance.
// Views with different viewports select theirs with SV_ViewportArrayIndex (at most 16 per request).
//   P[0] visible SRV (uint2), lists SRV (raw), state SRV (raw), list
//   P[1] phase (always 1: the service culls in one phase), list capacity, views SRV, viewport per view (0 = one viewport)
#include "Passes/Visibility/VisibilityCommon.hlsli"

struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct PrimitiveOut
{
    uint userData : USERDATA;
    uint material : MATERIAL;
    uint instance : INSTANCE;
    uint viewport : SV_ViewportArrayIndex;
};

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[128], out primitives PrimitiveOut prims[128],
          out indices uint3 tris[128])
{
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].y];
    const uint list = P[0].w, capacity = P[1].y;
    const uint count = min(state.Load(4 * (VS_LIST_COUNT + list)), capacity);
    const uint index = group.x + group.y * 65535;
    const bool valid = index < count;  // uniform over the group
    const uint visibleIndex = valid ? lists.Load(4 * (list * capacity + index)) : 0;
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[P[0].x];
    const uint2 entry = valid ? visible[visibleIndex] : uint2(0, 0);
    const uint view = entry.y >> 24;
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[1].z];
    const CullView v = views[view];
    const GpuInstance inst = loadInstance(entry.x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const GpuCluster cl = loadCluster(entry.y & 0xFFFFFFu);
    const uint material = clusterMaterial(inst, cl);
    const uint vertexCount = valid ? clusterVertexCount(cl) : 0, triangleCount = valid ? clusterTriangleCount(cl) : 0;
    SetMeshOutputCounts(vertexCount, triangleCount);
    StructuredBuffer<uint> clusterVertices = ResourceDescriptorHeap[g_clusterVertexIndices];
    for (uint i = lane; i < vertexCount; i += 64)
    {
        const uint meshVertex = clusterVertices[cl.vertexOffset + i];
        const DeformedVertex d = deformVertex(inst, mesh, meshVertex);
        verts[i].position = mul(v.viewProj, float4(d.world, 1));
        verts[i].uv = loadVertex(mesh, meshVertex).uv;
    }
    StructuredBuffer<uint> clusterTriangles = ResourceDescriptorHeap[g_clusterTriangles];
    for (uint t = lane; t < triangleCount; t += 64)
    {
        const uint packed = clusterTriangles[cl.triangleOffset + t];
        tris[t] = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
        prims[t].userData = v.userData;
        prims[t].material = material;
        prims[t].instance = entry.x;
        prims[t].viewport = P[1].w != 0 ? view : 0;
    }
}
