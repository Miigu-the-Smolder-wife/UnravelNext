// unx-kernel: cs_6_6 main
// unx-variants: AREA=0,1
// Coverage composite, stage F2 (CoverageShade.hlsli): heavy round r (P[5].y; COV_ROUNDS dispatches a frame), one group of
// 32 threads per open heavy pixel (round 0: every heavy pixel; round r: active list r % 2, which round r - 1 filled).
//   1. The next COV_ROUND fragments nearer first, merged from the pixel's sorted runs: each lane holds the heads of runs
//      lane, lane + 32, ...; per step the wave's nearest head (max depth, then min element) is taken and its run advances.
//      A head behind the band A surface, or no head, ends the pixel; one more step only looks, so a pixel whose last
//      fragment is the round's last is finished here.
//   2. The taken fragments composite in order in parallel: each lane's visible share uses the mask union of the pixel so
//      far and of the lanes before it (a groupshared prefix), its weight w = max(0, min(r, 1 - R)) with R the weights
//      before it (the cap binds once, so the prefix of the uncapped r gives the capped w), and the lanes with w > 0 shade
//      (covShadeFragment) in parallel.
//   3. Lane 0 stores the pixel's union, weight sum, radiance and done flag; an unfinished pixel goes to the next round's
//      list (after the last round CoverageHeavyFinish reports it: COV_M_ERROR_ROUNDS).
// Per group: COV_ROUND steps over the lane's runs (runs <= the pool's capacity / COV_BLOCK) and COV_ROUND shadings.
// P[0] = { chunk records (StructuredBuffer<uint4>), state UAV (raw), heavy records UAV (raw), run cursors UAV (raw) }
// P[1], P[3], P[4], P[5].x: the shading constants (CoverageShade.hlsli)
// P[2] = { 0, 0, 0, pairs (raw, CoverageHeavySort) }, P[5] = { .., round, heavy capacity, active lists UAV (raw, 2 x heavy capacity words) }
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

groupshared uint gs_mask[COV_ROUND], gs_visId[COV_ROUND], gs_depth[COV_ROUND];

[numthreads(32, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer state = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer heavy = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer cursors = ResourceDescriptorHeap[P[0].w];
    ByteAddressBuffer pairs = ResourceDescriptorHeap[P[2].w];
    RWByteAddressBuffer lists = ResourceDescriptorHeap[P[5].w];
    const uint roundIndex = P[5].y, capacity = P[5].z;
    const uint open = roundIndex == 0 ? min(state.Load(4 * COVS_HEAVY), capacity) : state.Load(4 * (COVS_OPEN + roundIndex % 2));
    const uint g = gid.x + gid.y * 65535;
    if (g >= open) return;  // uniform
    const uint h = roundIndex == 0 ? g : lists.Load(4 * ((roundIndex % 2) * capacity + g));
    const uint base = 4 * h * COVH_WORDS;
    const uint4 rec0 = heavy.Load4(base);        // pixel, segment, count, runs
    const uint4 rec1 = heavy.Load4(base + 16);   // cursors, covered, used, done
    const uint4 rec2 = heavy.Load4(base + 32);   // sum rgb, last depth
    const uint lastVisId = heavy.Load(base + 48);
    const uint2 pixel = uint2(rec0.x & 0xFFFFu, rec0.x >> 16);
    const uint4 probeRecord = covProbeFetch(pixel / COV_TILE_PX, lane);
    if (P[4].w != UNX_NONE && (P[3].z & 6) != 6) giProbeTileStore(lane, probeRecord);
    GroupMemoryBarrierWithGroupSync();
    Texture2D<float> bandDepth = ResourceDescriptorHeap[P[1].z];
    const uint bandA = asuint(bandDepth[pixel]);

    // 1. Merge the next COV_ROUND fragments from the runs.
    uint2 mine = COV_KEY_AFTER_ALL;
    uint taken = 0;
    bool exhausted = false;
    [loop] for (uint t = 0; t <= COV_ROUND; ++t)
    {
        uint2 best = COV_KEY_AFTER_ALL;
        uint bestRun = 0;
        for (uint run = lane; run < rec0.w; run += COV_ROUND)
        {
            const uint cursor = cursors.Load(4 * (rec1.x + run)), end = rec0.y + min((run + 1) * COV_BLOCK, rec0.z);
            if (cursor >= end) continue;
            const uint2 key = pairs.Load2(8 * cursor);
            if (covBefore(key, best))
            {
                best = key;
                bestRun = run;
            }
        }
        const uint depth = WaveActiveMax(best.x);
        const uint element = WaveActiveMin(best.x == depth ? best.y : 0xFFFFFFFFu);
        if ((depth == 0 && element == 0xFFFFFFFFu) || depth < bandA)
        {
            exhausted = true;  // no fragment left in front of the band A surface
            break;
        }
        if (t == COV_ROUND) break;  // a fragment is left for the next round
        if (best.x == depth && best.y == element) cursors.Store(4 * (rec1.x + bestRun), cursors.Load(4 * (rec1.x + bestRun)) + 1);
        if (lane == t) mine = uint2(depth, element);
        taken = t + 1;
    }

    // 2. Composite the taken fragments in order.
    const bool valid = lane < taken;
    CoverageFragment f = (CoverageFragment)0;
    if (valid) f = coverageUnpackRecord(records[mine.y]);
    gs_visId[lane] = f.visId;
    gs_depth[lane] = mine.x;
    GroupMemoryBarrierWithGroupSync();
    const uint prevVisId = lane == 0 ? lastVisId : gs_visId[lane - 1];
    const uint prevDepth = lane == 0 ? rec2.w : gs_depth[lane - 1];
    const bool counts = valid && !(f.visId == prevVisId && mine.x == prevDepth);  // the hardware's clip duplicate adds nothing
    gs_mask[lane] = counts ? f.mask : 0;
    GroupMemoryBarrierWithGroupSync();
    uint before = rec1.y;
    for (uint b = 0; b < lane; ++b) before |= gs_mask[b];
    const uint bits = countbits(f.mask);
    const float seen = bits > 0 ? countbits(f.mask & ~before) / (float)bits : 1 - countbits(before) / 32.0;
    const float r = counts ? coverageFragmentArea(f) * seen : 0;
    const float used = asfloat(rec1.z);
    const float w = max(0.0, min(r, 1 - (used + WavePrefixSum(r))));
    const float3 add = w > 0 ? w * covShadeFragment(f.visId, pixel, P[3].z) : 0;
    const float3 sum = asfloat(rec2.xyz) + float3(WaveActiveSum(add.x), WaveActiveSum(add.y), WaveActiveSum(add.z));
    const uint covered = rec1.y | WaveActiveBitOr(counts ? f.mask : 0u);
    const float usedNext = min(1.0, used + WaveActiveSum(r));
    const bool done = covered == COV_MASK_FULL || usedNext >= 1 || exhausted || taken < COV_ROUND;

    // 3. The pixel's state; an open pixel to the next round.
    const uint lastDepth = taken > 0 ? gs_depth[taken - 1] : rec2.w;
    const uint lastVis = taken > 0 ? gs_visId[taken - 1] : lastVisId;
    if (lane != 0) return;
    heavy.Store3(base + 20, uint3(covered, asuint(usedNext), done ? 1u : 0u));
    heavy.Store4(base + 32, uint4(asuint(sum), lastDepth));
    heavy.Store(base + 48, lastVis);
    if (done || roundIndex + 1 >= COV_ROUNDS) return;
    uint slot;
    state.InterlockedAdd(4 * (COVS_OPEN + (roundIndex + 1) % 2), 1, slot);
    lists.Store(4 * (((roundIndex + 1) % 2) * capacity + slot), h);
}
