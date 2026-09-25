// unx-kernel: cs_6_6 main
// unx-variants: STEP=2,3
// Collision candidate grid of the tick's surfaces (Particles.hlsli, fxSurfaceQuery): a spatial hash of cell g_gridCell
// with g_gridMask + 1 buckets. FxBegin clears the counts and fills, FxSurfaces writes each tick surface and counts its
// cells (or lists it as large).
//   STEP=2 scan: one group; thread t sums its run of buckets/1024 consecutive counts, a 1024-wide scan of the run totals,
//          then each run is written as an exclusive prefix -> start. (One dispatch for any bucket count.)
//   STEP=3 fill: thread per surface of the grid: entries[start + atomic fill] = surface (order inside a bucket is free:
//          the candidates' order never changes a result).
// Every loop is bounded by the counts; entries beyond the capacity (surfaces x 64) cannot occur.
#include "Passes/FX/Particles.hlsli"

#if STEP == 2
groupshared uint gs_partial[1024];
#endif

[numthreads(STEP == 2 ? 1024 : 64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID)
{
    FX_RWBUFFER(uint, counts, g_gridCount);
#if STEP == 2
    FX_RWBUFFER(uint, starts, g_gridStart);
    const uint t = gtid.x, buckets = g_gridMask + 1u, run = (buckets + 1023u) / 1024u, first = t * run;
    uint total = 0u;
    for (uint k = 0u; k < run; ++k)
        if (first + k < buckets) total += counts[first + k];
    gs_partial[t] = total;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 1024u; s <<= 1)
    {
        const uint o = t >= s ? gs_partial[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_partial[t] += o;
        GroupMemoryBarrierWithGroupSync();
    }
    uint at = gs_partial[t] - total;
    for (uint k2 = 0u; k2 < run; ++k2)
        if (first + k2 < buckets) { starts[first + k2] = at; at += counts[first + k2]; }
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
