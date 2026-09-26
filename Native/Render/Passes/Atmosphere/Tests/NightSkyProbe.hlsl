// unx-kernel: cs_6_6 main
// Test kernel (NightSkyTests): atmosphereCelestial (Celestial.hlsli) over a small image of the sky without the
// atmosphere (transmittance 1): pixel (i, j) has the ray D = centre + (i + 0.5 - W / 2) a tx + (j + 0.5 - H / 2) a ty
// (a = pixel angle, tx, ty unit tangents), Dx = a tx, Dy = a ty.
// P[0] = { celestial record SRV, output UAV (RWStructuredBuffer<float4>), W, H }, P[1] = { centre xyz, a },
// P[2] = { tx xyz, 0 }, P[3] = { ty xyz, 0 }
#include "Passes/Atmosphere/Celestial.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (id.x >= P[0].z || id.y >= P[0].w) return;
    const float3 centre = asfloat(P[1].xyz), tx = asfloat(P[2].xyz), ty = asfloat(P[3].xyz);
    const float a = asfloat(P[1].w);
    const float2 o = (float2(id) + 0.5 - float2(P[0].zw) * 0.5) * a;
    const float3 D = centre + tx * o.x + ty * o.y;
    AtmosphereSrvs atm;
    atm.transmittance = UNX_NONE;
    atm.multiScatter = UNX_NONE;
    atm.skyView = UNX_NONE;
    atm.aerial = UNX_NONE;
    RWStructuredBuffer<float4> output = ResourceDescriptorHeap[P[0].y];
    output[id.y * P[0].z + id.x] = float4(atmosphereCelestial(atm, P[0].x, D, tx * a, ty * a), 0);
}
