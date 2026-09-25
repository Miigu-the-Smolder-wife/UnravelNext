// unx-kernel: lib_6_6 main
// Planar reflection test only: a stand-in for FrameServices::renderView that ray-traces the reflection camera (V/M/S
// are other tracks). A reflection-camera ray is the mirror's reflected ray from the point where it crosses the clip plane
// (the mirror), so it is traced from there and shaded exactly like the ray path's hits (ReflectionHit.hlsli: the same
// cache cells at the same footprint level, created and requested the same way), x exposure.
// Root constants as ReflectionTrace's constant-sky variant: P[0].x = colour UAV, P[1] = { sky rgb, ray length },
// P[3].xyz = sun illuminance, P[4].z = GI cache UAV (raw), P[5].y = specular albedo LUT SRV, P[6], P[7] = RtSceneSrvs;
// frame constants b1 = the reflection view (its clip plane = the mirror, mirror roughness 0).
#define SKY 1
#include "Passes/Reflection/ReflectionHit.hlsli"
#include "Passes/Reflection/Reflection.hlsli"

[shader("raygeneration")]
void PlanarTestViewGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer cache = ResourceDescriptorHeap[P[4].z];
    const GiHeader h = giHeader(cache);
    const float3 target = worldFromDepth(float2(pixel), 1.0);
    const float3 dir = normalize(target - g_cameraPosition);
    const float denom = dot(g_clipPlane.xyz, dir);
    const float tPlane = denom > 0 ? max(-(dot(g_clipPlane.xyz, g_cameraPosition) + g_clipPlane.w) / denom, 0.0) : 0;
    const float3 onMirror = g_cameraPosition + dir * tPlane;
    RayDesc r;
    r.Origin = onMirror + g_clipPlane.xyz * 1e-3;
    r.Direction = dir;
    r.TMin = 0;
    r.TMax = giRayLength();
    const float lobe = reflectionLobeHalfAngle(0, abs(denom));
    float d;
    const float3 radiance = reflHitRadiance(rtScene(), cache, h, r, tan(lobe), pixel.x * 7919u + pixel.y * 104729u, d);
    colour[pixel] = float4(radiance * g_exposure, 1);
}
