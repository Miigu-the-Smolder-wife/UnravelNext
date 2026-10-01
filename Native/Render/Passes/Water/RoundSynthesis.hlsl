// unx-kernel: cs_6_6 main
// Round basins: the radial synthesis, one group per order m: the order's modes to groupshared, then thread j < 128 forms
// at ring j: H_m(j) = sum_n B_m[j][n] a_mn, Phi likewise, and the radial slope sum_n D_m[j][n] a_mn (D = k J_m'(k r));
// the m = 0 group also writes the centre: eta(0) = sum_n a_0n (J_0(0) = 1; the other orders vanish at r = 0), phi(0).
#include "Passes/Water/RoundPool.hlsli"

groupshared float4 g_modes[ROUND_RINGS];

[numthreads(128, 1, 1)]
void main(uint j : SV_GroupThreadID, uint m : SV_GroupID)
{
    const uint4 o = roundOrder(m);
    ByteAddressBuffer modes = ResourceDescriptorHeap[P[0].x];
    g_modes[j] = j < o.y ? asfloat(modes.Load4(16 * (o.x + j))) : float4(0, 0, 0, 0);
    GroupMemoryBarrierWithGroupSync();
    ByteAddressBuffer synthesis = ResourceDescriptorHeap[P[3].z];
    ByteAddressBuffer slope = ResourceDescriptorHeap[P[3].w];
    float4 hp = 0;
    float2 dr = 0;
    for (uint n = 0; n < o.y; ++n)
    {
        const float4 a = g_modes[n];
        hp += asfloat(synthesis.Load(4 * (o.w + j * o.y + n))) * a;
        dr += asfloat(slope.Load(4 * (o.w + j * o.y + n))) * a.xy;
    }
    RWByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].z];
    const uint at = 48 * (j * ROUND_ORDERS + m);
    spectrum.Store4(at, asuint(hp));
    spectrum.Store4(at + 16, asuint(float4(dr, 0, 0)));
    if (m == 0 && j == 0)
    {
        float4 c = 0;
        for (uint n2 = 0; n2 < o.y; ++n2) c += g_modes[n2];  // J_0(k 0) = 1 for every radial mode of order 0
        RWByteAddressBuffer centre = ResourceDescriptorHeap[P[4].y];
        const float4 old = asfloat(centre.Load4(0));
        centre.Store4(0, asuint(float4(c.x, c.z, old.x, 0)));  // (eta, phi, previous eta, 0)
    }
}
