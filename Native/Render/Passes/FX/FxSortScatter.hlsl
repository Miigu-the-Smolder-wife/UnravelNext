// unx-kernel: cs_6_6 main
// Radix sort, pass scatter: stable and deterministic. A group handles the 4096 keys of its histogram group (the hist
// kernel's partition) in two phases.
//   1. Local order: the keys are ranked as 32 rows of 128. In a row, WaveMatch (SM 6.5) gives each key the lanes of its
//      wave with the same digit: rank in the wave = those below it; the lowest of them adds the wave's count for that digit
//      to group memory. Local position = the digit's start in this group (prefix of the group's digit counts, from the
//      scanned histogram) + keys of the digit in earlier rows + counts of earlier waves in this row + rank in the wave.
//      gs_order[local position] = the key's index in the group's block. The group memory holds [waves][256] 8-bit counts
//      (a wave's count of one digit is at most its lane count, <= 128) for up to 32 waves (128 threads with 4-lane waves,
//      WARP), so the result is the same for every wave size.
//   2. Write-out in local order: thread i of a pass writes the key at local position i to digit base + this group's offset
//      in the digit + (i - the digit's local start). Consecutive threads write consecutive addresses inside each digit's
//      run (about 16 keys per digit per group), so the stores are coalesced instead of one 32 B sector per 4 B key; the
//      keys are re-read from the group's 16 KB block (cache resident).
// Lanes past the key count carry digit 256 (no position) and still take part in every wave operation.
// P[0].x digit shift, P[0].y ping-pong (0: A -> B, 1: B -> A)
#include "Passes/FX/Particles.hlsli"

#define ROW 128u
#define ROWS 32u
#define KEYS 4096u
groupshared uint gs_counts[32 * 64];  // [wave][digit quad]: digit d in byte (d & 3) of word d >> 2
groupshared uint gs_order[KEYS];      // local position -> index in the group's block
groupshared uint gs_running[256];     // keys of each digit in earlier rows
groupshared uint gs_local[256];       // digit start in this group's local order
groupshared uint gs_base[256];        // digit base + this group's offset in the digit

uint countOf(uint wave, uint d) { return (gs_counts[wave * 64u + (d >> 2)] >> ((d & 3u) * 8u)) & 0xFFu; }

[numthreads(128, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    FX_RWBUFFER(uint, counters, g_counters);
    FX_RWBUFFER(uint, hist, g_hist);
    FX_RWBUFFER(uint, keysA, g_keysA);
    FX_RWBUFFER(uint, valsA, g_valsA);
    FX_RWBUFFER(uint, keysB, g_keysB);
    FX_RWBUFFER(uint, valsB, g_valsB);
    const uint n = counters[FX_COUNTER_ALIVE], shift = P[0].x, flip = P[0].y, t = gtid.x, groups = g_numSortGroups;
    const uint lane = WaveGetLaneIndex(), lanes = WaveGetLaneCount(), wave = t / lanes, waves = ROW / lanes;
    const uint base = gid.x * KEYS;
    for (uint i = t; i < 32u * 64u; i += ROW) gs_counts[i] = 0u;
    // this group's count of digit d = next group's offset in d (or the digit total) - its own offset
    for (uint d = t; d < 256u; d += ROW)
    {
        const uint digitBase = hist[groups * 256u + d];
        const uint mine = hist[gid.x * 256u + d];
        const uint total = (d < 255u ? hist[groups * 256u + d + 1u] : n) - digitBase;
        const uint next = gid.x + 1u < groups ? hist[(gid.x + 1u) * 256u + d] : total;
        gs_running[d] = 0u;
        gs_base[d] = digitBase + mine;
        gs_local[d] = next - mine;
    }
    GroupMemoryBarrierWithGroupSync();
    // exclusive prefix of the group's digit counts (256 entries, Hillis-Steele over 2 entries per thread)
    for (uint s = 1u; s < 256u; s <<= 1)
    {
        const uint d0 = t, d1 = t + ROW;
        const uint x0 = d0 >= s ? gs_local[d0 - s] : 0u, x1 = d1 >= s ? gs_local[d1 - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_local[d0] += x0;
        gs_local[d1] += x1;
        GroupMemoryBarrierWithGroupSync();
    }
    // inclusive -> exclusive: gs_running holds 0 now; subtract the own count by shifting (read, sync, write)
    {
        const uint d0 = t, d1 = t + ROW;
        const uint e0 = d0 > 0u ? gs_local[d0 - 1u] : 0u, e1 = gs_local[d1 - 1u];
        GroupMemoryBarrierWithGroupSync();
        gs_local[d0] = e0;
        gs_local[d1] = e1;
    }
    GroupMemoryBarrierWithGroupSync();
    // lanes below mine, as a uint4 mask (wave sizes up to 128)
    uint4 below = uint4(0, 0, 0, 0);
    {
        const uint full = lane >> 5, rem = lane & 31u, partial = rem == 0u ? 0u : (0xFFFFFFFFu >> (32u - rem));
        below.x = full > 0u ? 0xFFFFFFFFu : partial;
        below.y = full > 1u ? 0xFFFFFFFFu : (full == 1u ? partial : 0u);
        below.z = full > 2u ? 0xFFFFFFFFu : (full == 2u ? partial : 0u);
        below.w = full > 3u ? 0xFFFFFFFFu : (full == 3u ? partial : 0u);
    }
    // 1. local order (the 32 keys of this thread are loaded first: 32 independent loads in flight instead of one per row
    // behind the row's barriers)
    uint keys[ROWS];
    [unroll] for (uint r0 = 0u; r0 < ROWS; ++r0)
    {
        const uint at = base + r0 * ROW + t;
        keys[r0] = at < n ? (flip != 0u ? keysB[at] : keysA[at]) : 0u;
    }
    [unroll] for (uint r = 0u; r < ROWS; ++r)
    {
        const uint index = r * ROW + t, at = base + index;
        const bool valid = at < n;
        const uint key = keys[r];
        const uint digit = valid ? (key >> shift) & 255u : 256u;
        const uint4 match = WaveMatch(digit);
        const uint4 lower = match & below;
        const uint rank = countbits(lower.x) + countbits(lower.y) + countbits(lower.z) + countbits(lower.w);
        const uint count = countbits(match.x) + countbits(match.y) + countbits(match.z) + countbits(match.w);
        if (valid && rank == 0u) InterlockedAdd(gs_counts[wave * 64u + (digit >> 2)], count << ((digit & 3u) * 8u));
        GroupMemoryBarrierWithGroupSync();
        if (valid)
        {
            uint before = 0u;
            for (uint w = 0u; w < wave; ++w) before += countOf(w, digit);
            const uint local = gs_local[digit] + gs_running[digit] + before + rank;
            if (local < KEYS) gs_order[local] = index;
        }
        GroupMemoryBarrierWithGroupSync();
        // digit owners: add the row's counts to the running totals, then clear the counts for the next row
        for (uint d = t; d < 256u; d += ROW)
        {
            uint total = 0u;
            for (uint w = 0u; w < waves; ++w) total += countOf(w, d);
            gs_running[d] += total;
        }
        GroupMemoryBarrierWithGroupSync();
        for (uint i = t; i < waves * 64u; i += ROW) gs_counts[i] = 0u;
        GroupMemoryBarrierWithGroupSync();
    }
    // 2. write-out in local order
    const uint valid = n > base ? min(n - base, KEYS) : 0u;
    for (uint i = t; i < valid; i += ROW)
    {
        const uint at = base + gs_order[i];
        const uint key = flip != 0u ? keysB[at] : keysA[at];
        const uint val = flip != 0u ? valsB[at] : valsA[at];
        const uint digit = (key >> shift) & 255u;
        const uint dest = gs_base[digit] + (i - gs_local[digit]);
        if (flip != 0u) { keysA[dest] = key; valsA[dest] = val; }
        else { keysB[dest] = key; valsB[dest] = val; }
    }
}
