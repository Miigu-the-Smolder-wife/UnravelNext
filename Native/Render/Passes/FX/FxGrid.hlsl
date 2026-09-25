// unx-kernel: cs_6_6 main
// unx-variants: STEP=2,3,4
// Collision candidate grid of the tick's surfaces (Particles.hlsli, fxSurfaceQuery): a spatial hash of cell g_gridCell
// with g_gridMask + 1 buckets. FxBegin clears the counts and fills, FxSurfaces writes each tick surface and counts its
// cells (or lists it as large).
//   STEP=2 scan: group per 1024 buckets, exclusive prefix of the counts inside the block -> start, block total -> blocks.
//   STEP=4 block offsets: one group, exclusive prefix of the block totals (a bucket's start = start + blocks[b >> 10]).
//          (A single-group scan of all buckets measured 19 us against 7 + 4 us for these two.)
//   STEP=3 fill: thread per surface of the grid: entries[start + atomic fill] = surface (order inside a bucket is free:
//          the candidates' order never changes a result).
// Every loop is bounded by the counts; entries beyond the capacity (surfaces x 64) cannot occur.
#include "Passes/FX/Particles.hlsli"

#if STEP == 2 || STEP == 4
groupshared uint gs_partial[1024];
#endif

[numthreads(STEP == 2 || STEP == 4 ? 1024 : 64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    FX_RWBUFFER(uint, counts, g_gridCount);
#if STEP == 2
    FX_RWBUFFER(uint, starts, g_gridStart);
    FX_RWBUFFER(uint, blocks, g_gridBlocks);
    const uint t = gtid.x, b = id.x, buckets = g_gridMask + 1u;
    const uint v = b < buckets ? counts[b] : 0u;
    gs_partial[t] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint o = t >= s ? gs_partial[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_partial[t] += o;
        GroupMemoryBarrierWithGroupSync();
    }
    if (b < buckets) starts[b] = gs_partial[t] - v;
    if (t == 1023u) blocks[b >> 10] = gs_partial[t];
#elif STEP == 4
    FX_RWBUFFER(uint, blocks, g_gridBlocks);
    const uint t = gtid.x, count = ((g_gridMask + 1u) + 1023u) / 1024u;
    const uint v = t < count ? blocks[t] : 0u;
    gs_partial[t] = v;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint o = t >= s ? gs_partial[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_partial[t] += o;
        GroupMemoryBarrierWithGroupSync();
    }
    if (t < count) blocks[t] = gs_partial[t] - v;
#else
    if (id.x >= g_surfaceCount) return;
    FX_RWBUFFER(uint, fills, g_gridFill);
    FX_RWBUFFER(StreamSurface, surfaces, g_tickSurfaces);
    int3 a, span;
    float3 lo, hi;
    float turn, carry;
    bool bounded;
    const uint cells = fxSurfaceCells(surfaces[id.x], a, span, lo, hi, turn, carry, bounded);
    if (cells == 0u) return;
    FX_RWBUFFER(uint, entries, g_gridEntries);
    for (uint k = 0u; k < cells; ++k)
    {
        const uint bucket = fxGridHash(fxGridCellOf(a, span, k)) & g_gridMask;
        uint j;
        InterlockedAdd(fills[bucket], 1u, j);
        const uint at = fxGridStart(bucket) + j;
        if (at < g_gridEntryCapacity) entries[at] = id.x;
        else fxStatus(FX_STATUS_RANGE);
    }
#endif
}
