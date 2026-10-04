// unx-kernel: ms_6_6 main
// unx-variants: GBUFFER=0,1
// S test stand-in for V's raster (V's cluster pipeline is built in parallel): draws the source triangles of the scene's
// instances, deformed with the shared deformVertex, 64 triangles per group (no vertex sharing, no culling).
// GBUFFER=0: the depth raster service contract (DepthRasterPixel, INTERFACES 5.3). GBUFFER=1: world normal for the
// test G-buffer pixel shader (TestGBuffer.hlsl).
// P[0].x chunks SRV (uint4: instance, first triangle, triangle count, 0), P[0].y views SRV (TestView), P[0].z view index,
// P[0].w chunk count (groups are dispatched as 65535 x N)
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
    nointerpolation uint instance : INSTANCE;
};
#endif

[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void main(uint3 groupId : SV_GroupID, uint lane : SV_GroupThreadID, out vertices Vertex verts[192], out indices uint3 tris[64])
{
    StructuredBuffer<uint4> chunks = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<TestView> views = ResourceDescriptorHeap[P[0].y];
    const uint group = groupId.y * 65535 + groupId.x;
    const uint4 chunk = group < P[0].w ? chunks[group] : uint4(0, 0, 0, 0);
    const TestView view = views[P[0].z];
    const GpuInstance inst = loadInstance(chunk.x);
    const bool inSet = view.pad.x == 0 || (view.pad.x == 2) == instanceShadowMovable(inst, chunk.x, view.pad.y);
    const uint triangles = inSet && (inst.flags & INSTANCE_HIDDEN) == 0 ? chunk.z : 0;
    SetMeshOutputCounts(triangles * 3, triangles);
    if (lane >= triangles) return;
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
        v.instance = chunk.x;
#endif
        verts[lane * 3 + j] = v;
    }
    tris[lane] = uint3(lane * 3, lane * 3 + 1, lane * 3 + 2);
}
