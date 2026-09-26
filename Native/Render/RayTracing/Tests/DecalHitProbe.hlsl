// unx-kernel: cs_6_6 main
// DecalHits test probe: rtHitDecals (HitDecals.hlsli) at given surface points, as R's hits call it. Each point is an
// upward-facing surface (normal +y) of a scene instance with material 0; the result is the material after the decals.
// P[0] = { points SRV (raw: float3 world position, uint scene instance per point), result UAV (raw: base colour xyz,
// roughness, metallic, 0, 0, 0 per point), count, footprint (float bits) }; P[6], P[7] = RtSceneSrvs.
// Frame constants: the main view (the decal frames' camera).
#include "RayTracing/HitDecals.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].z) return;
    ByteAddressBuffer points = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer result = ResourceDescriptorHeap[P[0].y];
    const uint4 q = points.Load4(id.x * 16);
    RtSurface s = (RtSurface)0;
    s.position = asfloat(q.xyz);
    s.normal = s.geometricNormal = float3(0, 1, 0);
    s.sceneInstance = q.w;
    s.frontFace = true;
    GpuMaterial m = loadMaterial(0);
    rtHitDecals(rtSceneSrvs(P[6], P[7]), s, asfloat(P[0].w), m);
    result.Store4(id.x * 32, asuint(float4(m.baseColor, m.roughness)));
    result.Store4(id.x * 32 + 16, asuint(float4(m.metallic, 0, 0, 0)));
}
