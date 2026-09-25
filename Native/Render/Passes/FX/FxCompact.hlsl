// unx-kernel: cs_6_6 main
// unx-variants: SCATTER=0,1
// Compaction (stream step 4), 1024 slots per group, deterministic (prefix sums, no atomics):
//   SCATTER=0: per block the number of alive slots and of slots that died in this tick -> blockSums[group] (uint2)
//   SCATTER=1: with the exclusive block offsets (FxScanSums): alive list + sort keys/values (slot order), dead list
//              (slot order), dying list (slot order); a dying slot becomes dead.
// Wave intrinsics give the in-wave ranks; the per-wave totals live in group memory sized for the smallest wave (4 lanes),
// so the result does not depend on the wave size (WARP runs 4-lane waves).
#include "Passes/FX/Particles.hlsli"

groupshared uint2 gs_wave[256];

[numthreads(1024, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
    FX_RWBUFFER(uint, alive, g_alive);
    FX_RWBUFFER(uint2, blockSums, g_blockSums);
    const uint i = id.x;
    const uint state = i < g_capacity ? alive[i] : FX_SLOT_DEAD;
    const bool live = state == FX_SLOT_ALIVE, dying = state == FX_SLOT_DYING;
    const uint lane = WaveGetLaneIndex(), lanes = WaveGetLaneCount(), wave = gtid.x / lanes, waves = 1024u / lanes;
    const uint2 inWave = uint2(WavePrefixCountBits(live), WavePrefixCountBits(dying));
    const uint2 waveCount = uint2(WaveActiveCountBits(live), WaveActiveCountBits(dying));
    if (lane == 0u) gs_wave[wave] = waveCount;
    GroupMemoryBarrierWithGroupSync();
#if !SCATTER
    if (gtid.x == 0u)
    {
        uint2 total = uint2(0, 0);
        for (uint w = 0u; w < waves; ++w) total += gs_wave[w];
        blockSums[gid.x] = total;
    }
#else
    if (i >= g_capacity) return;
    uint2 before = uint2(0, 0);
    for (uint w = 0u; w < wave; ++w) before += gs_wave[w];
    const uint2 at = blockSums[gid.x] + before + inWave;  // exclusive offsets of this slot among alive / dying slots
    if (live)
    {
        FX_RWBUFFER(uint, aliveList, g_aliveList);
        FX_RWBUFFER(uint, keysA, g_keysA);
        FX_RWBUFFER(uint, valsA, g_valsA);
        FX_RWBUFFER(uint, keyBySlot, g_keyBySlot);
        aliveList[at.x] = i;
        keysA[at.x] = keyBySlot[i];
        valsA[at.x] = i;
    }
    else
    {
        FX_RWBUFFER(uint, deadList, g_deadList);
        deadList[i - at.x] = i;
        if (dying)
        {
            FX_RWBUFFER(uint, dyingList, g_dyingList);
            dyingList[at.y] = i;
            alive[i] = FX_SLOT_DEAD;
        }
    }
#endif
}
