// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2
// Motion blur at the output resolution, after the temporal upscale (MotionBlur.cpp motionBlurUpscaled; the reference's
// MotionBlurVelocityFlatten.usf and MotionBlurTileGather.usf): the velocities of the internal samples prepared for the
// gather (MotionApply.hlsl).
//   STEP=0  flatten, one group per 16 x 16 internal pixels: each pixel's velocity - the vector of the nearest surface
//           among the pixel and its four diagonal neighbours (an edge blurs with the foreground's motion), in output
//           pixels over half the shutter (the gather goes both ways), at most P[2].x long - as length and angle with
//           the surface's linear depth; and the tile's shortest and longest velocity. With the rotation stage after
//           the gather (P[2].y; MotionRotation.hlsl) the camera's rotation is taken out of the vector first: v - v_rot,
//           v_rot(p) = p - the place of Q^T d_p (the previous view direction of what p sees now, under the rotation
//           alone). A first-person view model turns with the camera: it keeps its own vector.
//   STEP=1  per tile, the range with every tile around whose longest velocity reaches this tile (the tile swept along
//           its velocity, one tile wider for the gather's jitter): the longest of the longest, the shortest of the
//           shortest - what MotionApply.hlsl reads as the neighbourhood's velocities.
//   STEP=2  the upscaled colour at half resolution (2 x 2 means): the gather's taps more than two pixels apart read it.
// STEP=0: P[0] = { motion SRV (RG32F, output-UV offsets: UpscaleMotion.hlsl), depth SRV (device depth of the surface
//         the vectors are of), flat UAV (RGBA16F: velocity length in output pixels, angle in radians, linear depth in
//         metres, 0), tiles UAV (RGBA16F: shortest xy, longest xy) }, P[1] = { internal width, height, asuint(output
//         pixels per UV x half the shutter, x), asuint(the same, y) }, P[2] = { asuint(longest velocity, output
//         pixels), flags (1: the rotation is taken out), vis id SRV | UNX_NONE (no view model this frame), visible
//         clusters SRV }, P[3..5] = asfloat rows of Q^T (view space, xyz), P[6] = asfloat { the unjittered projection's
//         m00, m11, m02 - m03, m12 - m13 }. Frame constants b1 = the main view (near plane).
// STEP=1: P[0] = { tiles SRV, gathered tiles UAV, tiles x, tiles y }, P[1] = { asuint(tiles per output pixel of
//         velocity), radius in tiles, 0, 0 }
// STEP=2: P[0] = { colour SRV, half colour UAV, half width, half height }, P[1] = { colour width, height, 0, 0 }
#include "Bindless.hlsli"
#include "Passes/Common/Frame.hlsli"
#if STEP == 0
#include "Passes/Material/MaterialSurface.hlsli"
#endif

#define MOTION_FLATTEN_TILE 16

#if STEP == 0
groupshared float4 gs_range[MOTION_FLATTEN_TILE * MOTION_FLATTEN_TILE];  // longest (length, angle), shortest (length, angle)

bool isViewModel(int2 pixel)
{
    if (P[2].z == UNX_NONE) return false;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[2].z];
    const uint visId = vis.Load(int3(pixel, 0));
    if (visId == VIS_NONE) return false;
    return (loadInstance(loadVisibleCluster(P[2].w, visVisibleCluster(visId)).instance).flags & INSTANCE_VIEW_MODEL) != 0;
}

float2 polarToCartesian(float2 polar) { return float2(cos(polar.y), sin(polar.y)) * polar.x; }

[numthreads(MOTION_FLATTEN_TILE, MOTION_FLATTEN_TILE, 1)]
void main(uint2 gid : SV_GroupID, uint2 id : SV_DispatchThreadID, uint flat : SV_GroupIndex)
{
    const int2 size = int2(P[1].xy);
    Texture2D<float2> motion = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    const int2 pixel = min(int2(id), size - 1);  // (a tile over the border: its last pixels again - the range is the same)
    // the nearest surface of the pixel and its diagonal neighbours (reversed Z: the largest)
    int2 from = pixel;
    float nearest = depth.Load(int3(pixel, 0));
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const int2 q = clamp(pixel + int2(k & 1u ? 1 : -1, k & 2u ? 1 : -1), 0, size - 1);
        const float z = depth.Load(int3(q, 0));
        if (z > nearest)
        {
            nearest = z;
            from = q;
        }
    }
    float2 offset = motion.Load(int3(from, 0));
    if ((P[2].y & 1u) != 0 && all(abs(offset) < 1.5) && !isViewModel(from))
    {
        // the view-space direction of the sample, then where the rotation alone had it in the previous frame
        const float2 uv = (float2(from) + 0.5) / float2(size);
        const float4 lens = asfloat(P[6]);
        const float3 d = float3((uv.x * 2.0 - 1.0 + lens.z) / lens.x, (1.0 - uv.y * 2.0 + lens.w) / lens.y, -1);
        const float3 q = float3(dot(asfloat(P[3].xyz), d), dot(asfloat(P[4].xyz), d), dot(asfloat(P[5].xyz), d));
        if (q.z < -1e-6)
        {
            const float2 qn = float2(q.x * lens.x / -q.z - lens.z, q.y * lens.y / -q.z - lens.w);
            offset -= uv - float2(qn.x * 0.5 + 0.5, 0.5 - qn.y * 0.5);
        }
    }
    // (a point behind the previous camera has no vector: UpscaleMotion's (2, 2))
    float2 velocity = all(abs(offset) < 1.5) ? offset * asfloat(P[1].zw) : float2(0, 0);
    if (!all(isfinite(velocity))) velocity = 0;
    const float speed = length(velocity);
    const float2 polar = float2(min(speed, asfloat(P[2].x)), speed > 0 ? atan2(velocity.y, velocity.x) : 0.0);
    if (all(int2(id) < size))
    {
        RWTexture2D<float4> flatOut = ResourceDescriptorHeap[P[0].z];
        flatOut[id] = float4(polar, min(linearDepth(nearest), 60000.0), 0);
    }
    gs_range[flat] = float4(polar, polar);
    GroupMemoryBarrierWithGroupSync();
    for (uint s = MOTION_FLATTEN_TILE * MOTION_FLATTEN_TILE / 2; s > 0; s >>= 1)
    {
        if (flat < s)
        {
            const float4 a = gs_range[flat], b = gs_range[flat + s];
            gs_range[flat] = float4(b.x > a.x ? b.xy : a.xy, b.z < a.z ? b.zw : a.zw);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (flat == 0)
    {
        RWTexture2D<float4> tiles = ResourceDescriptorHeap[P[0].w];
        tiles[gid] = float4(polarToCartesian(gs_range[0].zw), polarToCartesian(gs_range[0].xy));
    }
}
#elif STEP == 1
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const int2 count = int2(P[0].zw);
    if (any(int2(id) >= count)) return;
    Texture2D<float4> tiles = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> gathered = ResourceDescriptorHeap[P[0].y];
    const float tilesPerPixel = asfloat(P[1].x);
    const int radius = (int)P[1].y;
    float4 range = tiles.Load(int3(id, 0));
    float shortest = dot(range.xy, range.xy), longest = dot(range.zw, range.zw);
    [loop] for (int y = -radius; y <= radius; ++y)
        [loop] for (int x = -radius; x <= radius; ++x)
        {
            const int2 at = int2(id) + int2(x, y);
            if ((x == 0 && y == 0) || any(at < 0) || any(at >= count)) continue;
            const float4 neighbour = tiles.Load(int3(at, 0));
            // the neighbour swept along its longest velocity (both ways), half a tile wide and half a tile more for the
            // gather's jittered lookup - 0.99 of it: a tile whose velocity points along the tiles' rows does not reach
            // the rows beside it
            const float2 sweep = neighbour.zw * tilesPerPixel;
            const float sweepLength = length(sweep);
            const float2 along = sweepLength > 1e-6 ? sweep / sweepLength : float2(1, 0);
            const float extent = (abs(along.x) + abs(along.y)) * 0.99;
            const float2 onQuad = float2(dot(along, float2(x, y)), dot(float2(-along.y, along.x), float2(x, y)));
            if (abs(onQuad.x) >= sweepLength + extent || abs(onQuad.y) >= extent) continue;
            const float neighbourLongest = dot(neighbour.zw, neighbour.zw), neighbourShortest = dot(neighbour.xy, neighbour.xy);
            if (neighbourLongest > longest)
            {
                longest = neighbourLongest;
                range.zw = neighbour.zw;
            }
            if (neighbourShortest < shortest)
            {
                shortest = neighbourShortest;
                range.xy = neighbour.xy;
            }
        }
    gathered[id] = range;
}
#else
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[0].zw)) return;
    Texture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> halfColour = ResourceDescriptorHeap[P[0].y];
    const int2 last = int2(P[1].xy) - 1;
    float4 sum = 0;
    [unroll] for (uint k = 0; k < 4; ++k) sum += colour.Load(int3(min(int2(id) * 2 + int2(k & 1u, k >> 1), last), 0));
    halfColour[id] = sum * 0.25;
}
#endif
