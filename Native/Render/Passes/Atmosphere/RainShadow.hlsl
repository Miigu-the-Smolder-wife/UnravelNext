// unx-kernel: lib_6_6 main
// S's rain shadow map (WeatherField.hlsli): one ray per texel from the top plane along the rain direction through the ray
// scene (opaque and alpha-tested geometry); the texel keeps the distance to the first hit (65504 = open to the ground's
// end of the ray). P[0] = { map UAV (RWTexture2D<float>, R32F), texels per side, weather record SRV, ray length (float) };
// P[6], P[7] = RtSceneSrvs. Recorded by R after its ray scene record (RayTracingTrack.cpp).
#include "RayTracing/RayShaders.hlsli"
#include "Passes/Atmosphere/WeatherField.hlsli"

[shader("raygeneration")]
void RainShadowGen()
{
    const uint2 id = DispatchRaysIndex().xy;
    if (any(id >= P[0].y)) return;
    const WeatherRecord w = weatherLoad(P[0].z);
    RayDesc r;
    r.Origin = w.mapOrigin + w.axisU * ((id.x + 0.5) * w.cell) + w.axisV * ((id.y + 0.5) * w.cell);
    r.Direction = w.rainDirection;
    r.TMin = 0;
    r.TMax = asfloat(P[0].w);
    const RtHit hit = rtTraceClosest(rtScene(), r, RAY_FLAG_NONE, RT_MASK_GI | RT_MASK_REFLECTION);
    RWTexture2D<float> map = ResourceDescriptorHeap[P[0].x];
    map[id] = hit.t >= 0 ? hit.t : 65504.0;
}
