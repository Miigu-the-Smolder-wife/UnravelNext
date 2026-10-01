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
    const uint4 entry = rays.Load4(reflRaysShadowOffset(capacity, DispatchRaysIndex().x + reflBand() * REFL_BAND));
    const uint slot = entry.w;
    const uint owner = rays.Load(reflRaysJobOffset(capacity, slot));
    const float visibility = reflSunVisibility(rtScene(), asfloat(entry.xyz), reflSunSeed(reflJobSeed(owner & 0x0FFFFFFFu), owner >> 28));
    const uint offset = reflRaysValueOffset(capacity, slot);
    const uint4 v = rays.Load4(offset);
    rays.Store4(offset, reflResolveSunValue(v, visibility));
}
