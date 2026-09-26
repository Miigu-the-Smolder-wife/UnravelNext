// Water stage 2 (FEATURES_GAME 1.9; INTERFACES v1.77): sunlight reaching a point under W's water, for render A's band A
// shading (ShadeOpaque and its fallback call it once per pixel; kept compact: one load when the frame has no water, four
// when it has). The sun-space water map (FrameResources::waterSunDepth / waterSunNormal / waterSunMedium /
// waterSunConstants, WaterSunMap.ms / .ps) holds, per texel of an orthographic view along the sun, the water surface
// nearest the sun, its normal and its medium.
//   waterSunLight(X): when the sun's straight ray to X passes water (a surface S above X towards the sun), X is lit
//   through that surface: lightDir = -t, t the exact Snell refraction of the incoming sunlight at S (the direction for
//   the sun's BRDF and cosine), transmittance = (1 - F(theta_s)) (cos theta_s / cos theta_t) T^d: F the exact
//   unpolarised Fresnel at the sun's incidence, cos theta_s / cos theta_t the beam's compression across the surface (the
//   refracted beam is narrower: the transmitted flux (1 - F) E cos theta_s per unit surface area, spread over the area
//   the beam covers across its own direction), T the medium's 1 m transmittance, d the path from X to the surface along
//   lightDir, measured to the plane through S with S's normal. The caller lights X with E x transmittance from lightDir,
//   so a horizontal Lambert floor under flat water gets E (1 - F) cos theta_s T^d: the flux per horizontal area is
//   conserved. Exact for a flat surface; on a curved surface the error is the surface's rise over the refracted path's
//   horizontal offset from S (FEATURES_GAME 1.9).
//   Conditions recorded there: (a) S's VSM shadows use the straight sun direction; (b) sky and GI light entering the
//   water is not attenuated yet (with R); (c) caustics come on the same map.
// `ior` > 1 overrides the medium's; otherwise the medium's is used. Returns false (lightDir = sunDir, transmittance = 1)
// when X is not under water from the sun or the map is absent (constSrv UNX_NONE or its valid word 0).
#ifndef UNX_WATER_LIGHT_HLSLI
#define UNX_WATER_LIGHT_HLSLI
#include "Bindless.hlsli"
#include "Passes/Common/Frame.hlsli"
#include "Passes/Water/WaterShading.hlsli"

// waterSunConstants (raw, 80 B): valid, texels per side, 0, 0; right.xyz, origin along right; up.xyz, origin along up;
// sun.xyz (towards the sun), smallest along the sun; 1 / extent along right, 1 / extent along up, depth range along the
// sun, 0.
float3 waterOctDecode(float2 e)
{
    float3 n = float3(e, 1 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = (1 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
    return normalize(n);
}
float2 waterOctEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    return n.z >= 0 ? n.xy : (1 - abs(n.yx)) * float2(n.x >= 0 ? 1 : -1, n.y >= 0 ? 1 : -1);
}

bool waterSunLight(uint depthSrv, uint normalSrv, uint mediumSrv, uint constSrv, float3 X, float3 sunDir, float ior, out float3 lightDir, out float3 transmittance)
{
    lightDir = sunDir;
    transmittance = 1;
    if (constSrv == UNX_NONE) return false;
    ByteAddressBuffer c = ResourceDescriptorHeap[constSrv];
    const uint4 head = c.Load4(0);
    if (head.x == 0) return false;
    const float4 r = asfloat(c.Load4(16)), u = asfloat(c.Load4(32)), s = asfloat(c.Load4(48)), k = asfloat(c.Load4(64));
    const float2 uv = float2((dot(X, r.xyz) - r.w) * k.x, 1 - (dot(X, u.xyz) - u.w) * k.y);
    if (any(uv < 0) || any(uv >= 1)) return false;
    const int2 texel = int2(uv * float(head.y));
    Texture2D<float> depth = ResourceDescriptorHeap[depthSrv];
    const float d = depth[texel];
    const float alongX = dot(X, s.xyz), alongS = s.w + d * k.z;
    if (d <= 0 || alongS <= alongX) return false;  // no water here, or the surface is not between X and the sun
    Texture2D<float2> normals = ResourceDescriptorHeap[normalSrv];
    Texture2D<float4> media = ResourceDescriptorHeap[mediumSrv];
    float3 n = waterOctDecode(normals[texel]);
    if (dot(n, s.xyz) < 0) n = -n;
    const float4 medium = media[texel];
    const float eta = 1.0 / (ior > 1 ? ior : max(medium.w, 1.0001));
    const float cosS = saturate(dot(s.xyz, n));
    float3 t;
    if (!waterRefract(s.xyz, n, eta, t)) return false;  // (not from air)
    lightDir = -t;
    const float3 S = X + s.xyz * (alongS - alongX);
    const float path = max(dot(S - X, n), 0.0) / max(dot(lightDir, n), 1e-4);
    const float cosT = max(dot(lightDir, n), 1e-4);
    transmittance = (1 - waterFresnel(cosS, eta)) * (cosS / cosT) * pow(clamp(medium.rgb, 1e-6, 1.0), path);
    return true;
}
#endif
