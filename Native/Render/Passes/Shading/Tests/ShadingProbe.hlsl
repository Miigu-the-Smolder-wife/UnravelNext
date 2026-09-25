// unx-kernel: cs_6_6 main
// M shading tests: evaluates shSunSpecular for query rows (E = 1 lux, frame constants = the test view's sun).
// Query (3 x float4): (n, alpha), (v, roughness), (f0, pixel angle). P[0] = { queries SRV, out UAV, count, 0 } (LUT: frame constant)
#include "Bindless.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    StructuredBuffer<float4> q = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float4> o = ResourceDescriptorHeap[P[0].y];
    const float4 a = q[3 * i], b = q[3 * i + 1], c = q[3 * i + 2];
    const float3 n = normalize(a.xyz), v = normalize(b.xyz);
    const float NoV = dot(n, v);
    const float e = modelDirectionalAlbedo(NoV, b.w);
    const float3 compensation = 1 + c.xyz * (1 / e - 1);
    o[i] = float4(shSunSpecular(c.xyz, b.w, a.w, compensation, n, v, NoV, normalize(g_sunDirection), 1.0.xxx, c.w), 0);
}
