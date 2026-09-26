// unx-kernel: ms_6_6 main
// Water layer (INTERFACES v1.61; A's decision on W's request 20260926_W_ocean_patch_stream): one mesh-shader group per 32
// triangles of a water stream's capacity (FrameResources::triangleStreams with layer 1; live count = draw arguments'
// vertex count / 3), drawn with one sample per pixel into the water layer over a copy of band A's depth (depth test and
// write, both faces): FrameResources::waterVis (the triangle's vis id, COV_STREAM_ID | slot | triangle) and waterDepth
// (linear view depth). Band A (what lies under the water) is left as it is.
//   P[0].x vertices SRV (raw, 32 B per vertex), P[0].y draw arguments SRV (raw), P[0].z capacity, P[0].w stream slot;
//   P[1].x views SRV (the main view is element 0).
#include "Passes/Visibility/CoverageLayer.hlsli"

struct VertexOut
{
    float4 position : SV_Position;
};

struct PrimitiveOut
{
    uint visId : VISID;
};

[outputtopology("triangle")]
[numthreads(32, 1, 1)]
void main(uint lane : SV_GroupThreadID, uint3 group : SV_GroupID, out vertices VertexOut verts[96], out primitives PrimitiveOut prims[32],
          out indices uint3 tris[32])
{
    ByteAddressBuffer args = ResourceDescriptorHeap[P[0].y];
    const uint live = min(args.Load(0) / 3, P[0].z);
    const uint first = (group.x + group.y * 65535) * 32;
    const uint count = first < live ? min(32u, live - first) : 0u;  // uniform over the group
    SetMeshOutputCounts(3 * count, count);
    if (lane >= count) return;
    const uint t = first + lane;
    StructuredBuffer<CullView> views = ResourceDescriptorHeap[P[1].x];
    const CullView v = views[0];
    ByteAddressBuffer vertexData = ResourceDescriptorHeap[P[0].x];
    [unroll] for (uint k = 0; k < 3; ++k)
        verts[3 * lane + k].position = mul(v.viewProj, float4(asfloat(vertexData.Load3(32 * (3 * t + k))), 1));
    tris[lane] = uint3(3 * lane, 3 * lane + 1, 3 * lane + 2);
    prims[lane].visId = COV_STREAM_ID | (P[0].w << 24) | t;
}
