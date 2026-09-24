// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2
// Test kernel: evaluates the public atmosphere lookups (Atmosphere.hlsli) at query points.
// MODE 0: sky radiance, query.xyz = world direction. MODE 1: sun disk radiance, query.xyz = world position.
// MODE 2: aerial perspective, query.xy = uv, query.z = view depth; writes inscatter then transmittance (2 float4).
// P[0] = AtmosphereSrvs, P[1].x queries SRV (StructuredBuffer<float4>), P[1].y output UAV (RWStructuredBuffer<float4>),
// P[1].z count
#include "Passes/Atmosphere/Atmosphere.hlsli"

[numthreads(64, 1, 1)]
void main(uint id : SV_DispatchThreadID)
{
    if (id >= P[1].z) return;
    AtmosphereSrvs s;
    s.transmittance = P[0].x;
    s.multiScatter = P[0].y;
    s.skyView = P[0].z;
    s.aerial = P[0].w;
    StructuredBuffer<float4> queries = ResourceDescriptorHeap[P[1].x];
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[1].y];
    const float4 q = queries[id];
#if MODE == 0
    output[id] = float4(atmosphereSkyRadiance(s, normalize(q.xyz)), 0);
#elif MODE == 1
    output[id] = float4(atmosphereSunRadiance(s, q.xyz), 0);
#else
    float3 inscatter, transmittance;
    atmosphereAerial(s, q.xy, q.z, inscatter, transmittance);
    output[2 * id] = float4(inscatter, 0);
    output[2 * id + 1] = float4(transmittance, 0);
#endif
}
