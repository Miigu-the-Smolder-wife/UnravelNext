// unx-kernel: cs_6_6 main
// unx-variants: OUTPUT=0,1
// Edge (E) composite (ARCHITECTURE 2.10; INTERFACES 5.5.1), one thread per edge pixel of the list the shading kernels
// EdgeDetect.hlsl filled (ExecuteIndirect; the list is compact, so every lane of a wave composites). For each edge pixel (Edge.hlsli) the
// 3 x 3 neighbourhood is split into surface groups (<= shading.edge_groups_max; the centre's first, then edge
// neighbours, then corners; the sky is a group without triangles): a neighbour on a triangle an earlier neighbour showed
// joins that neighbour's group (one triangle is one surface), any other joins the first group whose representative sees
// the same surface or opens a group. A group's coverage of the pixel square is the exact area of its distinct triangles
// inside it (edgeTriangleArea, deformed vertices projected camera-relative; each triangle is fetched once, and only for
// groups nearer than the farthest, whose share is the remainder), times, for
// an alpha-tested material, the part of the pixel its cut-out covers (edgeCutoutCoverage: the triangle's uv under the
// pixel centre). Groups hide each other in depth order at the pixel centre by their 32-subsample masks (V's
// coverageTriangleMask; error <= 1/32 of the overlap); a cut-out group hides the subsamples of its mask in proportion to
// its cut-out coverage. The farthest group (the sky when present) holds the remaining area: it continues behind the
// nearer ones. Each group contributes the exposed linear radiance of its representative pixel (the shading kernels kept it for
// every edge pixel), and the sum is tone mapped once (the pixel filter acts on radiance, not on display values).
// Every per-neighbour and per-group array is indexed by unrolled constants only, so it stays in registers.
// P[0] = { vis id SRV, visible clusters SRV, material word SRV, depth SRV }
// P[1] = { gbuffer SRV, edge radiance SRV, colour UAV, edge pixel list SRV (raw) }
// P[2] = { cos angle, footprint tolerance, distance tolerance (floats), groups max }
// P[3] = { experiment mask (shading.experiment_disable; cost attribution only: 64 = one triangle per group, its
//        representative's; 128 = no coverage geometry, the centre's radiance; 512 = no subsample masks; 1024 = no
//        cut-out coverage), M texture table SRV, planar tile mask SRV (R8_UINT; UNX_NONE = every tile is shaded),
//        resolved radiance UAV (RGBA16F; UNX_NONE = no coverage layer): the pixel's exposed linear sum, the band A layer
//        of the coverage composite on edge pixels (CoverageComposite.hlsl) }
// Planar reflection views: pixels of tiles without mirror pixels were never shaded (R's tile mask, v1.22) and count as
// outside the view, as in EdgeDetect.
#include "Bindless.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/Edge.hlsli"
#include "Passes/Visibility/Coverage.hlsli"

#define EDGE_GROUPS 3u
#define EDGE_NONE 3u  // group field of a neighbour outside every group

// Neighbour k in the grouping order: centre, edge neighbours (0,-1) (-1,0) (1,0) (0,1), corners (-1,-1) (1,-1) (-1,1)
// (1,1); 2-bit fields of dx + 1 and dy + 1.
int2 edgeNeighbour(uint k)
{
    const uint dx = 1u | 1u << 2 | 0u << 4 | 2u << 6 | 1u << 8 | 0u << 10 | 2u << 12 | 0u << 14 | 2u << 16;
    const uint dy = 1u | 0u << 2 | 1u << 4 | 1u << 6 | 2u << 8 | 0u << 10 | 0u << 12 | 2u << 14 | 2u << 16;
    return int2(int((dx >> (2 * k)) & 3u), int((dy >> (2 * k)) & 3u)) - 1;
}

// Pixel coordinates of a camera-relative point (x right, y down), w = view depth; w <= 0: behind the camera.
float3 edgeProject(float3 offset)
{
    const float3 v = float3(dot(g_view[0].xyz, offset), dot(g_view[1].xyz, offset), dot(g_view[2].xyz, offset));
    const float4 clip = mul(g_proj, float4(v, 1));
    return float3((clip.x / clip.w * 0.5 + 0.5) * g_viewWidth, (0.5 - clip.y / clip.w * 0.5) * g_viewHeight, clip.w);
}

struct EdgeGroup
{
    float z;          // depth of the group's plane along the pixel-centre ray
    float area;       // coverage of the pixel square by the group's triangles (exact area x cut-out coverage)
    float density;    // area over the triangles' geometric area: 1 for opaque surfaces, the cut-out's share otherwise
    uint mask;        // 32-subsample coverage
    float3 radiance;  // exposed linear radiance of the representative pixel
};

void edgeOrder(inout EdgeGroup a, inout EdgeGroup b)
{
    if (b.z < a.z)
    {
        const EdgeGroup t = a;
        a = b;
        b = t;
    }
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    ByteAddressBuffer list = ResourceDescriptorHeap[P[1].w];
    if (i >= list.Load(0)) return;
    const uint packed = list.Load(4 * (1 + i));
    const uint2 pixel = uint2(packed & 0xFFFFu, packed >> 16);
    Texture2D<uint> visIds = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].z];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].w];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[1].x];
    Texture2D<float4> edgeRadiance = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float4> color = ResourceDescriptorHeap[P[1].z];
    const EdgeParams ep = { asfloat(P[2].x), asfloat(P[2].y), asfloat(P[2].z) };
    const uint groupsMax = min(P[2].w, EDGE_GROUPS);
    if (P[3].x & 128)
    {
        color[pixel] = shEncodeExposed(shParticles(edgeRadiance[pixel].rgb, pixel, P[5].z, P[5].w));  // P[5].zw particle layer
        if (P[3].w != UNX_NONE)
        {
            RWTexture2D<float4> resolved = ResourceDescriptorHeap[P[3].w];
            resolved[pixel] = float4(edgeRadiance[pixel].rgb, 1);
        }
        return;
    }

    uint vis[9];
    bool valid[9];
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        const int2 q = int2(pixel) + edgeNeighbour(k);
        valid[k] = all(q >= 0) && q.x < int(g_viewWidth) && q.y < int(g_viewHeight);
        if (valid[k] && P[3].z != UNX_NONE)
        {
            Texture2D<uint> planarTiles = ResourceDescriptorHeap[P[3].z];
            valid[k] = planarTiles[uint2(q) / M_TILE] != 0;
        }
        vis[k] = valid[k] ? visIds[uint2(q)] : VIS_NONE;
    }

    // Surface groups: representatives (first member) in registers, each neighbour's group in 2-bit fields.
    EdgePixel rep[EDGE_GROUPS];
    uint2 repPixel[EDGE_GROUPS];
    rep[0] = edgePixel(pixel, words, depth, gbuffer);
    repPixel[0] = pixel;
    [unroll] for (uint g0 = 1; g0 < EDGE_GROUPS; ++g0)
    {
        rep[g0] = rep[0];
        repPixel[g0] = pixel;
    }
    uint groups = 1;
    uint groupOf = 0;                                  // neighbour k's group at bits 2k
    uint triangles = vis[0] != VIS_NONE ? 1u : 0u;     // bit k: neighbour k shows a triangle no earlier neighbour showed
    uint representatives = 1;                          // bit k: neighbour k represents its group
    [unroll] for (uint k1 = 1; k1 < 9; ++k1)
    {
        uint g = EDGE_NONE;
        bool known = false;
        [unroll] for (uint j = 0; j < k1; ++j)
            if (!known && valid[j] && vis[j] == vis[k1])
            {
                known = true;
                g = (groupOf >> (2 * j)) & 3u;
            }
        if (valid[k1] && !known)
        {
            const uint2 q = uint2(int2(pixel) + edgeNeighbour(k1));
            const EdgePixel e = edgePixel(q, words, depth, gbuffer);
            [unroll] for (uint j2 = 0; j2 < EDGE_GROUPS; ++j2)
                if (g == EDGE_NONE && j2 < groups && edgeSameSurface(rep[j2], e, ep)) g = j2;
            if (g == EDGE_NONE && groups < groupsMax)  // beyond the group budget the sample is left to the remainder
            {
                [unroll] for (uint j3 = 1; j3 < EDGE_GROUPS; ++j3)
                    if (j3 == groups)
                    {
                        rep[j3] = e;
                        repPixel[j3] = q;
                    }
                g = groups++;
                representatives |= 1u << k1;
            }
            if (g != EDGE_NONE && vis[k1] != VIS_NONE) triangles |= 1u << k1;
        }
        groupOf |= (valid[k1] ? g : EDGE_NONE) << (2 * k1);
    }
    if (P[3].x & 64) triangles &= representatives;

    // Depth of each group's plane along the centre ray. The farthest group takes whatever the nearer ones leave, so its
    // own area and mask are never used: only the nearer groups' triangles are fetched.
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    float z[EDGE_GROUPS];
    uint farthest = 0;
    [unroll] for (uint g0z = 0; g0z < EDGE_GROUPS; ++g0z)
    {
        const EdgePixel r = rep[g0z];
        const float nD = dot(r.normal, D);
        z[g0z] = g0z >= groups ? 3.0e38 : r.sky ? 1e30 : nD < -1e-6 ? dot(r.normal, r.position) / nD : linearDepth(depth[repPixel[g0z]]);
        if (g0z < groups && z[g0z] > z[farthest]) farthest = g0z;
    }
    [unroll] for (uint k2 = 0; k2 < 9; ++k2)
        if (((groupOf >> (2 * k2)) & 3u) == farthest) triangles &= ~(1u << k2);

    // Coverage and 32-subsample mask per nearer group, one triangle fetch per distinct triangle.
    float area[EDGE_GROUPS] = { 0, 0, 0 }, geometric[EDGE_GROUPS] = { 0, 0, 0 };
    uint mask[EDGE_GROUPS] = { 0, 0, 0 };
    while (triangles != 0)
    {
        const uint k = firstbitlow(triangles);
        triangles &= triangles - 1;
        const uint g = (groupOf >> (2 * k)) & 3u;
        const MTriangleCorners t = mTriangleCorners(visIds[uint2(int2(pixel) + edgeNeighbour(k))], P[0].y);
        const float3 a = edgeProject(t.w0 - g_cameraPosition);
        const float3 b = edgeProject(t.w1 - g_cameraPosition);
        const float3 c = edgeProject(t.w2 - g_cameraPosition);
        if (min(a.z, min(b.z, c.z)) <= g_nearPlane * 0.5) continue;  // crosses the camera plane: left to the remainder
        const float ar = edgeTriangleArea(a.xy, b.xy, c.xy, float2(pixel));
        const uint m = (P[3].x & 512) ? 0xFFFFFFFFu : coverageTriangleMask(a.xy, b.xy, c.xy, float2(pixel));
        float cut = 1;
        const MTextureSet ts = mLoadTextureSet(P[3].y, t.material);
        if (ts.coverage != UNX_NONE && (P[3].x & 1024) == 0)
        {
            float2 uv, duvdx, duvdy;
            mPlaneUv(t, D, Dx, Dy, uv, duvdx, duvdy);
            cut = edgeCutoutCoverage(ts, loadMaterial(t.material).alphaCutoff, uv, duvdx, duvdy);
        }
        [unroll] for (uint j = 0; j < EDGE_GROUPS; ++j)
            if (j == g)
            {
                area[j] += ar * cut;
                geometric[j] += ar;
                mask[j] |= m;
            }
    }

    // Front to back (sorting network on registers).
    EdgeGroup grp[EDGE_GROUPS];
    [unroll] for (uint g1 = 0; g1 < EDGE_GROUPS; ++g1)
    {
        grp[g1].z = z[g1];
        grp[g1].area = area[g1];
        grp[g1].density = geometric[g1] > 0 ? area[g1] / geometric[g1] : 1;
        grp[g1].mask = mask[g1];
        grp[g1].radiance = g1 < groups ? edgeRadiance[repPixel[g1]].rgb : 0;
    }
    edgeOrder(grp[0], grp[1]);
    edgeOrder(grp[1], grp[2]);
    edgeOrder(grp[0], grp[1]);

    // The farthest group takes what the nearer ones leave. A nearer group hides its mask's subsamples by its density
    // (opaque 1); with at most three groups only the middle one is seen through a nearer one, the front group's.
    uint covered = 0;
    float through = 1;  // share of the covered subsamples the nearer group lets through
    float used = 0;
    float3 sum = 0;
    [unroll] for (uint g2 = 0; g2 < EDGE_GROUPS; ++g2)
    {
        if (g2 >= groups) continue;
        float w;
        if (g2 + 1 == groups) w = max(1 - used, 0.0);
        else
        {
            const uint m = grp[g2].mask;
            const float seenFraction = countbits(m) > 0 ? (countbits(m & ~covered) + countbits(m & covered) * through) / (float)countbits(m)
                                                        : 1 - countbits(covered) / 32.0 * (1 - through);
            w = min(grp[g2].area * seenFraction, max(1 - used, 0.0));
            through = covered == 0 ? 1 - grp[g2].density : through * (1 - grp[g2].density);
            covered |= m;
        }
        used += w;
        sum += w * grp[g2].radiance;
    }
    color[pixel] = shEncodeExposed(shParticles(sum, pixel, P[5].z, P[5].w));
    if (P[3].w != UNX_NONE)
    {
        RWTexture2D<float4> resolved = ResourceDescriptorHeap[P[3].w];
        resolved[pixel] = float4(sum, 1);
    }
}
