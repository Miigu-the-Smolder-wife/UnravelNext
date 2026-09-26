// unx-kernel: cs_6_6 main
// Core test kernel (A9, v1.76): MaterialModel.hlsli modelEvaluateCoated (n = +z) at deterministic points - directions,
// base and coat from the thread index - through FrameConstants (g_coatTable and the albedo tables). Per point 20 floats
// into a raw UAV: v, l, baseColor, roughness, metallic, cover, roughness_c, coat (uint bits), eta, f (3), pad.
//   P[0].x output UAV (raw), P[0].y point count
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

float3 direction(uint seed)
{
    const float mu = 0.02 + 0.98 * hash01(seed), phi = 6.2831853 * hash01(seed + 7919u);
    const float s = sqrt(1 - mu * mu);
    return float3(s * cos(phi), s * sin(phi), mu);
}

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].y) return;
    const float3 n = float3(0, 0, 1), v = direction(4 * i + 1), l = direction(4 * i + 2);
    ModelSurface s;
    s.cls = MATERIAL_STANDARD;
    s.baseColor = float3(hash01(i * 11 + 3), hash01(i * 13 + 5), hash01(i * 17 + 7));
    s.roughness = hash01(i * 19 + 11);
    s.metallic = (i % 3) == 0 ? 1.0 : 0.0;
    s.specular = 0.5;
    s.transmission = 0;
    ModelCoat c;
    c.cover = 0.25 + 0.75 * hash01(i * 23 + 13);
    c.roughness = hash01(i * 29 + 17);
    c.coat = i % 2;
    c.eta = c.coat == 0 ? 1.5 : 1.33;
    const float3 f = modelEvaluateCoated(s, c, n, v, l);
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].x];
    const uint b = 80 * i;
    o.Store3(b, asuint(v));
    o.Store3(b + 12, asuint(l));
    o.Store3(b + 24, asuint(s.baseColor));
    o.Store4(b + 36, uint4(asuint(s.roughness), asuint(s.metallic), asuint(c.cover), asuint(c.roughness)));
    o.Store2(b + 52, uint2(c.coat, asuint(c.eta)));
    o.Store3(b + 60, asuint(f));
    o.Store(b + 72, 0);
}
