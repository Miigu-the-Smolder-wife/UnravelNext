// unx-kernel: cs_6_6 main
// Radix sort, pass scatter: stable and deterministic without wave operations. Each group walks its 16 rows of 256 keys;
// in a row, a key's rank among the earlier keys of the same digit is counted from the row's digits packed four per word
// in group memory (exact zero-byte count of word ^ digit x 0x01010101), so the result is the same for every wave size.
// dest = digit base + this group's offset in the digit + keys of the digit in earlier rows + rank in the row.
// P[0].x digit shift, P[0].y ping-pong (0: A -> B, 1: B -> A)
#include "Passes/FX/Particles.hlsli"

groupshared uint gs_digits[64];
groupshared uint gs_count[256];
groupshared uint gs_running[256];
groupshared uint gs_base[256];

// Number of zero bytes of x (exact: no borrow between bytes).
uint zeroBytes(uint x)
{
    const uint y = (x & 0x7F7F7F7Fu) + 0x7F7F7F7Fu;
    return countbits(~(y | x | 0x7F7F7F7Fu));
}

[numthreads(256, 1, 1)]
void main(uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    FX_RWBUFFER(uint, counters, g_counters);
    FX_RWBUFFER(uint, hist, g_hist);
    FX_RWBUFFER(uint, keysA, g_keysA);
    FX_RWBUFFER(uint, valsA, g_valsA);
    FX_RWBUFFER(uint, keysB, g_keysB);
    FX_RWBUFFER(uint, valsB, g_valsB);
    const uint n = counters[FX_COUNTER_ALIVE], shift = P[0].x, flip = P[0].y, t = gtid.x;
    gs_running[t] = 0u;
    gs_base[t] = hist[g_numSortGroups * 256u + t] + hist[gid.x * 256u + t];
    const uint base = gid.x * 4096u;
    for (uint r = 0u; r < 16u; ++r)
    {
        const uint at = base + r * 256u + t;
        const bool valid = at < n;  // a prefix of the row's threads
        uint key = 0u, val = 0u;
        if (valid) { key = flip != 0u ? keysB[at] : keysA[at]; val = flip != 0u ? valsB[at] : valsA[at]; }
        const uint digit = (key >> shift) & 255u;
        if (t < 64u) gs_digits[t] = 0u;
        gs_count[t] = 0u;
        GroupMemoryBarrierWithGroupSync();
        if (valid)
        {
            InterlockedOr(gs_digits[t >> 2], digit << ((t & 3u) * 8u));
            InterlockedAdd(gs_count[digit], 1u);
        }
        GroupMemoryBarrierWithGroupSync();
        if (valid)
        {
            const uint pattern = digit * 0x01010101u;
            uint rank = 0u;
            const uint words = t >> 2;
            for (uint w = 0u; w < words; ++w) rank += zeroBytes(gs_digits[w] ^ pattern);
            const uint lower = (t & 3u) * 8u;  // bytes of earlier threads in my word
            const uint mask = lower == 0u ? 0u : (0xFFFFFFFFu >> (32u - lower));
            rank += zeroBytes((gs_digits[words] ^ pattern) | ~mask);
            const uint dest = gs_base[digit] + gs_running[digit] + rank;
            if (flip != 0u) { keysA[dest] = key; valsA[dest] = val; }
            else { keysB[dest] = key; valsB[dest] = val; }
        }
        GroupMemoryBarrierWithGroupSync();
        gs_running[t] += gs_count[t];
        GroupMemoryBarrierWithGroupSync();
    }
}
