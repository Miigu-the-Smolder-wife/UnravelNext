// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Diaphragm depth of field, the radius tiles (DiaphragmDof.cpp; the reference's DOFCocTileFlatten.usf and
// DOFCocTileDilate.usf): what the gathers of an 8 x 8 half-resolution tile can meet, so they choose their kernel's
// radius, skip a tile nothing reaches, and take the plain mean where every radius in reach is the same.
// STEP=0, flatten: one group per tile. Of its 64 radii: the foreground's range (the radii <= 0: the smallest is the
//   widest blur), the background's range, the smallest background radius; the nearest closer surface's distance starts
//   as the smallest background radius.
// STEP=1, dilate: a tile takes the ranges of the tiles whose blur reaches it. Rings of tiles at 1 .. P[1].z samples of
//   P[1].w tiles each (a wide blur: several passes, the later ones stepping further), each ring a bucket:
//     mode 1  the widest foreground and background radii only: a bucket's widest radius counts when it reaches the tile
//             across the bucket's distance (radius x P[2].w > the distance; the factor carries the gather kernel's
//             random offset);
//     mode 2  with those widest radii known (P[2].yz: mode 1's result), the narrowest radii that the tile's kernel of the
//             widest radius can meet, from the buckets inside its reach;
//     mode 0  both in one pass (one pass suffices: the blur is at most P[1].z tiles wide).
// A tile: foreground RG16F { smallest radius, largest radius }, background RGBA16F { largest radius, smallest radius,
// smallest intersectable radius, distance to the nearest closer surface }.
// STEP=0: P[0] = { gather input SRV (a = radius), foreground UAV, background UAV, 0 }, P[1] = { half width, half
//         height, tiles x, tiles y }
// STEP=1: P[0] = { foreground SRV, background SRV, foreground UAV, background UAV }, P[1] = { tiles x, tiles y, ring
//         count, tiles per ring step }, P[2] = { mode, widest foreground SRV, widest background SRV, asuint(reach factor) }
#include "Bindless.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

#ifndef STEP
#define STEP 0
#endif

#if STEP == 0
groupshared float gCoc[DDOF_TILE * DDOF_TILE];

[numthreads(DDOF_TILE, DDOF_TILE, 1)]
void main(uint2 gid : SV_GroupID, uint2 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    Texture2D<float4> input = ResourceDescriptorHeap[P[0].x];
    gCoc[index] = input.Load(int3(min(id, P[1].xy - 1), 0)).a;
    GroupMemoryBarrierWithGroupSync();
    if (index != 0 || any(gid >= P[1].zw)) return;
    float fgdMin = 0, fgdMax = -DDOF_LARGE_COC, bgdMin = DDOF_LARGE_COC, bgdMax = 0, intersectable = DDOF_LARGE_COC;
    for (uint i = 0; i < DDOF_TILE * DDOF_TILE; ++i)
    {
        const float coc = gCoc[i], f = min(coc, 0.0), b = max(coc, 0.0);
        fgdMin = min(fgdMin, f);
        fgdMax = max(fgdMax, f);
        bgdMin = min(bgdMin, b);
        bgdMax = max(bgdMax, b);
        if (coc > 0) intersectable = min(intersectable, coc);
    }
    RWTexture2D<float4> foreground = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> background = ResourceDescriptorHeap[P[0].z];
    foreground[gid] = float4(fgdMin, fgdMax, 0, 0);
    background[gid] = float4(bgdMax, bgdMin, intersectable, bgdMin);
}
#else
#define MAX_RINGS 3  // (kMaxCocDilateSampleRadiusCount)

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const uint2 tiles = P[1].xy;
    if (any(id >= tiles)) return;
    Texture2D<float4> inForeground = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> inBackground = ResourceDescriptorHeap[P[0].y];
    const uint rings = min(P[1].z, MAX_RINGS), mode = P[2].x;
    const int stride = (int)P[1].w;
    const float reach = asfloat(P[2].w), toCoc = rcp(reach);

    // the rings' tiles into one bucket per ring
    DdofTile bucket[MAX_RINGS];
    [unroll] for (uint k = 0; k < MAX_RINGS; ++k)
    {
        bucket[k].fgdMin = 0;
        bucket[k].fgdMax = -DDOF_LARGE_COC;
        bucket[k].bgdMin = DDOF_LARGE_COC;
        bucket[k].bgdMax = 0;
        bucket[k].bgdMinIntersectable = DDOF_LARGE_COC;
        bucket[k].bgdCloser = DDOF_LARGE_COC;
    }
    for (uint b = 0; b < rings; ++b)
    {
        const int ring = (int)b + 1;
        for (int batch = 0; batch < 4 * ring; ++batch)
        {
            // (half of the square ring at 'ring' tiles: its other half is the mirrored offsets)
            int2 square;
            if (batch < ring) square = int2(ring, batch);
            else if (batch < 3 * ring) square = int2(ring - (batch - ring), ring);
            else square = int2(-ring, ring - (batch - 3 * ring));
            [unroll] for (int side = 0; side < 2; ++side)
            {
                const int2 offset = (side == 0 ? 1 : -1) * stride * square;
                const int2 at = int2(id) + offset;
                if (all(at >= 0) && all(at < int2(tiles)))
                {
                    // (the nearest two points of the two tiles are this far apart, in half-resolution pixels)
                    const float apart = length(max(DDOF_TILE * abs(float2(offset)) - DDOF_TILE, 0.0));
                    const DdofTile n = ddofLoadTile(inForeground, inBackground, at);
                    bucket[b].fgdMin = min(bucket[b].fgdMin, n.fgdMin);
                    bucket[b].fgdMax = max(bucket[b].fgdMax, n.fgdMax);
                    bucket[b].bgdMin = min(bucket[b].bgdMin, n.bgdMin);
                    bucket[b].bgdMax = max(bucket[b].bgdMax, n.bgdMax);
                    bucket[b].bgdMinIntersectable = min(bucket[b].bgdMinIntersectable, n.bgdMinIntersectable + apart * toCoc);
                    bucket[b].bgdCloser = min(bucket[b].bgdCloser, n.bgdCloser + apart * toCoc);
                }
            }
        }
    }

    DdofTile o = ddofLoadTile(inForeground, inBackground, int2(id));
    if (mode == 2)
    {
        Texture2D<float4> widestForeground = ResourceDescriptorHeap[P[2].y];
        Texture2D<float4> widestBackground = ResourceDescriptorHeap[P[2].z];
        o.fgdMin = widestForeground.Load(int3(id, 0)).x;
        o.bgdMax = widestBackground.Load(int3(id, 0)).x;
    }
    else
    {
        for (uint k = 0; k < rings; ++k)
        {
            const float across = (float)stride * ((float)(k + 1) * DDOF_TILE) - DDOF_TILE;
            if (-bucket[k].fgdMin * reach > across) o.fgdMin = min(o.fgdMin, bucket[k].fgdMin);
            if (bucket[k].bgdMax * reach > across) o.bgdMax = max(o.bgdMax, bucket[k].bgdMax);
        }
    }
    // the narrowest radii the kernel of those widest radii meets
    for (uint k2 = 0; k2 < rings; ++k2)
    {
        const float across = (float)stride * ((float)(k2 + 1) * DDOF_TILE) - DDOF_TILE;
        if (-o.fgdMin * reach > across) o.fgdMax = max(o.fgdMax, bucket[k2].fgdMax);
        if (o.bgdMax * reach > across)
        {
            o.bgdMin = min(o.bgdMin, bucket[k2].bgdMin);
            o.bgdMinIntersectable = min(o.bgdMinIntersectable, bucket[k2].bgdMinIntersectable);
        }
        o.bgdCloser = min(o.bgdCloser, bucket[k2].bgdCloser);
    }
    if (mode == 1)
    {
        o.fgdMax = 0;
        o.bgdMin = 0;
        o.bgdMinIntersectable = 0;
        o.bgdCloser = 0;
    }
    RWTexture2D<float4> outForeground = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<float4> outBackground = ResourceDescriptorHeap[P[0].w];
    outForeground[id] = float4(o.fgdMin, o.fgdMax, 0, 0);
    outBackground[id] = float4(o.bgdMax, o.bgdMin, o.bgdMinIntersectable, o.bgdCloser);
}
#endif
