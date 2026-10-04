// unx-kernel: cs_6_6 main
// unx-variants: PASS=0,1
// Calm water's reflection camera mask (A14; WaterSurface.hlsli waterPlanarCandidate), over the camera's rectangle of
// main-view pixels, for one candidate plane k of a W stream:
//   PASS=0 per pixel: 1 where the pixel's water-layer sample is this stream's and passes the candidate test, else 0 (R8
//          scratch), and their count into the statistics word WATER_STAT_PLANAR_MASK + k (the CPU's raster-or-rays
//          choice for later frames).
//   PASS=1 per 8 x 8 tile of the rectangle: ViewDesc::planarMask (INTERFACES v1.22 / M request: 1 = mirror pixel, 2 = its
//          3 x 3 apron, 0 = skipped) and planarTileMask (nonzero = the tile has a nonzero mask texel).
// P[0] = { waterVis SRV (PASS 0) or scratch SRV (PASS 1), stream vertices SRV (PASS 0) or mask UAV (PASS 1),
//          scratch UAV (PASS 0) or tile mask UAV (PASS 1), statistics UAV (UNX_NONE) }
// P[1] = plane (normal towards the camera, offset), P[2] = rectangle { x, y, w, h }, P[3] = { stream slot, k, 0, 0 }
// Frame constants b1 = the main view.
#include "WaterSurface.hlsli"

#if PASS == 0
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const bool inside = all(id.xy < P[2].zw);
    uint pass = 0;
    if (inside)
    {
        const uint2 pixel = P[2].xy + id.xy;
        Texture2D<uint> vis = ResourceDescriptorHeap[P[0].x];
        const uint v = vis[pixel];
        if ((v >> 30) == 3u && ((v >> 24) & 0x3Fu) == P[3].x)
        {
            float3 D, Dx, Dy;
            mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
            float3 p, n;
            if (waterTriangleHit(P[0].y, P[3].z, v & 0xFFFFFFu, g_cameraPosition, D, p, n))
                pass = waterPlanarCandidate(asfloat(P[1]), p, normalize(n), normalize(g_cameraPosition - p), shPixelAngle(D, Dx)) ? 1u : 0u;
        }
        RWTexture2D<uint> scratch = ResourceDescriptorHeap[P[0].z];
        scratch[id.xy] = pass;
    }
    const uint count = WaveActiveCountBits(pass != 0);
    if (P[0].w != UNX_NONE && WaveIsFirstLane() && count)
    {
        RWByteAddressBuffer st = ResourceDescriptorHeap[P[0].w];
        st.InterlockedAdd(4 * (WATER_STAT_PLANAR_MASK + P[3].y), count);
    }
}
#else
groupshared uint gs_any;
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    if (lane == 0) gs_any = 0;
    GroupMemoryBarrierWithGroupSync();
    const int2 size = int2(P[2].zw), q = int2(id.xy);
    if (all(q < size))
    {
        Texture2D<uint> scratch = ResourceDescriptorHeap[P[0].x];
        uint m = scratch[q] != 0 ? 1u : 0u;
        [unroll] for (int y = -1; y <= 1 && m == 0; ++y)
            [unroll] for (int x = -1; x <= 1; ++x)
            {
                const int2 r = q + int2(x, y);
                if (all(r >= 0) && all(r < size) && scratch[r] != 0) m = 2;
            }
        RWTexture2D<uint> mask = ResourceDescriptorHeap[P[0].y];
        mask[q] = m;
        if (m) InterlockedOr(gs_any, 1u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (lane == 0)
    {
        RWTexture2D<uint> tiles = ResourceDescriptorHeap[P[0].z];
        tiles[group.xy] = gs_any;
    }
}
#endif
