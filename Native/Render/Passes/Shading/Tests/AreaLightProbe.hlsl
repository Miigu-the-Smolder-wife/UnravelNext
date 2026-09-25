// unx-kernel: cs_6_6 main
// M shading tests: area-light integrals (AreaLight.hlsli) for one light per query, the shading point at the origin
// (light positions are relative to it). Query (2 x float4): (n, roughness), (v, f0).
// Out: (I front diffuse, I specular LTC, I back diffuse, shSpecularAlbedo(f0)).
// P[0] = { lights SRV (GpuLight), queries SRV, out UAV, count }, P[1] = { LTC table SRV, specular albedo LUT SRV }
#include "Bindless.hlsli"
#include "Passes/Shading/AreaLight.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].w) return;
    StructuredBuffer<GpuLight> lights = ResourceDescriptorHeap[P[0].x];
    StructuredBuffer<float4> q = ResourceDescriptorHeap[P[0].y];
    RWStructuredBuffer<float4> o = ResourceDescriptorHeap[P[0].z];
    const GpuLight l = lights[i];
    const float4 a = q[2 * i], b = q[2 * i + 1];
    const float3 n = normalize(a.xyz), v = normalize(b.xyz);
    const float NoV = dot(n, v);
    const float3x3 frame = shShadingFrame(n, v, NoV);
    const float3x3 back = float3x3(frame[0], -frame[1], -frame[2]);
    const float3x3 spec = mul(shLtcInverse(P[1].x, max(NoV, 1e-4), a.w), frame);
    o[i] = float4(shAreaIntegral(l, l.position, frame), shAreaIntegral(l, l.position, spec), shAreaIntegral(l, l.position, back),
                  shSpecularAlbedo(P[1].y, b.www, max(NoV, 1e-4), a.w).x);
}
