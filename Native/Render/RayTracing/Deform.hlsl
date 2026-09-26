// unx-kernel: cs_6_6 main
// Deformed BLAS vertices (ARCHITECTURE 2.8, 2.12): world-space positions and normals of skinned proxies, original meshes
// of the reflection exact set and wind-deformed exact-set foliage, through the same deformVertex() the raster and
// shadow pages use (INTERFACES 6.4). One thread per vertex; a group table maps groups to (job, first vertex).
//
// P[0] = { jobs SRV (DeformJob), groups SRV (uint2 job, first vertex), deformed pool UAV (RtDeformedVertex), group count }
// P[1] = { vertex map SRV (uint, compact -> mesh vertex), dispatch width in groups (2D dispatch past 65535 groups) }
#include "Deformation.hlsli"
#include "RayTracing/RayScene.hlsli"

struct DeformJob
{
    uint sceneInstance;
    uint deformedBase;  // first vertex of this job in the deformed pool
    uint vertexMap;     // first entry in the vertex map (compact -> mesh vertex), UNX_NONE = identity
    uint vertexCount;
};

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint groupIndex = group.y * P[1].y + group.x;
    if (groupIndex >= P[0].w) return;
    StructuredBuffer<DeformJob> jobs = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<uint2> groups = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<RtDeformedVertex> pool = ResourceDescriptorHeap[P[0].z];
    const uint2 g = groups[groupIndex];
    const DeformJob job = jobs[g.x];
    const uint local = g.y + lane;
    if (local >= job.vertexCount) return;
    uint meshVertex = local;
    if (job.vertexMap != UNX_NONE)
    {
        StructuredBuffer<uint> map = ResourceDescriptorHeap[P[1].x];
        meshVertex = map[job.vertexMap + local];
    }
    const GpuInstance inst = loadInstance(job.sceneInstance);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const DeformedVertex d = deformVertex(inst, mesh, meshVertex);
    RtDeformedVertex o;
    o.position = d.world;
    o.normalOct = octEncode(d.normal);
    const float3 m = d.world - d.prevWorld;
    o.motion = uint2(f32tof16(m.x) | (f32tof16(m.y) << 16), f32tof16(m.z));
    pool[job.deformedBase + local] = o;
}
