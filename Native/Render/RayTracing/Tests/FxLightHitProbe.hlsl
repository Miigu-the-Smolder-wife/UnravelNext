// unx-kernel: cs_6_6 main
// FxLightHits probe. Mode 0: the test's FX light writer - words of gpu::Light records from a raw upload into the scene
// light buffer's tail through the core's UAVs (GpuScene::fxLightRange), and the count word.
//   P[0] = { source SRV (raw), light UAV (raw), count UAV (raw), first light }, P[1] = { 0, lights F, record bytes, 0 }
// Mode 1: at each point, rtLocalLightSample (HitLocalLights.hlsli) over K stratified choices u0 = (k + 0.5) / K (point
// lights: delta, so u1 and u2 do not matter): the mean weight, whose expectation is the sum of every light's radiance at
// the point, and the share of choices that took an FX light.
//   P[0] = { points SRV (raw float4), result UAV (raw 4 words per point: mean weight rgb, FX share), count, K },
//   P[1] = { 1, 0, 0, 0 }; P[6], P[7] = RtSceneSrvs; frame constants of the main view.
#include "RayTracing/HitLocalLights.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (P[1].x == 0)
    {
        const uint words = P[1].y * (P[1].z / 4);
        if (id.x == 0)
        {
            RWByteAddressBuffer count = ResourceDescriptorHeap[P[0].z];
            count.Store(0, P[1].y);
        }
        if (id.x >= words) return;
        ByteAddressBuffer src = ResourceDescriptorHeap[P[0].x];
        RWByteAddressBuffer lights = ResourceDescriptorHeap[P[0].y];
        lights.Store(P[0].w * P[1].z + id.x * 4, src.Load(id.x * 4));
        return;
    }
    if (id.x >= P[0].z) return;
    ByteAddressBuffer points = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[0].y];
    const float3 x = asfloat(points.Load3(id.x * 16));
    const uint K = P[0].w;
    float3 sum = 0;
    uint fx = 0;
    [loop] for (uint k = 0; k < K; ++k)
    {
        const RtLocalSample s = rtLocalLightSample(rtSceneSrvs(P[6], P[7]), x, (k + 0.5) / K, 0.5, 0.5, 0);
        if (!s.valid) continue;
        sum += s.weight;
        fx += s.light >= g_lightCount ? 1u : 0u;
    }
    result.Store4(id.x * 16, uint4(asuint(sum / K), asuint((float)fx / K)));
}
