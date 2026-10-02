// unx-kernel: cs_6_6 main
// unx-variants: HALF=0,1
// Motion blur at the output resolution, after the temporal upscale (MotionBlur.cpp motionBlurUpscaled; the reference's
// MotionBlurApply.usf, one blur direction): the gather over the upscaled colour along the neighbourhood's longest
// velocity (MotionFlatten.hlsl's tiles), both ways from the pixel - the exposure is centred on the frame's time, as the
// reference's (the internal-resolution path's ends at it).
// One group per 16 x 16 output pixels. The group's class, from the 3 x 3 velocity tiles around it:
//   still              the longest velocity is under half a pixel: the colour is copied;
//   gather             every velocity of the neighbourhood is of the longest's size (shortest^2 > 0.4 longest^2): the
//                      plain mean of the taps along the longest velocity - what a uniform motion is;
//   scatter as gather  the velocities differ (a moving thing over a still one): each tap weighs by whether its own
//                      velocity reaches the pixel (its length in taps against the tap's distance), or the pixel's does
//                      when the pixel is the nearer one (linear depth, softly over 1 cm), mirrored across the pixel
//                      where the far tap is the nearer and slower one - a background is not pulled over what covers it;
//                      the taps' total weight is the blur's share, the pixel's own colour the rest.
//   half resolution    (flag 1) a group whose neighbourhood's longest velocity exceeds the tap count in pixels gathers
//                      once per 2 x 2 pixels, at their common corner; each pixel keeps its own colour for its own
//                      share. HALF=1 is that kernel (8 x 8 threads a group; it also copies the still groups), HALF=0
//                      the per-pixel one; each leaves the other's groups alone.
// Taps: 4 x ceil(longest / 4), at most P[1].w, half each way, at the strata's midpoints shifted by an interleaved
// gradient noise of the pixel; taps more than a pixel apart read towards the half-resolution colour (two pixels: it alone).
// P[0] = { colour SRV (output resolution, exposed linear), half colour SRV, flat SRV (RGBA16F, internal resolution:
//          length, angle, linear depth), gathered tiles SRV (RGBA16F: shortest xy, longest xy) }
// P[1] = { destination UAV, output width, height, tap count limit }, P[2] = { internal width, height, tiles x, tiles y }
// P[3] = { flags (1: half-resolution gather), 0, 0, 0 }
#include "Bindless.hlsli"

#define MOTION_FILTER_TILE 16
#define MOTION_FLATTEN_TILE 16.0
#define MOTION_MIN_VELOCITY 0.5
#define MOTION_DEPTH_SCALE 100.0  // 1 / (the depth over which the nearer of two surfaces is decided: 1 cm)

#define CLASS_GATHER_HALF 0u   // (and the still groups)
#define CLASS_GATHER_FULL 1u
#define CLASS_SCATTER_HALF 2u
#define CLASS_SCATTER_FULL 3u

float gradientNoise(float2 pixel, float index)
{
    pixel += index * (float2(47, 17) * 0.695);
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// A velocity of 'spread' pixels reaches a tap 'offset' taps away (taps are 1 / toTaps pixels apart).
float reachWeight(float spread, float offset, float toTaps) { return saturate(toTaps * spread - max(offset - 1.0, 0.0)); }

float3 colourAt(Texture2D<float4> colour, Texture2D<float4> halfColour, float2 uv, float mip)
{
    if (mip <= 0.0) return colour.SampleLevel(g_linearClamp, uv, 0).rgb;
    if (mip >= 1.0) return halfColour.SampleLevel(g_linearClamp, uv, 0).rgb;
    return lerp(colour.SampleLevel(g_linearClamp, uv, 0).rgb, halfColour.SampleLevel(g_linearClamp, uv, 0).rgb, mip);
}

#if HALF
[numthreads(MOTION_FILTER_TILE / 2, MOTION_FILTER_TILE / 2, 1)]
#else
[numthreads(MOTION_FILTER_TILE, MOTION_FILTER_TILE, 1)]
#endif
void main(uint2 gid : SV_GroupID, uint2 id : SV_DispatchThreadID)
{
    const int2 size = int2(P[1].yz), inSize = int2(P[2].xy), tileCount = int2(P[2].zw);
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    Texture2D<float4> halfColour = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> flatTexture = ResourceDescriptorHeap[P[0].z];
    Texture2D<float4> tiles = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[1].x];
    const float maxTaps = (float)P[1].w;
    const float2 toInternal = float2(inSize) / float2(size);

    // the group's class (the same for all its threads)
    uint groupClass = CLASS_GATHER_HALF;
    {
        const int2 flattenTile = int2((float2(gid) + 0.5) * toInternal);
        [unroll] for (int k = 0; k < 9; ++k)
        {
            const float4 range = tiles.Load(int3(clamp(flattenTile + int2(k % 3, k / 3) - 1, 0, tileCount - 1), 0));
            const float shortest = dot(range.xy, range.xy), longest = dot(range.zw, range.zw);
            const bool halfRes = longest > maxTaps * maxTaps && (P[3].x & 1u) != 0;
            uint suggested = halfRes ? CLASS_SCATTER_HALF : CLASS_SCATTER_FULL;
            if (longest < MOTION_MIN_VELOCITY * MOTION_MIN_VELOCITY) suggested = CLASS_GATHER_HALF;
            else if (shortest > 0.4 * longest) suggested = halfRes ? CLASS_GATHER_HALF : CLASS_GATHER_FULL;
            groupClass = max(groupClass, suggested);
        }
    }
#if HALF
    if (groupClass != CLASS_GATHER_HALF && groupClass != CLASS_SCATTER_HALF) return;
    const int2 corner = int2(id) * 2;
    if (any(corner >= size)) return;
    const float2 uv = (float2(corner) + 1.0) / float2(size);
#else
    if (groupClass != CLASS_GATHER_FULL && groupClass != CLASS_SCATTER_FULL) return;
    if (any(int2(id) >= size)) return;
    const float2 uv = (float2(id) + 0.5) / float2(size);
#endif

    // the neighbourhood's velocities: the tiles under the pixel's internal sample, jittered a quarter tile
    const float random = gradientNoise(float2(id), 0), random2 = gradientNoise(float2(id), 1);
    const int2 internalPixel = clamp(int2(uv * float2(inSize)), 0, inSize - 1);
    const float2 tileUv = min(((float2(internalPixel) + 0.5) / MOTION_FLATTEN_TILE + (float2(random, random2) - 0.5) * 0.5) / float2(tileCount),
                              1.0 - 0.5 / float2(tileCount));
    const float4 range = tiles.SampleLevel(g_linearClamp, tileUv, 0);
    const float2 longest = range.zw;
    const float longestLength = length(longest);
    const float taps = clamp(4.0 * ceil(longestLength / 4.0), 4.0, maxTaps);
    const float mip = saturate(longestLength / taps - 1.0);
    const float2 search = longest / float2(size);  // (UV)

    float3 blurred = 0;
    float ownShare = 1;
    if (longestLength >= MOTION_MIN_VELOCITY)
    {
        const bool uniformMotion = groupClass == CLASS_GATHER_HALF || groupClass == CLASS_GATHER_FULL || dot(range.xy, range.xy) > 0.4 * longestLength * longestLength;
        const float steps = taps * 0.5;
        if (uniformMotion)
        {
            [loop] for (float tap = 0; tap < steps; ++tap)
            {
                const float2 fraction = (tap + 0.5 + float2(random - 0.5, 0.5 - random)) / steps;
                blurred += colourAt(colour, halfColour, uv + fraction.x * search, mip) + colourAt(colour, halfColour, uv - fraction.y * search, mip);
            }
            blurred /= taps;
            ownShare = 0;
        }
        else
        {
            const float toTaps = steps / longestLength;
            const float3 centre = flatTexture.Load(int3(internalPixel, 0)).xyz;  // length, angle, depth
            float weightSum = 0;
            [loop] for (float tap = 0; tap < steps; ++tap)
            {
                const float2 offset = tap + 0.5 + float2(random - 0.5, 0.5 - random);
                const float2 fraction = offset / steps;
                const float2 uv0 = saturate(uv + fraction.x * search), uv1 = saturate(uv - fraction.y * search);
                const float3 tap0 = flatTexture.Load(int3(clamp(int2(uv0 * float2(inSize)), 0, inSize - 1), 0)).xyz;
                const float3 tap1 = flatTexture.Load(int3(clamp(int2(uv1 * float2(inSize)), 0, inSize - 1), 0)).xyz;
                // the pixel's own reach where it is the nearer one, else the tap's
                const float centreReach = reachWeight(centre.x, tap + 0.5, toTaps);
                float weight0 = saturate(0.5 + MOTION_DEPTH_SCALE * (tap0.z - centre.z)) * centreReach +
                                saturate(0.5 - MOTION_DEPTH_SCALE * (tap0.z - centre.z)) * reachWeight(tap0.x, tap + 0.5, toTaps);
                float weight1 = saturate(0.5 + MOTION_DEPTH_SCALE * (tap1.z - centre.z)) * centreReach +
                                saturate(0.5 - MOTION_DEPTH_SCALE * (tap1.z - centre.z)) * reachWeight(tap1.x, tap + 0.5, toTaps);
                // mirrored: a tap that is farther than its opposite takes the opposite's weight where it is also the
                // faster, the nearer where it is the slower
                const bool2 mirror = bool2(tap0.z > tap1.z, tap0.x < tap1.x);
                weight0 = all(mirror) ? weight1 : weight0;
                weight1 = any(mirror) ? weight1 : weight0;
                blurred += weight0 * colourAt(colour, halfColour, uv0, mip) + weight1 * colourAt(colour, halfColour, uv1, mip);
                weightSum += weight0 + weight1;
            }
            blurred /= taps;
            ownShare = saturate(1.0 - weightSum / taps);
        }
    }
#if HALF
    [unroll] for (uint q = 0; q < 4; ++q)
    {
        const int2 pixel = corner + int2(q & 1u, q >> 1);
        if (any(pixel >= size)) continue;
        const float4 own = colour.Load(int3(pixel, 0));
        output[pixel] = float4(own.rgb * ownShare + blurred, own.a);
    }
#else
    const float4 own = colour.Load(int3(id, 0));
    output[id] = float4(own.rgb * ownShare + blurred, own.a);
#endif
}
