// unx-kernel: lib_6_6 main
// Planar reflection test only: a stand-in for FrameServices::renderView that ray-traces the reflection camera (V/M/S
// are other tracks). Rays start at the view's clip plane (the mirror) like V's clipped raster; the colour is the hit's
// emission x exposure (the test's walls have albedo 0), sky radiance P[0].yzw on a miss.
// P[0] = { colour UAV, sky rgb (float bits) }; P[6], P[7] = RtSceneSrvs; frame constants b1 = the reflection view.
#include "RayTracing/RayShaders.hlsli"

[shader("raygeneration")]
void PlanarTestViewGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].x];
    const float3 target = worldFromDepth(float2(pixel), 1.0);
    RayDesc r;
    r.Origin = g_cameraPosition;
    r.Direction = normalize(target - g_cameraPosition);
    const float denom = dot(g_clipPlane.xyz, r.Direction);
    r.TMin = denom > 0 ? max(-(dot(g_clipPlane.xyz, r.Origin) + g_clipPlane.w) / denom, 0.0) + 1e-4 : 0;
    r.TMax = 1e5;
    const RtSceneSrvs s = rtScene();
    const RtHit h = rtTraceClosest(s, r, RAY_FLAG_NONE, RT_MASK_REFLECTION);
    float3 radiance = asfloat(P[0].yzw);
    if (h.t >= 0)
    {
        const RtSurface surf = rtSurface(s, h, r.Origin, r.Direction);
        const GpuMaterial m = loadMaterial(surf.material);
        radiance = surf.frontFace || (m.classFlags & MATERIAL_TWO_SIDED) != 0 ? m.emissive : 0;
    }
    colour[pixel] = float4(radiance * g_exposure, 1);
}
