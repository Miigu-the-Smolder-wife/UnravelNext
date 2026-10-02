// unx-kernel: cs_6_6 main
// unx-variants: SIZE=0,1
// Coverage composite, stage F1 (CoverageShade.hlsli): one group per (heavy pixel, run of COV_BLOCK records of its range)
// (x, y = heavy pixel, z = run; groups past the pixel's runs leave): the run's (depth key, element) pairs sorted nearer
// first by a bitonic network in groupshared and written to the pair buffer at the records' elements; the run's cursor
// starts at its first pair. The network is as large as the run: its records rounded up to a power of two, n, in
// log2(n) (log2(n) + 1) / 2 steps of n / 2 compare-exchanges (a run of 17..32 records: 15 steps of 16; a full run of
// COV_BLOCK: 55 steps of 512) - the order is the same total order whatever the size. Two kernels over the same groups,
// each taking its runs and leaving the others at once: SIZE=0 the runs of at most SORT_SHORT records in groups of 64
// threads (nearly every heavy pixel: a group of 256 would hold 2 to 64 working threads), SIZE=1 the longer ones in
// groups of 256. The group's work is at most one run of COV_BLOCK records whatever the pixel holds.
// P[0] = { state (raw), heavy records (raw), pairs UAV (raw, 2 words per record of V's pool), run cursors UAV (raw) },
// P[1] = { heavy capacity, V's records (StructuredBuffer<uint4>), visible clusters, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

#define SORT_SHORT 128u
#if SIZE == 0
#define SORT_THREADS 64u
#define SORT_KEYS SORT_SHORT
#else
#define SORT_THREADS 256u
#define SORT_KEYS COV_BLOCK
#endif
groupshared uint2 gs_run[SORT_KEYS];

[numthreads(SORT_THREADS, 1, 1)]
void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
    ByteAddressBuffer state = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer heavy = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer pairs = ResourceDescriptorHeap[P[0].z];
    RWByteAddressBuffer cursors = ResourceDescriptorHeap[P[0].w];
    StructuredBuffer<uint4> records = ResourceDescriptorHeap[P[1].y];
    const uint h = gid.x + gid.y * 65535, run = gid.z;
    if (h >= min(state.Load(4 * COVS_HEAVY), P[1].x)) return;  // uniform
    const uint4 rec = heavy.Load4(4 * h * COVH_WORDS);  // pixel, segment, count, runs
    if (run >= rec.w) return;                             // uniform
    const uint start = rec.y + run * COV_BLOCK, len = min(COV_BLOCK, rec.z - run * COV_BLOCK);
    if ((len > SORT_SHORT) != (SIZE == 1)) return;        // the other kernel's run (uniform)
    uint n = 2;  // the network's keys (uniform over the group)
    while (n < len) n <<= 1;
    [unroll] for (uint q = 0; q < SORT_KEYS / SORT_THREADS; ++q)
    {
        const uint i = q * SORT_THREADS + gi;
        if (i < n) gs_run[i] = i < len ? covKey(records, start + i) : COV_KEY_AFTER_ALL;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint size = 2; size <= n; size <<= 1)
        for (uint stride = size / 2; stride > 0; stride >>= 1)
        {
            [unroll] for (uint q2 = 0; q2 < SORT_KEYS / (2 * SORT_THREADS); ++q2)
            {
                const uint t = q2 * SORT_THREADS + gi;                 // compare-exchange t of n / 2
                if (t < n / 2)
                {
                    const uint i = (t / stride) * 2 * stride + t % stride, j = i + stride;
                    const bool up = (i & size) == 0;
                    const uint2 a = gs_run[i], b = gs_run[j];
                    if (up ? covBefore(b, a, records, P[1].z) : covBefore(a, b, records, P[1].z))
                    {
                        gs_run[i] = b;
                        gs_run[j] = a;
                    }
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    [unroll] for (uint q3 = 0; q3 < SORT_KEYS / SORT_THREADS; ++q3)
    {
        const uint i = q3 * SORT_THREADS + gi;
        if (i < len) pairs.Store2(8 * (start + i), gs_run[i]);
    }
    if (gi == 0) cursors.Store(4 * (heavy.Load(4 * (h * COVH_WORDS + COVH_CURSORS)) + run), start);
}
