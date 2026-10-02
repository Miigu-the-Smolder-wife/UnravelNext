// unx-kernel: cs_6_6 main
// Diaphragm depth of field, the reduce (DiaphragmDof.cpp; the reference's DOFReduce.usf with
// DOFHybridScatterCompilation.ush): the gathers' input and what is scattered instead of gathered.
//   levels    the half-resolution colour and radius (level 0) and up to three coarser levels of it, each texel its four
//             finer ones by ddofDownsample (the radius kept: operator 2; the sharpness halves per level) - a wide kernel
//             reads the level its sample spacing is the texel of. One group per 8 x 8 level-0 texels, the coarser levels
//             reduced in group memory. The level textures are padded to whole groups (the padding repeats the border).
//   scatter   a bright pixel with a wide radius would be a few noisy gather samples; it is drawn as a bokeh sprite
//             instead (DdofScatter.ms/.ps.hlsl) and removed from the gathered colour by its scatter factor:
//               luma4 x exposure / 0.4 - 1                      from 0 at 0.4 (luma4 = r + 2 g + b of the exposed colour)
//               x saturate(|radius| - P[3].x)                    (a small disc is gathered well enough)
//               x saturate(distance to the view's border - |radius|)   (its disc inside the view)
//               x the largest of saturate(d / 0.7 - 1) over the four diagonal quarter-resolution neighbours, d the
//                 largest channel by which the pixel (limited to P[3].y) exceeds the neighbour (only when the factor
//                 is still above 0.5): a highlight among its surroundings, not a uniformly bright area
//               x 2 - 1                                          (the faint ones are not worth a sprite)
//             Sprites are per 2 x 2 pixels (one quad covers four discs: less overdraw): a 2 x 2 block with a pixel
//             above 0.01 is appended to its layer's list (foreground: radius < 0), each pixel's colour x factor x
//             min(1, squeeze / (pi radius^2)) (a disc's energy over its area; an anamorphic lens's bokeh is
//             narrower by its squeeze). A full list: the pixel is gathered.
// P[0] = { gather input SRV (rgb, a = radius), quarter resolution SRV, level 0 UAV, level 1 UAV | none }
// P[1] = { level 2 UAV | none, level 3 UAV | none, foreground list UAV | none, background list UAV | none } (raw:
//          DdofCommon.hlsli DDOF_SCATTER_*)
// P[2] = { half width, half height, level count, list capacity (records) }
// P[3] = { asuint(least |radius| scattered), asuint(neighbour comparison's colour limit), asuint(exposure scale),
//          asuint(the lens's squeeze) }, P[4] = { quarter width, quarter height, 0, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/DdofCommon.hlsli"

#define NONE 0xFFFFFFFFu

groupshared float4 gTexel[64];
groupshared uint gForegroundMask, gBackgroundMask, gForegroundBase, gBackgroundBase;

float neighbourFactor(float3 colour, float2 quarterPixel, float exposure)
{
    Texture2D<float4> quarter = ResourceDescriptorHeap[P[0].y];
    const float2 size = float2(P[4].xy);
    const float limit = asfloat(P[3].y) / exposure;
    const float3 mine = min(colour, limit);
    float factor = 0;
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const float2 uv = min((quarterPixel + float2(kDdofCross[i])) / size, (size - 0.5) / size);
        const float3 other = min(quarter.SampleLevel(g_pointClamp, uv, 0).rgb, limit);
        const float3 above = max(mine - other, 0.0);
        factor = max(factor, saturate(max(above.r, max(above.g, above.b)) * exposure / 0.7 - 1.0));
    }
    return factor;
}

void writeLevel(uint level, uint2 texel, float4 value)
{
    const uint uav = level == 0 ? P[0].z : level == 1 ? P[0].w : level == 2 ? P[1].x : P[1].y;
    RWTexture2D<float4> destination = ResourceDescriptorHeap[uav];
    destination[texel] = value;
}

// A pixel's entry in its 2 x 2 block's record 'slot' of a list; false: the list is full.
bool append(uint uav, uint slot, uint member, uint2 id, float3 scattered, float radius)
{
    if (slot >= P[2].w) return false;
    RWByteAddressBuffer list = ResourceDescriptorHeap[uav];
    const uint o = DDOF_SCATTER_HEADER + slot * DDOF_SCATTER_BYTES;
    if (member == 0) list.Store4(o, uint4(asuint(float2(id) + 0.5), 0, 0));
    list.Store4(o + 16 + 16 * member, asuint(float4(scattered, radius)));
    return true;
}

[numthreads(8, 8, 1)]
void main(uint2 tid : SV_GroupThreadID, uint2 id : SV_DispatchThreadID, uint index : SV_GroupIndex)
{
    const uint2 size = P[2].xy;
    Texture2D<float4> input = ResourceDescriptorHeap[P[0].x];
    const float4 texel = input.Load(int3(min(id, size - 1), 0));
    const float coc = texel.a;
    float3 colour = texel.rgb;
    const bool foreground = coc < 0;
    const uint ownList = foreground ? P[1].z : P[1].w;

    if (index == 0)
    {
        gForegroundMask = 0;
        gBackgroundMask = 0;
        gForegroundBase = 0;
        gBackgroundBase = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    float factor = 0;
    if (ownList != NONE)
    {
        const float exposure = asfloat(P[3].z);
        factor = saturate(ddofLuma4(colour) * exposure / 0.4 - 1.0);
        factor *= saturate(abs(coc) - asfloat(P[3].x));
        const float2 centre = float2(id) + 0.5;
        const float border = min(min(centre.x, centre.y), min((float)size.x - centre.x, (float)size.y - centre.y));
        factor *= saturate(border - abs(coc));
        if (factor > 0.5) factor *= neighbourFactor(colour, centre * 0.5, exposure);
        factor = saturate(factor * 2.0 - 1.0);
    }
    const uint groupBit = 1u << ((tid.y >> 1) * 4 + (tid.x >> 1)), member = (tid.y & 1) * 2 + (tid.x & 1);
    if (factor > 0.01)
    {
        if (foreground) InterlockedOr(gForegroundMask, groupBit);
        else InterlockedOr(gBackgroundMask, groupBit);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint foregroundMask = gForegroundMask, backgroundMask = gBackgroundMask;
    if (index == 0)
    {
        // the tile's records: one append per list
        if (foregroundMask != 0)
        {
            RWByteAddressBuffer list = ResourceDescriptorHeap[P[1].z];
            uint base;
            list.InterlockedAdd(0, countbits(foregroundMask), base);
            gForegroundBase = base;
        }
        if (backgroundMask != 0)
        {
            RWByteAddressBuffer list = ResourceDescriptorHeap[P[1].w];
            uint base;
            list.InterlockedAdd(0, countbits(backgroundMask), base);
            gBackgroundBase = base;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // (a block listed for the other layer still takes the pixel's entry there, with no colour)
    const float loss = coc != 0 ? min(1.0, asfloat(P[3].w) * rcp(DDOF_PI * coc * coc)) : 1.0;
    const bool inForeground = (foregroundMask & groupBit) != 0, inBackground = (backgroundMask & groupBit) != 0;
    if (!(foreground ? inForeground : inBackground)) factor = 0;
    const float3 scattered = colour * (factor * loss);
    if (inForeground)
    {
        const bool kept = append(P[1].z, gForegroundBase + countbits(foregroundMask & (groupBit - 1)), member, id, foreground ? scattered : float3(0, 0, 0), abs(coc));
        if (!kept && foreground) factor = 0;
    }
    if (inBackground)
    {
        const bool kept = append(P[1].w, gBackgroundBase + countbits(backgroundMask & (groupBit - 1)), member, id, foreground ? float3(0, 0, 0) : scattered, abs(coc));
        if (!kept && !foreground) factor = 0;
    }

    colour *= 1.0 - factor;
    writeLevel(0, id, float4(colour, coc));
    gTexel[index] = float4(colour, coc);

    [unroll] for (uint level = 1; level < 4; ++level)
    {
        GroupMemoryBarrierWithGroupSync();
        const uint span = 1u << level, apart = span >> 1;
        const bool mine = level < P[2].z && all((tid & (span - 1)) == 0);
        float4 reduced = 0;
        if (mine)
        {
            const float4 a = gTexel[index], b = gTexel[index + apart], c = gTexel[index + 8 * apart], d = gTexel[index + 9 * apart];
            float3 colours[4] = { a.rgb, b.rgb, c.rgb, d.rgb };
            float radii[4] = { a.a, b.a, c.a, d.a };
            float3 mean;
            float radius;
            ddofDownsample(colours, radii, false, 0.5 / (float)(1u << (level - 1)), mean, radius);
            reduced = float4(mean, radius);
        }
        GroupMemoryBarrierWithGroupSync();
        if (mine)
        {
            gTexel[index] = reduced;
            writeLevel(level, id >> level, reduced);
        }
    }
}
