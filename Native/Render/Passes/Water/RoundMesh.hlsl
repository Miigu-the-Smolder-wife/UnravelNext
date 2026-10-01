// unx-kernel: cs_6_6 main
// Round basins: the surface's triangle stream (layer 1) from the field: the centre fan (512 triangles: centre, ring 0 at
// i, ring 0 at i + 1) then the ring quads (j, j + 1), two triangles each, counter-clockwise seen from above. Vertex
// (32 B): world position centre + e_r r + up eta, normal normalize(up - e_r eta_r - e_theta eta_theta / r) (the exact
// spectral slopes; the centre takes up: its slope is the order-1 modes', left out). Velocity (float4, world m/s):
// (eta - eta_previous) / dt vertically. Thread 0 writes the draw arguments.
// Root constants:
//   P[0] field SRV (texture 512 x 128), previous-eta SRV (raw: samples, then the centre), vertices UAV, velocities UAV
//   P[1] draw arguments UAV, 1 / dt (0: no motion), still level (world y, m), centre SRV (raw float4: eta, phi, previous)
//   P[2] world x, z of the basin's centre, R (m), 0
//   P[3] ax (x, z), az (x, z) (the basin's horizontal axes: local x and z in world)
#include "Bindless.hlsli"

#define ROUND_THETA 512u
#define ROUND_RINGS 128u
#define ROUND_PI 3.14159265358979

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    const uint triangles = ROUND_THETA * (1u + 2u * (ROUND_RINGS - 1u));
    if (id == 0)
    {
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].x];
        args.Store4(0, uint4(3u * triangles, 1, 0, 0));
    }
    if (id >= 3u * triangles) return;
    Texture2D<float4> field = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer previous = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer centre = ResourceDescriptorHeap[P[1].w];
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer velocities = ResourceDescriptorHeap[P[0].w];
    const float invDt = asfloat(P[1].y), level = asfloat(P[1].z), R = asfloat(P[2].z);
    const float3 origin = float3(asfloat(P[2].x), level, asfloat(P[2].y));
    const float3 ax = float3(asfloat(P[3].x), 0, asfloat(P[3].y)), az = float3(asfloat(P[3].z), 0, asfloat(P[3].w));
    const uint tri = id / 3u, corner = id % 3u;
    // the vertex (i, j) of this corner: j = -1 the centre
    int i = 0, j = 0;
    if (tri < ROUND_THETA)
    {
        // fan: centre, ring 0 at i, ring 0 at i + 1 (counter-clockwise from above: theta increases from ax towards az)
        i = int(tri) + (corner == 2 ? 1 : 0);
        j = corner == 0 ? -1 : 0;
    }
    else
    {
        const uint q = tri - ROUND_THETA, ring = q / (2u * ROUND_THETA), rem = q % (2u * ROUND_THETA), cell = rem / 2u, half = rem % 2u;
        // quad (cell, ring): (i, j) (i + 1, j) (i, j + 1) and (i + 1, j) (i + 1, j + 1) (i, j + 1)
        const int2 c0[3] = { int2(0, 0), int2(1, 0), int2(0, 1) }, c1[3] = { int2(1, 0), int2(1, 1), int2(0, 1) };
        const int2 c = half == 0 ? c0[corner] : c1[corner];
        i = int(cell) + c.x;
        j = int(ring) + c.y;
    }
    const uint iw = uint(i) % ROUND_THETA;
    float3 position, normal;
    float eta, before;
    if (j < 0)
    {
        const float4 c = asfloat(centre.Load4(0));
        eta = c.x;
        before = c.z;
        position = origin + float3(0, eta, 0);
        normal = float3(0, 1, 0);
    }
    else
    {
        const float4 f = field.Load(int3(int(iw), j, 0));
        const float theta = float(i) * (2 * ROUND_PI / float(ROUND_THETA)), r = (float(j) + 1) * R / float(ROUND_RINGS);
        const float3 er = ax * cos(theta) + az * sin(theta), et = az * cos(theta) - ax * sin(theta);
        eta = f.x;
        before = asfloat(previous.Load(4 * (iw + uint(j) * ROUND_THETA)));
        position = origin + er * r + float3(0, eta, 0);
        normal = normalize(float3(0, 1, 0) - er * f.y - et * f.z);
    }
    vertices.Store4(32 * id, asuint(float4(position, 1)));
    vertices.Store4(32 * id + 16, asuint(float4(normal, 0)));
    velocities.Store4(16 * id, asuint(float4(0, (eta - before) * invDt, 0, 0)));
}
