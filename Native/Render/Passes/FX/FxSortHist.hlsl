// unx-kernel: cs_6_6 main
// LSD radix sort of the alive list by the 24-bit key, pass histogram: 8-bit digit at shift P[0].x, ping-pong P[0].y
// (0: keysA -> keysB), written to the pass's histogram region P[0].z. (Counting the next pass inside the compaction and
// the scatters with global atomics measured slower: +36 us of atomics against 3 x 6 us of these passes.) 256 threads x 16 keys per group; group-major layout hist[group * 256 + digit] so the scan reads
// whole 1 KB rows (the reference kernel's corrected layout, design 3.3: 400 -> 69 us).
#include "Passes/FX/Particles.hlsli"

groupshared uint gs_hist[256];

[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    FX_RWBUFFER(uint, counters, g_counters);
    FX_RWBUFFER(uint, keysA, g_keysA);
    FX_RWBUFFER(uint, keysB, g_keysB);
    FX_RWBUFFER(uint, hist, g_hist);
    const uint n = counters[FX_COUNTER_ALIVE], shift = P[0].x;
    gs_hist[gtid.x] = 0u;
    GroupMemoryBarrierWithGroupSync();
    const uint base = gid.x * 4096u;
    for (uint r = 0u; r < 16u; ++r)
    {
        const uint at = base + r * 256u + gtid.x;
        if (at < n)
        {
            const uint key = P[0].y != 0u ? keysB[at] : keysA[at];
            InterlockedAdd(gs_hist[(key >> shift) & 255u], 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    hist[P[0].z + gid.x * 256u + gtid.x] = gs_hist[gtid.x];
}
