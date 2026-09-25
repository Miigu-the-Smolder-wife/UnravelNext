// unx-kernel: cs_6_6 main
// Edge (E) pixel detection (Edge.hlsli relation), one 8 x 8 tile per group, before the shading kernels (design revision 1
// 4.4: a thin kernel of its own, so the neighbour loads' latency hides behind many resident waves instead of stalling the
// shading kernel). Per pixel: the 3 x 3 neighbours' vis ids (L1 within the tile); only a neighbour showing another
// triangle reads its material word, and only one with the same material reads its depth and normal. Sky pixels (VIS_NONE)
// are edge pixels when a neighbour shows a surface, which is the same relation seen from the other side.
// Outputs: the tile's 64-bit edge mask (bit = y * 8 + x; the shading kernels keep an edge pixel's exposed linear radiance
// for the composite) and the edge pixel list (layout in Edge.hlsli; ShadeBegin zeroed its count), one global atomic per
// tile. Both are gathered with groupshared atomics, not wave operations: after the detection's divergent early exits the
// lanes taking part in a wave operation are not guaranteed to be the ones that reconverged (measured: a wave-OR mask lost
// bits that the list kept, and the composite read radiance no kernel wrote).
// Planar reflection views (R's planar tile mask, v1.22): tiles without mirror pixels are neither rasterised nor shaded, so
// their pixels count as outside the view here and in the composite (nothing reads a radiance that was never written).
// P[0] = { vis id SRV, material word SRV, depth SRV, G-buffer SRV }
// P[1] = { edge tile mask UAV (R32G32_UINT, tilesX x tilesY), edge pixel list UAV (raw), edge args UAV (raw),
//          planar tile mask SRV (R8_UINT; UNX_NONE = every tile of the view is shaded) }
// P[2] = { edge cos angle, edge footprint tolerance, edge distance tolerance (floats), experiment mask (bit 256: none) }
// P[3].x first tile row of this dispatch (the screen band's, M's banded shading passes; groups cover its tile rows)
#include "Bindless.hlsli"
#include "Passes/Common/VisBuffer.hlsli"
#include "Passes/Shading/Edge.hlsli"

groupshared uint gs_mask[2];
groupshared uint gs_count;
groupshared uint gs_base;

bool edgeShadedPixel(int2 q)
{
    if (any(q < 0) || q.x >= int(g_viewWidth) || q.y >= int(g_viewHeight)) return false;
    if (P[1].w == UNX_NONE) return true;
    Texture2D<uint> planarTiles = ResourceDescriptorHeap[P[1].w];
    return planarTiles[uint2(q) / M_TILE] != 0;
}

bool edgeDetect(uint2 pixel)
{
    Texture2D<uint> visIds = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint> words = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].w];
    const EdgeParams ep = { asfloat(P[2].x), asfloat(P[2].y), asfloat(P[2].z) };
    const uint vc = visIds[pixel];
    const bool sky = vc == VIS_NONE;
    uint material = 0;
    bool sampled = false;
    EdgePixel c = (EdgePixel)0;
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        if (k == 4) continue;
        const int2 q = int2(pixel) + int2(int(k % 3) - 1, int(k / 3) - 1);
        if (!edgeShadedPixel(q)) continue;
        if (visIds[uint2(q)] == vc) continue;  // the same triangle (or both sky)
        if (sky) return true;                   // a surface next to the sky
        if (!sampled)
        {
            material = mWordMaterial(words[pixel]);
            c = edgeSample(pixel, material, linearDepth(depth[pixel]), octDecode(gbuffer[pixel].x));
            sampled = true;
        }
        const uint mq = mWordMaterial(words[uint2(q)]);
        if (mq != material) return true;
        const EdgePixel e = edgeSample(uint2(q), mq, linearDepth(depth[uint2(q)]), octDecode(gbuffer[uint2(q)].x));
        if (!edgeSameSurface(c, e, ep)) return true;
    }
    return false;
}

[numthreads(8, 8, 1)]
void main(uint2 group : SV_GroupID, uint2 tid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
    const uint2 gid = uint2(group.x, group.y + P[3].x);
    if (gi < 2) gs_mask[gi] = 0;
    if (gi == 0) gs_count = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint2 pixel = gid * M_TILE + tid;
    const bool detect = (P[2].w & 256) == 0 && edgeShadedPixel(int2(pixel));
    const bool isEdge = detect && edgeDetect(pixel);
    uint slot = 0;
    if (isEdge)
    {
        InterlockedOr(gs_mask[gi >> 5], 1u << (gi & 31));
        InterlockedAdd(gs_count, 1, slot);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0)
    {
        RWTexture2D<uint2> tileMask = ResourceDescriptorHeap[P[1].x];
        tileMask[gid] = uint2(gs_mask[0], gs_mask[1]);
        uint base = 0;
        if (gs_count != 0)
        {
            RWByteAddressBuffer args = ResourceDescriptorHeap[P[1].z];
            args.InterlockedAdd(12, gs_count, base);
        }
        gs_base = base;
    }
    GroupMemoryBarrierWithGroupSync();
    if (isEdge)
    {
        RWByteAddressBuffer list = ResourceDescriptorHeap[P[1].y];
        list.Store(4 * (1 + gs_base + slot), pixel.x | (pixel.y << 16));
    }
}
