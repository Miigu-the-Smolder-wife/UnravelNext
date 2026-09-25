// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1,2
// Test kernel: evaluates the public atmosphere lookups (Atmosphere.hlsli) at query points.
// MODE 0: sky radiance, query.xyz = world direction. MODE 1: sun disk radiance, query.xyz = world position.
// MODE 2: air of the main view (atmosphereAirView), query.xy = uv, query.z = view depth; writes inscatter, transmittance
// and sun illuminance (3 float4). P[0].w = the air volume (FrameResources::aerialPerspective, built by froxels()).
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
    float3 inscatter, transmittance, sunIlluminance, inscatter2, transmittance2;
    atmosphereAirView(s, q.xy, q.z, inscatter, transmittance, sunIlluminance);
    atmosphereAerial(s, q.xy, q.z, inscatter2, transmittance2);  // same fetches: must agree bit for bit
    // Same air (the two inline differently: compare to 1e-5 relative).
    const bool same = all(abs(inscatter - inscatter2) <= 1e-5 * abs(inscatter) + 1e-30) && all(abs(transmittance - transmittance2) <= 1e-5 * transmittance + 1e-30);
    output[3 * id] = float4(inscatter, same ? 0 : 1);
    // Diagnostics (.w): the ray's distance to the model surface crossing and to the queried depth (airViewLookup).
    Texture2D<float4> pt = ResourceDescriptorHeap[s.multiScatter];
    uint pw, ph;
    pt.GetDimensions(pw, ph);
    const float bottom = pt.Load(int3(0, ph - 1, 0)).x;
    const float4 nq = mul(g_invViewProj, float4(q.x * 2 - 1, 1 - q.y * 2, 1, 1));
    const float3 dir = normalize(nq.xyz / nq.w - g_cameraPosition);
    const float3 o = g_cameraPosition;
    const float h2 = dot(o, o) + 2 * bottom * o.y, b = dot(o, dir) + bottom * dir.y, disc = b * b - h2;
    const float kink = h2 <= 0 ? 0 : (disc >= 0 && b < 0 ? h2 / (-b + sqrt(disc)) : -1);
    output[3 * id + 1] = float4(transmittance, kink);
    output[3 * id + 2] = float4(sunIlluminance, q.z / max(dot(dir, airViewForward()), 1e-4));
#endif
}
