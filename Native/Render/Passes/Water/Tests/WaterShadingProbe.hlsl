// unx-kernel: cs_6_6 main
// Track W water shading probe: thread i evaluates WaterShading.hlsli at incidence angle i x 0.5 degrees (0..90):
// out[12 i ..] = R from air, R from water, refracted from air (x, y, valid), refracted from water (x, y, valid), the
// cos and sin of the incidence the GPU used, 0, 0.
// P[0].x output UAV (raw), P[0].y angle count
#include "Bindless.hlsli"
#include "../WaterShading.hlsli"
#include "../WaterFragment.hlsli"  // compiled here so the interface file is always built
#include "../WaterLinear.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint thread : SV_GroupThreadID)
{
    const uint i = waterLinear(group, thread, 64);
    if (i >= P[0].y) return;
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].x];
    float theta = radians(0.5 * i);
    float3 n = float3(0, 1, 0), v = float3(sin(theta), cos(theta), 0);
    float3 tAir, tWater;
    bool okAir = waterRefract(v, n, 1.0 / kWaterIor, tAir), okWater = waterRefract(v, n, kWaterIor, tWater);
    float out12[12] = { waterFresnel(v.y, 1.0 / kWaterIor), waterFresnel(v.y, kWaterIor), tAir.x, tAir.y, okAir ? 1.0 : 0.0, tWater.x, tWater.y, okWater ? 1.0 : 0.0, v.y, v.x, waterFragmentRadiance(uint2(i, 0), 1.0, n, 0, 0, 0).x, 0 };
    [unroll] for (uint k = 0; k < 12; ++k) o.Store(4 * (12 * i + k), asuint(out12[k]));
}
