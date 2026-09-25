// unx-kernel: cs_6_6 main
// Compaction, middle step: one group scans the per-block (alive, dying) counts into exclusive offsets, then writes the
// counters (alive, dead, dying), checks alive against the CPU's alive_after (NV_STREAM_STATUS_ALIVE_MISMATCH) and
// fills the tick report (NV_StreamCounters: stream, generation, tick, alive, collision events, status).
// P[0].x = 1: check alive against alive_after (0 for the compaction right after a RESET, which only rebuilds the lists).
// Group-memory scan only (no wave operations).
#include "Passes/FX/Particles.hlsli"

groupshared uint2 gs_partial[1024];

[numthreads(1024, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID)
{
    FX_RWBUFFER(uint2, blockSums, g_blockSums);
    const uint t = gtid.x, per = (g_numScanBlocks + 1023u) / 1024u;
    uint2 local = uint2(0, 0);
    for (uint k = 0u; k < per; ++k)
    {
        const uint at = t * per + k;
        if (at < g_numScanBlocks) local += blockSums[at];
    }
    gs_partial[t] = local;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint2 v = t >= s ? gs_partial[t - s] : uint2(0, 0);
        GroupMemoryBarrierWithGroupSync();
        gs_partial[t] += v;
        GroupMemoryBarrierWithGroupSync();
    }
    uint2 run = gs_partial[t] - local;
    for (uint k = 0u; k < per; ++k)
    {
        const uint at = t * per + k;
        if (at < g_numScanBlocks)
        {
            const uint2 v = blockSums[at];
            blockSums[at] = run;
            run += v;
        }
    }
    if (t == 1023u)
    {
        const uint2 total = gs_partial[1023];
        FX_RWBUFFER(uint, counters, g_counters);
        counters[FX_COUNTER_ALIVE] = total.x;
        counters[FX_COUNTER_DEAD] = g_capacity - total.x;
        counters[FX_COUNTER_DYING] = total.y;
        uint status = counters[FX_COUNTER_STATUS];
        if (P[0].x != 0u && total.x != g_aliveAfter) status |= FX_STATUS_ALIVE_MISMATCH;
        counters[FX_COUNTER_STATUS] = status;
        // NV_StreamCounters (48 B): stream, generation, tick (uint64 each), alive, collision_events, status, reserved
        FX_RWBUFFER(uint, report, g_report);
        report[0] = g_streamLo; report[1] = g_streamHi;
        report[2] = g_generationLo; report[3] = g_generationHi;
        report[4] = g_tickLo; report[5] = g_tickHi;
        report[6] = total.x;
        report[7] = counters[FX_COUNTER_COLLISIONS];
        report[8] = status;
        report[9] = 0u; report[10] = 0u; report[11] = 0u;
        report[12] = total.y;  // module extras after the 48-byte record: dying, dead
        report[13] = g_capacity - total.x;
        report[14] = 0u; report[15] = 0u;
    }
}
