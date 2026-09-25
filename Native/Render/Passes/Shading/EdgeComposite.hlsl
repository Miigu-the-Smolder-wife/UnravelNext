// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// Edge (E) composite (ARCHITECTURE 2.10; INTERFACES 5.5.1), one 8 x 8 edge tile per group (ExecuteIndirect over the
// edge list the shading kernels filled). For each edge pixel (Edge.hlsli) the 3 x 3 neighbourhood is split into surface
// groups (<= shading.edge_groups_max, the centre's first, then edge neighbours, then corners; the sky is a group
// without triangles). A group's coverage of the pixel square is the exact area of its distinct triangles inside it
// (V's coverageTriangleArea, deformed vertices projected camera-relative), and groups hide each other in depth order at
// the pixel centre by their 32-subsample masks (coverageTriangleMask; error <= 1/32 of the overlap). The farthest group
// (the sky when present) holds the remaining area: it continues behind the nearer ones. Each group contributes the
// exposed linear radiance of its nearest pixel (the shading kernels kept it for every edge pixel), and the sum is tone
// mapped once (the pixel filter acts on radiance, not on display values).
// P[0] = { vis id SRV, visible clusters SRV, material word SRV, depth SRV }
// P[1] = { gbuffer SRV, edge radiance SRV, colour UAV, edge tile list SRV (raw) }
// P[2] = { cos angle, footprint tolerance, distance tolerance (floats), groups max }
#include "Bindless.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/Edge.hlsli"
#include "Passes/Visibility/Coverage.hlsli"

#define EDGE_GROUPS 3u

// Pixel coordinates of a camera-relative point (x right, y down), w = view depth; w <= 0: behind the camera.
float3 edgeProject(float3 offset)
{
    const float3 v = float3(dot(g_view[0].xyz, offset), dot(g_view[1].xyz, offset), dot(g_view[2].xyz, offset));
    const float4 clip = mul(g_proj, float4(v, 1));
    return float3((clip.x / clip.w * 0.5 + 0.5) * g_viewWidth, (0.5 - clip.y / clip.w * 0.5) * g_viewHeight, clip.w);
}

[numthreads(8, 8, 1)]
void main(uint3 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[1].w];
    const uint tile = list.Load(4 * gid.x);
    const uint2 pixel = uint2(tile & 0xFFFFu, tile >> 16) * M_TILE + tid;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint> visIds = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].z];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].w];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[1].x];
    Texture2D<float4> edgeRadiance = ResourceDescriptorHeap[P[1].y];
    const EdgeParams ep = { asfloat(P[2].x), asfloat(P[2].y), asfloat(P[2].z) };
    const uint groupsMax = min(P[2].w, EDGE_GROUPS);

    // Neighbourhood in the grouping order: centre, edge neighbours, corners.
    const int2 offsets[9] = { int2(0, 0), int2(0, -1), int2(-1, 0), int2(1, 0), int2(0, 1), int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1) };
    EdgePixel e[9];
    bool valid[9];
    uint vis[9];
    uint2 at[9];
    bool isEdge = false;
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const int2 q = int2(pixel) + offsets[k];
        valid[k] = all(q >= 0) && q.x < int(g_viewWidth) && q.y < int(g_viewHeight);
        at[k] = valid[k] ? uint2(q) : pixel;
        e[k] = edgePixel(at[k], words, depth, gbuffer);
        vis[k] = visIds[at[k]];
        if (k > 0 && valid[k] && vis[k] != vis[0] && !edgeSameSurface(e[0], e[k], ep)) isEdge = true;
    }
    if (!isEdge) return;

    // Surface groups (representative = first member, the nearest to the centre in the order above).
    uint group[9];
    uint rep[EDGE_GROUPS];
    uint groups = 1;
    rep[0] = 0;
    group[0] = 0;
    [unroll] for (uint k2 = 1; k2 < 9; ++k2)
    {
        group[k2] = EDGE_GROUPS;
        if (!valid[k2]) continue;
        uint g = EDGE_GROUPS;
        for (uint j = 0; j < groups; ++j)
            if (g == EDGE_GROUPS && (vis[rep[j]] == vis[k2] || edgeSameSurface(e[rep[j]], e[k2], ep))) g = j;
        if (g == EDGE_GROUPS)
        {
            if (groups < groupsMax)
            {
                g = groups++;
                rep[g] = k2;
            }
            else continue;  // beyond the group budget: this sample's surface is left to the remainder
        }
        group[k2] = g;
    }

    // Coverage, 32-subsample mask and depth at the pixel centre per group.
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    float area[EDGE_GROUPS], z[EDGE_GROUPS];
    uint mask[EDGE_GROUPS];
    for (uint g = 0; g < groups; ++g)
    {
        area[g] = 0;
        mask[g] = 0;
        const EdgePixel r = e[rep[g]];
        z[g] = r.sky ? 1e30 : dot(r.normal, r.position) / min(dot(r.normal, D), -1e-6);  // plane depth along the centre ray
        if (r.sky) continue;
        if (dot(r.normal, D) > -1e-6) z[g] = linearDepth(depth[at[rep[g]]]);
        for (uint k = 0; k < 9; ++k)
        {
            if (group[k] != g || vis[k] == VIS_NONE) continue;
            bool seen = false;
            for (uint j = 0; j < k; ++j) seen = seen || (group[j] == g && vis[j] == vis[k]);
            if (seen) continue;
            const float3 a = edgeProject(mTriangleVertex(vis[k], P[0].y, 0).world - g_cameraPosition);
            const float3 b = edgeProject(mTriangleVertex(vis[k], P[0].y, 1).world - g_cameraPosition);
            const float3 c = edgeProject(mTriangleVertex(vis[k], P[0].y, 2).world - g_cameraPosition);
            if (min(a.z, min(b.z, c.z)) <= g_nearPlane * 0.5) continue;  // crosses the camera plane: left to the remainder
            area[g] += coverageTriangleArea(a.xy, b.xy, c.xy, float2(pixel));
            mask[g] |= coverageTriangleMask(a.xy, b.xy, c.xy, float2(pixel));
        }
    }

    // Front to back; the farthest group takes what the nearer ones leave.
    uint order[EDGE_GROUPS] = { 0, 1, 2 };
    for (uint i = 1; i < groups; ++i)
        for (uint j = i; j > 0 && z[order[j]] < z[order[j - 1]]; --j)
        {
            const uint t = order[j];
            order[j] = order[j - 1];
            order[j - 1] = t;
        }
    uint covered = 0;
    float used = 0;
    float3 sum = 0;
    for (uint i2 = 0; i2 < groups; ++i2)
    {
        const uint g = order[i2];
        float w;
        if (i2 + 1 == groups) w = max(1 - used, 0.0);
        else
        {
            const uint m = mask[g];
            const float seenFraction = countbits(m) > 0 ? countbits(m & ~covered) / (float)countbits(m) : 1 - countbits(covered) / 32.0;
            w = min(area[g] * seenFraction, max(1 - used, 0.0));
            covered |= m;
        }
        used += w;
        sum += w * edgeRadiance[at[rep[g]]].rgb;
    }

    RWTexture2D<float4> color = ResourceDescriptorHeap[P[1].z];
    color[pixel] = shEncodeExposed(sum);
}
