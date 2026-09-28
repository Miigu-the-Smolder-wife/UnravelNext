// unx-kernel: cs_6_6 main
// Sun penumbra filter of the reflection hits S's VSM holds in a mixed region, one thread per hit ReflectionShadeRays
// queued (ReflectionRay.hlsli: the sun queue from its top, the hit's record holding its geometric normal and filter
// reach), dispatched indirectly with the queued count (ReflectionRayArgs stage 1). Adds the hit's sun term x visibility to
// its value, as ReflectionShadow does for the shadow rays. The value is the one the hit shading computed in one step
// before (shadowSunVisibilityAt: the same levels, taps and inputs); the filter's 5 + 16 dependent taps now run in dense
// waves instead of holding every hit-shading wave with one penumbra hit. Root constants: ReflectionRay.hlsli (P[5]).
#define SKY 1  // no sky lookups here
#include "Passes/Reflection/ReflectionRay.hlsli"
#include "Passes/Reflection/ReflectionShade.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    const uint index = (group.y * 65535u + group.x) * 64u + lane;  // 2D dispatch (ReflectionRayArgs)
    RWByteAddressBuffer rays = ResourceDescriptorHeap[P[5].y];
    const uint capacity = rays.Load(4);
    if (index >= min(rays.Load(16), capacity)) return;
    const uint4 a = rays.Load4(reflRaysPenumbraOffset(capacity, index));
    const uint slot = a.w & 0xFFFFFFu;
    const uint4 b = rays.Load4(reflRaysHitOffset(slot));
    const float visibility = shadowSunPenumbraDeferred(reflShadowSrvs(), asfloat(a.xyz), asfloat(b.xyz), a.w >> 24, asfloat(b.w));
    const uint offset = reflRaysValueOffset(capacity, slot);
    const uint4 v = rays.Load4(offset);
    rays.Store4(offset, reflResolveSunValue(v, visibility));
}
