// unx-kernel: cs_6_6 main
// Emissive surfaces as area lights, the receiver side (RENDERER_REDESIGN_V2 14.1b, L2b; owner A): per pixel of the
// view, the diffuse irradiance from the emissive quadtrees (EmissiveLights.hlsli, selected nodes as horizon-clipped
// Lambert polygons) on the viewer's side of the G-buffer normal, written exposed (x g_exposure) as RGBA16F for
// ShadeOpaque, which multiplies it by the diffuse BRDF. Its own kernel: the traversal inside ShadeOpaque took the
// kernel over the DXIL limit, and the tile-corner term of L2 lands in the same texture later. Sky pixels (device depth
// 0: reversed-Z infinite projection) get 0.
// P[0] = { depth SRV, G-buffer SRV, emissive lights buffer SRV (EmissiveLights.h image), irradiance UAV (RGBA16F) }
#include "Bindless.hlsli"
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Lights/EmissiveLights.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (pixel.x >= g_viewWidth || pixel.y >= g_viewHeight) return;
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    Texture2D<float> depthTex = ResourceDescriptorHeap[P[0].x];
    const float depthValue = depthTex[pixel];
    if (depthValue <= 0)
    {
        output[pixel] = 0;
        return;
    }
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].y];
    const GBufferSample g = decodeGBuffer(gbuffer[pixel]);
    const float linearZ = linearDepth(depthValue);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 offset = D * linearZ;  // camera-relative position
    const float3 v = -normalize(D);
    const float3 n = mNormalTowardsViewer(g.normal, v);
    const float NoV = dot(n, v);
    float3 E = 0;
    if (NoV > 0)
    {
        const float epsAbs = 1e-3 * SH_PI / max(g_exposure, 1e-12);  // half a display code at this frame's exposure
        uint evaluated;
        E = emissiveLightsIrradiance(P[0].z, offset, shShadingFrame(n, v, NoV), epsAbs, evaluated);
    }
    output[pixel] = float4(E * g_exposure, 0);
}
