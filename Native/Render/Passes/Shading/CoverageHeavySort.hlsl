// unx-kernel: cs_6_6 main
// Coverage composite, stage F1 (CoverageShade.hlsli): one group of 256 threads per (heavy pixel, run of COV_BLOCK
// records of its range) (x, y = heavy pixel, z = run; groups past the pixel's runs leave): the run's (depth key, element)
// pairs sorted nearer first by a bitonic network in groupshared (55 steps of 2 compare-exchanges per thread) and written
// to the pair buffer at the records' elements; the run's cursor starts at its first pair. The group's work is one run of
// COV_BLOCK records whatever the pixel holds.
// P[0] = { state (raw), heavy records (raw), pairs UAV (raw, 2 words per record of V's pool), run cursors UAV (raw) },
// P[1] = { heavy capacity, V's records (StructuredBuffer<uint4>), visible clusters, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/CoverageShade.hlsli"

groupshared uint2 gs_run[COV_BLOCK];

[numthreads(256, 1, 1)]
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
    [unroll] for (uint q = 0; q < COV_BLOCK / 256; ++q)
    {
        const uint i = q * 256 + gi;
        gs_run[i] = i < len ? covKey(records, start + i) : COV_KEY_AFTER_ALL;
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint size = 2; size <= COV_BLOCK; size <<= 1)
        for (uint stride = size / 2; stride > 0; stride >>= 1)
        {
            [unroll] for (uint q2 = 0; q2 < COV_BLOCK / 512; ++q2)
            {
                const uint t = q2 * 256 + gi;                          // compare-exchange t of 512
                const uint i = (t / stride) * 2 * stride + t % stride, j = i + stride;
                const bool up = (i & size) == 0;
                const uint2 a = gs_run[i], b = gs_run[j];
                if (up ? covBefore(b, a, records, P[1].z) : covBefore(a, b, records, P[1].z))
                {
                    gs_run[i] = b;
                    gs_run[j] = a;
                }
            }
            GroupMemoryBarrierWithGroupSync();
        }
    [unroll] for (uint q3 = 0; q3 < COV_BLOCK / 256; ++q3)
    {
        const uint i = q3 * 256 + gi;
        if (i < len) pairs.Store2(8 * (start + i), gs_run[i]);
    }
    if (gi == 0) cursors.Store(4 * (heavy.Load(4 * (h * COVH_WORDS + COVH_CURSORS)) + run), start);
}
