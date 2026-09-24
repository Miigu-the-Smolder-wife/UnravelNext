// unx-kernel: ms_6_6 main
// unx-variants: GBUFFER=0,1
// S test stand-in for V's raster (V's cluster pipeline is built in parallel): draws the source triangles of the scene's
// instances, deformed with the shared deformVertex, 64 triangles per group (no vertex sharing, no culling).
// GBUFFER=0: the depth raster service contract (DepthRasterPixel, INTERFACES 5.3). GBUFFER=1: world normal for the
// test G-buffer pixel shader (TestGBuffer.hlsl).
// P[0].x chunks SRV (uint4: instance, first triangle, triangle count, 0), P[0].y views SRV (TestView), P[0].z view index
#include "Bindless.hlsli"
#include "Deformation.hlsli"

struct TestView
{
    row_major float4x4 viewProj;
    uint userData;
    uint3 pad;
};

#if GBUFFER
struct Vertex
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
};
#else
struct Vertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation uint userData : USERDATA;
    nointerpolation uint material : MATERIAL;
};
#endif

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint group : SV_GroupID, uint lane : SV_GroupThreadID, out vertices Vertex verts[192], out indices uint3 tris[64])
{
    StructuredBuffer<uint4> chunks = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<TestView> views = ResourceDescriptorHeap[P[0].y];
    const uint4 chunk = chunks[group];
    const TestView view = views[P[0].z];
    SetMeshOutputCounts(chunk.z * 3, chunk.z);
    if (lane >= chunk.z) return;
    const GpuInstance inst = loadInstance(chunk.x);
    const GpuMesh mesh = loadMesh(inst.mesh);
    const uint t = chunk.y + lane;
    const uint3 idx = loadTriangle(mesh, t);
    uint material = 0;
    [loop] for (uint s = 0; s < mesh.submeshCount; ++s)
    {
        const GpuSubmesh sub = loadSubmesh(mesh.submeshOffset + s);
        if (t * 3 >= sub.indexOffset && t * 3 < sub.indexOffset + sub.indexCount) material = instanceMaterial(inst, sub, s);
    }
    [unroll] for (uint j = 0; j < 3; ++j)
    {
        const DeformedVertex d = deformVertex(inst, mesh, idx[j]);
        Vertex v;
        v.position = mul(view.viewProj, float4(d.world, 1));
#if GBUFFER
        v.normal = d.normal;
#else
        v.uv = loadVertex(mesh, idx[j]).uv;
        v.userData = view.userData;
        v.material = material;
#endif
        verts[lane * 3 + j] = v;
    }
    tris[lane] = uint3(lane * 3, lane * 3 + 1, lane * 3 + 2);
}
