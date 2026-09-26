// unx-kernel: cs_6_6 main
// Fluid surface: one group per active block (indirect, argument 1): each cell writes its triangles at its block's
// first triangle + its prefix. Vertices sit on the cube edges where the density crosses 0.5 (linear), normals are the
// negated density gradient (central differences at the corners, blended along the edge). Vertex = (world xyz, 1),
// (normal xyz, 0); velocities (world m/s, 0) in their own buffer.
#include "FluidSurface.hlsli"

groupshared float g_density[FS_WINDOW * FS_WINDOW * FS_WINDOW];
groupshared float3 g_momentum[FS_WINDOW * FS_WINDOW * FS_WINDOW];
float fsAt(int3 n) { return g_density[(n.z * FS_WINDOW + n.y) * FS_WINDOW + n.x]; }
float3 fsGradient(int3 n)
{
    return float3(fsAt(n + int3(1, 0, 0)) - fsAt(n - int3(1, 0, 0)), fsAt(n + int3(0, 1, 0)) - fsAt(n - int3(0, 1, 0)), fsAt(n + int3(0, 0, 1)) - fsAt(n - int3(0, 0, 1)));
}
[numthreads(512, 1, 1)]
void main(uint t : SV_GroupThreadID, uint g : SV_GroupID)
{
    RWByteAddressBuffer scan = ResourceDescriptorHeap[P[4].y];
    RWByteAddressBuffer info = ResourceDescriptorHeap[P[5].x];
    RWByteAddressBuffer blockTris = ResourceDescriptorHeap[P[5].y];
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[5].z];
    RWByteAddressBuffer velocities = ResourceDescriptorHeap[P[6].z];
    ByteAddressBuffer cases = ResourceDescriptorHeap[P[5].w];
    int3 block = fsBlockCoord(scan.Load(4 * (FS_SLOTS(fsTableSize()) + g)));
    int3 origin = block * 8 - 1;
    for (uint k = t; k < FS_WINDOW * FS_WINDOW * FS_WINDOW; k += FS_BLOCK_NODES)
    {
        uint4 node = fsNode(origin + int3(k % FS_WINDOW, (k / FS_WINDOW) % FS_WINDOW, k / (FS_WINDOW * FS_WINDOW)));
        g_density[k] = node.x / FS_SCALE; g_momentum[k] = (float3)(int3)node.yzw / FS_MOMENTUM;
    }
    GroupMemoryBarrierWithGroupSync();
    uint cell = info.Load(4 * (g * FS_BLOCK_NODES + t)), mask = cell & 255u;
    uint count = cases.Load(4 * mask * FS_CASE_STRIDE);
    if (count == 0) return;
    uint first = blockTris.Load(4 * (fsMaxBlocks() + g)) + (cell >> 8);
    int3 c = int3(t % 8, (t / 8) % 8, t / 64);
    float h = fsH(); float3 world0 = fsOrigin() + (float3)(block * 8 + c) * h;
    for (uint k2 = 0; k2 < count; ++k2)
    {
        uint tri = first + k2; if (tri >= fsMaxTriangles()) break;
        [unroll] for (uint v = 0; v < 3; ++v)
        {
            uint2 e = kFsEdges[cases.Load(4 * (mask * FS_CASE_STRIDE + 1 + 3 * k2 + v))];
            int3 ca = c + 1 + fsCorner(e.x), cb = c + 1 + fsCorner(e.y);
            float da = fsAt(ca), db = fsAt(cb), s = saturate((0.5 - da) / (db - da));
            float3 p = lerp((float3)fsCorner(e.x), (float3)fsCorner(e.y), s);
            float3 grad = lerp(fsGradient(ca), fsGradient(cb), s);
            float len = length(grad);
            float3 n = len > 0 ? -grad / len : float3(0, 1, 0);
            uint at = (tri * 3 + v) * 32;
            vertices.Store4(at, asuint(float4(world0 + p * h, 1)));
            vertices.Store4(at + 16, asuint(float4(n, 0)));
            // Velocity: the density-weighted mean along the edge (the vertex density is 0.5 of the rest density).
            uint ia = (ca.z * FS_WINDOW + ca.y) * FS_WINDOW + ca.x, ib = (cb.z * FS_WINDOW + cb.y) * FS_WINDOW + cb.x;
            float mass = lerp(da, db, s); float3 velocity = mass > 0 ? lerp(g_momentum[ia], g_momentum[ib], s) / mass : float3(0, 0, 0);
            velocities.Store4((tri * 3 + v) * 16, asuint(float4(velocity, 0)));
        }
    }
}
