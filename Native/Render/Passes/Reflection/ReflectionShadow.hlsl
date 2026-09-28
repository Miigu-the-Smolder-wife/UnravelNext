// unx-kernel: lib_6_6 main
// Sun visibility of the reflection hits S's VSM does not hold (off-screen), one shadow ray each toward a point of the
// solar disk (ARCHITECTURE 2.6 revision 1), dispatched indirectly with the queued count (ReflectionShadeRays). Adds the
// hit's sun term x visibility to its value. Root constants: ReflectionRay.hlsli (P[1].w ray length, P[5], P[6], P[7]).
#define SKY 1  // no sky lookups here (giSunDirection and the ray length only)
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "Passes/Reflection/ReflectionHit.hlsli"

[shader("raygeneration")]
void ReflectionShadowGen()
{
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    const uint capacity = rays.Load(4);
    const uint4 entry = rays.Load4(reflRaysShadowOffset(capacity, DispatchRaysIndex().x));
    const uint slot = entry.w;
    const uint owner = rays.Load(reflRaysJobOffset(capacity, slot));
    const float visibility = reflSunVisibility(rtScene(), asfloat(entry.xyz), reflSunSeed(reflJobSeed(owner & 0x0FFFFFFFu), owner >> 28));
    const uint offset = reflRaysValueOffset(capacity, slot);
    const uint4 v = rays.Load4(offset);
    const float3 radiance = float3(f16tof32(v.x), f16tof32(v.x >> 16), f16tof32(v.y)) + visibility * float3(f16tof32(v.z), f16tof32(v.z >> 16), f16tof32(v.w));
    rays.Store4(offset, uint4(f32tof16(radiance.r) | (f32tof16(radiance.g) << 16), f32tof16(radiance.b) | (v.y & 0xFFFF0000u), 0, v.w & 0xFFFF0000u));
}
