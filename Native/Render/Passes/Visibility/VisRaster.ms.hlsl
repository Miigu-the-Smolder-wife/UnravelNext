// unx-kernel: ms_6_6 main
// unx-variants: ALPHA=0,1
// Band A visibility buffer: one mesh-shader group per visible cluster of a draw list (VisibilityCommon.hlsli lists).
// Vertices go through deformVertex (skin, wind: the same geometry as shadows and rays), primitives carry the vis id
// (packVisId). A mixed sheet cluster (LIST_ENTRY_MIXED: also in the coverage list) keeps only its band A triangles here
// (sheetTriangleBandB, the same test as the coverage raster's). The clip plane of planar-reflection views goes to SV_ClipDistance0. ALPHA=1 (alpha-tested lists) also
// passes the uv and the triangle's material to the pixel kernel's alpha test.
//   P[0] visible SRV (uint2), lists SRV (raw), state SRV (raw), list
//   P[1] phase (1: entries of phase 1; 2: entries appended in phase 2), list capacity, views SRV, unused
#include "Passes/Visibility/VisibilityCommon.hlsli"
#include "VisBuffer.hlsli"

struct VertexOut
{
    float4 position : SV_Position;
    float clip : SV_ClipDistance0;
#if ALPHA
    float2 uv : TEXCOORD0;
#endif
};

struct PrimitiveOut
{
    uint visId : VISID;
    bool cull : SV_CullPrimitive;
#if ALPHA
    uint material : MATERIAL;
#endif
};

groupshared float3 gs_world[128];

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[128], out primitives PrimitiveOut prims[128],
          out indices uint3 tris[128])
{
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer lists = ResourceDescriptorHeap[P[0].y];
    const uint list = P[0].w, capacity = P[1].y;
    const uint count = min(state.Load(4 * (VS_LIST_COUNT + list)), capacity);
    const uint base = P[1].x == 1 ? 0 : state.Load(4 * (VS_LIST_PHASE1 + list));
    const uint index = base + group.x + group.y * 65535;
    const bool valid = index < count;  // uniform over the group
    const uint listEntry = valid ? lists.Load(4 * (list * capacity + index)) : 0;
    const uint visibleIndex = listEntry & ~LIST_ENTRY_MIXED;
    const bool mixed = (listEntry & LIST_ENTRY_MIXED) != 0;  // uniform over the group
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[P[0].x];
    const uint2 entry = valid ? visible[visibleIndex] : uint2(0, 0);
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[1].z];
    const CullView v = views[entry.y >> 24];
    const GpuInstance inst = loadInstance(entry.x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const GpuCluster cl = loadCluster(entry.y & 0xFFFFFFu);
    const uint vertexCount = valid ? clusterVertexCount(cl) : 0, triangleCount = valid ? clusterTriangleCount(cl) : 0;
    SetMeshOutputCounts(vertexCount, triangleCount);
    StructuredBuffer<uint> clusterVertices = ResourceDescriptorHeap[g_clusterVertexIndices];
    const bool clip = any(v.clipPlane != 0);
#if ALPHA
    const uint material = clusterMaterial(inst, cl);
#endif
    for (uint i = lane; i < vertexCount; i += 64)
    {
        const uint meshVertex = clusterVertices[cl.vertexOffset + i];
        const DeformedVertex d = deformVertex(inst, mesh, meshVertex);
        verts[i].position = viewModelClip(inst, mul(v.viewProj, float4(d.world, 1)));  // A12: 1 outside the main view
        verts[i].clip = clip ? dot(v.clipPlane.xyz, d.world) + v.clipPlane.w : 1.0;
        gs_world[i] = d.world;
#if ALPHA
        verts[i].uv = loadVertex(mesh, meshVertex).uv;
#endif
    }
    GroupMemoryBarrierWithGroupSync();
    StructuredBuffer<uint> clusterTriangles = ResourceDescriptorHeap[g_clusterTriangles];
    for (uint t = lane; t < triangleCount; t += 64)
    {
        const uint packed = clusterTriangles[cl.triangleOffset + t];
        const uint3 tri = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
        tris[t] = tri;
        prims[t].visId = packVisId(visibleIndex, t);
        prims[t].cull = (mixed && sheetTriangleBandB(v, gs_world[tri.x], gs_world[tri.y], gs_world[tri.z])) || patchDropsTriangle(inst, mesh, cl, tri);
#if ALPHA
        prims[t].material = material;
#endif
    }
}
