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

void roundVertex(int i, int j, out float4 outPosition, out float4 outNormal, out float4 outVelocity)
{
    Texture2D<float4> field = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer previous = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer centre = ResourceDescriptorHeap[P[1].w];
    const float invDt = asfloat(P[1].y), level = asfloat(P[1].z), R = asfloat(P[2].z);
    const float3 origin = float3(asfloat(P[2].x), level, asfloat(P[2].y));
    const float3 ax = float3(asfloat(P[3].x), 0, asfloat(P[3].y)), az = float3(asfloat(P[3].z), 0, asfloat(P[3].w));
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
    outPosition = float4(position, 1);
    outNormal = float4(normal, 0);
    outVelocity = float4(0, (eta - before) * invDt, 0, 0);
}

groupshared float4 g_position[81], g_normal[81], g_velocity[81];
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint2 base = group.xy * 8;
    if (all(group == 0) && lane == 0)
    {
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].x];
        args.Store4(0, uint4(3u * ROUND_THETA * (1u + 2u * (ROUND_RINGS - 1u)), 1, 0, 0));
    }
    for (uint node = lane; node < 81; node += 64)
    {
        const uint2 at = base + uint2(node % 9, node / 9);
        float4 p = 0, n = 0, v = 0;
        // The last tile has seven ring cells. Keep the original unwrapped angle
        // at i=512: its sin/cos must not be replaced by those of i=0.
        if (at.y < ROUND_RINGS) roundVertex(int(at.x), int(at.y), p, n, v);
        g_position[node] = p; g_normal[node] = n; g_velocity[node] = v;
    }
    GroupMemoryBarrierWithGroupSync();
    RWByteAddressBuffer vertices = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer velocities = ResourceDescriptorHeap[P[0].w];
    if (group.y == 0 && lane < 24)
    {
        const uint corner = lane % 3, theta = lane / 3;
        float4 p, n, v;
        if (corner == 0) roundVertex(int(base.x + theta), -1, p, n, v);
        else
        {
            const uint at = theta + (corner == 2 ? 1u : 0u);
            p = g_position[at]; n = g_normal[at]; v = g_velocity[at];
        }
        const uint id = base.x * 3 + lane;
        vertices.Store4(id * 32, asuint(p)); vertices.Store4(id * 32 + 16, asuint(n)); velocities.Store4(id * 16, asuint(v));
    }
    const uint2 corners[6] = {uint2(0,0), uint2(1,0), uint2(0,1), uint2(1,0), uint2(1,1), uint2(0,1)};
    for (uint v = lane; v < 384; v += 64)
    {
        const uint2 cell = uint2((v / 6) % 8, (v / 6) / 8);
        const uint2 globalCell = base + cell;
        if (globalCell.y >= ROUND_RINGS - 1) continue;
        const uint2 node = cell + corners[v % 6];
        const uint at = node.y * 9 + node.x;
        const uint id = ROUND_THETA * 3 + (globalCell.y * ROUND_THETA + globalCell.x) * 6 + v % 6;
        vertices.Store4(id * 32, asuint(g_position[at]));
        vertices.Store4(id * 32 + 16, asuint(g_normal[at]));
        velocities.Store4(id * 16, asuint(g_velocity[at]));
    }
}
