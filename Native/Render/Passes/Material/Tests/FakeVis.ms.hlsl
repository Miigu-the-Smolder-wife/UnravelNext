// unx-kernel: ms_6_6 main
// M tests: stand-in band-A raster (FakeVis.hlsli). One group per visible cluster (<= 64 triangles, <= 192 vertices).
// P[0] = { visibleClusters SRV, visible cluster count, texture table SRV, 0 }; frame constants b1 = the view.
#include "Passes/Material/Tests/FakeVis.hlsli"

groupshared float3 gs_world[192];

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint tid : SV_GroupThreadID, uint3 gid : SV_GroupID, out vertices FakeVisVertex verts[192], out indices uint3 tris[64],
          out primitives FakeVisPrimitiveOut prims[64])
{
    const uint vcIndex = gid.x + gid.y * 65535;
    const bool live = vcIndex < P[0].y;
    GpuVisibleCluster vc = (GpuVisibleCluster)0;
    GpuCluster c = (GpuCluster)0;
    uint vcount = 0, tcount = 0;
    if (live)
    {
        StructuredBuffer<GpuVisibleCluster> list = ResourceDescriptorHeap[P[0].x];
        vc = list[vcIndex];
        c = loadCluster(vc.cluster);
        vcount = clusterVertexCount(c);
        tcount = clusterTriangleCount(c);
    }
    SetMeshOutputCounts(vcount, tcount);
    const GpuInstance inst = loadInstance(vc.instance);
    const GpuMesh mesh = loadMesh(inst.mesh);
    StructuredBuffer<uint> vidx = ResourceDescriptorHeap[g_clusterVertexIndices];
    for (uint v = tid; v < vcount; v += 64)
    {
        const uint mv = vidx[c.vertexOffset + v];
        const DeformedVertex d = deformVertex(inst, mesh, mv);
        verts[v].position = mul(g_viewProj, float4(d.world, 1));
        verts[v].uv = loadVertex(mesh, mv).uv;
        gs_world[v] = d.world;
    }
    GroupMemoryBarrierWithGroupSync();
    if (tid < tcount)
    {
        StructuredBuffer<uint> ctris = ResourceDescriptorHeap[g_clusterTriangles];
        const uint packed = ctris[c.triangleOffset + tid];
        const uint3 t = uint3(packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu);
        tris[tid] = t;
        const float3 w0 = gs_world[t.x], w1 = gs_world[t.y], w2 = gs_world[t.z];
        const bool front = dot(cross(w1 - w0, w2 - w0), g_cameraPosition - w0) > 0;
        const uint material = clusterMaterial(inst, c);
        const GpuMaterial m = loadMaterial(material);
        prims[tid].visId = packVisId(vcIndex, tid);
        prims[tid].material = material;
        prims[tid].cull = !front && (m.classFlags & MATERIAL_TWO_SIDED) == 0;
    }
}
