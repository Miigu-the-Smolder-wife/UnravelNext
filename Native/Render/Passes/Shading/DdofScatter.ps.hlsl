// unx-kernel: ps_6_6 main
// Diaphragm depth of field, the scattered highlights' discs (DdofScatter.hlsli; the reference's
// DOFHybridScatterPixelShader.usf), added to the gathered layer they belong to (the blend adds rgb and keeps the layer's
// alpha). Per pixel of the quad, each of the block's four discs by how far the pixel is inside it, with one pixel of
// feather around the edge (half the energy exactly at the radius, as the gathers count a sample): a disc's edge at
// radius r, or the diaphragm's outline (the edge table; the foreground's is the background's turned half a turn). The
// pixel's offset is measured on the lens (DdofScatter.hlsli: the Petzval matrix, the squeeze).
// The background's discs are occluded by what the gather found in front: the layer's gathered radii at the pixel have
// mean m and variance v, and a disc of a radius wider than m (it lies behind) is seen by v / (v + (0.95 r - m)^2).
// The vignetting (DdofScatter.hlsli): the pixel's ray at the barrel's end is inside the barrel's rim (a soft edge
// over rim^2 +- 1 cm^2, the reference's) and on the axis's side of every flag's edge (over 1 / 128 cm).
#include "Passes/Shading/DdofScatter.hlsli"

#define NONE 0xFFFFFFFFu

float4 main(DdofSprite v) : SV_Target0
{
    const float2 fromFirst = v.position.xy - v.centre;
    const float4 sprite[4] = { v.s0, v.s1, v.s2, v.s3 };
    float2 statistics = float2(0, 1);
    if (P[0].w != NONE)
    {
        Texture2D<float4> gathered = ResourceDescriptorHeap[P[0].w];
        statistics = gathered.SampleLevel(g_linearClamp, v.position.xy / float2(P[1].xy), 0).xy;
    }
    const float turn = (float)(int)P[1].w;
    const float squeeze = asfloat(P[2].x), barrelRadius = asfloat(P[2].y);
    const bool vignetting = asfloat(P[2].z) >= 0.0;
    const float2 edge[DDOF_MATTE_BOX_FLAGS] = { v.flag01.xy, v.flag01.zw, v.flag2 };
    float3 colour = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        float2 offset = ddofTransform(v.petzval, fromFirst - float2(kDdofSquare[i]));
        offset.x *= squeeze;
        const float radius = max(sprite[i].w, 1e-3);
        float inside;
        if (P[0].z != NONE) inside = saturate(radius * ddofEdgeFactor(P[0].z, turn * offset) - length(offset) + 0.5);
        else inside = saturate(dot(offset, offset) * (-0.5 / radius) + (0.5 * radius + 0.5));  // (radius - distance + 0.5 about the edge, without the root)
        float seen = 1;
        if (P[0].w != NONE)
        {
            const float behind = max(radius * (1.0 - DDOF_FAST_GATHER_ERROR) - statistics.x, 0.0);
            seen = statistics.y / (statistics.y + behind * behind);
        }
        if (vignetting && inside > 0.0)
        {
            const float2 atEnd = v.bundle + float2(offset.x, -offset.y) * v.spread[i];
            seen *= saturate((barrelRadius * barrelRadius - dot(atEnd, atEnd)) * 1e4);
            [unroll] for (uint f = 0; f < DDOF_MATTE_BOX_FLAGS; ++f)
            {
                const float4 flag = asfloat(P[6 + f]);
                if (flag.w > 0.0) seen *= saturate(dot(edge[f] - atEnd, flag.xy) * 12800.0);
            }
        }
        colour += sprite[i].rgb * (inside * seen);
    }
    return float4(colour, 0);
}
