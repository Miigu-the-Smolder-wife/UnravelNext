// unx-kernel: lib_6_6 main
// GI tests only: primary visibility by rays (stands in for V's depth and M's G-buffer until those tracks land).
// Writes device depth (reversed Z, R32_FLOAT) and the G-buffer (GBuffer.hlsli) of the closest hit per pixel centre.
// P[0] = { depth UAV, gbuffer UAV, 0, 0 }, P[6], P[7] = RtSceneSrvs; frame constants b1 = the view.
#include "GBuffer.hlsli"
#include "RayTracing/RayShaders.hlsli"

[shader("raygeneration")]
void GiTestPrimaryGen()
{
    const uint2 pixel = DispatchRaysIndex().xy;
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].y];
    const float3 target = worldFromDepth(float2(pixel), 1.0);  // near plane point
    RayDesc r;
    r.Origin = g_cameraPosition;
    r.Direction = normalize(target - g_cameraPosition);
    r.TMin = 0;
    r.TMax = 1e5;
    const RtSceneSrvs s = rtScene();
    const RtHit h = rtTraceClosest(s, r, RAY_FLAG_NONE, RT_MASK_GI | RT_MASK_REFLECTION);  // not the area lights (no body)
    if (h.t < 0)
    {
        depth[pixel] = 0;
        gbuffer[pixel] = uint2(0, 0);
        return;
    }
    const RtSurface surf = rtSurface(s, h, r.Origin, r.Direction);
    const GpuMaterial m = loadMaterial(surf.material);
    const float4 clip = mul(g_viewProj, float4(surf.position, 1));
    depth[pixel] = clip.z / clip.w;
    GBufferSample g;
    g.normal = surf.normal;
    g.baseColor = m.baseColor;
    g.roughness = m.roughness;
    gbuffer[pixel] = encodeGBuffer(g);
}
