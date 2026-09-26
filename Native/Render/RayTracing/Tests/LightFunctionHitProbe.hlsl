// unx-kernel: cs_6_6 main
// LightFunctionHits probe: rtLocalLightSample (HitLocalLights.hlsli) at given points, as R's hits call it.
// P[0] = { points SRV (raw float4: position xyz, footprint width w), result UAV (raw 8 words per point: weight rgb, light,
// wi xyz, valid), count, 0 }; P[6], P[7] = RtSceneSrvs. Frame constants: the main view (g_time).
#include "RayTracing/HitLocalLights.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].z) return;
    ByteAddressBuffer points = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[0].y];
    const float4 q = asfloat(points.Load4(id.x * 16));
    const float u0 = frac(id.x * 0.6180339887), u1 = frac(id.x * 0.7548776662 + 0.25), u2 = frac(id.x * 0.5698402910 + 0.5);
    const RtLocalSample s = rtLocalLightSample(rtSceneSrvs(P[6], P[7]), q.xyz, u0, u1, u2, q.w);
    result.Store4(id.x * 32, uint4(asuint(s.weight), s.light));
    result.Store4(id.x * 32 + 16, uint4(asuint(s.wi), s.valid ? 1u : 0u));
}
