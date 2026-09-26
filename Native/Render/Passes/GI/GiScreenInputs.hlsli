// The inputs of M's per-pixel GI cache lookup (ShadeOpaque.hlsl shadeSurface -> screenProbeGatherTile -> ScreenProbes
// pad1 -> giCacheIrradianceScreen, front side), computed as M computes them: position = camera + mPixelRay(pixel + 0.5) x
// linear depth, normal = the G-buffer's normal bent towards the viewer (mNormalTowardsViewer), then its viewer side.
// R's r.gi.screen (GiScreenIrradiance.hlsl) evaluates the lookup there, so M reads the same numbers from a texture.
#ifndef UNX_GI_SCREENINPUTS_HLSLI
#define UNX_GI_SCREENINPUTS_HLSLI
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"

// False for pixels without a surface (reversed Z: depth 0 = far).
bool giScreenInputs(uint2 pixel, float depthValue, uint2 gbPacked, out float3 worldPos, out float3 nv)
{
    worldPos = 0;
    nv = float3(0, 0, 1);
    if (depthValue <= 0) return false;
    const GBufferSample g = decodeGBuffer(gbPacked);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    worldPos = g_cameraPosition + D * linearDepth(depthValue);
    const float3 v = -normalize(D);
    const float3 n = mNormalTowardsViewer(g.normal, v);
    nv = dot(n, v) > 0 ? n : -n;
    return true;
}
#endif
