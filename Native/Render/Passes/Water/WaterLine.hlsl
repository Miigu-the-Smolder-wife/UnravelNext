// unx-kernel: cs_6_6 main
// The waterline at the lens (W; FEATURES_GAME 1.3 (e)): with the camera half under a basin's water, the surface meets the
// near plane along a line across the picture. Per pixel: the point where the pixel's ray leaves the near plane and its
// height over the basin's surface there - the still level + the ripples' eta from the basin's field, so the line follows
// the ripples (at a near plane of 5 cm one centimetre of ripple moves it by two hundred pixels). Within the meniscus'
// reach of the line the pixel's light is taken down: the film that climbs the lens turns the rays out of the lens's path
// (a dark line; an appearance model - its width and strength are the quality file's).
// The media of the two sides (air above; the water's froxel medium below, WaterMedia.hlsl) are the froxels', at their
// tiles' resolution and from the still level: the tint's edge is soft and does not follow the ripples.
// P[0] = { colour UAV (exposed linear float), band A radiance UAV (UNX_NONE: none - M's later composites rebuild their
//          pixels from it), basin count (<= 4), 0 }
// P[1] = asuint{ the meniscus' width (m: the fall's e-fold), strength [0, 1], 0, 0 }
// P[2 + 2 i], P[3 + 2 i] = basin i: { centre x, still level, centre z, eta field SRV (RGBA32F 257^2: Pool.h; UNX_NONE: the
//          still level alone) }, asuint{ cos, sin of its yaw, size x (a round basin: the diameter), size z (0: round) }
// Frame constants of the main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(g_viewWidth, g_viewHeight))) return;
    const float3 lens = worldFromDepth(float2(id.xy), 1.0);  // (device depth 1: the near plane)
    float nearest = 3.0e38;
    for (uint i = 0; i < min(P[0].z, 4u); ++i)
    {
        const uint4 a = P[2 + 2 * i];
        const float4 b = asfloat(P[3 + 2 * i]);
        const float3 centre = asfloat(a.xyz);
        // the basin's axes (Pool.cpp: local x = dx cos - dz sin, local z = dx sin + dz cos), from its centre
        const float2 d = lens.xz - centre.xz;
        const float2 l = float2(d.x * b.x - d.y * b.y, d.x * b.y + d.y * b.x);
        const bool round = b.w == 0;
        if (round ? dot(l, l) > 0.25 * b.z * b.z : (abs(l.x) > 0.5 * b.z || abs(l.y) > 0.5 * b.w)) continue;
        float eta = 0;
        if (a.w != UNX_NONE && !round)
        {
            // sample (i, j) of the field is the basin's point (i hx, j hz) from its corner
            Texture2D<float4> field = ResourceDescriptorHeap[a.w];
            const float2 u = (l / float2(b.z, b.w) + 0.5) * 256.0;
            eta = field.SampleLevel(g_linearClamp, (u + 0.5) / 257.0, 0).x;
        }
        nearest = min(nearest, abs(lens.y - (centre.y + eta)));
    }
    const float width = asfloat(P[1].x);
    if (!(nearest < 8.0 * width)) return;
    const float keep = 1.0 - saturate(asfloat(P[1].y)) * exp(-nearest / width);
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    const float4 c = colour[id.xy];
    colour[id.xy] = float4(c.rgb * keep, c.a);
    if (P[0].y != UNX_NONE)
    {
        RWTexture2D<float4> bandA = ResourceDescriptorHeap[P[0].y];
        const float4 r = bandA[id.xy];
        bandA[id.xy] = float4(r.rgb * keep, r.a);
    }
}
