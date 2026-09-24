// unx-kernel: ms_6_6 main
// Render-graph test mesh shader: one ~4 px triangle at a seed-dependent position (reversed-Z depth 0.5).
// P[0].x seed, P[0].y viewport height in pixels
#include "Bindless.hlsli"

struct Vertex
{
    float4 position : SV_Position;
};

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(out vertices Vertex verts[3], out indices uint3 tris[1])
{
    SetMeshOutputCounts(3, 1);
    const uint seed = P[0].x;
    const float2 o = float2((seed % 97) / 97.0 * 1.8 - 0.9, ((seed / 97) % 89) / 89.0 * 1.8 - 0.9);
    const float s = 8.0 / max(P[0].y, 1u);
    verts[0].position = float4(o, 0.5, 1);
    verts[1].position = float4(o + float2(s, 0), 0.5, 1);
    verts[2].position = float4(o + float2(0, s), 0.5, 1);
    tris[0] = uint3(0, 1, 2);
}
