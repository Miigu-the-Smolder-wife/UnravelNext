// unx-kernel: cs_6_6 main
// M shading tests (A9 sheen, MATERIAL_LAYERS 1.4): the HLSL mirror of the sheen model for query rows - the lobe
// (modelSheenLobe), the directional albedo E_sh and the 4-point sun term ShadeOpaque LAYERED=2 uses (modelSheenSun) - from
// the sheen table in g_coatTable. Query (3 x float4): (n, r), (v, 0), (l, rho). Out: (lobe, E_sh(n.v), sun, 0).
// P[0] = { queries SRV, out UAV, count, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].z) return;
    StructuredBuffer<float4> q = ResourceDescriptorHeap[P[0].x];
    RWStructuredBuffer<float4> o = ResourceDescriptorHeap[P[0].y];
    const float4 a = q[3 * i], b = q[3 * i + 1], c = q[3 * i + 2];
    const float3 n = normalize(a.xyz), v = normalize(b.xyz), l = normalize(c.xyz);
    o[i] = float4(modelSheenLobe(a.w, n, v, l), modelSheenAlbedo(max(dot(n, v), 1e-4), a.w), modelSheenSun(a.w, n, v, l, c.w), 0);
}
