// unx-kernel: cs_6_6 main
// Bloom pyramid (Post.cpp), one level down: the 13-tap filter of Jimenez 2014 at half resolution (a centre box 0.5 of 4
// taps and four overlapping corner boxes 0.125 each of a 3 x 3 tap grid), every tap the mean of a 2 x 2 source texel box
// on a texel corner. With those boxes every source texel receives a total weight of exactly 1/4 whatever its parity
// (PostTests: an impulse keeps its energy at any position). The boxes are averaged here in fp32 from a groupshared tile
// (20 x 20 source texels per 8 x 8 group, about 6 loads per thread against 13 filtered taps), so the weights are exact
// without relying on the texture unit's filter precision; the store rounds to the nearest half (HalfRound.hlsli).
// P[0] = { source SRV, destination UAV, destination width, height }.
// The pyramid's first level with local exposure (LocalExposure.hlsli; the reference applies it in its bloom setup, so the
// lens tail is of the image as it is shown): P[1] = { grid SRV (UNX_NONE: none - every other level), blurred SRV,
// asuint(uv scale x), asuint(uv scale y) }, P[2] = { asuint(highlight), asuint(shadow), asuint(detail), asuint(blend) },
// P[3].x = asuint(log2 middle grey): every source texel is scaled by its factor.
#include "Bindless.hlsli"
#include "Passes/Shading/HalfRound.hlsli"
#include "Passes/Shading/LocalExposure.hlsli"

#ifndef POST_TILE_SIZE
#define POST_TILE_SIZE 8
#endif
#define POST_SOURCE_SIZE (2 * POST_TILE_SIZE + 4)
groupshared float3 s_tile[POST_SOURCE_SIZE][POST_SOURCE_SIZE];

[numthreads(POST_TILE_SIZE, POST_TILE_SIZE, 1)]
void main(uint2 id : SV_DispatchThreadID, uint2 local : SV_GroupThreadID, uint2 group : SV_GroupID, uint flat : SV_GroupIndex)
{
    Texture2D<float4> src = ResourceDescriptorHeap[P[0].x];
    uint sw, sh;
    src.GetDimensions(sw, sh);
    // the tile covers source texels 16 group - 2 .. 16 group + 17 (clamped at the borders)
    const int2 origin = int2(group) * (2 * POST_TILE_SIZE) - 2;
    const int2 hi = int2(sw, sh) - 1;
    LeParams le = (LeParams)0;
    le.grid = P[1].x;
    le.blurred = P[1].y;
    le.uvScale = asfloat(P[1].zw);
    le.highlight = asfloat(P[2].x);
    le.shadow = asfloat(P[2].y);
    le.detail = asfloat(P[2].z);
    le.blend = asfloat(P[2].w);
    le.logMiddleGrey = asfloat(P[3].x);
    for (uint i = flat; i < POST_SOURCE_SIZE * POST_SOURCE_SIZE; i += POST_TILE_SIZE * POST_TILE_SIZE)
    {
        const int2 t = int2(i % POST_SOURCE_SIZE, i / POST_SOURCE_SIZE);
        const int2 at = clamp(origin + t, int2(0, 0), hi);
        float3 value = src.Load(int3(at, 0)).rgb;
        if (le.grid != 0xFFFFFFFFu) value *= leScale(le, value, (float2(at) + 0.5) / float2(sw, sh), 0.0);
        s_tile[t.y][t.x] = value;
    }
    GroupMemoryBarrierWithGroupSync();
    if (any(id >= P[0].zw)) return;
    // box(o): the 2 x 2 source texels from 2 id + o (tile index 2 local + o + 2)
    const int2 b = int2(local) * 2 + 2;
    float3 box[5][5];
    [unroll] for (int y = -2; y <= 2; ++y)
        [unroll] for (int x = -2; x <= 2; ++x)
        {
            if ((x & 1) != (y & 1)) continue;  // the taps sit where both offsets are even or both odd
            const int2 p = b + int2(x, y);
            box[y + 2][x + 2] = (s_tile[p.y][p.x] + s_tile[p.y][p.x + 1] + s_tile[p.y + 1][p.x] + s_tile[p.y + 1][p.x + 1]) * 0.25;
        }
    // the centre box (the 4 inner taps), then the four overlapping corner boxes of the 3 x 3 outer taps
    const float3 inner = (box[1][1] + box[1][3] + box[3][1] + box[3][3]) * 0.25;
    const float3 b0 = (box[0][0] + box[0][2] + box[2][0] + box[2][2]) * 0.25, b1 = (box[0][2] + box[0][4] + box[2][2] + box[2][4]) * 0.25;
    const float3 b2 = (box[2][0] + box[2][2] + box[4][0] + box[4][2]) * 0.25, b3 = (box[2][2] + box[2][4] + box[4][2] + box[4][4]) * 0.25;
    RWTexture2D<float4> dst = ResourceDescriptorHeap[P[0].y];
    dst[id] = float4(halfRound(inner * 0.5 + (b0 + b1 + b2 + b3) * 0.125), 1);
}
