// unx-kernel: cs_6_6 main
// M shading tests: AreaQuadrature.hlsli (sheen and anisotropic area lights) for one light per query, the shading point at
// the origin. Query (5 x float4): (n, sheen roughness), (v, f0 grey), (t, alpha_t), (b, alpha_b), (compensation, 0, 0, 0).
// Out: (int sheen lobe x cos, int anisotropic f_s cos (grey), 0, 0) over the light.
// P[0] = { lights SRV (GpuLight), queries SRV, out UAV, count }
#include "Bindless.hlsli"
#include "Passes/Shading/AreaQuadrature.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].w) return;
    StructuredBuffer<GpuLight> lights = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<float4> q = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<float4> o = ResourceDescriptorHeap[P[0].z];
    const GpuLight l = lights[i];
    const float4 a = q[5 * i], b = q[5 * i + 1], t = q[5 * i + 2], bt = q[5 * i + 3], c = q[5 * i + 4];
    const float3 n = normalize(a.xyz), v = normalize(b.xyz);
    const float3x3 frame = float3x3(normalize(t.xyz), normalize(bt.xyz), n);
    const float sheen = shAreaSheen(l, l.position, frame, v, a.w);
    const float3 aniso = shAreaAniso(l, l.position, normalize(t.xyz), normalize(bt.xyz), n, v, float2(t.w, bt.w), b.www, c.xxx);
    o[i] = float4(sheen, aniso.x, 0, 0);
}
