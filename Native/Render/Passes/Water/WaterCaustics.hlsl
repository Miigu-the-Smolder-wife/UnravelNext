// unx-kernel: cs_6_6 main
// Water stage 2 caustics (FEATURES_GAME 1.3 (c) on the sun-space map; WaterLight.hlsli reads them): every water texel of
// the sun map sends its sunlight along the exact Snell refraction at its surface point S (normal and IOR from the map)
// and splats it, bilinearly in fixed point (2^-16), where the refracted ray reaches the slice depths z_k = 0.25, 0.5, 1,
// 2, 4 m below its entry point along the entry's normal (a receiver's depth is measured the same way: below its own
// surface point along that point's normal, WaterLight.hlsli; planes across the sun would not do - under an oblique sun a
// flat pool's own surface spans metres of depth along the sun). A
// slice texel then holds the light that arrives there per unit of the light that entered one texel of the surface:
// flat water shifts the whole texel grid uniformly, so every texel gets exactly 1 (the caustic factor of no focusing);
// a curved surface concentrates or spreads it, and the total is conserved (every texel's unit lands somewhere).
// Binned by the landing point's own sun-map coordinates on the caustic grid (min(map texels, 1024) per side over the same
// extent; a map texel carries (grid / map)^2 of a grid texel), so a receiver X reads the grid texel of X's projection.
// P[0] depth SRV, normal SRV, medium SRV, constants SRV; P[1] caustics UAV (RWTexture2DArray<uint>, WATER_CAUSTIC_SLICES)
#include "WaterLight.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    ByteAddressBuffer c = ResourceDescriptorHeap[P[0].w];
    const uint4 head = c.Load4(0);
    const uint n = head.y;
    if (head.x == 0 || any(id.xy >= n)) return;
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    const float d = depth[id.xy];
    if (d <= 0) return;
    const float4 r = asfloat(c.Load4(16)), u = asfloat(c.Load4(32)), s = asfloat(c.Load4(48)), k = asfloat(c.Load4(64));
    Texture2D<float2> normals = ResourceDescriptorHeap[P[0].y];
    Texture2D<float4> media = ResourceDescriptorHeap[P[0].z];
    float3 nrm = waterOctDecode(normals[id.xy]);
    if (dot(nrm, s.xyz) < 0) nrm = -nrm;
    float3 t;
    if (!waterRefract(s.xyz, nrm, 1.0 / max(media[id.xy].w, 1.0001), t)) return;
    // S from the texel centre: along right and up from the map's origin, along the sun from its depth.
    const float extent = 1.0 / k.x;
    const float2 uv = (float2(id.xy) + 0.5) / float(n);
    const float alongS = s.w + d * k.z;
    const float3 S = r.xyz * (r.w + uv.x * extent) + u.xyz * (u.w + (1 - uv.y) * extent) + s.xyz * alongS;
    const float cosT = dot(-t, nrm);  // the refracted ray's descent along the normal per unit length
    if (cosT <= 0) return;
    RWTexture2DArray<uint> caustics = ResourceDescriptorHeap[P[1].x];
    const uint nc = min(n, WATER_CAUSTIC_MAX);                       // the caustic grid over the same extent
    const float unit = float(nc) * float(nc) / (float(n) * float(n));  // one map texel's share of a caustic texel
    [unroll] for (uint slice = 0; slice < WATER_CAUSTIC_SLICES; ++slice)
    {
        const float3 Q = S + t * (waterCausticDepth(slice) / cosT);
        const float2 p = float2((dot(Q, r.xyz) - r.w) * k.x, 1 - (dot(Q, u.xyz) - u.w) * k.y) * float(nc) - 0.5;
        const int2 i0 = int2(floor(p));
        const float2 f = p - float2(i0);
        const float w[4] = { (1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y };
        [unroll] for (uint q = 0; q < 4; ++q)
        {
            const int2 at = i0 + int2(q & 1, q >> 1);
            if (any(at < 0) || any(at >= int(nc))) continue;
            InterlockedAdd(caustics[uint3(at, slice)], uint(round(w[q] * unit * 65536.0)));
        }
    }
}
