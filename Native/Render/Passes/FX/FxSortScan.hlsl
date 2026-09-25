// unx-kernel: cs_6_6 main
// Radix sort, pass scan: one group; thread d scans column d (digit d over the groups) with 16 independent coalesced row
// loads in flight, then a 256-wide scan of the digit totals gives the digit bases: hist[groups * 256 + d]. The pass's
// histogram region starts at P[0].z (counted by the compaction or the previous pass's scatter).
#include "Passes/FX/Particles.hlsli"

groupshared uint gs_total[256];

[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID)
{
    FX_RWBUFFER(uint, hist, g_hist);
    const uint d = gtid.x, groups = g_numSortGroups, region = P[0].z;
    uint run = 0u;
    for (uint g0 = 0u; g0 < groups; g0 += 16u)
    {
        uint v[16];
        [unroll] for (uint k = 0u; k < 16u; ++k) v[k] = g0 + k < groups ? hist[region + (g0 + k) * 256u + d] : 0u;
        [unroll] for (uint k = 0u; k < 16u; ++k)
            if (g0 + k < groups) { hist[region + (g0 + k) * 256u + d] = run; run += v[k]; }
    }
    gs_total[d] = run;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 256u; s <<= 1)
    {
        const uint x = d >= s ? gs_total[d - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_total[d] += x;
        GroupMemoryBarrierWithGroupSync();
    }
    hist[region + groups * 256u + d] = gs_total[d] - run;
}
