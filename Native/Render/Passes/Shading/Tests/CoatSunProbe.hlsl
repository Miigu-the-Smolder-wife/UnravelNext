// unx-kernel: cs_6_6 main
// M shading tests (A9): the clearcoat lobe's sun term as ShadeOpaque forms it - shSunSpecular (f0 = 1, compensation 1) at
// the coat's roughness times shCoatSunWeight - for query rows (E = 1 lux, frame constants = the test view's sun, the coat
// tables). Query (2 x float4): (n, coat roughness), (v, coat index). P[0] = { queries SRV, out UAV, count, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    StructuredBuffer<float4> q = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float4> o = ResourceDescriptorHeap[P[0].y];
    const float4 a = q[2 * i], b = q[2 * i + 1];
    const float3 n = normalize(a.xyz), v = normalize(b.xyz), l0 = normalize(g_sunDirection);
    const float NoV = dot(n, v);
    ModelCoat c;
    c.cover = 1;
    c.roughness = a.w;
    c.coat = uint(b.w);
    c.eta = c.coat == 0 ? 1.5 : 1.33;
    const float alpha = modelAlpha(c.roughness);
    const float3 spec = shSunSpecular(1.0.xxx, c.roughness, alpha, 1.0.xxx, n, v, NoV, l0, 1.0.xxx, 0);
    o[i] = float4(spec * shCoatSunWeight(c, v, l0, NoV, dot(n, l0), false), 0);
}
