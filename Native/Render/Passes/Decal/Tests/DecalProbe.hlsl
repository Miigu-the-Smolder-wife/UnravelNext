// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Decal tests (DecalTests.cpp): an analytic floor y = P[1].y (world; the camera sits at the origin of the frame).
//   MODE 0: its device depth (reversed Z, near / view depth; 0 = sky) into P[0].y (RWTexture2D<float>)
//   MODE 1: per floor pixel, decalApply over the start material (base 0.5, roughness 0.5, metallic 0, normal +Y) with the
//           tile lists and frames P[0].z / P[0].w (materials: constants only) -> P[0].y RWStructuredBuffer<float4>, two per
//           pixel: (base colour, roughness), (normal, metallic); sky pixels (0, 0, 0, -1)
// P[1].x floor height as float bits, P[1].y the floor's instance id; frame constants b1 = the view.
#include "Passes/Decal/Decal.hlsli"

float3 probeRay(float2 pixel)  // camera-relative, unit view depth
{
    const float2 ndc = float2(pixel.x / g_viewWidth * 2 - 1, 1 - pixel.y / g_viewHeight * 2);
    const float4 p = mul(g_invViewProj, float4(ndc, 1, 1));
    return (p.xyz / p.w - g_cameraPosition) / g_nearPlane;
}
bool floorHit(float2 pixel, float height, out float3 p, out float t)
{
    const float3 r = probeRay(pixel);
    t = (height - g_cameraPosition.y) / r.y;
    p = r * t;
    return r.y < 0 && t > 0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= uint2(g_viewWidth, g_viewHeight))) return;
    const float height = asfloat(P[1].x);
    const float2 centre = float2(id.xy) + 0.5f;
    float3 p;
    float t;
    const bool hit = floorHit(centre, height, p, t);
#if MODE == 0
    RWTexture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    depth[id.xy] = hit ? g_nearPlane / t : 0.0f;
#else
    RWStructuredBuffer<float4> results = ResourceDescriptorHeap[P[0].y];
    const uint i = 2 * (id.y * g_viewWidth + id.x);
    if (!hit)
    {
        results[i] = float4(0, 0, 0, -1);
        results[i + 1] = 0;
        return;
    }
    float3 px, py;
    float tx, ty;
    floorHit(centre + float2(1, 0), height, px, tx);
    floorHit(centre + float2(0, 1), height, py, ty);
    DecalSurface s;
    s.position = p;
    s.dpdx = px - p;
    s.dpdy = py - p;
    s.geometricNormal = float3(0, 1, 0);
    s.instance = P[1].y;
    s.geometricVariance = 0;
    DecalMaterial m;
    m.baseColor = 0.5f;
    m.roughness = 0.5f;
    m.metallic = 0;
    m.normal = float3(0, 1, 0);
    m.variance = 0;
    m.emissive = 0;
    DecalContext c;
    c.frames = P[0].w;
    c.tiles = P[0].z;
    c.materialTable = DECAL_NONE;
    decalApply(c, id.xy, s, m);
    results[i] = float4(m.baseColor, m.roughness);
    results[i + 1] = float4(m.normal, m.metallic);
#endif
}
