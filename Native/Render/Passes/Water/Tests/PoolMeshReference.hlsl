// unx-kernel: cs_6_6 main
// Closed basins: the surface's triangle stream (FrameResources.h TriangleStream, layer 1) from the evolved field: one
// thread per vertex; cell (i, j) of the 256 x 256 cells has two triangles, (i, j) (i, j+1) (i+1, j) and
// (i+1, j) (i, j+1) (i+1, j+1): counter-clockwise seen from above. Vertex (32 B): world position origin + i hx ax +
// j hz az + eta up, and the exact spectral normal normalize(up - eta_x ax - eta_z az) (ax, az: the basin's horizontal
// axes). Velocity (float4, world m/s): (eta - eta_previous) / dt vertically, so position - velocity dt is exactly the
// previous frame's surface (0 when there is none). Thread 0 writes the draw arguments (6 x 256^2 vertices).
// Root constants:
//   P[0] field SRV (texture 257 x 257: eta, eta_x, eta_z, phi), previous-eta SRV (raw float), vertices UAV, velocities UAV
//   P[1] draw arguments UAV, 1 / dt (0: no motion), still level (world y, m), 0
//   P[2] world position of sample (0, 0) (x, y, z; y unused), hx (m)
//   P[3] ax (x, z), az (x, z)
//   P[4] hz (m)
#include "Bindless.hlsli"

#define POOL_CELLS 256u
#define POOL_Q 257u

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id == 0)
    {
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].x];
        args.Store4(0, uint4(6u * POOL_CELLS * POOL_CELLS, 1, 0, 0));
    }
    if (id >= 6u * POOL_CELLS * POOL_CELLS) return;
    Texture2D<float4> field = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer previous = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer velocities = ResourceDescriptorHeap[P[0].w];
    const float invDt = asfloat(P[1].y), level = asfloat(P[1].z);
    const float3 origin = float3(asfloat(P[2].x), level, asfloat(P[2].z));
    const float hx = asfloat(P[2].w), hz = asfloat(P[4].x);
    const float3 ax = float3(asfloat(P[3].x), 0, asfloat(P[3].y)), az = float3(asfloat(P[3].z), 0, asfloat(P[3].w));
    const uint cell = id / 6u, corner = id % 6u;  // one thread per vertex: consecutive threads write consecutive vertices
    const uint2 offset[6] = { uint2(0, 0), uint2(0, 1), uint2(1, 0), uint2(1, 0), uint2(0, 1), uint2(1, 1) };
    const uint2 at = uint2(cell % POOL_CELLS, cell / POOL_CELLS) + offset[corner];
    const float4 f = field.Load(int3(at, 0));
    const float3 position = origin + ax * (float(at.x) * hx) + az * (float(at.y) * hz) + float3(0, f.x, 0);
    const float3 normal = normalize(float3(0, 1, 0) - ax * f.y - az * f.z);
    const float before = asfloat(previous.Load(4 * (at.y * POOL_Q + at.x)));
    vertices.Store4(32 * id, asuint(float4(position, 1)));
    vertices.Store4(32 * id + 16, asuint(float4(normal, 0)));
    velocities.Store4(16 * id, asuint(float4(0, (f.x - before) * invDt, 0, 0)));
}
