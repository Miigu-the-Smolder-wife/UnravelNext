// unx-kernel: cs_6_6 main
// Core test kernel (Subsurface class, stage A): MaterialModel.hlsli modelEvaluateSubsurface (n = +z) at deterministic
// points - the viewer above the surface, the light on either side of it, the surface from the thread index, the lobes from
// the scene's material records P[0].z .. P[0].z + P[0].w - 1 (modelSubsurfaceOf: the record's class slots) - through
// FrameConstants (the albedo tables). Per point 20 floats into a raw UAV: v, l, baseColor, roughness, metallic,
// transmission, mix, r_0, r_1, r_a, f (3), pad.
//   P[0].x output UAV (raw), P[0].y point count, P[0].z first material, P[0].w material count
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Common/MaterialModel.hlsli"

float hash01(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x >> 8) * (1.0 / 16777216.0);
}

float3 direction(float mu, uint seed)
{
    const float phi = 6.2831853 * hash01(seed);
    const float s = sqrt(1 - mu * mu);
    return float3(s * cos(phi), s * sin(phi), mu);
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    const float3 n = float3(0, 0, 1);
    const float3 v = direction(0.02 + 0.98 * hash01(4 * i + 1), 4 * i + 2);
    // the light: three of four points on the viewer's side, the fourth across the surface (the thin parts' term)
    const float muL = (0.02 + 0.98 * hash01(4 * i + 3)) * ((i % 4) == 3 ? -1.0 : 1.0);
    const float3 l = direction(muL, 4 * i + 4);
    ModelSurface s;
    s.cls = MATERIAL_SUBSURFACE;
    s.baseColor = float3(hash01(i * 11 + 3), hash01(i * 13 + 5), hash01(i * 17 + 7));
    s.roughness = 0.05 + 0.95 * hash01(i * 19 + 11);
    s.metallic = (i % 5) == 0 ? 1.0 : 0.0;
    s.specular = 0.5;
    s.transmission = (i % 3) == 0 ? 0.0 : hash01(i * 23 + 13);
    const GpuMaterial m = loadMaterial(P[0].z + i % P[0].w);
    const ModelSubsurface k = modelSubsurfaceOf(m, s.roughness);
    const float3 f = modelEvaluateSubsurface(s, k, n, v, l);
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].x];
    const uint b = 80 * i;
    o.Store3(b, asuint(v));
    o.Store3(b + 12, asuint(l));
    o.Store3(b + 24, asuint(s.baseColor));
    o.Store3(b + 36, uint3(asuint(s.roughness), asuint(s.metallic), asuint(s.transmission)));
    o.Store4(b + 48, uint4(asuint(k.mix), asuint(k.roughness0), asuint(k.roughness1), asuint(k.roughness)));
    o.Store3(b + 64, asuint(f));
    o.Store(b + 76, 0);
}
