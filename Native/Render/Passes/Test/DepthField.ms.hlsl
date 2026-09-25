// unx-kernel: ms_6_6 main
// Render-graph test mesh shader: a field of slanted quads, one per 16 x 16 px tile (group = tile), each with its own
// reversed-Z depth plane, so a depth buffer holds varied, compressible content (aliasing tests).
// P[0].xy tiles in x and y
#include "Bindless.hlsli"

struct Vertex
{
    float4 position : SV_Position;
};

float hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x & 0xFFFFFF) / 16777216.0;
}

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void main(uint3 group : SV_GroupID, out vertices Vertex verts[4], out indices uint3 tris[2])
{
    SetMeshOutputCounts(4, 2);
    const float2 tiles = float2(P[0].xy);
    const float2 lo = group.xy / tiles * 2 - 1, hi = (group.xy + 1) / tiles * 2 - 1;
    const uint id = group.x + group.y * P[0].x;
    const float d0 = 0.1 + 0.8 * hash(id), dx = 0.05 * (hash(id * 3 + 1) - 0.5), dy = 0.05 * (hash(id * 7 + 2) - 0.5);
    verts[0].position = float4(lo.x, lo.y, d0, 1);
    verts[1].position = float4(hi.x, lo.y, d0 + dx, 1);
    verts[2].position = float4(lo.x, hi.y, d0 + dy, 1);
    verts[3].position = float4(hi.x, hi.y, d0 + dx + dy, 1);
    tris[0] = uint3(0, 1, 2);
    tris[1] = uint3(2, 1, 3);
}
