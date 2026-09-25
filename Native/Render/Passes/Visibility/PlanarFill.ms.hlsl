// unx-kernel: ms_6_6 main
// Planar reflection view depth fill (ViewDesc::planarMask): one triangle over the whole view at the near plane (clip
// z = w: device depth 1, the nearest reversed-Z value). PlanarFill.ps keeps the pixels that are not mirror pixels, so
// their depth is the nearest and the vis buffer raster (GREATER_EQUAL) rejects every fragment there by the early depth
// test; they stay VIS_NONE.
struct VertexOut
{
    float4 position : SV_Position;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(out vertices VertexOut verts[3], out indices uint3 tris[1])
{
    SetMeshOutputCounts(3, 1);
    verts[0].position = float4(-1, -1, 1, 1);
    verts[1].position = float4(-1, 3, 1, 1);
    verts[2].position = float4(3, -1, 1, 1);
    tris[0] = uint3(0, 1, 2);
}
