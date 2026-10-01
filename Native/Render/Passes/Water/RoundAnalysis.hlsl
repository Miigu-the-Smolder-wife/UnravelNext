// unx-kernel: cs_6_6 main
// Round basins: the radial least-squares projection of the increment, one group per order m: the ring values H_m(j),
// Phi_m(j) (RoundRows) to groupshared, then thread n < count forms a_mn = sum_j F_m[n][j] H_m(j) (and Phi): the
// increment's complex mode amplitudes (RoundEvolve adds them). F_m = (B^T W B)^-1 B^T W (host double, RoundTables).
#include "Passes/Water/RoundPool.hlsli"

groupshared float4 g_ring[ROUND_RINGS];

[numthreads(128, 1, 1)]
void main(uint n : SV_GroupThreadID, uint m : SV_GroupID)
{
    ByteAddressBuffer spectrum = ResourceDescriptorHeap[P[0].z];
    g_ring[n] = asfloat(spectrum.Load4(48 * (n * ROUND_ORDERS + m)));  // ring n, order m: (H, Phi)
    GroupMemoryBarrierWithGroupSync();
    const uint4 o = roundOrder(m);
    if (n >= o.y) return;
    ByteAddressBuffer analysis = ResourceDescriptorHeap[P[3].y];
    float4 a = 0;
    for (uint j = 0; j < ROUND_RINGS; ++j) a += asfloat(analysis.Load(4 * (o.z + n * ROUND_RINGS + j))) * g_ring[j];
    RWByteAddressBuffer increments = ResourceDescriptorHeap[P[0].y];
    increments.Store4(16 * (o.x + n), asuint(a));
}
