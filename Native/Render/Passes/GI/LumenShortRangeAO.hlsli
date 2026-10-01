// Short-range ambient occlusion and bent normal of the screen-probe final gather (lumen.short_range_ao; owner A, for R's
// gi.lumen). The structure and default numbers follow Unreal Engine's Lumen short-range AO (ue6-main
// LumenScreenSpaceBentNormal.cpp / .usf, LumenScreenProbeGatherTemporal.usf, read on 2026-10-01); the code is ours.
// The screen probes are 16 px apart, so occlusion nearer than about two probe spacings is missing from their
// interpolated irradiance; this pass finds it per pixel in the depth buffer:
//   r.gi.sao        half resolution (one pixel of each 2 x 2 block per frame): horizon search over 2 view-space slices,
//                   3 steps each way per slice, out to 32 px (2 x the probe spacing), samples in front of the search
//                   area faded out; the cosine-weighted visibility of the slices (AO) and the bent normal;
//   r.gi.sao.temporal  full resolution: a stochastic bilinear pick of the 4 half-resolution pixels (plane and normal
//                   weights), the reprojected history clamped to the pick's 3 x 3 neighbourhood, at most 10 frames.
// Result: ViewResources::shortRangeAO, RGBA16F: xyz = world bent normal x AO, a = accumulated frames (0: no surface).
// How the final gather uses it (R's integration): diffuse indirect x lumenAoMultibounce(diffuse colour, AO); rough
// specular x lumenAoSpecular(n, roughness, AO, v, bent normal x AO).
#ifndef UNX_LUMEN_SHORT_RANGE_AO_HLSLI
#define UNX_LUMEN_SHORT_RANGE_AO_HLSLI
#include "Bindless.hlsli"

// xyz = unit bent normal (the pixel's normal when unoccluded data is missing), w = AO in [0, 1]. srv = UNX_NONE: (n, 1).
float4 lumenShortRangeAO(uint srv, uint2 pixel, float3 normal)
{
    if (srv == UNX_NONE) return float4(normal, 1);
    Texture2D<float4> t = ResourceDescriptorHeap[srv];
    const float4 v = t[pixel];
    const float ao = length(v.xyz);
    if (!(v.a > 0) || !(ao > 1e-4)) return float4(normal, v.a > 0 ? 0.03 : 1.0);
    return float4(v.xyz / ao, saturate(ao));
}

// AO for the diffuse indirect light with the energy the occluder bounces back (same albedo as the receiver assumed;
// the fit of "Improved Ambient Occlusion", Patapom 2018): the first two bounces' rescale. maxAlbedo (0.5): near-white
// albedo keeps some occlusion.
float3 lumenAoMultibounce(float3 diffuseColor, float ao, float maxAlbedo)
{
    const float a = min(ao, 0.999);
    const float f0 = a * (1 + (1 - a) / (2 * sqrt(sqrt(1 - a))));
    const float f1 = 27.576937094210385 * a * pow(1 - a, 1.5) * exp(-3.3364392003423804 * sqrt(sqrt(a)));
    const float tau = 1 - f1 / max(1 - f0, 1e-5);
    const float3 albedo = min(diffuseColor, maxAlbedo);
    return f0 + albedo / max(1 - albedo * tau, 1e-5) * f1;
}

// Occlusion of the specular lobe: the overlap of the reflection cone (half angle max(roughness, 0.1) pi) and the
// unoccluded cone (AO pi about the bent normal), smooth in the angle between them; near full occlusion the bent
// normal's direction means little and the result goes to 0.
float lumenAoSpecular(float3 n, float roughness, float ao, float3 v, float3 bentNormalTimesAo)
{
    const float reflection = max(roughness, 0.1) * 3.14159265, open = ao * 3.14159265;
    const float between = acos(clamp(dot(bentNormalTimesAo, reflect(-v, n)) / max(ao, 0.001), -1.0, 1.0));
    const float difference = abs(reflection - open);
    const float x = 1 - saturate((between - difference) / max(reflection + open - difference, 1e-6));
    const float overlap = x * x * (3 - 2 * x);
    return overlap * saturate((open - 0.1) / 0.2);
}

// 11-11-10 bits of a vector in [-1, 1]^3 (the half-resolution texture).
uint lumenAoPack(float3 v)
{
    const float3 u = saturate(v * 0.5 + 0.5);
    return uint(u.x * 2047.0 + 0.5) | (uint(u.y * 2047.0 + 0.5) << 11) | (uint(u.z * 1023.0 + 0.5) << 22);
}
float3 lumenAoUnpack(uint p)
{
    return float3((p & 2047u) / 2047.0, ((p >> 11) & 2047u) / 2047.0, (p >> 22) / 1023.0) * 2 - 1;
}

// The full-resolution pixel a half-resolution pixel stands on this frame (the 2 x 2 block's pixels in turn).
uint2 lumenAoFullPixel(uint2 half, uint factor, uint frame, uint2 viewSize)
{
    uint2 j = 0;
    if (factor >= 2)
    {
        const uint i = (half.x & 1u) + half.y * 2u + frame;
        j = uint2((i & 2u) ? 1u : 0u, (i & 1u) ? 0u : 1u);
    }
    return min(half * factor + j, viewSize - 1);
}
float lumenAoNoise(uint2 p, uint frame, uint salt)
{
    const float2 q = float2(p) + 5.588238 * float(frame & 63u) + float2(47.0, 17.0) * float(salt);
    return frac(52.9829189 * frac(dot(q, float2(0.06711056, 0.00583715))));
}
#endif
