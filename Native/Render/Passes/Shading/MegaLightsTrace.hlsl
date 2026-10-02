// unx-kernel: lib_6_6 main
// m.ml.trace (MegaLights.hlsli): one ray generation thread per light sample texel. A sample that asks for a ray is traced
// from its downsampled pixel's surface point (the key's view depth along the pixel's ray; the key's normal for the
// offset) toward its point on the light (HitLocalLights' sampler: the centre of point and spot lights, the sample's
// (u, v) on area lights); a blocked sample loses its visible bit. Lights that cast no shadow and merged samples ask for
// none. Ray: origin moved by the normal bias to the light's side of the surface, TMin = the bias, TMax = distance - the
// end bias (r.MegaLights.HardwareRayTracing.Bias / NormalBias / EndBias of Unreal, in metres here; the end bias default
// is S's rule instead: what lies within 5 cm of a light - its own fixture - casts no shadow, VsmLocalLight::nearM).
// P[0] = { samples UAV (R32G32_UINT), downsampled key SRV, the dispatch's first row, 0 }: the sample texture goes in bands
//        of rows, each at most 262,144 rays (MegaLights.cpp; DISPATCH_BOUNDS_KO.md)
// P[1] = { downsampled width, height, factor | N << 8, 0 }
// P[2] = { ray bias, normal bias, end bias (m, floats), 0 }
// P[6], P[7] = RtSceneSrvs (RayShaders.hlsli)
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/MegaLightsWorld.hlsli"  // mlSampleVisible

[shader("raygeneration")]
void MegaLightsTraceGen()
{
    const uint2 texel = uint2(DispatchRaysIndex().x, DispatchRaysIndex().y + P[0].z);
    RWTexture2D<uint2> samples = ResourceDescriptorHeap[P[0].x];
    const uint2 stored = samples[texel];
    const MlSample s = mlUnpack(stored);
    if (!s.needsRay || s.light == ML_LIGHT_NONE) return;
    const uint factor = P[1].z & 0xFFu, count = (P[1].z >> 8) & 0xFFu;
    const uint2 ds = texel / mlSampleGrid(count);
    Texture2D<uint2> keys = ResourceDescriptorHeap[P[0].y];
    const uint2 key = keys[ds];
    const float linearZ = asfloat(key.x);
    if (!(linearZ > 0)) return;
    const uint2 pixel = mlFullPixel(ds, factor, g_frameIndex);
    float3 D, Dx, Dy;
    mPixelRay(float2(pixel) + 0.5, D, Dx, Dy);
    const float3 x = g_cameraPosition + D * linearZ;
    const float3 n = octDecode(key.y);
    const bool visible = mlSampleVisible(rtScene(), x, n, s.light, s.uv, asfloat(P[2].x), asfloat(P[2].y), asfloat(P[2].z));
    if (!visible) samples[texel] = uint2(stored.x & 0x7FFFFFFFu, stored.y);
}
